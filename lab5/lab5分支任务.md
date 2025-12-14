### 一、初步调试ecall及思考：

我先来说一下我初步调试时候的思路，首先第一个任务是需要我们对QEMU处理ecall指令进行调试，基于此，我先使用`info functions ecall`命令，找到QEMU源码中有关于ecall指令的函数，然后发现了`trans_ecall`这个函数，从字面意义上来看，这是处理ecall指令翻译的函数。

接着我按照之前lab2中调试的经验以及演示视频中的指导，成功停在了ecall指令之前，通过在打下断点`break trans_ecall`，并执行这条ecall指令，我们成功地停在了trans_ecall函数的入口处，这说明我们本次ecall应该是程序开始时执行的第一次ecall指令（通过询问大模型，我了解到第一次执行ecall指令时，由于在翻译缓存块TB中查询不到，故需要一次完整的翻译过程，下次执行相同 PC 范围内的指令时会直接执行缓存的本地代码，就不需要走这个翻译过程了，除非 TB 被刷新）。通过bt指令，可以看到此时的函数调用栈状态：

```powershell
(gdb) bt
#0  trans_ecall (ctx=0x7cccb1f7c760, a=0x7cccb1f7c660)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/insn_trans/trans_privileged.inc.c:24
#1  0x000055ca7f28e1de in decode_insn32 (ctx=0x7cccb1f7c760, 
    insn=115) at target/riscv/decode_insn32.inc.c:1614
#2  0x000055ca7f296826 in decode_opc (ctx=0x7cccb1f7c760)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:746
#3  0x000055ca7f296a35 in riscv_tr_translate_insn (
    dcbase=0x7cccb1f7c760, cpu=0x55ca8fffdfb0)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:800
#4  0x000055ca7f2093b9 in translator_loop (
    ops=0x55ca7faf0380 <riscv_tr_ops>, db=0x7cccb1f7c760, 
    cpu=0x55ca8fffdfb0, 
    tb=0x7cccb1f7e040 <code_gen_buffer+19>, max_insns=1)
    at /home/lin/workspace/qemu-4.1.1/accel/tcg/translator.c:95
#5  0x000055ca7f296bb0 in gen_intermediate_code (
    cs=0x55ca8fffdfb0, tb=0x7cccb1f7e040 <code_gen_buffer+19>, 
    max_insns=1)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:848
#6  0x000055ca7f2077aa in tb_gen_code (cpu=0x55ca8fffdfb0, 
    pc=8388866, cs_base=0, flags=24576, cflags=-16252928)
    at /home/lin/workspace/qemu-4.1.1/accel/tcg/translate-all.c:1738
--Type <RET> for more, q to quit, c to continue without paging--c
#7  0x000055ca7f203f91 in tb_find (cpu=0x55ca8fffdfb0, last_tb=0x0, tb_exit=0, cf_mask=524288) at /home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:409
#8  0x000055ca7f20489a in cpu_exec (cpu=0x55ca8fffdfb0) at /home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:731
#9  0x000055ca7f1b6f16 in tcg_cpu_exec (cpu=0x55ca8fffdfb0) at /home/lin/workspace/qemu-4.1.1/cpus.c:1435
#10 0x000055ca7f1b77cf in qemu_tcg_cpu_thread_fn (arg=0x55ca8fffdfb0) at /home/lin/workspace/qemu-4.1.1/cpus.c:1743
#11 0x000055ca7f63bbd3 in qemu_thread_start (args=0x55ca90014690) at util/qemu-thread-posix.c:502
#12 0x00007cccb4894ac3 in start_thread (arg=<optimized out>) at ./nptl/pthread_create.c:442
#13 0x00007cccb49268c0 in clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:81
```

这个过程我们可以概括为：**`硬件线程 -> qemu线程 -> CPU执行循环 -> 翻译块查找 -> 生成代码 -> 翻译指令`** 

通过继续调试，我们发现这个`trans_ecall`函数会继续去调用其他的函数，调用链如下：

```
trans_ecall (生成ecall翻译)
    ↓
generate_exception (生成异常)
    ↓
gen_helper_raise_exception (生成helper调用)
    ↓
tcg_gen_callN (生成TCG调用代码)
    ↓
[翻译完成，生成翻译块]
```

其中：

1. `gen_helper_raise_exception`获取`helper_raise_exception`函数的地址
2. 调用`tcg_gen_callN`生成调用helper的TCG代码

这就是完整的翻译过程，当执行时，会去实际调用翻译过程生成的`helper_raise_exception`这个函数。

然后继续调试，接着继续使用finish指令调试，直到翻译完成，：

