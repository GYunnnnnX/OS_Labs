# 操作系统课程ucore Lab5

## 练习0：填写已有实验

已实现。

## 练习1：加载应用程序并执行

### Q1：补充`load_icode`的第6步

> 补充`load_icode`的第6步，建立相应的用户内存空间来放置应用程序的代码段、数据段等，且要设置好`proc_struct`结构中的成员变量`trapframe`中的内容，确保在执行此进程后，能够从应用程序设定的起始执行地址开始执行。设置正确的`trapframe`内容。

**`do_execve`**函数调用`load_icode`函数来**加载**并**解析**一个处于内存中的**ELF**执行文件格式的应用程序。我们要补充的就是`load_icode`函数中的第六步。

在此之前，`load_icode`函数已经实现了以下的五个步骤：

- 创建一个新的`mm`
- 创建新的`PDT`
- 加载**elf**文件（text/data/bss等）
- 构建用户栈
- 切换地址空间（页表）

然后到了我们需要补充的步骤：初始化新用户环境的`trapframe`。首先，回顾`trapframe`结构：

```c
struct trapframe
{
    struct pushregs gpr;
    uintptr_t status;
    uintptr_t epc;
    uintptr_t tval;
    uintptr_t cause;
};
```

`trapframe`的结构如上；下面我们需要设置`tf->gpr.sp, tf->epc, tf->status`。我们用了四行代码来设置这三个值，代码如下：

```c
    /* should set tf->gpr.sp, tf->epc, tf->status
     * NOTICE: If we set trapframe correctly, then the user level process can return to USER MODE from kernel. So
     *          tf->gpr.sp should be user stack top (the value of sp)
     *          tf->epc should be entry point of user program (the value of sepc)
     *          tf->status should be appropriate for user program (the value of sstatus)
     *          hint: check meaning of SPP, SPIE in SSTATUS, use them by SSTATUS_SPP, SSTATUS_SPIE(defined in risv.h)
     */
    tf->gpr.sp = USTACKTOP;
    tf->epc = elf->e_entry;
    tf->status = sstatus & ~SSTATUS_SPP;
    tf->status |= SSTATUS_SPIE;

    ret = 0;
```

下面是他们的详细解释。

- `tf->gpr.sp`。`tf->gpr.sp = USTACKTOP`设置了用户栈指针，`USTACKTOP` 是用户栈的顶部地址。
- `tf->epc`。`tf->epc = elf->e_entry`设置了程序入口点，`elf->e_entry` 是ELF文件头中记录的程序入口地址，这个地址会被加载到`sepc`寄存器。
- `tf->status`。`tf->status = sstatus & ~SSTATUS_SPP`语句用来清除`SPP`位（为0表示返回用户态），`SSTATUS_SPP`表示触发异常前的特权级：0 = 用户态 (U-mode)，1 = 内核态 (S-mode)。`tf->status |= SSTATUS_SPIE`语句用来设置`SPIE`位。`SPIE`保存了触发异常前的中断使能状态，执行 `sret` 后，`SPIE`的值会被复制到`SIE`位，恢复中断使能。这样用户程序就可以正常响应中断了。

### 附：用户进程创建流程：

- 首先，创建用户进程。通过`kernel_thread()`函数里面，调用`do_fork()`、`copy_thread()`方法，设置新进程的内核栈上下文。

- 调用`kernel_execve()`函数，加载用户程序。我们需要利用中断机制，这里面主要触发了一个`ebreak`（不用`ecall`的原因是，目前我们在 `S mode` 下，所以不能通过 `ecall` 来产生中断。我们这里采取一个取巧的办法，用 `ebreak` 产生断点中断进行处理，通过设置 `a7` 寄存器的值为10 （**特殊标识**）说明这不是一个普通的断点中断，而是要转发到 `syscall()`）。`trap.c`的`exception_handler()`函数中，对这个逻辑进行了判断：

  ```c
  case CAUSE_BREAKPOINT:
      cprintf("Breakpoint\n");
      if (tf->gpr.a7 == 10)//判断
      {
          tf->epc += 4; //跳过ebreak指令，防止死循环
          syscall(); //调用syscall()
          kernel_execve_ret(tf, current->kstack + KSTACKSIZE);
      }
  ```

  其中的`kernel_execve_ret()`函数，它负责复制`trapframe`到新的栈顶，并且正常返回用户态。

