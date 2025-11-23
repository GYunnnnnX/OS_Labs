# 操作系统课程ucore Lab4

## 练习0：填写已有实验

已实现。

## 练习1：分配并初始化一个进程控制块（需要编码）

### Q1：简要说明alloc_proc函数初始化一个进程控制块的设计实现过程

这个函数的功能是创建一个新的进程控制块，在最初的第0个内核线程 idleproc 初始化（proc_init）的时候，以及创建一个新的子进程（do_fork）时会被调用。它主要做了两件事：

1. 用 `kmalloc` 给进程控制块（`proc_struct`）分配了一块内核的堆内存，用于承载这个进程控制块结构体的字段内容；

	```
	struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
	```

2. 把这个进程控制块的各个字段都初始化到一个干净的、未运行的内核线程状态。

	```cpp
	proc->state = PROC_UNINIT;  // 设置进程为“初始”态
	proc->pid = -1;  // 设置进程pid的未初始化值
	proc->runs = 0;  // 记录这个进程/线程 被调度运行了多少次，初始为 0
	proc->kstack = 0;  // 进程的内核栈地址，初始为 0，表示还没给它分配内核栈
	proc->need_resched = 0;  // 表示当前进程不需要主动让出 CPU（不默认为占位进程）
	proc->parent = NULL;  // 标识父进程，也就是从谁那里fork来的
	proc->mm = NULL;  // 内核线程不需要自己的 mm（因为是共享内核地址空间），所以为 NULL
	memset(&(proc->context), 0, sizeof(struct context));  // 把内核上下文context全部清零
	proc->tf = NULL;  // 对内核新建的 PCB 来说，还没有 trapframe，自然为 NULL
	proc->pgdir = boot_pgdir_pa;  // 使用内核页目录表的基址
	proc->flags = 0;  // 各种进程标志位，比如是否正在退出、是否被杀掉等，这里全部清零，表示干净
	memset(proc->name, 0, sizeof(proc->name));  // 进程名清空，后面通过 set_proc_name 设置
	```

### Q2：说明`proc_struct`中`struct context context`和`struct trapframe *tf`成员变量含义和在本实验中的作用

**context** 是**线程在内核态运行时所需保存的寄存器集合**，是用于**线程切换**的**CPU 内核态上下文**，包括`ra`（返回地址）、`sp`（内核栈指针），以及`s0~s11`（callee-saved 寄存器）

> 因为线程切换在 `switch_to()` 函数当中，所以编译器会自动帮助我们生成保存和恢复调用者保存寄存器（如 `a0-a7`、`t0-t6`）的代码，因此在实际的进程切换过程中我们只需要保存被调用者（callee-saved）寄存器

每次进行线程切换时，都会调用 `switch_to()` 函数，将之前线程的上下文保存到它的内核栈中，然后把当前新线程的上下文从内核栈加载出来，最终执行 `ret` 回到新线程的上下文中的ra寄存器指向的位置，这样就可以紧接着线程之前的状态开始继续运行，而不会错乱。

> 对于刚被初始化、才第一次执行的进程（例如本次实验中的Initproc）来说，显然它是没有上下文的，因此我们会主动构造它的上下文，将 context.ra 设置为 forkret，用手工构造的 trapframe 初始化线程寄存器和状态、运行线程函数（实际上对用户进程，tf 是 trap/系统调用进入时自动产生的，就无需手动伪造了）

---

**trapframe** 虽然同样能够保存各种寄存器状态，但是它则是用于记录 CPU 在 trap 事件发生时的各种信息，包括所有通用寄存器（a0–a7, t0–t6, s0–s11, ra, sp）、`sepc`（异常返回地址）、`sstatus`（状态寄存器），以及各种中断信息字段，反映的是**线程的整个执行现场**

在本次实验中，它还承担了进程首次执行时的初始化任务，具体来说，内核线程创建时由 copy_thread 构造 tf，进程切换时 forkret 则会从 `proc->tf` 中恢复所有寄存器、执行 `sret`，最后回到 `tf->epc` 指向的位置（启动代码 kernel_thread_entry）继续执行，通过 tf 恢复进入 fn(arg)

