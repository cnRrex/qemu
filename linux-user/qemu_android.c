/*
 * nb-qemu
 *
 * Copyright (c) 2019 Michael Goffioul
 * Copyright (c) 2025 Zyrrex Zhao
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu_android.h"
#include "qemu/osdep.h"
#include "qemu.h"
#include "qemu/guest-random.h"
#include "disas/disas.h"
#include "tcg/tcg.h"
#include "tcg/startup.h"
#include "elf.h"
#include "cpu_loop-common.h"
#include "user/nb-qemu.h"
#include "user-internals.h"
#include "user-mmap.h"
#include "loader.h"
#include "qemu_android_interface.h"
#include <glib.h>

const char LOG_TAG[] =  "QemuAndroid-" TARGET_NAME ;
//#define LOG_NDEBUG 0

// NOTE: this will drown log, debug only
#define QEMU_LOG_MASK "page,unimp" // exec,nochain

//use QA_abi_ptr for qemuAndroidItf, this struct should be wrap by c++ class in nb-qemu
//QA_abi_ptr is a union of guest_addr, has guest width uint. 
//as we may have 32/64 bit guest in 64-bit host, this ptr may have different types
#ifdef TARGET_ABI32
#define QA_ABI_PTR_SET(ptr, val) ( (ptr).addr32 = val )
#define QA_ABI_PTR_GET(ptr) ( (uint32_t)((ptr).addr32) )
#else
#define QA_ABI_PTR_SET(ptr, val) ( (ptr).addr64 = val )
#define QA_ABI_PTR_GET(ptr) ( (uint64_t)((ptr).addr64) )
#endif


static abi_ptr thread_allocate_;
static abi_ptr thread_deallocate_;

/* info1 and bprm are store in main, get info and bprm(pointer) copied to here */
static struct image_info *info;
static struct linux_binprm *bprm;
static const char **cpu_type; /* point to cpu_type in main */

__thread abi_ulong alloc_child_stack = 0;
__thread abi_ulong alloc_new_tls = 0;
__thread bool need_new_cpu = false;
__thread bool need_del_cpu = false;

syscall_handler_t syscall_handler = NULL;
svc_handler_t svc_handler = NULL;

/**
 * TODO:
 * build libnb-qemu and guest to test these changes
 * 64bit host 32bit guest thunk (need java bridge support)
 */

/* When enter qemu_main there are many things we need, like image_info and binprm.
 * Send them to here by this */
int qemu_android_setup_guest(struct image_info *info_, struct linux_binprm *bprm_, const char **cpu_type_){
    info = info_;
    bprm = bprm_;
    cpu_type = cpu_type_;
    //this two should be initialized before we run everything
    nb_stop = QA_ABI_PTR_GET(qemu_android_lookup_symbol("__qemu_call_stop__"));
    nb_call_host = QA_ABI_PTR_GET(qemu_android_lookup_symbol("__qemu_call_host_stop__"));
    if (nb_stop && nb_call_host) {
      return 0;
    }
    return -1;

}



/* qemuAndroid entry set _nb_qemu_ here instead of qemu_main */
//TODO: initialize only once
int qemu_android_initialize(const char *procname, const char *tmpdir,
                            const char **qemu_envp, const char *guest_entry) {
  if(first_cpu){
      __a_log_print(2, LOG_TAG, "Qemu cpus_queue is already created: %p", (void *)first_cpu);
      return 0;
  }else{
    _nb_qemu_ = true;
    // anyother option goes qemu_envp, QEMU_SET_ENV, QEMU_LD_PREFIX,
    // QEMU_INTERPRETER, QEMU_LOG etc. end with NULL
    char *argv[] = {(char *)LOG_TAG, (char *)"-no-p-flag", (char *)"-T", (char *)tmpdir,
                    (char *)"-0", (char *)procname, (char *)guest_entry};
    int result = qemu_main(sizeof(argv) / sizeof(char *), argv, (char **)qemu_envp);
    //let log_handle maintained by log_redirector, get its fcn pointer for log.
    if (result == 0) {
        thread_allocate_ = QA_ABI_PTR_GET(qemu_android_lookup_symbol("qemu_android_allocateThread"));
        __a_log_print(2, LOG_TAG, "QemuAndroid::thread_allocate_: %p", (void *)thread_allocate_);
        thread_deallocate_ = QA_ABI_PTR_GET(qemu_android_lookup_symbol("qemu_android_deallocateThread"));
        __a_log_print(2, LOG_TAG, "QemuAndroid::thread_deallocate_: %p", (void *)thread_deallocate_);
        __a_log_print(2, LOG_TAG, "New thread cpu: %p", thread_cpu);
        __a_log_print(2, LOG_TAG, "QemuAndroid::nb_call_host: %p", (void *)nb_call_host);
        __a_log_print(2, LOG_TAG, "QemuAndroid::nb_stop: %p", (void *)nb_stop);

    }
    return result;
  }
}

QA_abi_ptr qemu_android_lookup_symbol(const char *name) {
  if (dynsyminfos) {
    struct syminfo *s = dynsyminfos;

    while (s) {
#ifdef TARGET_ABI32
      struct elf32_sym *syms = s->disas_symtab.elf32;
#else
      struct elf64_sym *syms = s->disas_symtab.elf64;
#endif

      //TODO: can this find the mark?
      for (int i = 0; i < s->disas_num_syms; i++) {
        if (strcmp(s->disas_strtab + syms[i].st_name, name) == 0) {
          QA_abi_ptr guest_addr_ptr;
          QA_ABI_PTR_SET(guest_addr_ptr, syms[i].st_value);
          return guest_addr_ptr;
        }
      }
      s = s->next;
    }
  }

  return 0;
}