```powershell
(gdb) finish
Run till exit from #0  trans_ecall (ctx=0x718b24ffd760, a=0x718b24ffd660)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/insn_trans/trans_privileged.inc.c:24
decode_insn32 (ctx=0x718b24ffd760, insn=115) at target/riscv/decode_insn32.inc.c:1614
1614                        if (trans_ecall(ctx, &u.f_empty)) return true;
Value returned is $1 = true
(gdb) 
Run till exit from #0  decode_insn32 (ctx=0x718b24ffd760, insn=115)
    at target/riscv/decode_insn32.inc.c:1614
decode_opc (ctx=0x718b24ffd760) at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:746
746             if (!decode_insn32(ctx, ctx->opcode)) {
Value returned is $2 = true
(gdb) 
Run till exit from #0  decode_opc (ctx=0x718b24ffd760)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:746
riscv_tr_translate_insn (dcbase=0x718b24ffd760, cpu=0x6169ccc61fb0) at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:801
801         ctx->base.pc_next = ctx->pc_succ_insn;
(gdb) 
Run till exit from #0  riscv_tr_translate_insn (dcbase=0x718b24ffd760, 
    cpu=0x6169ccc61fb0)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:801
translator_loop (ops=0x616990fb4380 <riscv_tr_ops>, db=0x718b24ffd760, cpu=0x6169ccc61fb0, tb=0x718b1e000040 <code_gen_buffer+19>, max_insns=1) at /home/lin/workspace/qemu-4.1.1/accel/tcg/translator.c:99
99              if (db->is_jmp != DISAS_NEXT) {
(gdb) 
Run till exit from #0  translator_loop (ops=0x616990fb4380 <riscv_tr_ops>, 
    db=0x718b24ffd760, cpu=0x6169ccc61fb0, 
    tb=0x718b1e000040 <code_gen_buffer+19>, max_insns=1)
    at /home/lin/workspace/qemu-4.1.1/accel/tcg/translator.c:99
gen_intermediate_code (cs=0x6169ccc61fb0, tb=0x718b1e000040 <code_gen_buffer+19>, max_insns=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:849
849     }
(gdb) 
Run till exit from #0  gen_intermediate_code (cs=0x6169ccc61fb0, 
    tb=0x718b1e000040 <code_gen_buffer+19>, max_insns=1)
    at /home/lin/workspace/qemu-4.1.1/target/riscv/translate.c:849
tb_gen_code (cpu=0x6169ccc61fb0, pc=8388866, cs_base=0, flags=24576, cflags=-16252928) at /home/lin/workspace/qemu-4.1.1/accel/tcg/translate-all.c:1739
1739        tcg_ctx->cpu = NULL;
(gdb) 
Run till exit from #0  tb_gen_code (cpu=0x6169ccc61fb0, pc=8388866, cs_base=0, 
    flags=24576, cflags=-16252928)
    at /home/lin/workspace/qemu-4.1.1/accel/tcg/translate-all.c:1739
0x00006169906c7f91 in tb_find (cpu=0x6169ccc61fb0, last_tb=0x0, tb_exit=0, cf_mask=524288) at /home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:409
warning: Source file is more recent than executable.
409             tb = tb_gen_code(cpu, pc, cs_base, flags, cf_mask);
Value returned is $3 = (TranslationBlock *) 0x718b1e000040 <code_gen_buffer+19>
(gdb) 
Run till exit from #0  0x00006169906c7f91 in tb_find (cpu=0x6169ccc61fb0, 
    last_tb=0x0, tb_exit=0, cf_mask=524288)
    at /home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:409
0x00006169906c889a in cpu_exec (cpu=0x6169ccc61fb0) at /home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:731
731                 tb = tb_find(cpu, last_tb, tb_exit, cflags);// 查找TB
Value returned is $4 = (TranslationBlock *) 0x718b1e000040 <code_gen_buffer+19>
```

我们发现，若干次使用finish调试之后，代码会停在`/home/lin/workspace/qemu-4.1.1/accel/tcg/cpu-exec.c:731`这个位置，

```c++
            tb = tb_find(cpu, last_tb, tb_exit, cflags);// 查找TB
            cpu_loop_exec_tb(cpu, tb, &last_tb, &tb_exit);// 执行TB
```

点进去查看之后，我们知道，731行的这句代码，就是CPU去查询是否存在翻译块TB的过程，如果没有的话，就创建翻译块，也就是我们上面提到的逻辑，最后保存在变量tb中；而732行的这句代码，其以tb作为参数，就是去执行TB，从而触发异常。

**`tb_find()` 的作用：**

1. 根据当前PC查找缓存的翻译块
2. 如果未命中，生成新的翻译块
3. 返回翻译块指针

**`cpu_loop_exec_tb()` 的作用：**

1. 执行翻译块的本地代码
2. 处理执行结果（正常结束、异常、退出等）
3. 更新执行状态

这些代码位于`cpu_exec`这个函数中，通过询问AI，我了解到当执行系统调用ecall时，真正的流程应该如下：

```
1. 用户程序执行到ecall指令
   - PC = 0x800102 (ecall指令地址)
   
2. tb_find()查找这个PC的翻译块
   - 如果之前执行过：返回缓存的TB（包含ecall的翻译）
   - 如果是第一次：生成新的TB，包括ecall的翻译

3. cpu_loop_exec_tb()执行这个TB
   - 执行到ecall时，调用helper_raise_exception(8)

4. helper_raise_exception → riscv_raise_exception
   - 设置：cpu->exception_index = 8
   - 调用：cpu_loop_exit() → longjmp()

5. longjmp()跳回cpu_exec()的sigsetjmp位置

6. 继续循环，cpu_handle_exception()检测到exception_index=8
   - 调用：cpu_do_interrupt() → riscv_cpu_do_interrupt()
```

也就是执行完TB之后，由于此时触发了异常，cpu_handle_exception()函数检测到exception_index=8，从而回去调用cpu_do_interrupt()这个函数，也就是riscv_cpu_do_interrupt()这个函数。