> **总结来说：**
> **context 用于调度 (switch_to)，tf 用于 trap 和线程首次启动 (forkret → forkrets)**
> **context.sp 让 switch_to 能切到新线程，tf 让该线程真正运行起来。**


## 练习2：为新创建的内核线程分配资源（需要编码）

### Q1：简要说明设计实现过程。

`do_fork` 函数是 ucore 中创建内核线程的核心函数。它的作用是创建当前内核线程的一个副本，它们的执行上下文、代码、数据都一样，但是存储位置不同。在这个过程中，需要给新内核线程分配资源，并且复制原进程的状态。本实验中，此函数的设计实现过程设计如下：

1. **获得一块用户信息块 (`alloc_proc`)**：

   - 首先检查当前系统进程总数 `nr_process` 是否超过最大限制 `MAX_PROCESS`。
   - 调用 `alloc_proc()` 分配并初始化一个新的进程控制块（`PCB`）。如果分配失败，返回错误。
   - 设置父子指针：将新进程的 `parent` 指针指向当前进程 （`current`）。

2. **分配内核栈 (`setup_kstack`)**：

   - 调用 `setup_kstack()` 为新进程分配大小为 `KSTACKPAGE` 个页大小的内存作为内核栈。之后，内核状态下的进程将在内核栈中进行操作。
   - 调用 `get_pid()` 为新进程分配一个全局唯一的进程标识符。

3. **内存管理信息复制 (`copy_mm`)**：

   - 调用 `copy_mm()`。在本实验中，创建内核线程共享内核的内存空间，因此在当前实验阶段并**不**需要进行内存管理信息的复制；

   ​       在Lab4中，这个函数 "**do nothing**" 。

4. **上下文复制 (`copy_thread`)**：

   - 调用 `copy_thread()`，在新进程内核栈栈顶设置新进程的中断帧（`tf`），同时设置上下文（`context`）信息。
   - 将`tf->gpr.a0`设为0，这样子进程就知道自己是刚分裂来的。
   - 设置子进程的用户栈指针（`tf->gpr.sp`），对于内核线程，传入 `esp` == 0，让 `sp` 指向 `proc->tf`。
   - 将 `context.ra` 设置为 `forkret` 函数的入口地址。当新进程被调度执行时，会跳转到 `forkret`，进而跳转到 `forkrets`，利用中断帧恢复寄存器，最终进入新进程的代码执行流。
   - 将 `context.sp` 指向刚才设置好的中断帧位置。

5. **将新进程添加到进程列表 (`list_add` & `hash_proc`)**：

   - 调用 `hash_proc()` 将新进程加入 PID 哈希表，以便通过 PID 快速查找。
   - 将新进程加入全局进程链表 `proc_list`。
   - 增加进程计数 `nr_process`。

6. **唤醒新进程 (`wakeup_proc`)**：

   - 将新进程的状态设置为 `PROC_RUNNABLE`，使其可以被调度器选中执行。

7. **返回与错误处理**：

   - 如果上述任何步骤失败，跳转到错误处理标签（ `bad_fork_cleanup_kstack` 和 `bad_fork_cleanup_proc`），释放已分配的资源。
   - 若成功，返回新进程的 PID。

### Q2：请说明`ucore`是否做到给每个新fork的线程一个唯一的id？请说明你的分析和理由。

**答：** 是的，ucore **做到**了给每个新fork的线程一个唯一的id。

**分析和理由：**

这一机制是由 `get_pid` 函数来保证的。根据代码逻辑分析如下：

1. **确保id空间足够**： 首先利用 `static_assert(MAX_PID > MAX_PROCESS);`语句确保PID的取值范围大于系统允许的最大进程数。

