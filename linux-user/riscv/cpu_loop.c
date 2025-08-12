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
#include "qemu/error-report.h"
#include "qemu.h"
#include "user-internals.h"
#include "cpu_loop-common.h"
#include "signal-common.h"
#include "elf.h"
#include "semihosting/common-semi.h"
#include "user/nb-qemu.h"

void cpu_loop(CPURISCVState *env)
{
    CPUState *cs = env_cpu(env);
    int trapnr;
    target_ulong ret;
    bool nb_need_stop = false;

    for (;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        process_queued_cpu_work(cs);

        switch (trapnr) {
        case EXCP_INTERRUPT:
            /* just indicate that signals should be handled asap */
            break;
        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;
        case RISCV_EXCP_U_ECALL:
            env->pc += 4;
            /* let's check which kind of interrupt we meet */
            if (_nb_qemu_){
                // is it a qemu_call pc stop?
                if (env->pc - 4 == nb_stop) {
                    //we cant return at here, as the next time we have to re cpu_loop again.
                    nb_need_stop = true;
                    break;
                }
                // is it a guest call host generic call addr?
                else if (env->pc - 4 == nb_call_host) {
                    // call qemu_android_call_host_handler
                    // if error happen in handler, exit directly.
                    qemu_android_call_host_handler(env);
                    break;
                }
            }
            // Now the call_host_static could work with binfmt_misc mode

            // is it a static call host tramp interrupt?
            unsigned int insn; //TODO: this may need to put outside
            get_user_u32(insn, env->pc); /* FIXME - what to do if get_user() fails? */
            fprintf(stderr, "cpu_loop get_user_u32: 0x%x, remove this log once we tested\n", insn);
            if (insn == 0x000000EF) { // asm: "jal ." is an Infinite loop
                // check and extract the number after this, send it to the handler
                uint16_t lib_num, sym_num;

                get_user_u16(lib_num, env->pc + 4);
                get_user_u16(sym_num, env->pc + 6);
                //TODO: call handler
                // if handle successful, skip the insn to return,
                // else just exit anyway, should make some assertion in handler.
                // but if the number is not accepted , run it any way
                if (!qemu_android_call_host_static(env, lib_num, sym_num))
                    env->pc += 8;//skip jal . and  number
                //do something
                break;
            }
            //After 4 if and a get_user, do syscall as usual

            if (env->gpr[xA7] == TARGET_NR_arch_specific_syscall + 15) {
                /* riscv_flush_icache_syscall is a no-op in QEMU as
                   self-modifying code is automatically detected */
                ret = 0;
            } else {
                ret = do_syscall(env,
                                 env->gpr[(env->elf_flags & EF_RISCV_RVE)
                                    ? xT0 : xA7],
                                 env->gpr[xA0],
                                 env->gpr[xA1],
                                 env->gpr[xA2],
                                 env->gpr[xA3],
                                 env->gpr[xA4],
                                 env->gpr[xA5],
                                 0, 0);
            }
            if (ret == -QEMU_ERESTARTSYS) {
                env->pc -= 4;
            } else if (ret != -QEMU_ESIGRETURN) {
                env->gpr[xA0] = ret;
            }
            if (cs->singlestep_enabled) {
                goto gdbstep;
            }
            break;
        case RISCV_EXCP_ILLEGAL_INST:
            force_sig_fault(TARGET_SIGILL, TARGET_ILL_ILLOPC, env->pc);
            break;
        case RISCV_EXCP_BREAKPOINT:
        case EXCP_DEBUG:
        gdbstep:
            force_sig_fault(TARGET_SIGTRAP, TARGET_TRAP_BRKPT, env->pc);
            break;
        case RISCV_EXCP_SEMIHOST:
            do_common_semihosting(cs);
            env->pc += 4;
            break;
        default:
            EXCP_DUMP(env, "\nqemu: unhandled CPU exception %#x - aborting\n",
                     trapnr);
            exit(EXIT_FAILURE);
        }

        process_pending_signals(env);

        if (nb_need_stop) return;
    }
}

void target_cpu_copy_regs(CPUArchState *env, struct target_pt_regs *regs)
{
    CPUState *cpu = env_cpu(env);
    TaskState *ts = get_task_state(cpu);
    struct image_info *info = ts->info;

    env->pc = regs->sepc;
    env->gpr[xSP] = regs->sp;
    env->elf_flags = info->elf_flags;

    if ((env->misa_ext & RVE) && !(env->elf_flags & EF_RISCV_RVE)) {
        error_report("Incompatible ELF: RVE cpu requires RVE ABI binary");
        exit(EXIT_FAILURE);
    }

    ts->stack_base = info->start_stack;
    ts->heap_base = info->brk;
    /* This will be filled in on the first SYS_HEAPINFO call.  */
    ts->heap_limit = 0;
}