那我们的核心就应该放在**riscv_cpu_do_interrupt()**这个函数，通过为这个函数打下断点，我们可以进入到这个函数内部：

```c++
void riscv_cpu_do_interrupt(CPUState *cs)//1.
{
#if !defined(CONFIG_USER_ONLY)
    RISCVCPU *cpu = RISCV_CPU(cs);
    CPURISCVState *env = &cpu->env;
    
    bool async = !!(cs->exception_index & RISCV_EXCP_INT_FLAG);
    target_ulong cause = cs->exception_index & RISCV_EXCP_INT_MASK;
    target_ulong deleg = async ? env->mideleg : env->medeleg;
    target_ulong tval = 0;

    static const int ecall_cause_map[] = {
        [PRV_U] = RISCV_EXCP_U_ECALL,
        [PRV_S] = RISCV_EXCP_S_ECALL,
        [PRV_H] = RISCV_EXCP_H_ECALL,
        [PRV_M] = RISCV_EXCP_M_ECALL
    };

    if (!async) {
        /* set tval to badaddr for traps with address information */
        switch (cause) {
        case RISCV_EXCP_INST_ADDR_MIS:
        case RISCV_EXCP_INST_ACCESS_FAULT:
        case RISCV_EXCP_LOAD_ADDR_MIS:
        case RISCV_EXCP_STORE_AMO_ADDR_MIS:
        case RISCV_EXCP_LOAD_ACCESS_FAULT:
        case RISCV_EXCP_STORE_AMO_ACCESS_FAULT:
        case RISCV_EXCP_INST_PAGE_FAULT:
        case RISCV_EXCP_LOAD_PAGE_FAULT:
        case RISCV_EXCP_STORE_PAGE_FAULT:
            tval = env->badaddr;
            break;
        default:
            break;
        }
        /* ecall is dispatched as one cause so translate based on mode */
        if (cause == RISCV_EXCP_U_ECALL) {
            assert(env->priv <= 3);
            cause = ecall_cause_map[env->priv];
        }
    }

    trace_riscv_trap(env->mhartid, async, cause, env->pc, tval, cause < 16 ?
        (async ? riscv_intr_names : riscv_excp_names)[cause] : "(unknown)");

    if (env->priv <= PRV_S &&
            cause < TARGET_LONG_BITS && ((deleg >> cause) & 1)) {
        /* handle the trap in S-mode */
        target_ulong s = env->mstatus;
        s = set_field(s, MSTATUS_SPIE, env->priv_ver >= PRIV_VERSION_1_10_0 ?
            get_field(s, MSTATUS_SIE) : get_field(s, MSTATUS_UIE << env->priv));
        s = set_field(s, MSTATUS_SPP, env->priv);
        s = set_field(s, MSTATUS_SIE, 0);
        env->mstatus = s;
        env->scause = cause | ((target_ulong)async << (TARGET_LONG_BITS - 1));
        env->sepc = env->pc;
        env->sbadaddr = tval;
        env->pc = (env->stvec >> 2 << 2) +
            ((async && (env->stvec & 3) == 1) ? cause * 4 : 0);
        riscv_cpu_set_mode(env, PRV_S);
    } else {
        /* handle the trap in M-mode */
        target_ulong s = env->mstatus;
        s = set_field(s, MSTATUS_MPIE, env->priv_ver >= PRIV_VERSION_1_10_0 ?
            get_field(s, MSTATUS_MIE) : get_field(s, MSTATUS_UIE << env->priv));
        s = set_field(s, MSTATUS_MPP, env->priv);
        s = set_field(s, MSTATUS_MIE, 0);
        env->mstatus = s;
        env->mcause = cause | ~(((target_ulong)-1) >> async);
        env->mepc = env->pc;
        env->mbadaddr = tval;
        env->pc = (env->mtvec >> 2 << 2) +
            ((async && (env->mtvec & 3) == 1) ? cause * 4 : 0);
        riscv_cpu_set_mode(env, PRV_M);
    }

#endif
    cs->exception_index = EXCP_NONE; /* mark handled to qemu */
}
```

通过查看函数内容，我们发现这就是我们要找的核心函数，`riscv_cpu_do_interrupt()` 是RISC-V架构在qemu中的**中断/异常处理核心函数**，它负责处理所有RISC-V架构定义的中断和异常。接下来我们对这个函数进行具体分析：

#### 1、函数声明和条件编译

```c
void riscv_cpu_do_interrupt(CPUState *cs)
{
#if !defined(CONFIG_USER_ONLY)
```

**解析**：

- 函数接收一个`CPUState *`参数，表示当前CPU的状态
- `#if !defined(CONFIG_USER_ONLY)`：这是一个条件编译，意味着这个函数只在**系统模式**下编译，在用户模式下不包含这段代码，因为用户模式下不需要处理特权级切换。

#### 2、获取CPU和状态指针

```c++
    RISCVCPU *cpu = RISCV_CPU(cs);
    CPURISCVState *env = &cpu->env;
```

**解析**：
- `RISCVCPU *cpu = RISCV_CPU(cs)`：将通用的`CPUState`指针转换为RISC-V特定的`RISCVCPU`结构体指针
- `CPURISCVState *env = &cpu->env`：获取RISC-V CPU的详细状态结构体
- `env`包含了所有RISC-V架构寄存器：`gpr`、`fpr`、`pc`、`mstatus`、`stvec`等