2. **搜索与冲突检测 (`get_pid` 逻辑)**：

   - 函数维护了一个静态变量 `last_pid`，记录上一次分配的 PID。

   - 每次分配时，尝试 `++last_pid`。

   - **核心检查逻辑**：如果 `last_pid` 进入了可能发生冲突的区域（即大于 `next_safe`），函数会遍历当前的全局进程链表 `proc_list`：

     ```C
     while ((le = list_next(le)) != list)
     {
         proc = le2proc(le, list_link);
         //如果遍历发生了冲突，需要更新last_pid
         if (proc->pid == last_pid)
         {
             //如果相等，先递增last_pid
             if (++last_pid >= next_safe)
             {
                 //如果当前遍历到最大值，发现都被占用，就需要从1开始新一轮遍历了。
                 if (last_pid >= MAX_PID)
                 {
                     last_pid = 1;
                 }
                 next_safe = MAX_PID;
                 goto repeat;
             }
         }
         //没冲突的话，考虑确定next_safe的值
         else if (proc->pid > last_pid && next_safe > proc->pid)
         {
             next_safe = proc->pid;
         }
     }
     ```

   - 一旦在链表中发现有进程的 PID 等于当前的 `last_pid`，代码会立即递增 `last_pid` 并跳转到 `repeat` 标签**重新开始**整个链表的遍历检查。直到找到第一个没被占用的 `last_pid`。

   - 在 `last_pid`已经分配好后，将`next_safe`设置在下一个`pid`处，即“大于 `last_pid`的第一个已分配的`pid`”，在此之前的id空间可以放心分配。比如当前被占用的序列是1、2、7，那么 `last_pid`将会被分配为3， `next_safe`设置为7，在此之间的id空间（4、5、6）就可以安全分配了。这样可以有效剪枝一些不必要的扫描，提高效率。

   - 如果某次分配，`last_pid >= next_safe`，就将`next_safe`设置为`MAX_PID`，重新进行上述的扫描过程。

**总结：**通过上面的分析，我们发现代码已经通过了严格的扫描遍历检查机制，严格保证了分配给新线程的 PID 是**唯一**的。

## 练习3：编写`proc_run` 函数（需要编码）

### 设计思路：

在 `kern/process/proc.c` 中的 `proc_run` 函数是整个 ucore 操作系统中进程调度机制的核心组成部分。这个函数负责将指定的进程切换到 CPU 上运行，是实现多任务并发执行的关键。在实现过程中，我们需要确保进程切换的**原子性**、**状态一致性**并保证**执行效率**。

进程切换是一个复杂的操作，涉及到硬件状态的保存和恢复、地址空间的切换、以及调度状态的更新。为了保证系统的稳定性和正确性，这个过程必须是原子的，即在切换过程中不能被中断打断，同时，我们还需要考虑到性能优化，从而避免不必要的切换操作。

### 完整代码实现：

```C++
if (proc != current)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *prev = current;
    current = proc;
    lsatp(proc->pgdir);
    switch_to(&(prev->context), &(proc->context));
    local_intr_restore(intr_flag);
}
```

接下来我们对这段代码进行详细地分析：

**1. 条件检查与优化**

```C++
if (proc != current) {
    // 执行进程切换
}
```

操作：判断要切换的进程是否与当前进程相同，如果相同则直接返回，避免不必要的切换开销。

分析：我们知道，进程上下文切换是操作系统中最昂贵的操作之一。每次切换需要涉及保存和恢复大量寄存器状态、硬件状态等。通过预先检查，要切换的进程是否与当前进程相同，从而可以避免不必要的切换开销，显著提升系统性能。

**2.禁用中断**

```C++
bool intr_flag;
local_intr_save(intr_flag);
```

操作：使用 `local_intr_save(intr_flag)` 保存当前中断使能状态，并禁用中断；`local_intr_save` 会读取 sstatus 的 SIE 位，若为 1 则调用 `intr_disable()` 并返回 1 保存到 `intr_flag`中，用于辅助后续恢复中断。

分析：进程切换涉及修改全局指针 current 、页表寄存器 satp 、寄存器现场保存与恢复，因此必须具备原子性，若在切换过程中被中断打断，可能导致“旧现场尚未完全保存，新现场又开始恢复”的不一致状态，进而引发不可预测的错误。

**3.保存前一个进程指针**

```c++
struct proc_struct *prev = current;
```

