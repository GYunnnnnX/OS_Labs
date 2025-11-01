# 操作系统课程ucore Lab3

## 练习一：完善中断处理

本题的实现要求是在对时钟中断进行处理的部分填写`kern/trap/trap.c`函数中处理时钟中断的部分，使操作系统每遇到100次时钟中断后，调用`print_ticks`子程序，向屏幕上打印一行文字”100 ticks”，在打印完10行后调用`sbi.h`的`shut_down()`函数关机。

首先，`kern/trap/trap.c`文件中已经定义好了`print_ticks`函数：

```c
#define TICK_NUM 100

static void print_ticks() {
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}
```

其中的`TICK_NUM`已经通过宏定义的方式设定为100，`print_ticks()`函数中的`cprintf("%d ticks\n", TICK_NUM)`语句会输出100 ticks，所以遇到100次时钟中断后，直接调用`print_ticks()`函数即可。

下面，定位**时钟中断**处理部分：

```c
#include <sbi.h>
int clock_print_num = 0;
......

		case IRQ_S_TIMER:
            /*(1)设置下次时钟中断- clock_set_next_event()
             *(2)计数器（ticks）加一
             *(3)当计数器加到100的时候，我们会输出一个`100ticks`表示我们触发了100次时钟中断，同时打印次数（num）加一
            * (4)判断打印次数，当打印次数为10时，调用<sbi.h>中的关机函数关机
            */
            clock_set_next_event();
            if(++ticks % TICK_NUM == 0)
            {
                print_ticks();
                if(++clock_print_num == 10)
                {
                    sbi_shutdown();
                }
            }
            break;
......
```

这部分在函数`interrupt_handler()`中的分支`case IRQ_S_TIMER`中。我们首先用`clock_set_next_event()`来设置下一次中断，这样就能**延续**输出操作。通过`if(++ticks % TICK_NUM == 0)`判断是否足够100次时钟中断，每100次中断就调用`print_ticks()`函数输出”100 ticks”。且定义全局变量`clock_print_num`来记录打印次数，在打印十行后，调用`sbi.h`（引入头文件）的`shut_down()`函数关机。

至此，练习一的部分就完成了，可以通过运行命令`make qemu`检测正确性。

## 拓展练习Challenge1：描述与理解中断流程

## 拓展练习Challenge2：理解上下文切换机制

### Q1：在trapentry.S中汇编代码 `csrw sscratch, sp`；`csrrw s0, sscratch, x0`实现了什么操作，目的是什么？

- `csrw sscratch, sp`这条指令表示将当前栈指针 `sp` 的值写入控制状态寄存器 `sscratch`，用于保存用户空间的栈指针；
- `csrrw s0, sscratch, x0`这条指令表示将 `sscratch` 的当前值（也就是上一步保存的用户空间的栈指针）读出到通用寄存器 `s0` 中，并将源操作数 `x0`的值写入 `sscratch`，也就是清零 `sscratch`；

这两条指令位于汇编宏 `SAVE_ALL`中，此时发生的是保存CPU的寄存器到内存（栈上）的操作，结合后面的    `STORE s0, 2*REGBYTES(sp)`这条指令，所以这两条指令的核心操作就是把用户空间的栈指针存储到栈上用于保存信息。

**那么为什么不直接对sp进行store操作呢？**

首先，我们需要保存的是用户空间的栈指针，也就是没有进行开辟栈空间操作之前的栈指针，所以必须得提前保存下来这个信息，我们就使用了`csrw sscratch, sp`这条指令将将**用户空间的栈指针**安全地暂存到 `sscratch` 中；那么，**为什么不直接保存`sscratch`寄存器的值到栈上呢？**从指导书中我们得知，这是因为**RISCV不能直接从CSR写到内存, **需要`csrr`把CSR读取到通用寄存器，再从通用寄存器STORE到内存。

解决了这个问题之后，我们再来看，**为什么需要清零 `sscratch`呢？**

通过查阅相关资料，我了解到，在 RISC-V 约定中，在用户态，`sscratch` 保存内核栈的地址；在内核态，`sscratch` 的值为 0。在处理Trap时，当 `sscratch` 为非零值时，表示发生 Trap 时 CPU 处于用户模式；当 `sscratch` 为零时，表示处于内核模式。将其清零，标志着 CPU 现在已经正式在内核上下文中运行，如果在内核态执行时再次发生异常（比如缺页中断），CPU 看到 `sscratch` 为 0，就会知道当前已经在内核态，从而使用当前的内核栈 `sp` 进行处理，而不会错误地尝试交换 `sp` 和 `sscratch`（即处理新的Trap）。所以**这是一个关键的安全措施**。

### Q2：save all里面保存了stval scause这些csr，而在restore all里面却不还原它们？那这样store的意义何在呢？

结合代码，我们知道，我们在`RESTORE_ALL`中恢复了`sepc`和`sstatus`这两个csr寄存器，而没有恢复`sscratch`、`scause`、`stval`这几个csr寄存器。这是因为这些csr寄存器在Trap处理的生命周期中扮演着不同的角色，我们可以把它们分为**执行状态寄存器**和**事件信息寄存器**两类。

要想回答这个问题，我们需要先把这些csr寄存器的功能说明白了：

**执行状态寄存器（需要恢复）**：

- `sepc`：程序计数器，决定**返回后从哪里执行**
- `sstatus`：CPU状态，包含中断使能、特权级等信息，决定**返回后的运行环境**

需要恢复执行状态寄存器，是因为它们定义了进程在Trap处理结束后，返回到用户态时该如何继续执行，它们保存的是进程在陷入内核之前的用户态执行状态。

**事件信息寄存器（不需要恢复）**：

- `sscratch`：记录**是否在内核态的信息**

- `scause`： 记录**发生异常或中断的原因**
- `stval`： 记录**异常的附加信息**

我们可以总结为这三个寄存器的作用是用于提供信息，为了**让内核的Trap处理程序能够查询这些信息**，从而决定如何处理这个异常。比如，结合之前体系结构的实验，我们知道`scause[6:2]`为`0'1000`时，表示的是系统调用，那么内核的Trap处理程序就会去处理系统调用的Trap。这就是store这些寄存器带来的价值，给内核提供trap信息。那么为什么不需要restore它们呢？我们可以从以下几个角度进行解释：

**旧事件已过时**：这几个寄存器描述的是刚刚处理过的那个Trap，当从 Trap 返回用户态时，场景已经完全不同，与导致上次 Trap 的那个瞬间没有直接关系，所以恢复这几个事件信息寄存器的值毫无意义，反而可能造成混淆。

**CPU 会覆盖它们**： 当进程恢复执行后，如果再次发生新的异常/中断，CPU 硬件会自动地用新的原因和新的地址覆盖 `scause` 和 `stval`，`sscratch`也会被重新清零，保留旧的值毫无意义，可能还会干扰新异常的处理。

总之，对于这几个csr寄存器是否需要恢复，就看它在trap结束的这个瞬间是否还有用了，没用了就没必要进行恢复。

## 拓展练习Challenge3：完善异常中断