#### 3、解析异常信息

```c++
    bool async = !!(cs->exception_index & RISCV_EXCP_INT_FLAG);
    target_ulong cause = cs->exception_index & RISCV_EXCP_INT_MASK;
    target_ulong deleg = async ? env->mideleg : env->medeleg;
    target_ulong tval = 0;
```

**解析：**

1. **`bool async = !!(cs->exception_index & RISCV_EXCP_INT_FLAG);`**
   - `RISCV_EXCP_INT_FLAG`：中断标志位（通常是最高位）
   - `!!`：双重逻辑非，确保结果为布尔值（0或1）
   - **作用**：判断是**中断**(async=1)还是**异常**(async=0) 
   - `ecall`是同步异常，所以async=0
2. **`target_ulong cause = cs->exception_index & RISCV_EXCP_INT_MASK;`**
   - `RISCV_EXCP_INT_MASK`：掩码，去掉中断标志位
   - **作用**：提取纯异常原因码
   - 对于ecall：`cs->exception_index = 8`，所以`cause = 8`
3. **`target_ulong deleg = async ? env->mideleg : env->medeleg;`**
   - 三目运算符：如果是中断，使用`mideleg`（机器中断委托）；如果是异常，使用`medeleg`（机器异常委托）
   - **作用**：选择正确的委托寄存器
   - 对于`ecall`异常：`deleg = env->medeleg`
4. **`target_ulong tval = 0;`**
   - 初始化`tval`为0，用于存储某些异常的附加信息（比如错误地址等）

#### 4、ECALL异常映射表

```c++
    static const int ecall_cause_map[] = {
        [PRV_U] = RISCV_EXCP_U_ECALL,
        [PRV_S] = RISCV_EXCP_S_ECALL,
        [PRV_H] = RISCV_EXCP_H_ECALL,
        [PRV_M] = RISCV_EXCP_M_ECALL
    };
```

**解析**：

- 数组下标是特权级，值是异常原因码
- **映射关系**：
  - PRV_U(0) → RISCV_EXCP_U_ECALL(8) 用户态ecall
  - PRV_S(1) → RISCV_EXCP_S_ECALL(9)  监管态ecall  
  - PRV_H(2) → RISCV_EXCP_H_ECALL(10) 虚拟监管态ecall
  - PRV_M(3) → RISCV_EXCP_M_ECALL(11) 机器态ecall

#### 5、同步异常处理

```c++
    if (!async) {
        /* set tval to badaddr for traps with address information */
        switch (cause) {
        case RISCV_EXCP_INST_ADDR_MIS:
        case RISCV_EXCP_INST_ACCESS_FAULT:
        case RISCV_EXCP_LOAD_ADDR_MIS:
        case RISCV_EXCP_STORE_AMO_ADDR_MIS:
        case RISCV_EXCP_LOAD_ACCESS_FAULT:
        case RISCV_EXCP_STORE_AMO_ACCESS_FAULT:
        case RISCV_EXCP_INST_PAGE_FAULT:
        case RISCV_EXCP_LOAD_PAGE_FAULT:
        case RISCV_EXCP_STORE_PAGE_FAULT:
            tval = env->badaddr;
            break;
        default:
            break;
        }
```

**解析**：

- `if (!async)`：只处理同步异常（ecall、页错误等）
- `switch (cause)`：根据异常类型设置tval
- 只有**地址相关异常**才需要设置tval为错误的地址
- ecall不在这个列表中，所以`tval`保持为0

#### 6、ECALL特权级映射

```c++
        /* ecall is dispatched as one cause so translate based on mode */
        if (cause == RISCV_EXCP_U_ECALL) {
            assert(env->priv <= 3);
            cause = ecall_cause_map[env->priv];
        }
    }
```

**解析**：所有特权级的ecall都使用相同的异常码`RISCV_EXCP_U_ECALL`，需要根据当前特权级进行映射

1. **`if (cause == RISCV_EXCP_U_ECALL)`**
   - **条件**：如果异常原因是用户ecall（值为8）

2. **`assert(env->priv <= 3);`**
   - **断言**：当前特权级必须≤3（0-3是有效特权级）
   - 如果失败，程序会终止并报错

3. **`cause = ecall_cause_map[env->priv];`**
   - **映射**：根据当前特权级，从映射表中获取正确的异常原因
   - 例如：如果`env->priv = 0`（用户态），则`cause = ecall_cause_map[0] = 8`
   - 在调试中我们观察到：`env->priv = 0`，`cause`从8映射后还是8

#### 7、跟踪日志输出

```c++
    trace_riscv_trap(env->mhartid, async, cause, env->pc, tval, cause < 16 ?
        (async ? riscv_intr_names : riscv_excp_names)[cause] : "(unknown)");
```

**解析**：
- `trace_riscv_trap`：qemu的跟踪函数，用于调试输出
- **参数**：
  1. `env->mhartid`：硬件线程ID
  2. `async`：是否为中断
  3. `cause`：异常原因码
  4. `env->pc`：异常发生时的PC
  5. `tval`：异常附加信息
  6. 异常名称字符串：根据cause选择中断名或异常名

