#include <ulib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PGSIZE 4096

static void t_fail(const char *name, const char *msg) {
    cprintf("[FAIL] %s: %s\n", name, msg);
    exit(-1);
}
static void t_pass(const char *name) {
    cprintf("[PASS] %s\n", name);
}

typedef int (*case_fn_t)(void);

/* 运行单个 case：
 * - 子进程 exit(0) => PASS
 * - 子进程 exit(!=0) => FAIL
 * - expect_killed=1：期望子进程异常退出（exit_code != 0）
 * - 返回值：0=PASS，-1=FAIL
 */
static int run_case(const char *name, case_fn_t fn, int expect_killed) {
    int pid = fork();
    if (pid < 0) {
        cprintf("[FAIL] %s: fork failed\n", name);
        return -1;
    }
    if (pid == 0) {
        int r = fn();
        exit(r);
    }

    int code = 0x12345678; // 哨兵值，便于发现 store 没被写
    int ret = waitpid(pid, &code);
    if (ret != 0) {
        cprintf("[FAIL] %s: waitpid failed (ret=%d)\n", name, ret);
        return -1;
    }

    if (!expect_killed) {
        if (code == 0) { t_pass(name); return 0; }
        cprintf("[FAIL] %s: exit code=%d (expected 0)\n", name, code);
        return -1;
    } else {
        if (code != 0) { t_pass(name); return 0; }
        cprintf("[FAIL] %s: exit code=0 (expected killed)\n", name);
        return -1;
    }
}

/* ===================== 全局测试数据区 ===================== */
// data/bss：可写页必须触发 COW
static volatile int g_data = 42;
static volatile int g_bss;

// 跨页：两页 buffer（可写页）
static char g_buf[PGSIZE * 2];

// 模拟“匿名页/堆”
static char g_heap_like[PGSIZE * 2];

/* ===================== Case 1：基础 COW（data/bss） ===================== */
static int case_basic_data_bss(void) {
    g_data = 42;
    g_bss  = 100;

    int pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        if (g_data != 42 || g_bss != 100) return -2;
        g_data = 7;
        g_bss  = 9;
        if (g_data != 7 || g_bss != 9) return -3;
        return 0;
    }

    int code = 0;
    if (waitpid(pid, &code) != 0 || code != 0) return -4;

    if (g_data != 42 || g_bss != 100) return -5;
    return 0;
}

/* ===================== Case 2：多共享者写隔离（ref>1 路径） ===================== */
static int case_multi_writer_ref_gt1(void) {
    g_data = 1;

    int pid1 = fork();
    if (pid1 < 0) return -1;
    if (pid1 == 0) { g_data = 10; return (g_data == 10) ? 0 : -2; }

    int pid2 = fork();
    if (pid2 < 0) return -3;
    if (pid2 == 0) { g_data = 20; return (g_data == 20) ? 0 : -4; }

    int code = 0;
    if (waitpid(pid1, &code) != 0 || code != 0) return -5;
    if (waitpid(pid2, &code) != 0 || code != 0) return -6;

    if (g_data != 1) return -7;
    return 0;
}

/* ===================== Case 3：ref==1 快路径（子退出后父写） ===================== */
static int case_ref_eq1_fastpath(void) {
    g_data = 5;

    int pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        g_data = 8;
        return (g_data == 8) ? 0 : -2;
    }

    int code = 0;
    if (waitpid(pid, &code) != 0 || code != 0) return -3;

    // 子退出后，父写：应该不崩溃且值正确
    g_data = 9;
    if (g_data != 9) return -4;
    return 0;
}

/* ===================== Case 4：跨页逐页 COW ===================== */
static int case_cross_page(void) {
    g_buf[0] = 'A';
    g_buf[PGSIZE] = 'B';

    int pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        g_buf[0] = 'C';
        g_buf[PGSIZE] = 'D';
        if (g_buf[0] != 'C') return -2;
        if (g_buf[PGSIZE] != 'D') return -3;
        return 0;
    }

    int code = 0;
    if (waitpid(pid, &code) != 0 || code != 0) return -4;

    if (g_buf[0] != 'A') return -5;
    if (g_buf[PGSIZE] != 'B') return -6;

    return 0;
}

/* ===================== Case 5：模拟“堆/匿名页”写 COW ===================== */
static int case_heap_like(void) {
    memset(g_heap_like, 0x11, sizeof(g_heap_like));

    int pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        g_heap_like[0] = 0x22;
        g_heap_like[PGSIZE] = 0x33;
        if ((unsigned char)g_heap_like[0] != 0x22) return -2;
        if ((unsigned char)g_heap_like[PGSIZE] != 0x33) return -3;
        return 0;
    }

    int code = 0;
    if (waitpid(pid, &code) != 0 || code != 0) return -4;

    if ((unsigned char)g_heap_like[0] != 0x11) return -5;
    if ((unsigned char)g_heap_like[PGSIZE] != 0x11) return -6;
    return 0;
}

/* ===================== Case 6：只读写应被 kill（非 COW） ===================== */
static int case_readonly_write_should_kill(void) {
    const char *s = "readonly";
    ((char *)s)[0] = 'X';   // 应触发 store page fault，且不是 COW -> do_exit(-E_KILLED)
    return 0;               // 正常情况下到不了这里
}

int main(void) {
    cprintf("=== COW final test suite start (pid=%d) ===\n", getpid());

    int failed = 0;
    failed |= (run_case("1) 基础：data/bss COW 隔离", case_basic_data_bss, 0) != 0);
    failed |= (run_case("2) 多写者：ref>1 写隔离",     case_multi_writer_ref_gt1, 0) != 0);
    failed |= (run_case("3) ref==1：子退出后父写",     case_ref_eq1_fastpath, 0) != 0);
    failed |= (run_case("4) 跨页：逐页触发 COW",       case_cross_page, 0) != 0);
    failed |= (run_case("5) 匿名页：模拟堆写 COW",     case_heap_like, 0) != 0);
    failed |= (run_case("6) 只读写：应异常终止",       case_readonly_write_should_kill, 1) != 0);

    if (!failed) {
        cprintf("=== COW final test suite PASS ===\n");
        return 0;
    } else {
        cprintf("=== COW final test suite FAIL ===\n");
        return -1;
    }
}
