/*
 *  qemu user cpu loop
 *
 *  Copyright (c) 2003-2008 Fabrice Bellard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu.h"
#include "user-internals.h"
#include "cpu_loop-common.h"
#include "signal-common.h"
#include "qemu/guest-random.h"
#include "semihosting/common-semi.h"
#include "target/arm/syndrome.h"
#include "target/arm/cpu-features.h"
#include "user/nb-qemu.h"

#define get_user_code_u32(x, gaddr, env)                \
    ({ abi_long __r = get_user_u32((x), (gaddr));       \
        if (!__r && bswap_code(arm_sctlr_b(env))) {     \
            (x) = bswap32(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_code_u16(x, gaddr, env)                \
    ({ abi_long __r = get_user_u16((x), (gaddr));       \
        if (!__r && bswap_code(arm_sctlr_b(env))) {     \
            (x) = bswap16(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_data_u32(x, gaddr, env)                \
    ({ abi_long __r = get_user_u32((x), (gaddr));       \
        if (!__r && arm_cpu_bswap_data(env)) {          \
            (x) = bswap32(x);                           \
        }                                               \
        __r;                                            \
    })

#define get_user_data_u16(x, gaddr, env)                \
    ({ abi_long __r = get_user_u16((x), (gaddr));       \
        if (!__r && arm_cpu_bswap_data(env)) {          \
            (x) = bswap16(x);                           \
        }                                               \
        __r;                                            \
    })

#define put_user_data_u32(x, gaddr, env)                \
    ({ typeof(x) __x = (x);                             \
        if (arm_cpu_bswap_data(env)) {                  \
            __x = bswap32(__x);                         \
        }                                               \
        put_user_u32(__x, (gaddr));                     \
    })

#define put_user_data_u16(x, gaddr, env)                \
    ({ typeof(x) __x = (x);                             \
        if (arm_cpu_bswap_data(env)) {                  \
            __x = bswap16(__x);                         \
        }                                               \
        put_user_u16(__x, (gaddr));                     \
    })

/* AArch64 main loop */
void cpu_loop(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);
    int trapnr, ec, fsc, si_code, si_signo;
    abi_long ret;
    bool nb_need_stop = false;

    for (;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        process_queued_cpu_work(cs);

        switch (trapnr) {
        case EXCP_SWI:
            /* On syscall, PSTATE.ZA is preserved, PSTATE.SM is cleared. */
            aarch64_set_svcr(env, 0, R_SVCR_SM_MASK);
            //TODO: should we hook before the aarch64_set_svcr?

            // /* FIXME: Linux ignores the immediate, so an adversarial program could use a non-zero value */
            // /* We need a judgement to ensure the environment is from guest thunk lib */
            // if( (env->exception.syndrome & 0xffff)!=0 && svc_handler ){
            //     svc_handler(env, env->exception.syndrome & 0xffff);
            //     break;
            // }

            /* new method, let's check which kind of interrupt we meet */
            if (_nb_qemu_){
                // is it a qemu_call pc stop?
                if (env->pc - 4 == nb_stop) {//TODO: the address may need to convert
                    //we cant return at here, as the next time we have to re cpu_loop again.
                    nb_need_stop = true;
                    break;
                }
                // is it a guest call host generic call addr?
                else if (env->pc - 4 == nb_call_host) {//TODO: the address may need to convert
                    // call qemu_android_call_host_handler
                    // if error happen in handler, exit directly.
                    qemu_android_call_host_handler(env);
                    break;
                }
            }
            // Now the call_host_static could work with binfmt_misc mode

            // is it a static call host tramp interrupt?
            unsigned int insn; //TODO: this may need to put outside
            get_user_code_u32(insn, env->pc, env); /* FIXME - what to do if get_user() fails? *///TODO: the address may need to convert
            fprintf(stderr, "cpu_loop get_user_code_u32: 0x%x, remove this log once we tested\n", insn);
            if (insn == 0x14000000) { // asm: "b ." is an Infinite loop
                // check and extract the number after this, send it to the handler
                uint16_t lib_num, sym_num;
                get_user_u16(lib_num, env->pc + 4);//TODO: the address may need to convert
                get_user_u16(sym_num, env->pc + 6);//TODO: the address may need to convert
                //TODO: call handler
                // if handle successful, skip the insn to return,
                // else just exit anyway, should make some assertion in handler.
                // but if the number is not accepted , run it any way
                if (!qemu_android_call_host_static(env, lib_num, sym_num))
                    env->pc += 8;//skip b. and  number
                //do something
                break;
            }
            // After 4 if and a get_user_code_u32, do syscall as usual

            ret = do_syscall(env,
                             env->xregs[8],
                             env->xregs[0],
                             env->xregs[1],
                             env->xregs[2],
                             env->xregs[3],
                             env->xregs[4],
                             env->xregs[5],
                             0, 0);
            if (ret == -QEMU_ERESTARTSYS) {
                env->pc -= 4;
            } else if (ret != -QEMU_ESIGRETURN) {
                env->xregs[0] = ret;
            }
            break;
        case EXCP_INTERRUPT:
            /* just indicate that signals should be handled asap */
            break;
        case EXCP_UDEF:
            force_sig_fault(TARGET_SIGILL, TARGET_ILL_ILLOPN, env->pc);
            break;
        case EXCP_PREFETCH_ABORT:
        case EXCP_DATA_ABORT:
            ec = syn_get_ec(env->exception.syndrome);
            switch (ec) {
            case EC_DATAABORT:
            case EC_INSNABORT:
                /* Both EC have the same format for FSC, or close enough. */
                fsc = extract32(env->exception.syndrome, 0, 6);
                switch (fsc) {
                case 0x04 ... 0x07: /* Translation fault, level {0-3} */
                    si_signo = TARGET_SIGSEGV;
                    si_code = TARGET_SEGV_MAPERR;
                    break;
                case 0x09 ... 0x0b: /* Access flag fault, level {1-3} */
                case 0x0d ... 0x0f: /* Permission fault, level {1-3} */
                    si_signo = TARGET_SIGSEGV;
                    si_code = TARGET_SEGV_ACCERR;
                    break;
                case 0x11: /* Synchronous Tag Check Fault */
                    si_signo = TARGET_SIGSEGV;
                    si_code = TARGET_SEGV_MTESERR;
                    break;
                case 0x21: /* Alignment fault */
                    si_signo = TARGET_SIGBUS;
                    si_code = TARGET_BUS_ADRALN;
                    break;
                default:
                    g_assert_not_reached();
                }
                break;
            case EC_PCALIGNMENT:
                si_signo = TARGET_SIGBUS;
                si_code = TARGET_BUS_ADRALN;
                break;
            default:
                g_assert_not_reached();
            }
            force_sig_fault(si_signo, si_code, env->exception.vaddress);
            break;
        case EXCP_DEBUG:
        case EXCP_BKPT:
            force_sig_fault(TARGET_SIGTRAP, TARGET_TRAP_BRKPT, env->pc);
            break;
        case EXCP_SEMIHOST:
            do_common_semihosting(cs);
            env->pc += 4;
            break;
        case EXCP_YIELD:
            /* nothing to do here for user-mode, just resume guest code */
            break;
        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;
        default:
            EXCP_DUMP(env, "qemu: unhandled CPU exception 0x%x - aborting\n", trapnr);
            abort();
        }

        /* Check for MTE asynchronous faults */
        if (unlikely(env->cp15.tfsr_el[0])) {
            env->cp15.tfsr_el[0] = 0;
            force_sig_fault(TARGET_SIGSEGV, TARGET_SEGV_MTEAERR, 0);
        }

        process_pending_signals(env);
        /* Exception return on AArch64 always clears the exclusive monitor,
         * so any return to running guest code implies this.
         */
        env->exclusive_addr = -1;
        /* in nb-qemu mode, stop and handle by qemu_android call */
        // if (_nb_qemu_ && (trapnr==EXCP_YIELD)){
        //     return;
        // }
        if (nb_need_stop) return;
    }
}

void target_cpu_copy_regs(CPUArchState *env, struct target_pt_regs *regs)
{
    ARMCPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);
    TaskState *ts = get_task_state(cs);
    struct image_info *info = ts->info;
    int i;

    if (!(arm_feature(env, ARM_FEATURE_AARCH64))) {
        fprintf(stderr,
                "The selected ARM CPU does not support 64 bit mode\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < 31; i++) {
        env->xregs[i] = regs->regs[i];
    }
    env->pc = regs->pc;
    env->xregs[31] = regs->sp;
#if TARGET_BIG_ENDIAN
    env->cp15.sctlr_el[1] |= SCTLR_E0E;
    for (i = 1; i < 4; ++i) {
        env->cp15.sctlr_el[i] |= SCTLR_EE;
    }
    arm_rebuild_hflags(env);
#endif

    if (cpu_isar_feature(aa64_pauth, cpu)) {
        qemu_guest_getrandom_nofail(&env->keys, sizeof(env->keys));
    }

    ts->stack_base = info->start_stack;
    ts->heap_base = info->brk;
    /* This will be filled in on the first SYS_HEAPINFO call.  */
    ts->heap_limit = 0;
}