- 中断返回后，调用`sys_exec()`函数，它又会调用`do_execve()`函数，它会清空当前的进程内存空间，再调用`load_icode()`函数，来为新的进程加载内容（具体流程在Q1中已经介绍）。

### Q2：简要描述这个用户态进程被ucore选择占用CPU执行（RUNNING态）到具体执行应用程序第一条指令的整个经过

- 经过时钟中断、调度器选中后，这个用户态进程被选择占用CPU执行了。那么需要调用`proc_run()`函数，让进程真正“跑起来”。

  当需要切换进程时，`proc_run()`函数会执行以下内容：

  ```c
  bool intr_flag;
  local_intr_save(intr_flag); //禁用中断
  struct proc_struct *prev = current;
  current = proc;
  lsatp(proc->pgdir); //切换页表，使用进程自己的虚拟地址空间
  switch_to(&(prev->context), &(proc->context)); //切换上下文
  local_intr_restore(intr_flag);//启用中断
  ```

  先禁用中断（结束后再启用），然后切换页表、切换上下文。

- `switch_to`中，保存旧进程的上下文，恢复新进程的上下文。然后，调用`ret`，跳转到`ra`（即`forkret`函数）。再`forkrets`汇编方法中，先进行 `move sp, a0`，将栈指针设置为指向`trapframe`；然后执行`j __trapret`跳到`__trapret`方法。`__trapret`中，进行`RESTORE_ALL` 恢复所有寄存器，然后执行`sret`返回用户态。

- `sret`后，硬件完成以下操作：PC从`sepc`（被设为`elf->e_entry`）中读取值（即下一条指令对应了应用程序的**第一条指令**了）；

  特权级设为`sstatus.SPP`（0，用户态），再把`sstatus.SPP`置0；`SIE`设为`sstatus.SPIE`（1，开启中断），再把`sstatus.SPIE`置1。

至此，CPU下一条就开始执行程序的**第一条**指令了。



## 练习2：父进程复制自己的内存空间给子进程

> 创建子进程的函数`do_fork`在执行中将拷贝当前进程（即父进程）的用户内存地址空间中的合法内容到新进程中（子进程），完成内存资源的复制。具体是通过`copy_range`函数（位于kern/mm/pmm.c中）实现的，请补充`copy_range`的实现，确保能够正确执行。
>
> 请在实验报告中简要说明你的设计实现过程。

创建用户子进程时，会通过系统调用 `sys_fork()` 调用对应内核函数 `do_fork()`，我们的期望是子进程要拥有一份和父进程一样的用户地址空间，而 `do_fork()` 完整流程是：

1. `alloc_proc()`：分配并初始化 PCB（`proc_struct`）
2. `setup_kstack()`：给子进程分配内核栈（内核态运行必须）
3. **`copy_mm(clone_flags, proc)`：复制/共享内存管理结构 mm**
4. `copy_thread(proc, stack, tf)`：准备子进程的 trapframe 和 context
5. `hash_proc(proc)` + `set_links(proc)`：加入系统进程集合并建立父子关系
6. `wakeup_proc(proc)`：把子进程设为 runnable
7. 返回子进程 pid 给父进程

其中`copy_mm()` 在完成 `mm_struct` 和页目录的创建后，会调用 `dup_mmap()` 遍历父进程的所有 `vma`，并在每一个 `vma` 的地址范围内调用 `copy_range()`。

> 在 uCore 中，每个用户进程都有一个 `mm_struct`，用于描述该进程的**完整虚拟地址空间**：
>
> - `mm_struct` 中维护了：
> 	- 页目录（`pgdir`）
> 	- 一组 `vma_struct`（虚拟内存区域）
> - 每个 `vma_struct` 描述了一段合法的虚拟地址区间 `[vm_start, vm_end)`，例如代码段、数据段、堆、用户栈等。
>
> 在 fork 过程中，复制用户地址空间（也就是整个 `copy_mm` 函数）的本质就是：
>
> **为子进程重新构造一套 vma + 页表，并将父进程 vma 覆盖范围内的所有有效虚拟页逐页复制。**

`copy_range()` 是本实验需要补充实现的关键函数，它负责在给定的虚拟地址区间 `[start, end)` 内，逐页复制父进程的用户内存。

其核心实现逻辑如下：

1. **按页遍历虚拟地址区间**

	```
	start % PGSIZE == 0 && end % PGSIZE == 0
	```

	保证复制以页为单位进行。