#### 8、异常委托判断

```c++
    if (env->priv <= PRV_S &&
            cause < TARGET_LONG_BITS && ((deleg >> cause) & 1)) {
```

**解析**：

1. **`env->priv <= PRV_S`**
   - **条件**：当前特权级≤S-mode（0或1）
   - 用户态(0)或监管态(1)的异常可能被委托

2. **`cause < TARGET_LONG_BITS`**
   - **条件**：异常原因码有效（小于目标架构的长整型位数）
   - 通常`TARGET_LONG_BITS`是32或64

3. **`((deleg >> cause) & 1)`**
   - **位操作**：将委托寄存器右移cause位，检查最低位是否为1
   - **含义**：检查该异常是否被委托给低特权级处理
   - 例如：如果`medeleg`的第8位为1，则用户ecall被委托给S-mode处理

**整体逻辑**：如果当前是低特权级、异常码有效、且该异常被委托，则在S-mode处理；否则在M-mode处理。

##### 8.1、S-mode异常处理（用户ecall的路径）

```c++
        /* handle the trap in S-mode */
        target_ulong s = env->mstatus;
        s = set_field(s, MSTATUS_SPIE, env->priv_ver >= PRIV_VERSION_1_10_0 ?
            get_field(s, MSTATUS_SIE) : get_field(s, MSTATUS_UIE << env->priv));
```

**解析**：

1. **`target_ulong s = env->mstatus;`**
   - 复制当前mstatus寄存器值到局部变量s

2. **设置`MSTATUS_SPIE`（S-mode前一个中断使能）**
   - `env->priv_ver >= PRIV_VERSION_1_10_0`：检查特权级架构版本
   - **新版本**：`get_field(s, MSTATUS_SIE)` 获取当前S-mode中断使能
   - **旧版本**：`get_field(s, MSTATUS_UIE << env->priv)` 根据特权级计算
   - **作用**：保存当前的中断使能状态到SPIE，以便异常返回时恢复

```c++
        s = set_field(s, MSTATUS_SPP, env->priv);
        s = set_field(s, MSTATUS_SIE, 0);
        env->mstatus = s;
```

3. **`s = set_field(s, MSTATUS_SPP, env->priv);`**
   - 设置SPP字段为**当前特权级**
   - **作用**：记录异常发生前的特权级，sret返回时需要

4. **`s = set_field(s, MSTATUS_SIE, 0);`**
   - 禁用S-mode中断
   - **安全考虑**：进入异常处理后，默认禁用中断

5. **`env->mstatus = s;`**
   - 将修改后的值写回mstatus寄存器

```c++
        env->scause = cause | ((target_ulong)async << (TARGET_LONG_BITS - 1));
        env->sepc = env->pc;
        env->sbadaddr = tval;
```

6. **`env->scause = cause | ((target_ulong)async << (TARGET_LONG_BITS - 1));`**
   - **组成**：异常原因码 + 中断标志位（最高位）
   - **作用**：设置S-mode异常原因寄存器
   - 对于ecall：`async=0`，所以最高位为0，`scause = cause = 8`

7. **`env->sepc = env->pc;`**
   - **关键操作**：保存异常发生时的PC到sepc
   - **作用**：sret指令会从sepc恢复PC
   - 在调试中：`env->pc = 0x800102`，所以`env->sepc = 0x800102`

8. **`env->sbadaddr = tval;`**
   - 设置S-mode badaddr寄存器
   - 对于ecall：`tval = 0`

```c++
        env->pc = (env->stvec >> 2 << 2) +
            ((async && (env->stvec & 3) == 1) ? cause * 4 : 0);
        riscv_cpu_set_mode(env, PRV_S);
```

9. **设置新的PC到异常处理程序**
   - `(env->stvec >> 2 << 2)`：stvec的低2位清零，得到基地址
   - `stvec[1:0]`：模式位
     - `00`：直接模式，所有异常跳到同一地址
     - `01`：向量模式，不同异常跳到不同偏移
   - 对于ecall（async=0）：`env->pc = stvec基地址 + 0`
   - 在调试中：`env->stvec = 0xffffffffc0200de4`，所以`env->pc = 0xffffffffc0200de4`

10. **`riscv_cpu_set_mode(env, PRV_S);`**
    - **关键操作**：切换CPU特权级到S-mode
    - **作用**：从用户态(U-mode)切换到监管态(S-mode)
    - 在调试中：`env->priv`从0变为1

##### 8.2、M-mode异常处理（备选路径）

```c++
    } else {
        /* handle the trap in M-mode */
        target_ulong s = env->mstatus;
        s = set_field(s, MSTATUS_MPIE, env->priv_ver >= PRIV_VERSION_1_10_0 ?
            get_field(s, MSTATUS_MIE) : get_field(s, MSTATUS_UIE << env->priv));
        s = set_field(s, MSTATUS_MPP, env->priv);
        s = set_field(s, MSTATUS_MIE, 0);
        env->mstatus = s;
        env->mcause = cause | ~(((target_ulong)-1) >> async);
        env->mepc = env->pc;
        env->mbadaddr = tval;
        env->pc = (env->mtvec >> 2 << 2) +
            ((async && (env->mtvec & 3) == 1) ? cause * 4 : 0);
        riscv_cpu_set_mode(env, PRV_M);
    }
```