操作：将当前进程 `current` 保存到局部变量 `prev` 中，用于后续的上下文切换。

分析：为 `switch_to(&(prev->context), &(proc->context))` 提供源与目标两个 context ： prev 用来写回旧进程的寄存器， proc 用来恢复新进程的寄存器。

**4.更新当前进程**：

```c++
current = proc;
```

操作：将全局变量 `current` 设置为要运行的新进程 `proc`。

分析：切换的是全局运行主体，调度器和后续打印等操作需要依赖 current 来识别当前进程。

**5.切换页表（地址空间切换）**

```c++
lsatp(proc->pgdir);
```

操作：调用 `lsatp(proc->pgdir)` 修改 RISC-V 的 satp 寄存器，使其指向目标进程的页表根。

分析：satp 结构含有模式（比如 Sv39 ）与页表根物理页号（PPN）， lsatp 会将 pgdir 右移页大小（ `RISCV_PGSHIFT` ）从而得到 PPN，并与模式组合写入 satp ，使地址翻译使用新进程的页表。

在本实验中，内核线程共享同一内核页表（ `boot_pgdir_pa` ），所以 `proc->pgdir` 基本相同；这步更多是为了框架化的实现，如果未来需要切换到用户进程地址空间，这步就至关重要，需要进行修改。

**6.执行上下文切换（寄存器现场切换）**

```c++
switch_to(&(prev->context), &(proc->context));
```

操作：调用 `switch_to(&(prev->context), &(proc->context))` 完成从旧进程到新进程的寄存器恢复。

分析：只保存/恢复被调用者保存寄存器（callee‑saved， s0~s11 ）以及 ra 和 sp这14个寄存器。在进程上下文切换时，我们不需要保存所有的寄存器，这是因为**进程切换本质上是发生在一个标准的函数调用过程中**，而编译器已经为我们自动处理了大部分寄存器的保存和恢复。具体来说，RISC-V 架构的寄存器分为两类：调用者保存寄存器和被调用者保存寄存器。当进程调用 `switch_to()` 函数进行切换时，编译器会自动将调用者保存寄存器（如 `a0-a7`、`t0-t6`）保存到当前进程的内核栈上，这些寄存器主要用于临时计算和参数传递，其值在函数调用边界没有长期意义。

因此，在上下文切换时，我们只需要手动保存那些承载重要长期数据的被调用者保存寄存器（`s0-s11`）以及控制程序执行流的关键寄存器（`ra`、`sp`）即可，这种设计既保证了正确性，又避免了不必要的性能开销。

**7.恢复中断**

```c++
local_intr_restore(intr_flag);
```

操作：使用 `local_intr_restore(intr_flag)` 将中断状态恢复到切换前的样子；根据`intr_flag`这个标志，恢复时若标志为 1 则调回 `intr_enable()`从而恢复中断。

分析：结束临界区，恢复先前的中断使能状态；避免长期屏蔽中断影响时钟与外设、导致系统不响应。

### 执行流程分析

通过学习指导书中的内容以及在代码中跟踪函数调用，我们了解到了进程切换并运行的整个流程，当系统决定从 `idleproc` 切换到 `initproc` 时，会触发以下详细的执行序列：

```
proc_run → switch_to → forkret → forkrets → __trapret → sret → kernel_thread_entry → init_main
```

在 **`proc_run(initproc)`** 被调用后，系统启动了一次精心设计的执行流切换。整个过程始于 **`switch_to`** （`kern/process/switch.S`）汇编函数，它完成了最基础的上下文寄存器的切换，并将返回地址ra恢复为 `initproc` 的 `context.ra`，即 **`forkret`** 函数的入口。随后，`forkret` 调用 **`forkrets`** 并转入到 **`__trapret`**（ `kern/trap/trapentry.S`），在这里 CPU 执行最关键的 `sret` 指令，该指令将程序计数器跳转至我们预设的 `trapframe.epc`，即指向 **`kernel_thread_entry`** 汇编入口，同时根据 `trapframe.status` 恢复中断使能状态。最后，在 `kernel_thread_entry`（`kern/process/entry.S`）中，通过 `move a0, s1` 和 `jalr s0` 两条指令，将预先存入 `s1` 的参数 `"Hello world!!"` 传递至 `a0`，并跳转执行 `s0` 所指向的 **`init_main`** 函数。整个流程通过模拟中断返回，实现了从底层的上下文切换至高级语言函数的无缝衔接，完成了内核线程的启动。