2. **在父进程页表中查找虚拟页映射**

	使用 `get_pte(from, start, 0)` 查找父进程在该虚拟地址处是否存在有效 PTE：

	- 若不存在，说明该页未映射，跳过；
	- 若存在且有效，则需要复制该页。

3. **为子进程分配新的物理页**

	```
	struct Page *npage = alloc_page();
	```

	子进程不能直接使用父进程的物理页，否则会破坏进程间的内存隔离。

4. **复制页内容并建立子进程的页表映射（我们补充实现的具体内容）**

	`src_kvaddr = page2kva(page);`

	- `page` 是父进程对应的某个物理页
	- `page2kva` 把它转换成 **内核可直接访问的虚拟地址**

	`dst_kvaddr = page2kva(npage);`

	- `npage` 是刚分配给子进程的新物理页
	- 同样转成内核地址，才能写入

	`memcpy(dst_kvaddr, src_kvaddr, PGSIZE);`

	- **复制整页内容（4KB）**
	- 这一步保证子进程拿到一份“与父进程一致”的内存快照

	`page_insert(to, npage, start, perm);`

	- 在子进程页表 `to` 中，把子物理页 `npage` 映射到相同的虚拟地址 `start`
	- `perm` 是父页的用户权限位（读写执行 U 等），子进程也与其保持一致



## 练习3：阅读分析源代码，理解进程执行fork/exec/wait/exit的实现，以及系统调用的实现

系统调用是用户程序请求操作系统服务的唯一途径，在ucore中，系统调用的完整流程如下：

```
[用户态]                    [内核态]
用户程序
  ↓
库函数封装 (fork/exec/wait/exit)
  ↓
sys_xxx() 函数
  ↓
syscall() 准备参数
  ↓
ecall 指令 ────────────→  触发异常
                           ↓
                        __alltraps (保存上下文)
                           ↓
                        trap()
                           ↓
                        exception_handler()
                           ↓
                        syscall() 分发
                           ↓
                        sys_xxx() 内核实现
                           ↓
                        do_xxx() 核心处理
                           ↓
                        设置返回值到 trapframe
                           ↓
                        __trapret (恢复上下文)
                           ↓
sret 指令 ←────────────  返回用户态
  ↓
用户程序继续执行
```

由此，我们知道，系统调用的通用逻辑和状态级转换是：

```
用户态程序 → 用户态库函数 → ecall指令 → 陷入内核 → 内核态处理 → sret返回 → 用户态继续执行
```

在此基础上，我们详细介绍进程如何执行**fork/exec/wait/exit**操作。



> 问题：请分析fork/exec/wait/exit的执行流程。重点关注哪些操作是在用户态完成，哪些是在内核态完成？内核态与用户态程序是如何交错执行的？内核态执行结果是如何返回给用户程序的？

首先，在`user/libs/ulib.c` 和 `user/libs/syscall.c`定义了用户程序调用的函数，在此处，**fork/wait/exit**三者的逻辑是一致的，而由于**exec**是**内核态**进行的操作，用户进程没有函数需要调用。首先，在`ulib.c`中，用户可直接调用的函数定义如下：

```c
void
exit(int error_code) {
    sys_exit(error_code);
    cprintf("BUG: exit failed.\n");
    while (1);
} //exit

int
fork(void) {
    return sys_fork();
} //fork 

int
wait(void) {
    return sys_wait(0, NULL);
} //wait
```

进而，到了`sys_fork/sys_wait/sys_exit`函数，它们在`user/libs/syscall.c`下面。

```c
int
sys_exit(int64_t error_code) {
    return syscall(SYS_exit, error_code);
} 

int
sys_fork(void) {
    return syscall(SYS_fork);
}

int
sys_wait(int64_t pid, int *store) {
    return syscall(SYS_wait, pid, store);
}
```

他们都会统一进入到`syscall`函数中，不同的调用会作为参数传进去。

```c
// syscall函数通过内联汇编执行系统调用
static inline int 
syscall(int64_t num, ...) {
    // 准备参数到寄存器 a0-a5
    asm volatile (
        "ld a0, %1\n"      // 系统调用号 -> a0
        "ld a1, %2\n"      // 参数1 -> a1
        ...
        "ecall\n"          // 触发系统调用
        "sd a0, %0"        // 返回值从a0取出
        : "=m" (ret)
        : "m"(num), "m"(a[0]), ...
    );
}
```