**解析**：
- 与S-mode处理类似，但使用M-mode的寄存器：
  - `mstatus` → MPP, MPIE, MIE位
  - `mcause`代替`scause`
  - `mepc`代替`sepc`
  - `mbadaddr`代替`sbadaddr`
  - `mtvec`代替`stvec`
- 切换到M-mode（特权级3）

#### 9、异常处理完成标记

```c++
#endif
    cs->exception_index = EXCP_NONE; /* mark handled to qemu */
}
```

**解析**：

1. **`#endif`**：结束条件编译块
2. **`cs->exception_index = EXCP_NONE;`**
   - **关键操作**：将异常索引设置为`EXCP_NONE`
   - **作用**：标记异常已处理，告诉qemu主循环可以继续执行
   - 在调试中我们看到：`cs->exception_index`从8变为-1

综上所述，我们归纳出QEMU处理ecall指令的整体流程应该如下：

```
用户程序PC=0x800102 (ecall)
    ↓
tb_find(0x800102)  // 查找或生成TB
    ↓ 如果第一次，调用gen_intermediate_code()
        ↓ translator_loop()
            ↓ riscv_tr_translate_insn()
                ↓ decode_insn32(insn=0x73)  // 识别ecall opcode
                    ↓ trans_ecall()  // ← 在这里识别并翻译！
                        ↓ generate_exception(RISCV_EXCP_U_ECALL)
                            ↓ 生成调用helper_raise_exception的TCG代码
    ↓ 返回TB（包含ecall的翻译）
    ↓
cpu_loop_exec_tb(TB)  // 执行TB
    ↓ 执行到ecall对应的TCG代码
        ↓ 调用helper_raise_exception(8)  // ← 在这里触发异常
            ↓ riscv_raise_exception()
                ↓ 设置cpu->exception_index = 8
                ↓ cpu_loop_exit() → longjmp()
    
回到cpu_exec()的sigsetjmp
    ↓
cpu_handle_exception()检测到exception_index=8
    ↓ 调用riscv_cpu_do_interrupt()
```

### 二、ecall调试演示：

接下来我们进行ecall指令的调试演示，我们主要来看ecall执行前后的一些关键寄存器值的变化：

首先同样的，在终端一、二、三分别运行如下进入到下一条指令就是ecall的状态:

```
终端一：
make debug

终端二：
pgrep -f qemu-system-riscv64
sudo gdb
attach 进程ID
handle SIGPIPE nostop noprint
c

终端三：
终端3：
make gdb
set remotetimeout unlimited
add-symbol-file obj/__user_exit.out
break /home/lin/workspace/OS_Labs/lab5/user/libs/syscall.c:18
c
d
si单步执行到ecall指令之前
```

此时，我们在终端二中打下断点：`break riscv_cpu_do_interrupt`并continue继续执行。

进入riscv_cpu_do_interrupt的入口处，此时再次打下断点`break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:509`，便于我们查看ecall进入时的cpu状态：

```powershell
(gdb) printf "【ECALL进入】priv=%d PC=0x%lx exc=0x%x(%d) stvec=0x%lx mstatus=0x%lx\n", 
       env->priv, env->pc, cs->exception_index, cs->exception_index, env->stvec, env->mstatus
【ECALL进入】priv=0 PC=0x800102 exc=0x8(8) stvec=0xffffffffc0200de4 mstatus=0x8000000000046002
```

**状态解析：**

1. **priv=0**：CPU处于用户态（U-mode）
2. **PC=0x800102**：正在执行ecall指令（用户程序地址空间）
3. **exc=0x8**：异常码8 = `RISCV_EXCP_U_ECALL`（用户ecall）
4. **stvec=0xffffffffc0200de4**：S-mode异常向量基地址（内核中断入口）
5. **mstatus=0x8000000000046002**：机器状态寄存器，此时SIE为1，SPIE为0，SPP为0

再次打下断点break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:548查看cause寄存器ecall映射之后的状态：

```powershell
(gdb) p/x cause
$2 = 0x8
```

1. **cause=0x8**：经过ecall映射后，cause保持为8
2. **映射过程**：由于`env->priv=0`，`ecall_cause_map[0] = 8`
3. **说明**：用户态ecall不需要改变异常码，直接使用`RISCV_EXCP_U_ECALL`

打下断点break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:588：

```powershell
(gdb) printf "【状态保存】mstatus=0x%lx(SPP=%ld SPIE=%ld SIE=%ld) scause=0x%lx sepc=0x%lx sbadaddr=0x%lx newPC=0x%lx newPriv=%d\n",
       env->mstatus,
       (env->mstatus >> 8) & 1,  
       (env->mstatus >> 5) & 1, 
       (env->mstatus >> 1) & 1,   
       env->scause,
       env->sepc,
       env->sbadaddr,
       env->pc,
       env->priv
【状态保存】mstatus=0x8000000000046020(SPP=0 SPIE=1 SIE=0) scause=0x8 sepc=0x800102 sbadaddr=0x0 newPC=0xffffffffc0200de4 newPriv=1
```

**1. mstatus寄存器变化**

- **原始**：`0x8000000000046002`
- **新值**：`0x8000000000046020`

实际位值：

