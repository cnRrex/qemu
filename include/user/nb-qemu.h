/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Declaration of nb-qemu.
 *
 */

#ifndef USER_NB_QEMU_H
#define USER_NB_QEMU_H

#ifndef CONFIG_USER_ONLY
#error Cannot include this header from system emulation
#endif

#include <stddef.h>
#include <stdint.h>

extern bool _nb_qemu_;

extern bool _nb_debug_;

extern bool _binfmt_with_nb_;

extern int (* __a_log_print)(int prio, const char *tag, const char *fmt, ...);

int qemu_android_setup_guest(struct image_info *info_, struct linux_binprm *bprm_, const char **cpu_type_);

int start_logger(const char *name);

/*
 * TODO: target related abi_ulong
 * when 64bit-host 32-bit-guest all these interface need to redesign
 */
typedef intptr_t (*syscall_handler_t)(void *cpu_env, int num,
                                                   intptr_t arg1, intptr_t arg2,
                                                   intptr_t arg3, intptr_t arg4,
                                                   intptr_t arg5,
                                                   intptr_t arg6);

typedef void (*svc_handler_t)(void *cpu_env, uint16_t num);

extern syscall_handler_t syscall_handler;

extern svc_handler_t svc_handler;

void qemu_android_call_host_handler(CPUArchState *env);

int qemu_android_call_host_static(CPUArchState *env, uint16_t lib_num, uint16_t sym_num);

// These are for pthread things on new qemu_cpu
extern __thread abi_ulong alloc_child_stack;
extern __thread target_ulong alloc_new_tls;
extern __thread bool need_new_cpu;
extern __thread bool need_del_cpu;

// old pc stop is using arm yield, 
// new method uses the syscall interrupt and check something

// The instuction addr of where a qemu_call stop (host call guest)
extern abi_ulong nb_stop;
// The instuction addr of where a call host tramp reach (host call guest)
extern abi_ulong nb_call_host;


#if defined(TARGET_I386) && defined(TARGET_ABI32) //x86
// target/i386/tcg/fpu_helper.c

extern void helper_fpush(CPUX86State *env);
extern void helper_fpop(CPUX86State *env);

// These two helpers contain fpush
extern void helper_flds_ST0(CPUX86State *env, uint32_t val);
extern void helper_fldl_ST0(CPUX86State *env, uint64_t val);

// These two helpers NOT contain fpop
extern uint32_t helper_fsts_ST0(CPUX86State *env);
extern uint64_t helper_fstl_ST0(CPUX86State *env);
#endif

#endif