### 问题：在本实验的执行过程中，创建且运行了几个内核线程？

**答：** 在本实验的执行过程中，创建且运行了 **2个** 内核线程：`idleproc` 与 `initproc`。这体现在我们的创建与运行流程中：

#### 创建过程：

**`idleproc` 创建与初始化：**

```c++
// 在 proc_init() 函数中直接创建第0个内核线程
if ((idleproc = alloc_proc()) == NULL) {
    panic("cannot alloc idleproc.\n");
}

// 对 idleproc 进行完整初始化
idleproc->pid = 0;                    // 分配特殊PID 0，标识为第0个内核线程
idleproc->state = PROC_RUNNABLE;      // 设置为可运行状态，准备被调度执行
idleproc->kstack = (uintptr_t)bootstack;  // 使用系统启动时建立的内核栈
idleproc->need_resched = 1;           // 关键特性：主动要求重新调度
set_proc_name(idleproc, "idle");      // 设置进程名称为"idle"
nr_process++;                         // 进程计数器增加

current = idleproc;                   // 将当前运行进程设置为idleproc
```

idleproc 的创建始于 `proc_init()` 函数，它首先调用 `alloc_proc()` 分配进程控制块，该函数使用 kmalloc 获取内存并将所有字段初始化为默认值：状态设为 `PROC_UNINIT`，表示还未真正初始化，pid 设为 -1表示还未进行分配，关键的是将 pgdir 设置为 `boot_pgdir_pa`，表明其共享内核页表。随后在 `proc_init` 中对其进行针对性的初始化操作：赋予合法的 `PID 0`，状态改为 `PROC_RUNNABLE`，表示可运行，内核栈直接使用系统启动栈 bootstack，并将 `need_resched` 置为 1，表明其愿意主动让出 CPU，最后设置名称为 "idle" 并更新进程计数。

**`initproc` 创建与初始化：**

```c++
// 通过 kernel_thread 函数创建第1个内核线程
int pid = kernel_thread(init_main, "Hello world!!", 0);
if (pid <= 0) {
    panic("create init_main failed.\n");
}

// 通过PID查找并设置initproc
initproc = find_proc(pid);            // 在进程哈希表中查找对应的进程控制块
set_proc_name(initproc, "init");      // 设置进程名称为"init"
```

initproc 通过 `proc_init()` 调用 `kernel_thread(init_main, "Hello world!!", 0)` 创建。`kernel_thread` 函数核心工作是构造一个初始中断帧 tf：将函数指针 `init_main` 存入 `tf.gpr.s0`，参数字符串地址存入 `tf.gpr.s1`，设置状态寄存器确保内核模式与中断配置，并将入口点 epc 设为 `kernel_thread_entry`。随后调用 `do_fork`函数，依次执行 `alloc_proc` 分配 PCB、`setup_kstack` 分配独立内核栈、`copy_thread` 设置执行上下文，最终将新进程加入管理系统并唤醒，完成创建，并设置进程名称为"init"。

#### 运行过程：

**`idleproc` 运行过程：**

```c++
// idleproc 执行 cpu_idle() 函数，作为系统的空闲线程
void cpu_idle(void) {
    while (1) {
        if (current->need_resched) {  // 持续检查是否需要重新调度
            schedule();               // 调用调度器选择其他进程运行
        }
    }
}
```

idleproc 的运行实体是 `cpu_idle()` 函数，它在系统初始化完成后被调用。该函数是一个无限循环，持续检查当前进程的 `need_resched` 标志。由于初始化时该标志被设为 1，循环会立即调用 `schedule()` 函数触发调度。`schedule` 函数将遍历进程链表，寻找处于 `PROC_RUNNABLE` 状态的其他进程（即 initproc），找到后便调用 `proc_run` 进行切换。因此，idleproc 的核心作用就是在系统无其他任务时占用 CPU，负责在无其他任务时运行并主动让出CPU。

