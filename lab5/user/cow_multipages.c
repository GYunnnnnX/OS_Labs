// 目标：验证 copy_range 覆盖多页、fault 能逐页拆分。

#include <ulib.h>
#include <stdio.h>
#include <unistd.h>

#define N (4096*8)

static char big[N];

int main(void) {
    for (int i = 0; i < N; i += 4096) big[i] = (char)(i/4096);

    int pid = fork();
    if (pid == 0) {
        for (int i = 0; i < N; i += 4096) big[i] = (char)(100 + i/4096);
        exit(0);
    }

    int code;
    waitpid(pid, &code);

    // parent validate unchanged
    for (int i = 0; i < N; i += 4096) {
        if (big[i] != (char)(i/4096)) {
            cprintf("FAIL at page %d\n", i/4096);
            return -1;
        }
    }
    cprintf("PASS multipage\n");
    return 0;
}