QA_abi_ptr qemu_android_malloc(size_t size) {
  abi_ptr guest_addr;
  QA_abi_ptr guest_addr_ptr;

  guest_addr = target_mmap(0, size + sizeof(size), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (guest_addr == -1){
    QA_ABI_PTR_SET(guest_addr_ptr, 0);
    return guest_addr_ptr;
  }
  copy_to_user(guest_addr, &size, sizeof(size));
  QA_ABI_PTR_SET(guest_addr_ptr, guest_addr + sizeof(size))
  return guest_addr_ptr;
}

void qemu_android_free(QA_abi_ptr addr) {
  size_t size;
  abi_ptr guest_addr = QA_ABI_PTR_GET(addr) - sizeof(size);

  if (copy_from_user(&size, guest_addr, sizeof(size)) == 0) {
    target_munmap(guest_addr, size);
  }
}

void qemu_android_memcpy(QA_abi_ptr dest, const void *src, size_t length) {
  copy_to_user(QA_ABI_PTR_GET(dest), (void *)src, length);
}

const char *qemu_android_get_string(QA_abi_ptr addr) {
  return (const char *)lock_user_string(QA_ABI_PTR_GET(addr));
}

void qemu_android_release_string(const char *s, QA_abi_ptr addr) {
  unlock_user((void *)s, QA_ABI_PTR_GET(addr), 0);
}

void *qemu_android_get_memory(QA_abi_ptr addr, size_t length) {
  return lock_user(VERIFY_READ, QA_ABI_PTR_GET(addr), length, 1);
}

void qemu_android_release_memory(void *ptr, QA_abi_ptr addr, size_t length) {
  unlock_user(ptr, QA_ABI_PTR_GET(addr), length);
}

// NOTE: when virtual address smaller or 32bit target on 64bit host we need to
// check valid NOTE: arm64 tag may have problem
QA_abi_ptr qemu_android_h2g(void *addr) {
  QA_abi_ptr guest_ptr;
  if(h2g_valid(addr))
    QA_ABI_PTR_SET(guest_ptr, h2g(addr));
  else
    QA_ABI_PTR_SET(guest_ptr, 0);
  return guest_ptr;
}

bool qemu_android_h2g_valid(void *addr) {
  return h2g_valid(addr);
}

void *qemu_android_g2h(QA_abi_ptr addr) {
    if(thread_cpu)
        return g2h(thread_cpu, QA_ABI_PTR_GET(addr));
    __a_log_print(6, LOG_TAG, "thread_cpu is NULL, fallback to g2h_untagged");
    return g2h_untagged(QA_ABI_PTR_GET(addr));
}

void qemu_android_register_syscall_handler(
    qemu_android_syscall_handler_t func) {
  syscall_handler = func;
}

void qemu_android_register_svc_handler(qemu_android_svc_handler_t func) {
  svc_handler = func;
}

/////////////////////////////////////////////////////////////////////////////
// qemu_call func
// TODO: check stack after call, we need to fix longjmp maybe
#if defined(TARGET_ARM) && defined(TARGET_ABI32) //arm32

#define REGS(r) env->regs[r]
#define SP_REG REGS(13)
#define LR_REG REGS(14)
#define PC_REG REGS(15)
#define RET_REG REGS(0)
//perform android arm calling
static inline void qemu_android_call_arm(abi_ptr addr, QemuAndroidCallInfo *info, QemuAndroidCallData *data){
  CPUArchState *env = cpu_env(data->cpu);

  abi_ulong saved_lr = LR_REG;
  abi_ulong saved_pc = PC_REG;
  abi_ulong saved_thumb = env->thumb;

  //TODO: what kind of arm addr qemu provide?
  PC_REG = addr & 0xfffffffe;//arm specific, the last bit is thumb mark ,see init_thread
  env->thumb = addr & 1;
  LR_REG = nb_stop; //the nb_stop is arm code so the addr is not addr & 1

  //copy regs
  uint32_t int_regs_used = ( info->int_regs_used <= 4 ) ? info->int_regs_used : 4;
  uint32_t float_regs_used = ( info->float_regs_used <= 4 ) ? info->float_regs_used : 4;

  for (uint32_t i = 0; i < int_regs_used; i++){
    REGS(i) = data->int32_regs[i];
  }
  //TODO: how to copy float regs?
  //TODO: armhf vfp calling convention (maybe in os lib register some guest vulkan fcn)
  //NOTE: to copy directly in here, we also use float128 (construct by nb-qemu)
  for (uint32_t i = 0; i < float_regs_used; i++){
    env->vfp.zregs[i].d[0] = data->float128_regs[i].low;
    env->vfp.zregs[i].d[1] = data->float128_regs[i].high;
  }

  if (data->stack) {
    /* alloc space to push arg stack */
    /* TODO: What if stack have not enough space? */
    SP_REG -= info->stack_size;
    memcpy_to_target(SP_REG, data->stack, info->stack_size);
  }
  /* Now cpu is in exception, reset it */
  cpu->exception_index = -1;

  /* env has setup, start cpu to call guest fcn code */
  cpu_loop(env);

  if (data->stack) {
    /* the way of __cdecl __fastcall need us to pop all the arg, restore */
    SP_REG += info->stack_size;
  }

  /* restore the retuen pointer, pc, thumb mode */
  LR_REG = saved_lr;
  PC_REG = saved_pc;
  env->thumb = saved_thumb;
  /* reset exception */
  cpu->exception_index = -1;

  //handle return, arm will return at REGS(0) and REGS(1)
  switch(info->ret_type){
    case RET_TYPE_VOID:
        break;
    case RET_TYPE_INT_REG_32:
        data->int32_ret = REGS(0);
        break;
    case RET_TYPE_INT_REG_64:
        uint64_t ret_t = REGS(1);
        ret_t <<= 32;
        ret_t |= REGS(0);
        data->int64_ret = ret_t;
        break;
    case RET_TYPE_FLOAT_REG_32:
        //TODO: armhf calling convention
        data->int32_ret = env->vfp.zregs[0].d[0] & 0xffffffff;
        break;
    case RET_TYPE_FLOAT_REG_64:
        //TODO: armhf calling convention
        data->int64_ret = env->vfp.zregs[0].d[0];
        break;
    case RET_TYPE_FLOAT_REG_128:
        data->int128_ret.low = env->vfp.zregs[0].d[0];
        data->int128_ret.high = env->vfp.zregs[0].d[1];
        break;
    defalut:
        __a_log_print(7, LOG_TAG, "call_arm failed with ret_type: %d", info->ret_type);
        abort();
  }
}

#elif defined(TARGET_AARCH64) //arm64

#define REGS(x) env->xregs[x]
#define SP_REG REGS(31)
#define LR_REG REGS(30)
#define PC_REG env->pc
#define RET_REG REGS(0)
//perform android arm64 calling
static inline void qemu_android_call_aarch64(abi_ptr addr, QemuAndroidCallInfo *info, QemuAndroidCallData *data){
  CPUArchState *env = cpu_env(data->cpu);

  abi_ulong saved_lr = LR_REG;
  abi_ulong saved_pc = PC_REG;

  PC_REG = addr & ~0x3ULL;//arm64 specific ,see init_thread
  LR_REG = nb_stop;

  //copy regs
  uint32_t int_regs_used = ( info->int_regs_used <= 8 ) ? info->int_regs_used : 8;
  uint32_t float_regs_used = ( info->float_regs_used <= 8 ) ? info->float_regs_used : 8;
  for (uint32_t i = 0; i < int_regs_used; i++){
    REGS(i) = data->int64_regs[i];
  }
  for (uint32_t i = 0; i < float_regs_used; i++){
    env->vfp.zregs[i].d[0] = data->float128_regs[i].low;
    env->vfp.zregs[i].d[1] = data->float128_regs[i].high;
  }

  if (data->stack) {
    /* alloc space to push arg stack */
    /* TODO: What if stack have not enough space? */
    SP_REG -= info->stack_size;
    memcpy_to_target(SP_REG, data->stack, info->stack_size);
  }
  /* cpu maybe in exception, reset it*/
  cpu->exception_index = -1;

  /* env has setup, start cpu to call guest fcn code */
  cpu_loop(env);

  if (data->stack) {
    /* the way of __cdecl __fastcall need us to pop all the arg, restore */
    SP_REG += info->stack_size;
  }

  /* restore the retuen pointer, pc, thumb mode */
  LR_REG = saved_lr;
  PC_REG = saved_pc;
  /* reset exception */
  cpu->exception_index = -1;

  //handle return, arm64 will return with REGS(0) and vfp
  switch(info->ret_type){
    case RET_TYPE_VOID:
        break;
    case RET_TYPE_INT_REG_32:
        data->int32_ret = REGS(0) & 0xffffffff;
        break;
    case RET_TYPE_FLOAT_REG_32:
        data->int32_ret = env->vfp.zregs[0].d[0] & 0xffffffff;
        break;
    case RET_TYPE_INT_REG_64:
        data->int64_ret = REGS(0);
        break;
    case RET_TYPE_FLOAT_REG_64:
        data->int64_ret = env->vfp.zregs[0].d[0];
        break;
    case RET_TYPE_INT_REG_128:
        data->int128_ret.low = REGS(0);
        data->int128_ret.high = REGS(1);
        break;
    case RET_TYPE_FLOAT_REG_128:
        data->int128_ret.low = env->vfp.zregs[0].d[0];
        data->int128_ret.high = env->vfp.zregs[0].d[1];
        break;
    defalut:
        __a_log_print(7, LOG_TAG, "call_aarch64 failed with ret_type: %d", info->ret_type);
        abort();
  }
}

#elif defined(TARGET_I386) && defined(TARGET_ABI32) //x86

#define REGS(r) env->regs[r]
#define SP_REG REGS(R_ESP)
//no LR
#define PC_REG env->eip
#define BP_REG REGS(R_EBP)
#define BX_REG REGS(R_EBX)
#define RET_REG REGS(R_EAX)
//perform android x86 calling
static inline void qemu_android_call_i386(abi_ptr addr, QemuAndroidCallInfo *info, QemuAndroidCallData *data){
  CPUArchState *env = cpu_env(data->cpu);

  abi_ulong saved_bp = BP_REG;
  abi_ulong saved_pc = PC_REG;
  abi_ulong saved_bx = BX_REG;

  //clean return
  REGS(R_EAX) = 0;
  REGS(R_EDX) = 0; //must

  // copy arg first
  if (data->stack) {
    /* alloc space to push arg stack */
    /* TODO: What if stack have not enough space? */
    SP_REG -= info->stack_size;
    memcpy_to_target(SP_REG, data->stack, info->stack_size);
  }
  //push the return addr, in this case, let it return to pc_stop
  SP_REG -= sizeof(abi_ulong);
  memcpy_to_target(SP_REG, nb_stop, 4);
  //PC can point to addr now
  PC_REG = addr;
  // the guest func will push ebp, so no need to handle here
  /* cpu maybe in exception, reset it*/
  cpu->exception_index = -1;

  /* env has setup, start cpu to call guest fcn code */
  cpu_loop(env);
  // after ret, return addr has been pop to eip, so when reach tcg dias, the pc_next = pc_stop

  if (data->stack) {
    /* the way of __cdecl __fastcall need us to pop all the arg, restore */
    SP_REG += info->stack_size;
  }
  
  /* restore the retuen pointer, pc, thumb mode */
  BP_REG = saved_bp;
  PC_REG = saved_pc;
  BX_REG = saved_bx;
  /* reset exception */
  cpu->exception_index = -1;

  //handle return, x86 will return with eax,edx and st0,st1
  switch(info->ret_type){
    case RET_TYPE_VOID:
        break;
    case RET_TYPE_INT_REG_32:
        data->int32_ret = REGS(R_EAX);
        break;
    case RET_TYPE_FLOAT_STACK_32:
        //TODO
        data->int32_ret = helper_fsts_ST0(env);
        helper_fpop(env);
        break;
    case RET_TYPE_INT_REG_64:
        uint64_t ret_t = REGS(R_EDX);
        ret_t <<= 32;
        ret_t |= REGS(R_EAX);
        data->int64_ret = ret_t;
        break;
    case RET_TYPE_FLOAT_STACK_64:
        //see target/i386/tcg/fpu_helper.c
        //TODO
        data->int64_ret = helper_fstl_ST0(env);
        helper_fpop(env);
        break;
    case RET_TYPE_INT_REG_128:
    case RET_TYPE_FLOAT_REG_32:
    case RET_TYPE_FLOAT_REG_64:
    case RET_TYPE_FLOAT_REG_128:
    defalut:
        __a_log_print(7, LOG_TAG, "call_i386 failed with ret_type: %d", info->ret_type);
        abort();
  }
}

#elif defined(TARGET_X86_64) //x86_64

#define REGS(r) env->regs[r]
#define SP_REG REGS(R_ESP)
//no LR
#define PC_REG env->eip
#define BP_REG REGS(R_EBP)
#define RET_REG REGS(R_EAX)
const uint32_t x86_64_arg_regs[6] = { R_EDI, R_ESI, R_EDX, R_ECX, R_R8, R_R9 }; 
//perform android x86 calling
static inline void qemu_android_call_x86_64(abi_ptr addr, QemuAndroidCallInfo *info, QemuAndroidCallData *data){
  CPUArchState *env = cpu_env(data->cpu);

  abi_ulong saved_bp = BP_REG;
  abi_ulong saved_pc = PC_REG;

  // copy REGS
  REGS(R_EAX) = 0;
  uint32_t int_regs_used = ( info->int_regs_used <= 6 ) ? info->int_regs_used : 6;
  uint32_t float_regs_used = ( info->float_regs_used <= 8 ) ? info->float_regs_used : 8;
  for (uint32_t i = 0; i < int_regs_used; i++){
    REGS(x86_64_arg_regs[i]) = data->int64_regs[i];
  }
  for (uint32_t i = 0; i < float_regs_used; i++){
    env->xmm_regs[i].ZMM_Q(0) = data->float128_regs[i].low;
    env->xmm_regs[i].ZMM_Q(1) = data->float128_regs[i].high;
  }

  if (data->stack) {
    /* alloc space to push arg stack */
    /* TODO: What if stack have not enough space? */
    SP_REG -= info->stack_size;
    memcpy_to_target(SP_REG, data->stack, info->stack_size);
  }
  //push the return addr, in this case, let it return to pc_stop
  SP_REG -= sizeof(abi_ulong);
  memcpy_to_target(SP_REG, nb_stop, 8);
  //PC can point to addr now
  PC_REG = addr;
  // the guest func will push ebp, so no need to handle here
  /* cpu maybe in exception, reset it*/
  cpu->exception_index = -1;

  /* env has setup, start cpu to call guest fcn code */
  cpu_loop(env);
  // after ret, return addr has been pop to eip, so when reach tcg dias, the pc_next = pc_stop

  if (data->stack) {
    /* the way of __cdecl __fastcall need us to pop all the arg, restore */
    SP_REG += info->stack_size;
  }

  /* restore the retuen pointer, pc */
  BP_REG = saved_bp;
  PC_REG = saved_pc;
  /* reset exception */
  cpu->exception_index = -1;

  //handle return, x86_64 will return with eax,edx and xmm0
  switch(info->ret_type){
    case RET_TYPE_VOID:
        break;
    case RET_TYPE_INT_REG_32:
        data->int32_ret = REGS(R_EAX) & 0xffffffff;
        break;
    case RET_TYPE_FLOAT_REG_32:
        data->int32_ret = env->xmm_regs[i].ZMM_Q(0) & 0xffffffff;
        break;
    case RET_TYPE_INT_REG_64:
        data->int64_ret = REGS(R_EAX);
        break;
    case RET_TYPE_FLOAT_REG_64:
        data->int64_ret = env->xmm_regs[i].ZMM_Q(0);
        break;
    case RET_TYPE_INT_REG_128:
        data->int128_ret.low = REGS(R_EAX);
        data->int128_ret.high = REGS(R_EDX);
        break;
    case RET_TYPE_FLOAT_REG_128:
        data->int128_ret.low = env->xmm_regs[i].ZMM_Q(0);
        data->int128_ret.high = env->xmm_regs[i].ZMM_Q(1);
        break;
    defalut:
        __a_log_print(7, LOG_TAG, "call_x86_64 failed with ret_type: %d", info->ret_type);
        abort();
  }
}

#elif defined(TARGET_RISCV64) //riscv64

#define REGS(r) env->gpr[r] //gprh is for 128bit regs
#define SP_REG REGS(xSP)
#define RA_REG REGS(xRA)
#define PC_REG env->pc
#define RET_REG REGS(xA0)
const uint32_t riscv64_arg_regs[8] = { xA0, xA1, xA2, xA3, xA4, xA5, xA6, xA7 };
const uint32_t riscv64_arg_fpregs[8] = { 10, 11, 12, 13, 14, 15, 16, 17 }; //TODO: fa0/f10 is it right?
//TODO: vector regs
//perform android riscv64 calling
static inline void qemu_android_call_riscv64(abi_ptr addr, QemuAndroidCallInfo *info, QemuAndroidCallData *data){
  CPUArchState *env = cpu_env(data->cpu);

  abi_ulong saved_ra = RA_REG;
  abi_ulong saved_pc = PC_REG;

  PC_REG = addr;
  RA_REG = nb_stop;

  //copy regs
  uint32_t int_regs_used = ( data->int_regs_used <= 8 ) ? data->int_regs_used : 8;
  uint32_t float_regs_used = ( data->float_regs_used <= 8 ) ? data->float_regs_used : 8;
  for (uint32_t i = 0; i < int_regs_used; i++){
    REGS(riscv64_arg_regs[i]) = data->int64_regs[i];
  }
  for (uint32_t i = 0; i < float_regs_used; i++){
    env->fpr[riscv64_arg_fpregs[i]] = data->float64_regs[i];
  }

  if (data->stack) {
    /* alloc space to push arg stack */
    /* TODO: What if stack have not enough space? */
    SP_REG -= data->stack_size;
    memcpy_to_target(SP_REG, data->stack, data->stack_size);
  }
  /* cpu maybe in exception, reset it*/
  cpu->exception_index = -1;

  /* env has setup, start cpu to call guest fcn code */
  cpu_loop(env);

  if (data->stack) {
    /* the way of __cdecl __fastcall need us to pop all the arg, restore */
    SP_REG += data->stack_size;
  }

  /* restore the retuen pointer, pc, thumb mode */
  RA_REG = saved_ra;
  PC_REG = saved_pc;
  /* reset exception */
  cpu->exception_index = -1;

  //handle return, riscv64 will return with REGS(A0) and FP_REGS(FA0)
  switch(info->ret_type){
    case RET_TYPE_VOID:
        break;
    case RET_TYPE_INT_REG_32:
        data->int32_ret = REGS(xA0) & 0xffffffff;
        break;
    case RET_TYPE_FLOAT_REG_32:
        data->int32_ret = env->fpr[10]; & 0xffffffff;
        break;
    case RET_TYPE_INT_REG_64:
        data->int64_ret = REGS(xA0);
        break;
    case RET_TYPE_FLOAT_REG_64:
        data->int64_ret = env->fpr[10];
        break;
    case RET_TYPE_INT_REG_128:
        data->int128_ret.low = REGS(xA0);
        data->int128_ret.high = REGS(xA1);
        break;
    case RET_TYPE_FLOAT_REG_128:
        //NOTE: This will not happenned
        data->int128_ret.low = env->fpr[10];
        data->int128_ret.high = env->fpr[11];
        break;
    defalut:
        __a_log_print(7, LOG_TAG, "call_riscv64 failed with ret_type: %d", info->ret_type);
        abort();
  }
}

#else
#error not support
#endif

//generic itf
//the rtype is about target, will not handle thunk here
//TODO: this should use thread cpu, if it use other thread cpu, new method should detect cpu running status

//Actually the addr could be put inside info
void qemu_android_call(QemuAndroidCallInfo *info,
                       QemuAndroidCallData *data ){
#if defined(TARGET_ARM) && defined(TARGET_ABI32) //arm32
  qemu_android_call_arm(QA_ABI_PTR_GET(info->qa_addr), data, ret);
#elif defined(TARGET_AARCH64)
  qemu_android_call_aarch64(QA_ABI_PTR_GET(info->qa_addr), data, ret);
#elif defined(TARGET_I386) && defined(TARGET_ABI32)
  qemu_android_call_i386(QA_ABI_PTR_GET(info->qa_addr), data, ret);
#elif defined(TARGET_X86_64)
  qemu_android_call_x86_64(QA_ABI_PTR_GET(info->qa_addr), data, ret);
#elif defined(TARGET_RISCV64)
  qemu_android_call_riscv64(QA_ABI_PTR_GET(info->qa_addr), data, ret);
#else
#error not support
#endif
}


/* NOTE: check init_thread to get the way of copy pc and stack
 * check the target_cpu_copy_regs to see its env register
 */
// TODO: related to target instead of host arch
#ifdef __LP64__
#define HEXFMT "0x%016w64x"
#define HEXFMTP "0x%016x"
#else
#define HEXFMT "0x%08w32x"
#define HEXFMTP "0x%08x"
#endif
// TODO: some type may need abi_type abi_long instead of host intptr_t
// TODO: type thunk

/*
 * Improved method to create and delete thread_CPU
 *
 * Based on the old method, but we dont need modified libc
 *
 * Hook the __clone and __exit when doing this two method
 *
 * using __thread to mark two bool variable: need_new_cpu need_del_cpu
 *
 * delete is easier, mark need_del_cpu to dont let it pthread_exit and _exit
 * Just delete cpu.
 *
 * In new/del cpu, simple implement of call should fit for each arch
 *
 * new cpu need to add a thread_struct to store child_stack and tls,
 * skip the pthread_create and some signal, the start_routine should not be run
 */
void *qemu_android_get_cpu(void) { return thread_cpu; }

/* return: thread_cpu */
void *qemu_android_new_cpu(void) {
  if (!thread_cpu && thread_allocate_) {
    CPUState *cpu;
    CPUArchState *env;
    struct target_pt_regs regs1;
    struct target_pt_regs *regs = &regs1;
    TaskState *ts;
    abi_long sp, stackp, tlsp, thrp;
    /* NOTE: this struct is public,
     * start from a10, it has a private small struct for libndk
     * will place in front of this*/
    struct {
      abi_long prev, next;
      pid_t tid, pid;
    } thr;
    //abi_long *alloc_result;

    __a_log_print(2, LOG_TAG, "Creating new CPU env");

    // same with main, but havent set thread_cpu
    cpu = cpu_create(*cpu_type);
    env = cpu_env(cpu);
    cpu_reset(cpu);

    /* thread_cpu is NULL, set this earlier */
    //thread_cpu = cpu;

    //same as main
    ts = g_new0(TaskState, 1);
    init_task_state(ts);
    /* build Task State */
    ts->info = info;
    ts->bprm = bprm;
    cpu->opaque = ts;
    task_settid(ts);
    //in do_fork, cpu_clone_regs_ parent/child will set flag and child_sp (our stack), good help, set to our cpu at that time
    //in do_fork, ts.signal_mask will also set
    /* We are about to call a void function. no in, no out */

    /* See STACK_LOWER_LIMIT in linux-user/elfload.c use 32*/
#define NEW_STACK_SIZE (32 * TARGET_PAGE_SIZE)
    /* old android (9 and below) has same tls for every arch, 0-9, 0=self, 1=id
     * start from android 10 it start to vary
     * __get_tls will point to the &tls_slot(0)
     * 8-word tcb in arm/arm64, 0 is DTV, 1 is thread_id, max is 7, reserve place before the tls for more
     * x86/x86_64 start from 0, up to 10 in android15, dtv=8, thread-id=1
     * riscv max is -1 , min to -10, dtv=-8, thread_id=-7
     * So, allocate 32 size for it, at a rage of -16 ~ 15, it should enough for each arch, and store thread at tlsp+1
    */
#define TLS_SIZE (sizeof(abi_long) * 16)

    /* only PARISC is stack grows down and have 64 page_alignment */
    //NOTE: This is a temp stack space, it only store necessary args and data
    //as the new cpu does not have threads info, a temp tls will apply
    sp = target_mmap(0, NEW_STACK_SIZE,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); //allocate STACK, see setup_arg_pages in linux-user/elfload.c
    if(sp == -1){
      __a_log_print(7, LOG_TAG, "Failed to allocate temporary stack for new cpu");
      abort();
    }

    tlsp = sp + NEW_STACK_SIZE - TLS_SIZE; //at bottom place tls,size in 128 for 32-bit , 256 for 64-bit, tlsp point to the middle
    thrp = sp + NEW_STACK_SIZE - TARGET_PAGE_SIZE; //space for store the structure of pthread_internal_t (thr, not a completed struct)
    stackp = QEMU_ALIGN_DOWN(sp + NEW_STACK_SIZE - (2 * TARGET_PAGE_SIZE), TARGET_PAGE_SIZE); //stack should be align

    /* NOTE: the new method put tid in do_fork */
    thr.tid = ts->ts_tid;
    thr.pid = getppid();
    memcpy_to_target(thrp, &thr, sizeof(thr)); //place the thr struct
    memcpy_to_target(tlsp + sizeof(abi_long), &thrp, sizeof(abi_long)); //tls_slot[1], point to thr


    //---also the begin of clone_func----
    // but we dont create new thread, we should apply to this thread
    rcu_register_thread();
    tcg_register_thread();
    cpu->random_seed = qemu_guest_random_seed_thread_part1();//this is before pthread_create
    qemu_guest_random_seed_thread_part2(cpu->random_seed); // this is in clone func, now rest of the do fork only left the ret (tid), handle it later

    //the things of main, because our env need to call function
    do_init_thread(regs, info); // set entry_point, start_stack, regs(0)=0, from initial info
    target_cpu_copy_regs(env, regs); // copy initial regs to env
    SP_REG = stackp; // change env stack to our stack
    cpu_set_tls(env, tlsp); // change env tls to our temp tls
    env->pc_stop = nb_stop; //set stop at the nb_guest entry 

    //TODO: allocate thread_struct and memset it
    // qemu_set_log(CPU_LOG_TB_IN_ASM|qemu_str_to_log_mask(QEMU_LOG_MASK));
    //set return addr
#if defined(TARGET_ARM) || defined(TARGET_AARCH64)
    /* return to pc_stop */
    LR_REG = nb_stop;
#elif defined(TARGET_RISCV64)
    RA_REG = nb_stop;
#elif defined(TARGET_I386) || defined(TARGET_X86_64)
    /* x86 need to push the return addr to stack */
    SP_REG -= sizeof(abi_ulong);
    memcpy_to_target(SP_REG, nb_stop, sizeof(abi_ulong));
#endif

    //set addr
#ifdef TARGET_AARCH64
    PC_REG = thread_allocate_ & ~0x3ULL;//arm64 specific ,see init_thread
#elif defined(TARGET_ARM) && defined(TARGET_ABI32)
    PC_REG = thread_allocate_ & 0xfffffffe;//arm specific, the last bit is thumb mark ,see init_thread
    env->thumb = thread_allocate_ & 1;
#else //defalut case, x86/x86_64, riscv64
    PC_REG = thread_deallocate_;
#endif
    need_new_cpu = true;
    __a_log_print(2, LOG_TAG, "Allocating new CPU thread by nb-qemu-guest");

    thread_cpu = cpu;

    cpu_loop(env);
    // qemu_set_log(qemu_str_to_log_mask(QEMU_LOG_MASK));

    //OK now we have handle things in do_fork:
    //store: new_sp
    //store: new_tls
    //do nothing in do_fork, just return tid to guest

    //TODO: should check what we store
    if (RET_REG == 0 && alloc_child_stack && alloc_new_tls ) {

      __a_log_print(2, LOG_TAG, "New CPU thread allocated: stack=" HEXFMTP ", tls=" HEXFMTP,
                    alloc_child_stack,
                    alloc_new_tls);
      SP_REG = alloc_child_stack; //stack
      //set the new tls
      cpu_set_tls(env, alloc_new_tls);//tls_slot
      target_munmap(sp, NEW_STACK_SIZE);
      __a_log_print(2, LOG_TAG, "New CPU created = %p", thread_cpu);
    } else {
      //ALOGF
      __a_log_print(7, LOG_TAG, "CPU thread allocation failed: %d", RET_REG);
      abort();
    }

    //        thread_cpu = cpu;
    //TODO: the finialize should prompt qemu_android to do preexit_cleanup

    need_new_cpu = false;
  }

  return thread_cpu;
}

/* the design of nb-qemu QemuCPU class is thread_local, thus deletion only accept thread_cpu */
/* We can remove the first cpu, but be aware of that, that's mean we are doing finalize,
 * dont let the qemu stop our process.
 *
 * in: thread_cpu, only allow cpu of this thread
 * return: 0 if success, -1 if failed.
 */
int qemu_android_delete_cpu(void *_cpu) {
  CPUState *cpu = (CPUState *)_cpu;
  CPUArchState *env = cpu_env(cpu);

  __a_log_print(2, LOG_TAG, "Deleting CPU = %p", cpu);
  if (thread_cpu != cpu){
    __a_log_print(6, LOG_TAG, "Unexpected CPU %p is not equal to thread_cpu= %p", cpu, thread_cpu);
    return -1;
  }

  if (thread_deallocate_) {
    // qemu_set_log(CPU_LOG_TB_IN_ASM|qemu_str_to_log_mask(QEMU_LOG_MASK));
    //we dont set a pc_stop, cpu should be clean by our thread

    //set return addr
#if defined(TARGET_ARM) || defined(TARGET_AARCH64)
    /* return to pc_stop */
    LR_REG = nb_stop;
#elif defined(TARGET_RISCV64)
    RA_REG = nb_stop;
#elif defined(TARGET_I386) || defined(TARGET_X86_64)
    /* x86 need to push the return addr to stack */
    SP_REG -= sizeof(abi_ulong);
    memcpy_to_target(SP_REG, nb_stop, sizeof(abi_ulong));
#endif
    //set addr
#ifdef TARGET_AARCH64
    PC_REG = thread_deallocate_ & ~0x3ULL;//arm64 specific ,see init_thread
#elif defined(TARGET_ARM) && defined(TARGET_ABI32)
    PC_REG = thread_deallocate_ & 0xfffffffe;//arm specific, the last bit is thumb mark ,see init_thread
    env->thumb = thread_deallocate_ & 1;
#else //defalut case, x86/x86_64, riscv64
    PC_REG = thread_deallocate_;
#endif
    /* set this is because we dont need exit, handle in syscall exit */
    need_del_cpu = true;
    __a_log_print(2, LOG_TAG, "Deallocating CPU thread by nb-qemu-guest");

    cpu_loop(env);
    // qemu_set_log(qemu_str_to_log_mask(QEMU_LOG_MASK));
    // all work should have done in __NR_exit, except first_cpu

    if( !thread_cpu ){
      __a_log_print(2, LOG_TAG, "CPU thread %p deallocated and deleted", cpu);
    }else{
      /* if thread_cpu is still here, it has two situation */
      if(CPU_NEXT(first_cpu)){
        __a_log_print(7, LOG_TAG, "CPU thread %p deallocation failed", cpu);
        abort();
      }else if(thread_cpu == first_cpu){
        __a_log_print(2, LOG_TAG, "Final CPU thread %p deallocated and is safe to delete now", cpu);
      }else{
        __a_log_print(7, LOG_TAG, "Unexpected point reached");
        abort();
      }
    }
    need_del_cpu = false;
  }
  return 0;
}

/**
 * TODO: generic call_host
 * the call host tramp in guest comes from a closure of ffi_call, the guest should give something to this handler:
 * 
 * handle: which host function to call? (abi_ptr to host addr or function id)
 * **args: the pointer for store the args pointers (abi_ptr)
 * *ret: the pointer for store the return (abi_ptr)
 * 
 * the addr should point to a host fcn and ffi_cif should give signature and abi
 * but the the host cif is different, and the host may have 32bit or 64bit addr
 * so we store a handle to get a class object it built before. 
 * when creating this tramp, return an object handle to guest.
 * tell the guest that what kind of host is, so the 32bit guest could store 64bit or 32bit address.
 * the guest send ptr, and we get it by hand (get ptr from regs, get data from ptr)
 * 
 * nb-qemu should register a handler for here, QA send three ptr to that handler 
 */

void qemu_android_call_host_handler(CPUArchState *env) {
  // NOTE that unlike qemu_call, qemu_call is act like caller, this handler act like callee
  // extract args for every arch
  QA_abi_ptr host_handle, call_args, call_ret;
#if defined(TARGET_ARM) && defined(TARGET_ABI32) //arm32
  host_handle.addr32 = REGS(0);
  call_args.addr32 = REGS(1);
  call_ret.addr32 = REGS(2);
#elif defined(TARGET_AARCH64)
  host_handle.addr64 = REGS(0);
  call_args.addr64 = REGS(1);
  call_ret.addr64 = REGS(2);
#elif defined(TARGET_I386) && defined(TARGET_ABI32)
  //x86 get the values from stack (the value that SP_REG stored is the gaddr, point to the guest stack, lock/unlock_user by the gaddr)
  //get_user_u32(return_pc.addr32, SP_REG);
  get_user_u32(host_handle.addr32, SP_REG+4);
  get_user_u32(call_args.addr32, SP_REG+8);
  get_user_u32(call_ret.addr32, SP_REG+12);
#elif defined(TARGET_X86_64)
  host_handle.addr64 = REGS(x86_64_arg_regs[0]);
  call_args.addr64 = REGS(x86_64_arg_regs[1]);
  call_ret.addr64 = REGS(x86_64_arg_regs[2]);
#elif defined(TARGET_RISCV64)
  host_handle.addr64 = REGS(riscv64_arg_regs[0]);
  call_args.addr64 = REGS(riscv64_arg_regs[1]);
  call_ret.addr64 = REGS(riscv64_arg_regs[2]);
#else
#error not support
#endif
  //TODO: call the handler (no return)

  //if anything else do here

}


/** 
 * TODO: static call_host
 * This is to replace the SVC handler and syscall handler used by old nb-qemu.
 * nb-qemu should register a handler for this to provide info. 
 * to give the access to register and stack for os lib, provide some interface to map the guest cpu state
 * each arch has its own interface like a c++ class in CPUArchState, but we build host lib seperately. 
 * 
 * so, we send the itf or pointer from this handler
 * 
 * libqemu-***.so       libnb-qemu.so                    libnb-qemu_libEGL.so
 * call_host_static --> get_lib_handler from nb-qemu --> call handler
 * 
 * libnb-qemu_libEGL.so access GuestState, which contains pointers pointing to the members of ArchState
 * 
 * return 0 to let qemu skip the bad code
 */
//As we support proxy for binfmt, we may need to dlopen nb-qemu to get its itf

//TODO: introduce a patch for linker to enable searching nb stub libs first
//      only if we have this itf on, the nb libs could be used, even in binfmt mode
//      this mean the stub libs must put in nb folder, and the patch should have the ability to identify
int qemu_android_call_host_static(CPUArchState *env, uint16_t lib_num, uint16_t sym_num) {
  // 1. call interface from nb-qemu to get real lib_handler

  // 2. call the real lib handler, send and expose some of the guest state ptr to it
  return 0;
}


//


/* TODO: library constucter and deconstructer to ensure all cpu thread is quit. */
////////////////////////////////////////////////

struct QemuAndroidCallbacks QemuAndroidItf = {
/*initialize*/ qemu_android_initialize,
/*lookup_symbol*/ qemu_android_lookup_symbol,
/*malloc*/ qemu_android_malloc,
/*free*/ qemu_android_free,
/*memcpy*/ qemu_android_memcpy,
/*get_string*/ qemu_android_get_string,
/*release_string*/ qemu_android_release_string,
/*get_memory*/ qemu_android_get_memory,
/*release_memory*/ qemu_android_release_memory,
/*h2g*/ qemu_android_h2g,
/*g2h*/ qemu_android_g2h,
/*register_syscall_handler*/ qemu_android_register_syscall_handler,
/*register_svc_handler*/ qemu_android_register_svc_handler,
/*get_cpu*/ qemu_android_get_cpu,
/*new_cpu*/ qemu_android_new_cpu,
/*delete_cpu*/ qemu_android_delete_cpu,
/*call*/ qemu_android_call
};