到此为止，都是在**用户态**进行的操作。**fork/wait/exit**通过封装的`fork()/wait()/exit()`函数，进而到了`sys_fork()/sys_wait()/sys_exit()`函数，然后统一到了`syscall`这个函数中。在`syscall`中，通过内联汇编，执行系统调用（`ecall`指令），实现从**用户态**到**内核态**的特权级转换。

下面，进行**内核态**的操作：

- `ecall`之后，进入`kern/trap/trapentry.S`的`__alltraps`入口。在这里，先对`trapframe`进行保存，再将当前栈指针（指向 `trapframe`）作为`trap()`的参数，再跳转到`trap()`处理；

- `trap()`（`kern/trap/trap.c`）调用`trap_dispatch()`函数，将这个`ecall`分发给`exception_handler()`处理。

  ```c
  case CAUSE_USER_ECALL:
          // cprintf("Environment call from U-mode\n");
          tf->epc += 4;
          syscall();//调用syscall()
          break;
  ```

- `syscall()`函数如下，它会接受寄存器中的参数并解析：

  ```c
  void
  syscall(void) {
      struct trapframe *tf = current->tf;
      uint64_t arg[5];
      int num = tf->gpr.a0;
      if (num >= 0 && num < NUM_SYSCALLS) {
          if (syscalls[num] != NULL) {
              arg[0] = tf->gpr.a1;
              arg[1] = tf->gpr.a2;
              arg[2] = tf->gpr.a3;
              arg[3] = tf->gpr.a4;
              arg[4] = tf->gpr.a5;
              tf->gpr.a0 = syscalls[num](arg);
              return ;
          }
      }
      print_trapframe(tf);
      panic("undefined syscall %d, pid = %d, name = %s.\n",
              num, current->pid, current->name);
  }
  ```

- 下面就是在**内核**中**分别**对`fork/exec/wait/exit`进行进一步操作了。注意，这里包括了`exec`。我们在本部分之前的分析没有包括`exec`，是因为我们这里的分析都是从用户态调用来的，而`exec`不需要用户调用。

  **那么`exec`是从哪来的呢？**

  我们之前分析过了，`exec`是在内核中调用的，而在内核中我们不能用`ecall`，我们用的是`ebreak`指令，用 `ebreak` 产生断点中断进行处理，通过设置 `a7` 寄存器的值为10 （特殊标识）说明这不是一个普通的断点中断，而是要转发到 `syscall()`。所以，`exec`的执行流在此处与`fork/wait/exit`汇合了，由`syscall()`统一调度，分配到下面的函数中：

  ```c
  static int
  sys_exit(uint64_t arg[]) {
      int error_code = (int)arg[0];
      return do_exit(error_code);
  }
  
  static int
  sys_fork(uint64_t arg[]) {
      struct trapframe *tf = current->tf;
      uintptr_t stack = tf->gpr.sp;
      return do_fork(0, stack, tf);
  }
  
  static int
  sys_wait(uint64_t arg[]) {
      int pid = (int)arg[0];
      int *store = (int *)arg[1];
      return do_wait(pid, store);
  }
  
  static int
  sys_exec(uint64_t arg[]) {
      const char *name = (const char *)arg[0];
      size_t len = (size_t)arg[1];
      unsigned char *binary = (unsigned char *)arg[2];
      size_t size = (size_t)arg[3];
      return do_execve(name, len, binary, size);
  }
  ```

  至此，`fork/exec/wait/exit`的操作就都分发至了`do_fork()/do_exec()/do_wait()/do_exit()`函数中了。从`__alltraps`一直到这里，都是在**内核态**中完成的。而**`do_execve`**函数是如何调用`load_icode`函数、再到**返回用户态**执行指令的过程，我们在**练习一**中分析过了，此处不再重复分析了。而`do_exit()`和`do_wait()`到这里就不是我们目前实验的重点了，所以我们以`do_fork()`为例继续向下分析。