- Bit 1 (SIE)：保持0（异常处理期间禁用中断）
- Bit 5 (SPIE)：保持1（保存了进入前的SIE状态）
- Bit 8 (SPP)：保持0（记录异常前为U-mode，用于返回）

**2. 异常相关寄存器设置**

- **scause=0x8**：设置异常原因为用户ecall（最高位0表示异常）
- **sepc=0x800102**：保存异常发生时的PC（ecall指令地址，用于返回）
- **sbadaddr=0x0**：ecall没有错误地址，设为0

**3. 控制流转移**

- **newPC=0xffffffffc0200de4**：PC跳转到stvec地址（内核中断处理程序）
- **newPriv=1**：特权级从0（U-mode）切换到1（S-mode）

最后打下断点break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:591

```powershell
(gdb) p cs->exception_index
$3 = -1
```

**状态解析：**

- **exception_index = -1**：对应`EXCP_NONE`
- **作用**：标记异常已处理完成，qemu主循环可以继续执行

接下来内核跳转到异常处理代码（0xffffffffc0200de4）并根据系统调用号调用相应的异常处理函数。

### 三、初步调试sret及思考：

同样，使用`info functions sret`命令，查找有关于sret指令的处理函数：

```powershell
(gdb) info functions sret
All functions matching regular expression "sret":

File /home/lin/workspace/qemu-4.1.1/target/riscv/helper.h:
74:     static void gen_helper_sret(TCGv_i64, TCGv_ptr, TCGv_i64);

File /home/lin/workspace/qemu-4.1.1/target/riscv/insn_trans/trans_privileged.inc.c:
43:     static _Bool trans_sret(DisasContext *, arg_sret *);

File /home/lin/workspace/qemu-4.1.1/target/riscv/op_helper.c:
74:     target_ulong helper_sret(CPURISCVState *, target_ulong);
```

同样的，sret有其翻译函数trans_sret和qemu中的sret执行函数gen_helper_sret，负责产生并运行sret指令，这里多出来了一个helper_sret函数，我们怀疑这个就是qemu中处理sret的核心函数。

打下断点break trans_sret，并使用bt查看调用栈，可以得到和ecall几乎类似的调用栈，然后若干次finish执行之后也进入cpu_exec这个函数，所以基本的逻辑都是一样的，**翻译+执行->处理**。

打下断点break helper_sret，我们进入helper_sret函数：

```c++
target_ulong helper_sret(CPURISCVState *env, target_ulong cpu_pc_deb)
{
    if (!(env->priv >= PRV_S)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }

    target_ulong retpc = env->sepc;
    if (!riscv_has_ext(env, RVC) && (retpc & 0x3)) {
        riscv_raise_exception(env, RISCV_EXCP_INST_ADDR_MIS, GETPC());
    }

    if (env->priv_ver >= PRIV_VERSION_1_10_0 &&
        get_field(env->mstatus, MSTATUS_TSR)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }

    target_ulong mstatus = env->mstatus;
    target_ulong prev_priv = get_field(mstatus, MSTATUS_SPP);
    mstatus = set_field(mstatus,
        env->priv_ver >= PRIV_VERSION_1_10_0 ?
        MSTATUS_SIE : MSTATUS_UIE << prev_priv,
        get_field(mstatus, MSTATUS_SPIE));
    mstatus = set_field(mstatus, MSTATUS_SPIE, 0);
    mstatus = set_field(mstatus, MSTATUS_SPP, PRV_U);
    riscv_cpu_set_mode(env, prev_priv);
    env->mstatus = mstatus;

    return retpc;
}
```

可以看到，这个`helper_sret`函数模拟了sret指令的硬件行为，完成状态恢复和特权级切换。具体分析如下：

#### 1、特权级检查

```c++
    if (!(env->priv >= PRV_S)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
```

**作用**：确保当前在S-mode或M-mode执行sret。
**原理**：只有S-mode及以上特权级才能执行sret，用户态(U-mode)执行sret会触发非法指令异常。

#### 2、地址对齐检查

```c++
    target_ulong retpc = env->sepc;
    if (!riscv_has_ext(env, RVC) && (retpc & 0x3)) {
        riscv_raise_exception(env, RISCV_EXCP_INST_ADDR_MIS, GETPC());
    }
```

**作用**：检查返回地址的对齐。
**原理**：

- 获取保存在`sepc`中的返回地址
- 如果没有C扩展（压缩指令2字节），地址必须是4字节对齐（低2位为0）
- 不对齐会触发指令地址不对齐异常

#### 3、 TSR（Trap SRET）检查

```c++
    if (env->priv_ver >= PRIV_VERSION_1_10_0 &&
        get_field(env->mstatus, MSTATUS_TSR)) {
        riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
    }
```

**作用**：检查是否允许执行sret。
**原理**：

- 特权级架构版本≥1.10时有效
- `MSTATUS_TSR`位为1时，sret会触发非法指令异常（当 `TSR = 1` 时，**禁止在S-mode下执行sret指令**）

#### **4、核心状态恢复块**

