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

### 描述ucore中处理中断异常的流程

#### 初始化阶段：

```
void idt_init(void) {
    extern void __alltraps(void);
    write_csr(sscratch, 0);
    write_csr(stvec, &__alltraps);
}
```

系统启动时，通过`idt_init()`函数完成中断处理环境的初始化。首先将`sscratch`寄存器清零，通过查阅资料，我们了解到，在RISC-V架构中，`sscratch`寄存器具有特殊的用途，当它的值为0时表示内核态，非零时通常存储着内核栈指针，用于用户态到内核态的栈切换，我们进行的这个清零的操作就标志着当前CPU处于内核态。

其次，通过`write_csr(stvec, &__alltraps)`设置异常向量地址。`stvec`（Supervisor Trap Vector）寄存器告诉CPU当异常或中断发生时应跳转到哪个地址执行。这里设置为`__alltraps`，意味着所有类型的中断和异常都会首先跳转到这个统一的入口点进行处理。

#### Trap产生：

- 中断（Interrupt）：**异步事件**，由外部设备触发，与当前执行的指令无关。例如：定时器中断、磁盘I/O完成中断。
- 异常（Exception）：**同步事件**，由当前执行的指令触发。例如：访问无效内存地址、执行非法指令、缺页异常，以及执行`ecall`和`ebreak`指令。

#### 硬件自动响应：

一旦发生中断或异常，CPU硬件会自动完成以下操作：

- 将当前程序计数器（PC）保存到`sepc`寄存器，这样在中断处理完成后才能准确返回到被中断的指令位置；
- 将异常原因存入`scause`寄存器，比如系统调用对应的编码是`01000`；
- 将异常附加信息存入`stval`寄存器，比如在缺页异常中这里存储的是引发异常的地址；
- 在`sstatus`寄存器中保存当前的特权级状态，包括中断使能状态和之前的运行模式；
- 最后将PC设置为`stvec`寄存器指向的地址，即跳转到`__alltraps`开始执行软件的中断处理程序。

#### 保存上下文：

```asm
    .globl __alltraps

.align(2) #中断入口点 __alltraps必须四字节对齐
__alltraps:
    SAVE_ALL #保存上下文
```

`SAVE_ALL` 宏指令将**所有通用寄存器**和**CSR**的值压入**内核栈**，负责保存完整的执行上下文，目的是保存用户进程的完整执行状态或者为Trap处理程序提供异常信息，为后续的Trap处理函数提供了访问所有寄存器状态的统一接口。此处说明：对于store和restore的逻辑我们不再展开详细分析了，因为在challeng2中已经有了较为详细的分析过程。

####  调用C语言处理程序：

```asm
    move  a0, sp #传递参数。
    #按照RISCV calling convention, a0寄存器传递参数给接下来调用的函数trap。
    #trap是trap.c里面的一个C语言函数，也就是我们的中断处理程序
    jal trap 
    #trap函数指向完之后，会回到这里向下继续执行__trapret里面的内容，RESTORE_ALL,sret
```

- `move a0, sp` 将当前内核栈栈顶指针（指向刚保存的上下文结构 `trapframe`）作为参数传递给C语言函数 `trap()`，C代码可以通过这个指针访问所有保存的寄存器状态；
- 然后`jal trap`指令跳转到`trap.c`中的`trap()`函数，开始了顶层的中断处理逻辑。

```C++
void trap(struct trapframe *tf) {
    // dispatch based on what type of trap occurred
    trap_dispatch(tf);
}
```

在`trap()`函数中，调用`trap_dispatch()`进行中断分发。

```C++
static inline void trap_dispatch(struct trapframe *tf) {
    if ((intptr_t)tf->cause < 0) {
        // interrupts
        interrupt_handler(tf);
    } else {
        // exceptions
        exception_handler(tf);
    }
}
```

这个分发逻辑基于`scause`寄存器的最高位：如果为1表示中断，为0表示异常，这种设计使得硬件中断和软件异常可以分别处理。以中断为例：

```C++
void interrupt_handler(struct trapframe *tf) {
    intptr_t cause = (tf->cause << 1) >> 1;
    switch (cause) {
        case xxxx:
            break;
        default:
            break;
    }
} 
```

在`interrupt_handler()`函数中，`intptr_t cause = (tf->cause << 1) >> 1;`这一行代码将scause寄存器的值先左移一位再右移一位，相当于清除了最高位的中断标志，只保留低63位的异常原因编码，以便与已定义的宏进行匹配。而`exception_handler()`中不需要这么处理，直接使用`scause`寄存器即可。并根据cause的值通过**switch-case**逻辑选择不同的中断处理。

#### 恢复上下文并返回：

```asm
    .globl __trapret
__trapret:
    RESTORE_ALL
    # return from supervisor call
    sret
```

中断处理完成后，执行流程跳转到`__trapret`开始进行寄存器的恢复：

- `RESTORE_ALL`宏按照与保存时相反的顺序恢复寄存器状态，首先恢复控制状态寄存器`sstatus`和`sepc`的值并写回对应的CSR寄存器，接着恢复通用寄存器，按照`x31`到`x1`的顺序逐个从栈中加载值。特别注意栈指针`x2`是最后一个被恢复的，这种顺序避免了在恢复过程中破坏栈帧的结构。如果先恢复栈指针，后续的加载操作可能会使用错误的栈地址。
- `sret`指令将`sepc`的值载入PC，并根据`sstatus`恢复用户态，返回到被中断的用户程序，恢复中断使能状态。