- `do_fork()`的主要操作如下：

  ```c
  int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
  {   ...
      proc = alloc_proc();
      if (proc == NULL)
      {
          goto fork_out;
      }
      // 设置父节点和获取pid号
      proc->parent = current;
      proc->pid = get_pid();
      // 调用函数setup_kstack()和copy_mm()，并且检查运行结果
      if (setup_kstack(proc) != 0)
      {goto bad_fork_cleanup_proc;}
      if (copy_mm(clone_flags, proc) != 0)
      {goto bad_fork_cleanup_kstack;}
      // 设置tf & context
      copy_thread(proc, stack, tf);
      // 插入hash_list和proc_list
      hash_proc(proc);
      list_add(&proc_list, &(proc->list_link));
      nr_process++;
      // 唤醒进程
      wakeup_proc(proc);
      ret = proc->pid;
  fork_out:
      return ret;
      ...}
  ```

  至于`do_fork()`的具体实现，我们在上次的实验中已经详细做过了，此处不再赘述。下面，完成了fork操作的使命，该准备返回**用户态**了。

- 下面的过程，就是一步一步返回了：

  ```c
  static int
  sys_fork(uint64_t arg[]) {
      struct trapframe *tf = current->tf;
      uintptr_t stack = tf->gpr.sp;
      return do_fork(0, stack, tf);
  }
  ```

  `do_fork()`返回进程号，再到`sys_fork()`进一步返回到`syscall()`，在这里将`PID`写入`tf->gpr.a0`。下面，返回`exception_handler()`，返回`trap_dispatch()`，返回`trap()`，再次返回，就到了内核态开始的地方：`__alltraps`。

  `trap()`的使命完成，它的下一个命令就是`j __trapret`。`__trapret`中，进行`RESTORE_ALL` 恢复所有寄存器，然后执行`sret`，硬件会完成以下操作：PC从`sepc`中读取值、特权级设为`sstatus.SPP`（0，用户态），再把`sstatus.SPP`置0；设置`sstatus.SPIE`等......

  至此，就重新切换回**用户态**了，也意味着**fork/exec/wait/exit**操作都执行完成了。

  

> 问题：请给出ucore中一个用户态进程的执行状态生命周期图（包执行状态，执行状态之间的变换关系，以及产生变换的事件或函数调用）。（字符方式画即可）

进程有下面四种状态：

```
- PROC_UNINIT   : 未初始化状态
- PROC_SLEEPING : 睡眠状态（等待事件）
- PROC_RUNNABLE : 就绪/运行状态
- PROC_ZOMBIE   : 僵尸状态（已退出，等待回收）
```

进程图如下：

```
                     [进程创建]
                         │
                    alloc_proc()
                         │
                         ↓
                 ┌───────────────┐
                 │ PROC_UNINIT   │  (未初始化状态)
                 │ (刚分配PCB)    │
                 └───────────────┘
                         │
                         │ proc_init() (系统初始化)
                         │ wakeup_proc() (fork后唤醒)
                         │
                         ↓
    ┌────────────────────────────────────────────────┐
    │            ┌───────────────┐                   │
    │            │PROC_RUNNABLE  │ (就绪/运行状态)    │
    │            │  (可运行)     │                    │
    │            └───────────────┘                   │
    │                    │  ↑                        │
    │      proc_run()    │  │  proc_run()            │
    │      (调度器选中)   │  │  (切换回来)             │
    │                    ↓  │                        │
    │            ┌───────────────┐                   │
    │            │   RUNNING     │ (运行中-逻辑状态)   │
    │            │   CPU执行中    │                   │
    │            └───────────────┘                   │
    └────────────────────────────────────────────────┘
                         │
            ┌────────────┼────────────┬──────────────┐
            │            │            │              │
      do_yield()    do_wait()    do_sleep()    do_exit()
      时间片到期        等待子进程    主动睡眠       进程退出
            │            │            │              │
            │            ↓            │              │
            │    ┌───────────────┐    │              │
            │    │PROC_SLEEPING  │ ←──┘              │
            │    │  (睡眠状态)    │                   │
            │    └───────────────┘                   │
            │            │                           │
            │    wakeup_proc()                       │
            │    (等待事件发生)                       │
            │            │                           │
            └────────────┴───────────────────────────┤
                         │                           │
                         ↓                           ↓
                 ┌───────────────┐          ┌───────────────┐
                 │PROC_RUNNABLE  │          │ PROC_ZOMBIE   │
                 │  (重新就绪)    │          │ (僵尸状态)    │
                 └───────────────┘          └───────────────┘
                                                    │
                                            父进程 do_wait()
                                            (回收资源)
                                                    │
                                                    ↓
                                            [进程彻底销毁]
                                            unhash_proc()
                                            remove_links()
                                            put_kstack()
                                            kfree(proc)
```



## 拓展练习 Challenge