```c++
    target_ulong mstatus = env->mstatus;//1.
    target_ulong prev_priv = get_field(mstatus, MSTATUS_SPP);
    mstatus = set_field(mstatus,//2
        env->priv_ver >= PRIV_VERSION_1_10_0 ?
        MSTATUS_SIE : MSTATUS_UIE << prev_priv,
        get_field(mstatus, MSTATUS_SPIE));
    mstatus = set_field(mstatus, MSTATUS_SPIE, 0);//3
    mstatus = set_field(mstatus, MSTATUS_SPP, PRV_U);//4
    riscv_cpu_set_mode(env, prev_priv);//5
    env->mstatus = mstatus;//6
```

1.首先将当前的 `env->mstatus` 复制到局部变量 `mstatus` 中以便修改，同时从 `MSTATUS_SPP` 字段中读取异常发生前 CPU 所处的特权级并保存至 `prev_priv`（该值是在执行 `ecall` 异常陷入时由 `riscv_cpu_do_interrupt` 保存的）。

2.然后恢复中断使能位SIE，根据 RISC-V 特权级架构版本有两种策略选择策略：

- **版本≥1.10**：将 `SIE` 位设置为 `SPIE` 的值（即恢复进入异常前 S-mode 的中断使能状态）
- **旧版本**：`UIE<<prev_priv = SPIE`（将 `UIE` 左移 `prev_priv` 位后对应的位置设置为 `SPIE` 的值（根据不同特权级恢复用户中断使能）
- 其中 `SPIE` 是在进入异常处理时由硬件自动保存的原 `SIE`（或对应特权级的中断使能）状态。

3.接着需要更新SPIE位，将 `MSTATUS_SPIE` 位清零，表示本次异常处理过程中保存的旧中断使能状态已恢复完毕，为下一次异常处理做准备。

4.将 `MSTATUS_SPP` 位设置为 `PRV_U`（用户态），表示当前异常处理已完成特权级切换，下一次若再发生异常，应默认记录为来自 U-mode。

5.调用 `riscv_cpu_set_mode(env, prev_priv)` 函数，将 CPU 当前特权级从 S-mode 切换回 `prev_priv`（即异常发生前的特权级，如 U-mode），完成从内核态返回用户态的实际切换。

6.将修改后的 `mstatus` 局部变量值写回 `env->mstatus` 寄存器，确保所有状态位的更新（如 SIE、SPIE、SPP）在硬件层面生效。

#### 5、返回值

```c++
    return retpc;
```

**作用**：返回要跳转的地址。
**原理**：

- 返回`sepc`的值
- 调用者（TCG生成的代码）需要将`env->pc`设置为这个返回值，从而完成跳转
- 实现了PC的恢复：`pc = sepc`

### 四、sret调试演示：

基于上面的分析，我们进行最后的sret的调试，为了验证调试的正确性，我们选取上次演示的ecall之后对应的sret指令。

```
终端一：
make debug

终端二：
pgrep -f qemu-system-riscv64
sudo gdb
attach 进程ID
handle SIGPIPE nostop noprint
c

终端三：
终端3：
make gdb
set remotetimeout unlimited
add-symbol-file obj/__user_exit.out
break /home/lin/workspace/OS_Labs/lab5/user/libs/syscall.c:18
c
break /home/lin/workspace/OS_Labs/lab5/kern/trap/trapentry.S:133
c
```

此时就找到了我们之前演示的ecall所对应的那条sret指令。

此时，我们在终端二中打下断点：`break helper_sret`并continue继续执行。进入`helper_sret`函数的入口处，在此我们可以查看一些重要寄存器的信息：

```powershell
(gdb) printf "SRET入口: priv=%d sepc=0x%lx mstatus=0x%lx SPP=%ld SPIE=%ld SIE=%ld\n", 
env->priv, 
env->sepc, 
env->mstatus, 
(env->mstatus >> 8) & 1,
(env->mstatus >> 5) & 1, 
(env->mstatus >> 1) & 1
SRET入口: priv=1 sepc=0x800106 mstatus=0x8000000000046020 SPP=0 SPIE=1 SIE=0
```

可以看到，特权级为1，说明此时处于内核态，因为ecall指令还没回到用户态；epc寄存器从ecall调试时的0x800102变成了0x800106，指向ecall的下一条指令；SPP、SPIE、SIE这三位对应ecall处理完的状态，完全一致。

此时再次打下断点`break /home/lin/workspace/qemu-4.1.1/target/riscv/op_helper.c:100`，便于我们查看这个函数处理完`sret`指令之后的相关寄存器的信息：

```powershell
printf "【状态恢复】mstatus=0x%lx(SPP=%ld SPIE=%ld SIE=%ld) sepc=0x%lx newPC=0x%lx newPriv=%d\n",
           env->mstatus,
           (env->mstatus >> 8) & 1,  
           (env->mstatus >> 5) & 1, 
           (env->mstatus >> 1) & 1,   
           env->sepc,
           retpc,
           env->priv
【状态恢复】mstatus=0x8000000000046002(SPP=0 SPIE=0 SIE=1) sepc=0x800106 newPC=0x800106 newPriv=0
```

可以看到，在执行完对`sret`的处理之后，特权级通过`prev_priv = SPP = 0`由1变成了0，表示切换到用户态；中断状态SIE通过SPIE从0恢复为1，SPIE设为0，SPP位被设置为PRV_U(0)；返回的pc值`retpc = sepc = 0x800106`，说明`sret`后执行`ecall`的下一条指令。

综上，我们就完成了对`sret`指令的调试。

