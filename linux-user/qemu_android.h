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

#ifndef QEMU_ANDROID_H_
#define QEMU_ANDROID_H_

#include <stddef.h>
#include <stdint.h>
#include "qemu_android_interface.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * qemu_android_initialize: initialize the QemuAndroid
 *
 * procname: name to pass to arg0
 * tmpdir: app code_cache tmpdir
 * TODO: pass more thing
 * return: qemu_main exec result
 */
int qemu_android_initialize(const char *procname, const char *tmpdir,
                            const char **qemu_envp, const char *guest_entry);

/**
 * qemu_android_lookup_symbol: find symbol from nb-qemu-guest
 *
 * name: symbol name
 * return: symbol address in disas_symtab (guest address maybe)
 */
QA_abi_ptr qemu_android_lookup_symbol(const char *name);

/**
 * qemu_android_malloc: alloc size of memory in guest
 * This first do target_mmap, then push the size to the head, return the rest
 * mem start address
 *
 * size: size of memory to alloc in guest
 * return: guest address (virtual)
 */
QA_abi_ptr qemu_android_malloc(size_t size);

/**
 * qemu_android_free: dealloc the memory in guest
 * This first moveback the pointer to the head of where target_mmap was return,
 * then target_munmap
 *
 * addr: guest address of guest memory which qemu_android_malloc return
 */
void qemu_android_free(QA_abi_ptr addr);

/**
 * qemu_android_memcpy: copy data to user
 * A wrapper of copy_to_user
 *
 * dest: guest address of guest memory to store data in
 * src: host data pointer
 * length: size of data
 */
void qemu_android_memcpy(QA_abi_ptr dest, const void *src, size_t length);

/**
 * qemu_android_get_string: lock a pointer of guest string (a copy maybe)
 * A wrapper of lock_user_string
 *
 * addr: guest address of a guest string
 * return: the host pointer of the locked guest string
 */
const char *qemu_android_get_string(QA_abi_ptr addr);

/**
 * qemu_android_release_string: return the string to guest (unlock)
 * A wrapper of string unlock_user
 *
 * s: the host pointer of the locked guest string from qemu_android_get_string
 * addr: the guest address of the string
 */
void qemu_android_release_string(const char *s, QA_abi_ptr addr);

/**
 * qemu_android_get_memory: lock a size of guest memory
 * A wrapper of lock_user
 *
 * addr: guest address of guest memory
 * length: size of the guest memory
 * return: host pointer to the locked memory
 */
void *qemu_android_get_memory(QA_abi_ptr addr, size_t length);

/**
 * qemu_android_release_memory: unlock a size of guest memory (not free it)
 * A wrapper of unlock_user
 *
 * ptr: host pointer to the locked memory
 * addr: guest address of the guest memory
 * length: the size of guest/locked memory
 */
void qemu_android_release_memory(void *ptr, QA_abi_ptr addr, size_t length);

/* qemu_android_h2g: wrapper of h2g_nocheck, make a guest addr from host
 * pointer*/
QA_abi_ptr qemu_android_h2g(void *addr);

/* qemu_android_g2h: wrapper of g2h, make a host pointer from guest address */
void *qemu_android_g2h(QA_abi_ptr addr);

/**
 * qemu_android_register_syscall_handler: Pass the syscall handler function
 * pointer to qemu
 */
void qemu_android_register_syscall_handler(qemu_android_syscall_handler_t func);

/**
 * qemu_android_register_svc_handler: Pass the svc handler function pointer to
 * qemu
 */
void qemu_android_register_svc_handler(qemu_android_svc_handler_t func);

//void *qemu_android_get_regs(void *env, unsigned int index);

//void *qemu_android_get_sp_reg(void *env);

//void *qemu_android_get_lr_reg(void *env);

//void *qemu_android_get_pc_reg(void *env);

/* qemu_android_get_cpu: get the thread_cpu pointer */
void *qemu_android_get_cpu(void);

/**
 * qemu_android_new_cpu: alloc a qemu_android_cpu to execute guest function
 * TODO: the cpu need guest function pointer __pthread_allocate_self in guest
 * bionic, can we move it to nb-qemu-guest?
 *
 * return: CPUState pointer to the cpu
 */
void *qemu_android_new_cpu(void);

/**
 * qemu_android_delete_cpu: dealloc a qemu_android_cpu
 *
 * cpu: CPUState pointer to thread_cpu
 *
 * return: 0 if success, -1 if failed
 */
int qemu_android_delete_cpu(void *cpu);

/**
 * qemu_android_call: perform function call in guest
 * this function is used by a qemu_android_cpu, first alloc a cpu/thread then
 * execute by it
 *
 * cpu: QEMU CPUState pointer, create by qemu_android_new_cpu, it should use thread_cpu
 * addr: the guest function address
 * data: QemuAndroidCallData, pass the regs, float regs, and stack, etc.
 * ret: QemuAndroidCallRet, contains return type, and store the return
 * return: void
 */
void qemu_android_call(void *cpu,
                       QA_abi_ptr addr,
                       struct QemuAndroidCallData *data,
                       struct QemuAndroidCallRet *ret);

#ifdef __cplusplus
};
#endif

#endif
