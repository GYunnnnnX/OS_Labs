# 操作系统课程ucore Lab5

## 练习0：填写已有实验

已实现。

## 练习1：加载应用程序并执行

### Q1：补充`load_icode`的第6步

**题目要求**：补充`load_icode`的第6步，建立相应的用户内存空间来放置应用程序的代码段、数据段等，且要设置好`proc_struct`结构中的成员变量`trapframe`中的内容，确保在执行此进程后，能够从应用程序设定的起始执行地址开始执行。设置正确的`trapframe`内容。

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

	- `page` 是父进程某虚拟地址对应的物理页描述符
	- `page2kva` 把它转换成 **内核可直接访问的虚拟地址**

	`dst_kvaddr = page2kva(npage);`

	- `npage` 是刚分配给子进程的新物理页
	- 同样转成内核地址，才能写入

	`memcpy(dst_kvaddr, src_kvaddr, PGSIZE);`

	- 复制整页内容（4KB）
	- 这一步保证子进程拿到一份“与父进程一致”的内存快照

	`page_insert(to, npage, start, perm);`

	- 在子进程页表 `to` 中，把子物理页 `npage` 映射到相同的虚拟地址 `start`
	- `perm` 是父页的用户权限位（读写执行 U 等），保持一致



## 练习3：阅读分析源代码，理解进程执行fork/exec/wait/exit的实现，以及系统调用的实现

## 拓展练习 Challenge