#ifndef CHICKADEE_P_LIB_H
#define CHICKADEE_P_LIB_H
#include "lib.hh"
#include "x86-64.h"
#if CHICKADEE_KERNEL
#error "p-lib.hh should not be used by kernel code."
#endif

// p-lib.hh
//
//    Support code for Chickadee processes.


// SYSTEM CALLS

inline uintptr_t syscall0(int syscallno) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
                  : "+a" (rax)
                  :
                  : "cc", "rcx", "rdx", "rsi", "rdi",
                    "r8", "r9", "r10", "r11");
    return rax;
}

inline uintptr_t syscall0(int syscallno, uintptr_t arg0) {
    register uintptr_t rax asm("rax") = syscallno;
    register uintptr_t rdi asm("rdi") = arg0;
    asm volatile ("syscall"
                  : "+a" (rax), "+D" (rdi)
                  :
                  : "cc", "rcx", "rdx", "rsi",
                    "r8", "r9", "r10", "r11");
    return rax;
}

// sys_getpid
//    Return current process ID.
static inline pid_t sys_getpid(void) {
    return syscall0(SYSCALL_GETPID);
}

// sys_yield
//    Yield control of the CPU to the kernel. The kernel will pick another
//    process to run, if possible.
static inline void sys_yield(void) {
    syscall0(SYSCALL_YIELD);
}

// sys_page_alloc(addr)
//    Allocate a page of memory at address `addr`. `Addr` must be page-aligned
//    (i.e., a multiple of PAGESIZE == 4096). Returns 0 on success and -1
//    on failure.
static inline int sys_page_alloc(void* addr) {
    return syscall0(SYSCALL_PAGE_ALLOC, reinterpret_cast<uintptr_t>(addr));
}

// sys_fork()
//    Fork the current process. On success, return the child's process ID to
//    the parent, and return 0 to the child. On failure, return -1.
static inline pid_t sys_fork(void) {
    return syscall0(SYSCALL_FORK);
}

// sys_exit(status)
//    Exit this process. Does not return.
static inline void __attribute__((noreturn)) sys_exit(int status) {
    syscall0(SYSCALL_EXIT, status);
    while (1) {
    }
}

static inline int sys_map_console(void* addr) {
    return syscall0(SYSCALL_MAP_CONSOLE, reinterpret_cast<uintptr_t>(addr));
}

// sys_pause()
//    A version of `sys_yield` that spins in the kernel long enough
//    for kernel timer interrupts to occur.
static inline void sys_pause() {
    syscall0(SYSCALL_PAUSE);
}

// sys_kdisplay(display_type)
//    Set the display type (one of the KDISPLAY constants).
static inline int sys_kdisplay(int display_type) {
    return syscall0(SYSCALL_KDISPLAY, display_type);
}

// sys_msleep(msec)
//    Block for approximately `msec` milliseconds.
static inline int sys_msleep(unsigned msec) {
    return E_NOSYS;
}

// sys_getppid()
//    Return parent process ID.
static inline pid_t sys_getppid() {
    return E_NOSYS;
}

// sys_panic(msg)
//    Panic.
static inline pid_t __attribute__((noreturn)) sys_panic(const char* msg) {
    syscall0(SYSCALL_PANIC, reinterpret_cast<uintptr_t>(msg));
    while (1) {
    }
}


// OTHER HELPER FUNCTIONS

// app_printf(format, ...)
//    Calls console_printf() (see lib.h). The cursor position is read from
//    `cursorpos`, a shared variable defined by the kernel, and written back
//    into that variable. The initial color is based on the current process ID.
void app_printf(int colorid, const char* format, ...);


extern "C" {
void process_main(void);
}

#endif
