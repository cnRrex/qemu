/*
 * nb-qemu
 *
 * Copyright (c) 2019 Michael Goffioul
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

#ifndef QEMU_ANDROID_INTERFACE_H_
#define QEMU_ANDROID_INTERFACE_H_

//TODO: 
//    mips
// a0..a3 (env->active_tc.gpr[4]-[7])
// f12,f14, if first two float, then the 3rd use a2, 4 arg pass by regs (env->active_fpu.fpr[]) 64bit
// return v0,v1 (gpr[2] gpr[3])
// return f0, f2

//    mips64
//a0..a7 (env->active_tc.gpr[4]-[11])
//f12,f13,...f19, if first two int, then 3rd use f14, 8 arg pass by regs 64bit
// return v0,v1 (gpr[2] gpr[3])
// return f0, f2


#include <stddef.h>
#include <stdint.h>

//TODO: target related abi_ulong
//when 64bit-host 32-bit-guest all these interface need to redesign
//these two is not use in qemu, just a copy for nb-qemu to use
typedef intptr_t (*qemu_android_syscall_handler_t)(void *cpu_env, int num,
                                                   intptr_t arg1, intptr_t arg2,
                                                   intptr_t arg3, intptr_t arg4,
                                                   intptr_t arg5,
                                                   intptr_t arg6);

typedef void (*qemu_android_svc_handler_t)(void *cpu_env, uint16_t num);

// 128 bit type, when use little-endian, it can use memcpy to this structure
typedef struct {
    uint64_t low;
    uint64_t high;
} QA_UInt128;

// abi_ptr type, use for h2g when 64-bit host 32-bit guest is ready
typedef struct {
    union{
        uint32_t addr32;
        uint64_t addr64;
    };
} QA_abi_ptr; //something like binder64, use ptr for both 32bit and 64bit

//TODO: remove this in tramp header
enum argProcessType {
  ARG_TYPE_UNKNOW = 0, //void
  ARG_TYPE_INT32_REG, //arg will store in int32 regs
  ARG_TYPE_INT32_REG_DOUBLE, //arg will store in two int32 regs
  ARG_TYPE_INT32_REG_PTR, //arg will first h2g, then store in int32 regs
  ARG_TYPE_INT64_REG, //arg will store in int64 regs
  ARG_TYPE_INT64_REG_DOUBLE, //arg will store in two int64 regs
  ARG_TYPE_INT64_REG_PTR, //arg will first h2g, then store in int64 regs
  ARG_TYPE_FLOAT32_REG, //arg will store in fp 32 regs
  ARG_TYPE_FLOAT32_REG_DOUBLE, //arg will store in two fp 32 regs
  ARG_TYPE_FLOAT64_REG, //arg will store in fp 64 regs
  ARG_TYPE_FLOAT64_REG_DOUBLE, //arg will store in two fp 64 regs
  ARG_TYPE_FLOAT128_REG, //arg will store in fp 128 regs
  ARG_TYPE_FLOAT128_REG_LOW, //64bit arg will store in the low bits in fp 128 regs
  ARG_TYPE_STACK32, //arg will store in stack and cost 4 bytes
  ARG_TYPE_STACK64, //arg will store in stack and cost 8 bytes
  ARG_TYPE_STACK128, //arg will store in stack and cost 16 bytes
  ARG_TYPE_STACK32_PTR, //arg will first h2g to 32bit pointer, then store in stack
  ARG_TYPE_STACK64_PTR, //arg will first h2g to 64bit pointer, then store in stack
  //below for struct is not implemented
  ARG_TYPE_STACK_COMPLEX, //arg will store in stack, and need to calculate its size
  ARG_TYPE_INT32_AND_STACK, //in arm32 when 16 bytes type is the 2nd/3rd regs, then it need stack to pass the rest 8 bytes
  ARG_TYPE_INT64_SPLIT_STACK // in x86_64 when uint128 is the 6th regs, it will be splited to reg and stack
};

//tell qemuAndroid how to copy return
enum retProcessType {
  RET_TYPE_VOID = 0, //process a void return
  RET_TYPE_INT_REG_32, //process a 32bit return from reg
  RET_TYPE_FLOAT_REG_32, //process a 32bit return from fpreg
  RET_TYPE_INT_REG_64, //process a 64bit return from reg
  RET_TYPE_FLOAT_REG_64, //process a 64bit return from fpreg
  RET_TYPE_INT_REG_128, //process a 128bit return from reg
  RET_TYPE_FLOAT_REG_128, //process a 128bit return from fpreg
  //not implement
  RET_TYPE_FLOAT_STACK_32, //process a 32bit return from x86 x87 float stack
  RET_TYPE_FLOAT_STACK_64, //process a 64bit return from x86 x87 float stack
  RET_TYPE_FLOAT_STACK_128, //process a 128bit return from x86 x87 float stack
  //not implement
  RET_TYPE_STACK_32, //process a 32bit return from stack
  RET_TYPE_STACK_64, //process a 64bit return from stack
  RET_TYPE_STACK_128, //process a 128bit return from stack
};

// Struct for qemu_android call to pass data
// A temp struct allocated when call
// Generic data space for every supported arch
// Now here we allocate these for once from pmr pool (If stack then twice)
typedef struct {
    union{
        uint32_t int32_regs[4]; //32bit target regs --- arm: 4, x86: none
        uint64_t int64_regs[8]; //64bit target regs --- arm64/riscv64: 8, x86_64: 6
        // 16Bytes~64Bytes
    }; 

    union{
        uint32_t float32_regs[4]; //32bit fpu regs TODO
        uint64_t float64_regs[8]; //64bit fpu regs --- riscv64
        QA_UInt128 float128_regs[8]; //128bit fpu regs --- arm64/x86_64
        // ?~64Bytes~128Bytes
    };

    char *stack; //store stack

    void *cpu; //Actually this is useless

    union{
        uint32_t int32_ret; //32bit type return
        uint64_t int64_ret; //64bit type return
        QA_UInt128 int128_ret; //128bit type return
        //TODO: for float type
        // 16Bytes
    };

} QemuAndroidCallData; //224+ Bytes on 64bit

typedef struct {
    QA_abi_ptr qa_addr; //a tramp map to a fcn addr
    uint32_t int_regs_used; //on non-mips platform, usually we dont need too much regs
    uint32_t float_regs_used; //set these to reduce copy times
    uint32_t stack_size; //stack size;

    enum retProcessType ret_type; //return type
} QemuAndroidCallInfo;



// NOTE: check /target/arm/cpu.h for definition of ARMVectorReg
typedef struct QA_ARMVectorReg {
    uint64_t d[2 * 16] __attribute__((aligned(16)));
}QA_ARMVectorReg;

// Structs for os_bridge/static_call_host to access guest state
// All of them are pointers

typedef struct {
    uint32_t *regs; //16 regs, sp:13, lr:14, pc:15
    bool *thumb;
    QA_ARMVectorReg *vfp_zregs; //32 vregs
} GuestStateArm;

typedef struct {
    uint64_t *xregs; //32 regs, lr:30, sp:31
    uint64_t *pc;
    QA_ARMVectorReg *vfp_zregs; //32 vregs
} GuestStateArm64;

typedef struct {
    uint32_t *regs; //8 regs
    uint32_t *eip;
    //TODO: float x87
} GuestStateX86;

typedef struct {
    uint64_t *regs; //16 regs
    uint64_t *eip;
    //TODO: xmm
} GuestStateX64;

typedef struct {
    uint64_t *gpr; //32 regs
    uint64_t *fpr; //32 fp regs
    // NOTE: check /target/riscv/cpu.h for definition of vreg and RV_VLEN_MAX=1024: uint64_t vreg[32 * RV_VLEN_MAX / 64] QEMU_ALIGNED(16);
    uint64_t *vreg; //32 vregs maybe, here is 512 of uint64, 256 uint128
    uint64_t *pc;

} GuestStateRiscv64;

//TODO: mips/mips64

/* Functions will be exported by qemuAndroid.
 * This may changed much unlike NativeBridgeCallbacks */
struct QemuAndroidCallbacks {
    int (*initialize)(const char *procname, const char *tmpdir,
                            const char **qemu_envp, const char *guest_entry);
    intptr_t (*lookup_symbol)(const char *name);
    intptr_t (*malloc)(size_t size);
    void (*free)(intptr_t addr);
    void (*memcpy)(intptr_t dest, const void *src, size_t length);
    const char* (*get_string)(intptr_t addr);
    void (*release_string)(const char *s, intptr_t addr);
    void* (*get_memory)(intptr_t addr, size_t length);
    void (*release_memory)(void *ptr, intptr_t addr, size_t length);
    QA_abi_ptr (*h2g)(void *addr);
    void* (*g2h)(QA_abi_ptr addr);
    void (*register_syscall_handler)(qemu_android_syscall_handler_t func);
    void (*register_svc_handler)(qemu_android_svc_handler_t func);
    void* (*get_cpu)(void);
    void* (*new_cpu)(void);
    int (*delete_cpu)(void *cpu);
    void (*call)(QemuAndroidCallInfo *info,
                 QemuAndroidCallData *data );
};

#endif
