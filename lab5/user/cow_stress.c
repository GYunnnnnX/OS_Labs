// 目标：逼近 Dirty COW 类型的竞态：在 COW fault 拆分时反复抢占/打断，看是否出现父进程数据被污染或 refcount 异常。

// 做法（不需要给你任何提权利用代码）：
// 用户态：父进程 fork 出多个子进程，每个子进程疯狂写同一片只读/COW 的内存区域，父进程同时不停校验自己数据不变。
// 内核态：在 cow_handle_fault() 里（仅 DEBUG 模式）加一个“可选的人工延迟/让出”，扩大竞态窗口，比如在复制前后插入 schedule() 或忙等若干次 tick（注意只在实验 debug 宏打开时用）。

#include <ulib.h>
#include <stdio.h>
#include <unistd.h>

#define NCHILD 8
#define ITER 20000
static int shared[1024];

int main(void) {
    for (int i = 0; i < 1024; i++) shared[i] = i;

    for (int k = 0; k < NCHILD; k++) {
        int pid = fork();
        if (pid == 0) {
            for (int t = 0; t < ITER; t++) {
                shared[t % 1024] = 0xdead0000 + k;
            }
            exit(0);
        }
    }

    // parent keep validating
    for (int t = 0; t < ITER; t++) {
        for (int i = 0; i < 1024; i++) {
            if (shared[i] != i) {
                cprintf("RACE FAIL: parent corrupted at %d val=%x\n", i, shared[i]);
                return -1;
            }
        }
    }
    cprintf("PASS stress\n");
    return 0;
}