### Q1：其中`mov a0，sp`的目的是什么？

在`SAVE_ALL`宏执行完成后，栈指针`sp`指向的是一个完整构建的`struct trapframe`结构体，这个结构体包含了所有寄存器的接口。我们知道，在RISC-V中，a0和a1寄存器是最常用的参数寄存器，通过`move a0, sp`将这个指针作为参数传递给C语言函数`trap()`，使得C语言代码能够**以安全的方式指向性地**访问所有寄存器状态，例如只需要`tf->cause`就可以拿到`scause`寄存器的值获取异常原因，进而进行后续的Trap处理。

### Q2：`SAVE_ALL`中寄存器保存在栈中的位置是什么确定的？

主要是通过在`trap.h`中定义的**`struct trapframe`和`struct pushregs`这两个结构体**来确定寄存器保存在栈中的位置的。这两个结构体明确规定了每个寄存器在栈帧中的偏移位置，只需要通过`sp`指向的结构体指针，即可完成对各寄存器的访问。除此之外，`SAVE_ALL`和`RESTORE_ALL`宏严格遵循这个内存布局，这些共同决定了访问的安全与准确性。

### Q3：对于任何中断，`__alltraps` 中都需要保存所有寄存器吗？请说明理由。

需要。**对于任何中断，`__alltraps` 中都需要保存所有寄存器，这是基于操作系统可靠性、安全性和一致性的核心设计原则。**理由如下：

- **上下文完整性**：由于内核无法预知被打断的用户程序使用到了哪些寄存器，在被打断的那条指令处，编译器可能正在使用任何一个通用寄存器来存放临时的计算结果，所以为保障上下文恢复的准确性，必须保存全部寄存器。
- **C函数调用约定**：C函数的调用约定允许编译器自由使用调用者保存寄存器（如t0-t6, a0-a7等），如果在调用C处理函数前不保存这些寄存器，它们的值可能会被C代码破坏，导致用户程序状态损坏。
- **统一处理流程：**所有中断共享同一入口和出口代码，这样就简化了设计，避免了为不同类型中断编写专用寄存器的保存/恢复逻辑，提高代码可维护性和可靠性。

## 拓展练习Challenge2：理解上下文切换机制

### Q1：在`trapentry.S`中汇编代码 `csrw sscratch, sp`；`csrrw s0, sscratch, x0`实现了什么操作，目的是什么？

- `csrw sscratch, sp`这条指令表示将当前栈指针 `sp` 的值写入控制状态寄存器 `sscratch`，用于保存用户空间的栈指针；
- `csrrw s0, sscratch, x0`这条指令表示将 `sscratch` 的当前值（也就是上一步保存的用户空间的栈指针）读出到通用寄存器 `s0` 中，并将源操作数 `x0`的值写入 `sscratch`，也就是清零 `sscratch`；

这两条指令位于汇编宏 `SAVE_ALL`中，此时发生的是保存CPU的寄存器到内存（栈上）的操作，结合后面的    `STORE s0, 2*REGBYTES(sp)`这条指令，所以这两条指令的核心操作就是把用户空间的栈指针存储到栈上用于保存信息。

**那么为什么不直接对`sp`进行store操作呢？**

首先，我们需要保存的是用户空间的栈指针，也就是没有进行开辟栈空间操作之前的栈指针，所以必须得提前保存下来这个信息，我们就使用了`csrw sscratch, sp`这条指令将将**用户空间的栈指针**安全地暂存到 `sscratch` 中；那么，**为什么不直接保存`sscratch`寄存器的值到栈上呢？**从指导书中我们得知，这是因为**RISCV不能直接从CSR写到内存, **需要`csrr`把CSR读取到通用寄存器，再从通用寄存器STORE到内存。

解决了这个问题之后，我们再来看，**为什么需要清零 `sscratch`呢？**

通过查阅相关资料，我了解到，在 RISC-V 约定中，在用户态，`sscratch` 保存内核栈的地址；在内核态，`sscratch` 的值为 0。在处理Trap时，当 `sscratch` 为非零值时，表示发生 Trap 时 CPU 处于用户模式；当 `sscratch` 为零时，表示处于内核模式。将其清零，标志着 CPU 现在已经正式在内核上下文中运行，如果在内核态执行时再次发生异常（比如缺页中断），CPU 看到 `sscratch` 为 0，就会知道当前已经在内核态，从而使用当前的内核栈 `sp` 进行处理，而不会错误地尝试交换 `sp` 和 `sscratch`（即处理新的Trap）。所以**这是一个关键的安全措施**。

### Q2(a)：save all里面保存了`stval` `scause`这些`csr`，而在`restore all`里面却不还原它们？

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

### Q2(b)：那这样store的意义何在呢？

上面说到的store这些不需要恢复的`csr`寄存器的意义有些含糊，在这里给出明确回答，我认为这样的意义就是，**通过store，使得栈空间与结构体相对应，C语言程序可以直接通过`tf->cause`获得这些csr寄存器的值，否则，由于C语言不能指向性地访问寄存器，就拿不到csr寄存器的值了，也就无法进行异常处理。**

## 拓展练习Challenge3：完善异常中断