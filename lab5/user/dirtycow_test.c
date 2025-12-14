#include <ulib.h>
#include <unistd.h>
#include <stdio.h>
#include <syscall.h>
static inline int64_t __syscall2(int64_t num, uintptr_t a1, uintptr_t a2) {
    register uintptr_t _a0 asm("a0") = (uintptr_t)num;
    register uintptr_t _a1 asm("a1") = a1;
    register uintptr_t _a2 asm("a2") = a2;
    asm volatile("ecall" : "+r"(_a0) : "r"(_a1), "r"(_a2) : "memory");
    return (int64_t)_a0;
}

static inline int sys_dirtycow_buggy(uintptr_t addr, int v) {
    cprintf("sys_dirtycow_buggy: addr=%p, val=%c\n", (void *)addr, (char)v);
    return (int)__syscall2(SYS_dirtycow_buggy, addr, (uintptr_t)v);
}
static inline int sys_dirtycow_fixed(uintptr_t addr, int v) {
    cprintf("sys_dirtycow_fixed: addr=%p, val=%c\n", (void *)addr, (char)v);
    return (int)__syscall2(SYS_dirtycow_fixed, addr, (uintptr_t)v);
}

#define PAGE_SIZE 4096
static char gpage[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static volatile char *g = gpage;

int main(void) {
    *g = 'A';
    cprintf("[parent] g=%p init=%c\n", g, *g);

    int pid = fork();
    if (pid == 0) {
        cprintf("[child ] before=%c\n", *g);

        sys_dirtycow_buggy((uintptr_t)g, 'X');
        // sys_dirtycow_fixed((uintptr_t)g, 'X');

        cprintf("[child ] after =%c\n", *g);
        exit(0);
    }
    wait();
    cprintf("[parent] after wait=%c\n", *g);
    return 0;
}