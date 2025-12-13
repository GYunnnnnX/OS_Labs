// 目标：验证“共享 + 写时复制”工作正常。

#include <ulib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int g = 123;
static char buf[4096];

int main(void) {
    strcpy(buf, "parent");

    int pid = fork();
    if (pid == 0) {
        strcpy(buf, "child");
        g = 456;
        cprintf("child: buf=%s g=%d\n", buf, g);
        exit(0);
    }

    int code = 0;
    assert(waitpid(pid, &code) == 0);
    cprintf("parent: buf=%s g=%d\n", buf, g);

    // 关键断言：父进程必须保持原值
    assert(strcmp(buf, "parent") == 0);
    assert(g == 123);

    cprintf("cow_basic pass.\n");
    return 0;
}