**`initproc` 运行过程：**

```c++
// initproc 执行 init_main() 函数，输出验证信息
static int init_main(void *arg) {
    cprintf("this initproc, pid = %d, name = \"%s\"\n", 
            current->pid, get_proc_name(current));  // 输出进程信息
    cprintf("To U: \"%s\".\n", (const char *)arg);  // 输出"Hello world!!"
    cprintf("To U: \"en.., Bye, Bye. :)\"\n");      // 输出结束信息
    return 0;                                       // 函数返回
}
```

initproc 执行实际的初始化工作，其运行始于调度器选择它后调用 `proc_run(initproc)`，该函数在关闭中断的保护下，通过 `switch_to` 完成上下文切换，将执行流导向其 `context.ra` 指向的 `forkret`；`forkret` 进而调用 `forkrets` 准备中断返回环境，并跳转到 `__trapret`；`__trapret` 从 initproc 内核栈顶的 `trapframe` 恢复所有寄存器，其中 `epc` 指向 `kernel_thread_entry`，`s0`/`s1` 保存着目标函数与参数；执行 `sret` 后，CPU 开始执行 `kernel_thread_entry`，该入口函数将参数从 `s1` 移入 `a0`，然后通过 `jalr s0` 跳转至 `init_main` 函数，最终输出 "Hello world!!" 等信息。

## 扩展练习 Challenge：

### 说明语句`local_intr_save(intr_flag);`....`local_intr_restore(intr_flag);`是如何实现开关中断的？

首先，我们找到这两个宏的定义：

```c++
#define local_intr_save(x) \
    do {                   \
        x = __intr_save(); \
    } while (0)
#define local_intr_restore(x) __intr_restore(x);
```

可以看到，它们是对底层函数 `__intr_save` 和 `__intr_restore` 的封装，`do {...} while (0)` 结构确保宏在使用时像单个语句一样工作，避免语法问题。接着去找这两个底层函数：

```c++
static inline bool __intr_save(void) {
    if (read_csr(sstatus) & SSTATUS_SIE) {
        intr_disable();
        return 1;
    }
    return 0;
}

static inline void __intr_restore(bool flag) {
    if (flag) {
        intr_enable();
    }
}
```

在 `__intr_save()` 函数中，首先执行 `read_csr(sstatus)` 读取状态寄存器，然后通过 `& SSTATUS_SIE` 的按位与操作检查 SIE 位。如果 SIE 为1，调用 `intr_disable()`  清除 SIE 位关闭中断，并返回1；否则直接返回0。

而在 `__intr_restore(bool flag)` 函数中，检查传入的 flag 值。如果 flag 为1，调用 `intr_enable()` 设置 SIE 位开启中断；如果 flag 为0，则不进行任何操作，保持中断关闭状态。

最终，`intr_disable` 和 `intr_enable` 通过直接操作 RISC-V 的控制状态寄存器 (CSR) 来实现：

```c++
/* intr_enable - enable irq interrupt */
void intr_enable(void) { set_csr(sstatus, SSTATUS_SIE); }

/* intr_disable - disable irq interrupt */
void intr_disable(void) { clear_csr(sstatus, SSTATUS_SIE); }
```

这两个宏定义如下：

```asm
#define set_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrs %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); \
  __tmp; })

#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); \
  __tmp; })
```

在底层硬件层面，`set_csr` 和 `clear_csr` 宏通过 RISC-V 汇编指令直接操控控制状态寄存器。`set_csr` 使用 `csrrs` 指令，该指令原子性地读取指定 CSR 的当前值到临时变量 `__tmp`，同时将 `bit` 参数指定的位设置为 1。具体到 `intr_enable()` 中，就是将 `sstatus` 寄存器的 `SSTATUS_SIE` 位设为 1，从而开启中断。

相应地，`clear_csr` 使用 `csrrc` 指令，该指令同样原子性地读取 CSR 值到 `__tmp`，但将 `bit` 参数指定的位清除为 0。在 `intr_disable()` 的上下文中，就是将 `sstatus` 寄存器的 `SSTATUS_SIE` 位清 0，实现中断关闭。

综合来看，`local_intr_save` 和 `local_intr_restore` 通过层层封装，从高级的宏接口到底层的汇编指令，构建了一个完整的中断控制机制。这种设计提供了安全可靠的临界区保护，为ucore 操作系统的进程调度和资源管理提供了坚实的基础保障。

### 深入理解不同分页模式的工作原理（思考题）

#### Q1：`get_pte()`函数中有两段形式类似的代码， 结合sv32，sv39，sv48的异同，解释这两段代码为什么如此相像。

**答：**核心原因是因为ucore使用的是“**两级页表结构**”，而 `get_pte()` 需要顺着页表结构逐级向下，为每一级目录项不存在时分配一个新页。而两级页表在`if`内的处理几乎是一样的，所以会出现两段几乎一样的代码。而 RISC-V 分页机制（sv32 / sv39 / sv48）都是**多级页表**，每一级都要有一段相像的代码。分别来说，sv32是2级页表，需要两段相似代码；sv39是3级页表，需要三段相似代码；sv48是4级页表，需要四段相似代码。

具体来说，`pde_t *pdep1 = &pgdir[PDX1(la)]`语句从顶级页目录 pgdir 中，用虚拟地址la的高位索引PDX1(la)，找到对应的一级页目录项的位置，把它的地址赋给 pdep1。如果该 PDE 无效（表示对应的下一级页表页不存在）：

- 若 `create == false`（调用者不允许创建新页表页），直接返回 `NULL`：表示找不到对应 PTE。
- 否则尝试 `alloc_page()` 分配一个新的物理页来作为下一级页表页；若分配失败，返回 `NULL`。

否则，分配新页。`set_page_ref(page, 1)`语句把新分配页的引用计数设为1，防止被回收；然后使用`page2pa(page)`语句从Page结构体中，获取这个Page的物理地址；`memset(KADDR(pa), 0, PGSIZE)`：把物理地址转换为内核的虚拟地址，再把新分配的页清零，初始化为空页表；`pte_create(page2ppn(page), PTE_U | PTE_V)`：构造PDE值（把页的物理页号`page2ppn(page)`放入PDE的物理地址字段，并设置权限/标志）。这里设置了 `PTE_U | PTE_V`（用户位 + 有效位），然后写回 `*pdep1`，使第一级目录项指向新分配的二级页表页。

第二段代码，首先使用`pde_t *pdep0 = &((pte_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)]`语句借助`pdep1`和从 `la` 中提取的第二级索引访问下一级页目录项（在ucore中，实际是页表项）。后面的逻辑与上一段就几乎相同了。

由此可见，在多级页表结构（sv32 / sv39 / sv48都是）中，顺次逐级访问页表，都需要对应的几段相似的代码。不同之处在于，sv32 / sv39 / sv48分别是2、3、4级页表，分别需要2、3、4段相似代码。



#### Q2：目前`get_pte()`函数将页表项的查找和页表项的分配合并在一个函数里，你认为这种写法好吗？有没有必要把两个功能拆开？

**答：**我认为这种设计**不好**，查和分配都在一个函数中，容易产生很多工程上的问题：

- **查和分配职责不清**：查不是单纯的查找，而是可能会在内部做一些分配（不可见不可控），而这些有时候可能不是我们想要的。
- **读写混乱，不利于并发**：查对应着“读”操作，分配对应着“写”操作。在实际的操作系统中，很多并发的操作需要根据读写操作上不同的锁（或作不同的权限处理），目前的函数将查和分配合并在一块，不利于并发环境。
- **代码可读性差，不如将操作分开清晰**：目前的函数在使用的过程中可读性太差，不易于我们发现什么时候查找、什么时候分配，不如将两种操作分开来得更加清晰、可控。

但是，在教学使用方面，这样写能确实能简化一部分处理：“如果缺项就分配”，然后直接给出分配好的一步到位的结果，也算是省事了。何况，函数额外提供了一个参数`create`，来让我们选择缺项后是否由函数直接分配。所以，仅在课程实验的场景下，我觉得也**不必分开**。

## 知识点分析：

### 1. 重要知识点与OS原理对应知识点

| **实验中重要的知识点**                | **对应的 OS 原理知识点**                    | **对应关系**                                                 |
| ------------------------------------- | ------------------------------------------- | ------------------------------------------------------------ |
| **`struct proc_struct` (PCB) 结构体** | **进程控制块 (Process Control Block, PCB)** | PCB是用于描述和管理进程生命周期的数据结构。 `proc_struct` 是 OS 原理中 PCB 概念在 ucore 中的具体代码实现。 **差异**：原理中的 PCB 是抽象的，包含所有可能的属性（如计费信息、IO状态等）；而实验中的 `proc_struct` 是具体的，包含了针对 ucore 内存管理（`mm`, `pgdir`）和上下文切换（`context`, `tf`）所需的特定字段，且在 Lab4 中仅涉及内核线程，部分用户态字段暂未充分利用。 |
| `switch_to` 与 **`struct context`**   | **上下文切换 (Context Switch)**             | 上下文切换指 CPU 从一个进程切换到另一个进程时保存和恢复现场的过程。`switch_to` 函数利用 `struct context` 实现了原理中的“保存旧进程状态，恢复新进程状态”。 **差异**：OS 原理通常笼统地称为“保存上下文”；而实验中具体区分了 **Context**（调度切换时保存的 Callee-saved 寄存器）和 **Trapframe**（中断发生时保存的全部寄存器），揭示了软件切换和硬件中断现场保护的区别。 |
| **`do_fork` 函数**                    | **进程创建 (Process Creation / Fork)**      | `do_fork`函数是父进程创建子进程的核心机制。 `do_fork` 函数实现了原理中 fork 操作的核心逻辑：复制资源、分配 PID、设置状态。 **差异**：OS 原理中的 fork 通常指复制用户进程；本实验中的 `do_fork` 主要用于创建内核线程（`kernel_thread`），它们共享内核地址空间，不涉及复杂的用户态内存复制，比原理中描述的标准 fork 更轻量。 |
| **`local_intr_save/restore`**         | **临界区与原子操作 (Critical Section)**     | 临界区保证某段代码执行时不被中断打断。实验中在 `proc_run` 等关键调度代码中使用关中断指令，对应原理中实现互斥（Mutual Exclusion）的硬件手段之一。 **差异**：OS 原理介绍了信号量、锁等多种互斥方式；而在内核核心调度器这种底层场景下，实验直接采用简单粗暴的“关中断”来保证原子性，这是实现上层锁机制的基础。 |

### 2. 实验中未对应的 OS 原理重要知识点

1. **用户态与内核态的特权级隔离 **

   Lab4 主要关注**内核线程**，`idleproc` 和 `initproc` 都运行在内核态。OS 原理中的“用户态进程试图执行特权指令触发保护”以及完整的系统调用（System Call）边界跨越，在本实验中没有体现，这通常在后续加载用户程序时才会涉及。

2. **复杂的进程调度算法 **

   OS 原理中包含 FCFS、SJF、多级反馈队列（MLFQ）等多种调度算法。Lab4 的实验代码（如 `schedule` 函数）虽然搭建了调度框架，但目前仅实现了一个极简的 FIFO 或轮询机制，并未包含原理中那些复杂的优先级调整、时间片轮转（RR）的具体策略实现。

3. **进程间通信**

   OS 原理中，进程间的协作离不开通信机制（如管道、消息队列、共享内存、信号量）。Lab4 中虽然创建了多个内核线程，但它们之间各自独立运行，除了通过全局变量，并没有实现标准的 IPC 机制来交换数据或同步。

4. **死锁处理**

   本实验中的内核线程资源需求简单（仅 CPU 和 内存），且逻辑单一，不涉及复杂的资源竞争和循环等待，因此 OS 原理中关于死锁检测、避免和预防的复杂机制在本实验中没有对应。