### 一、调试环境搭建

首先按照指导书的要求，重新编译一个带调试信息的QEMU版本：

```apl
# 进入QEMU源码目录
cd qemu-4.1.1

# 清理之前的编译结果
make distclean

# 重新配置，启用调试选项
./configure --target-list=riscv32-softmmu,riscv64-softmmu --enable-debug

# 重新编译
make -j$(nproc)
```

**关键注意点**：编译完成后**不执行**`sudo make install`命令，避免覆盖系统中已安装的标准版QEMU。编译生成的调试版QEMU可执行文件位于`riscv64-softmmu/qemu-system-riscv64`。

同时，要修改`ucore`项目中的`Makefile`，使其使用我们新编译的调试版QEMU：

```makefile
ifndef QEMU
#QEMU := qemu-system-riscv64
QEMU := /home/lin/workspace/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64
endif
```

### 二、双重GDB调试具体过程（粗略版）

实验采用三个终端窗口并行工作，形成完整的调试链条：

- **终端1**：运行调试版QEMU，显示虚拟机控制台输出
- **终端2**：使用标准GDB调试QEMU进程本身
- **终端3**：使用交叉编译工具链的GDB调试运行在QEMU中的ucore内核

#### 终端一：启动调试环境

首先，终端一运行：

```apl
make debug
```

这个命令会启动QEMU并立即暂停，在终端1中可以看到虚拟机控制台的初始化输出，但内核尚未开始执行。

#### 终端二：附加到QEMU进程

首先需要获取QEMU进程的PID：

```apl
# 查找qemu-system-riscv64进程的PID
pgrep -f qemu-system-riscv64
```

在终端二中运行：

```apl
# 启动gdb并附加到进程
sudo gdb
(gdb) attach <PID>
(gdb) handle SIGPIPE nostop noprint		# 忽略SIGPIPE信号，避免调试干扰
(gdb) c  # 继续执行进程
```

#### 终端三：调试ucore内核

在终端三中连接QEMU内建的GDB stub，开始调试ucore内核：

```apl
make gdb
set remotetimeout unlimited   # 设置无限超时，避免连接断开
break kern_init
c
```

此时，我们完成了三个终端的初始化配置，此时ucore内核停止在了kern_init内核初始化函数的入口，接下来我们开始真正的调试。

首先运行`x/10i $pc`命令可以先查看一下接下来的指令序列：

```apl
(gdb) x/10i $pc
=> 0xffffffffc02000d6 <kern_init>:		auipc       a0,0x6
   0xffffffffc02000da <kern_init+4>:	addi        a0,a0,-190
   0xffffffffc02000de <kern_init+8>:	auipc       a2,0x6
   0xffffffffc02000e2 <kern_init+12>:	addi        a2,a2,-102
   0xffffffffc02000e6 <kern_init+16>:	addi        sp,sp,-16
   0xffffffffc02000e8 <kern_init+18>:	sub a2,a2,a0
   0xffffffffc02000ea <kern_init+20>:   li      a1,0
   0xffffffffc02000ec <kern_init+22>:	sd  ra,8(sp)
   0xffffffffc02000ee <kern_init+24>:	jal 0xffffffffc0201704 <memset>
   0xffffffffc02000f2 <kern_init+28>:	jal 0xffffffffc0200228 <dtb_init>
```

通过7次单步执行(`si`)到达sd指令前：

```apl
(gdb) x/i $pc
=> 0xffffffffc02000ec <kern_init+22>:
    sd  ra,8(sp)
```

可以看到下一条指令正是我们要跟踪的`sd`指令，该指令将保存返回地址到栈中。

此时我们回到终端二，ctrl+c中断`qemu`的执行，并打入关键断点`break get_physical_address`，然后continue恢复执行。

```apl
(gdb) break get_physical_address 
Breakpoint 1 at 0x644295f54bb5: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 158.
(gdb) c
```

然后回到终端三，si单步执行我们发现的这条`sd`指令。此时终端二中的GDB会在`get_physical_address`函数处暂停，但观察发现第一次命中的地址并非预期：此时执行`p/x addr`查看却对应的是`0xffffffffc02000d6`这个地址，对应的是kern_init的入口地址。

```apl
Thread 1 "qemu-system-ris" hit Breakpoint 1, get_physical_address (env=0x60f07eb8b9c0, physical=0x7ffe42962838, prot=0x7ffe42962830, addr=18446744072637907158, access_type=0, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:158
158     {
(gdb) p/x addr
$1 = 0xffffffffc02000d6
(gdb) c
```

而需要再次执行一次continue，下一次断点命中时，再次`p/x addr`查看，正确命中到了sd指令对应的内存访问地址。

```apl
Thread 3 "qemu-system-ris" hit Breakpoint 1, get_physical_address (env=0x60f07eb8b9c0, physical=0x79ef0cf06220, prot=0x79ef0cf06214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:158
158     {
(gdb) p/x addr
$2 = 0xffffffffc0204ff8
(gdb) c
```

我怀疑第一次命中对应地址是`0xffffffffc02000d6`这个地址，是由于取指翻译造成的，但是我们在`sd`这条指令之前已经执行了很多条指令，按理来说这页虚拟地址到物理地址的映射已经被存入了TLB中，查了一下可能是因为 GDB 的断点干扰（写内存可能导致 QEMU 刷新TLB），导致 `I-TLB` 失效，被迫重新翻译。询问大模型后，具体情况如下：

```
// GDB设置软件断点的原理：
// 1. GDB将目标地址的指令替换为ebreak（trap指令）
// 2. 这修改了内存内容，QEMU的TB（翻译块）系统会检测到内存修改
// 3. QEMU刷新涉及该内存区域的翻译块和TLB
// 4. 下次执行时需要重新翻译指令，触发I-TLB未命中
```

为了第一次直接就能看到0xffffffffc0204ff8，可以在执行`sd`这条指令之前，把终端3中的断点删掉。

```apl
(gdb) p/x addr
$1 = 0xffffffffc0204ff8
```

这样，第一次命中的就是我们`sd`指令访问内存的那个地址。

```apl
(gdb) watch *physical
Hardware watchpoint 2: *physical
(gdb) break tlb_set_page
Breakpoint 3 at 0x644295e9ba04: file /home/lin/workspace/qemu-4.1.1/accel/tcg/cputlb.c, line 847.
```

在正确命中`sd`指令的内存访问后，可以打下`watch *physical`停在下一次physical指针指向的地址变化的时刻，进一步设置监控点跟踪物理地址变化，打下`break tlb_set_page`查看填入`TLB`的时刻。

```apl
Thread 3 "qemu-system-ris" hit Hardware watchpoint 2: *physical

Old value = 0
New value = 2149597184
get_physical_address (env=0x6442d512b9c0, physical=0x7d5a69ffe220, prot=0x7d5a69ffe214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:337
337                 if ((pte & PTE_R) || ((pte & PTE_X) && mxr)) {
(gdb) p/x *physical
$2 = 0x80204000
(gdb)  p/x addr
$3 = 0xffffffffc0204ff8
(gdb)  p/x ((*physical) + (addr & 0xfff))
$4 = 0x80204ff8
```

当硬件观察点触发时：停在了我们设置得第二个观测点`*physical`处。此时可以查看地址转换的详细信息：通过`p/x *physical`查看物理地址基址，`p/x addr`查看虚拟地址，`p/x ((*physical) + (addr & 0xfff))`查看加入了页内偏移计算的物理地址

```apl
Thread 3 "qemu-system-ris" hit Breakpoint 3, tlb_set_page (cpu=0x6442d5122fb0, vaddr=18446744072637923328, paddr=2149597184, prot=7, mmu_idx=1, size=4096) at /home/lin/workspace/qemu-4.1.1/accel/tcg/cputlb.c:847
847         tlb_set_page_with_attrs(cpu, vaddr, paddr, MEMTXATTRS_UNSPECIFIED,
(gdb) p/x vaddr 
$5 = 0xffffffffc0204000
(gdb)  p/x paddr
$6 = 0x80204000
(gdb) p/x size
$7 = 0x1000
```

继续执行后，TLB设置断点被触发，停在了第三个断点 `tlb_set_page`处，`p/x vaddr`查看虚拟地址页基址，`p/x paddr`查看对应的物理地址页基址，`p/x size`查看对应的页大小。

### 三、调试过程关键发现与分析

#### 地址转换计算过程

通过调试信息，可以清晰看到地址转换的计算过程：

```
虚拟地址: 0xffffffffc0204ff8
   ↓ 页表查询得到物理页基址
物理页基址: 0x80204000
   ↓ 加上页内偏移(低12位)
完整物理地址: 0x80204ff8 = 0x80204000 + (0xffffffffc0204ff8 & 0xfff)
```

#### TLB缓存机制观察

通过`tlb_set_page`函数的参数分析，可以看到：

- QEMU将`0xffffffffc0204000`到`0x80204000`的映射关系缓存到TLB中
- 映射的权限位`prot=7`表示可读、可写、可执行
- 映射的粒度`size=0x1000`表示4KB标准页大小
- 此后相同页内的地址访问可以直接通过TLB快速转换，无需再次查询页表

以上就是我们初步调试的一个比较粗略的流程，接下来我们结合QEMU源码，进行更详细的调试过程。

### 四、结合QEMU源码的具体调试过程

首先我们先对QEMU源码中有关地址翻译的源码进行相关分析，根据`cpu_helper.c`文件，QEMU中RISC-V的地址翻译和TLB管理流程可以分为三个主要层次，对应三个主要函数：

1. **前端接口层**：`riscv_cpu_tlb_fill()` - 处理TLB未命中异常
2. **翻译核心层**：`get_physical_address()` - 执行页表遍历和权限检查
3. **后端填充层**：`tlb_set_page()` - 填充TLB缓存条目

当需要把虚拟地址翻译为物理地址时，首先会先去TLB中查找是否有相应的虚拟地址到物理地址的映射，若有，直接使用即可；若没有的话，TLB未命中，调用`riscv_cpu_tlb_fill()`，这个函数会去把获得物理基址的过程委派给`get_physical_address()`函数用于获取物理基址，如下：

```c++
ret = get_physical_address(env, &pa, &prot, address, access_type, mmu_idx);
```

在获得了物理基址之后，会进行TLB的填入工作，交付给`tlb_set_page()`函数。

下面，我们对`get_physical_address()`这个函数进行详细的分析：

#### 1. 函数定义与初始化

```c++
static int get_physical_address(CPURISCVState *env, hwaddr *physical,
                                int *prot, target_ulong addr,
                                int access_type, int mmu_idx)
{
    int mode = mmu_idx;

    if (mode == PRV_M && access_type != MMU_INST_FETCH) {
        if (get_field(env->mstatus, MSTATUS_MPRV)) {
            mode = get_field(env->mstatus, MSTATUS_MPP);
        }
    }

    if (mode == PRV_M || !riscv_feature(env, RISCV_FEATURE_MMU)) {
        *physical = addr;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return TRANSLATE_SUCCESS;
    }

    *prot = 0;
```

**分析：**

1. 函数首先获取当前的 MMU 索引作为初始特权级模式 (`mode`)。
2. 接着检查是否处于 Machine Mode (M-Mode) 且开启了 `MPRV` (Modify Privilege) 位。如果开启，且当前操作不是取指，则使用 `mstatus.MPP` 中定义的特权级进行后续的地址翻译，这允许 M 模式代码以较低特权级的视角访问内存。
3. 随后，代码判断是否直接使用物理地址（直通模式）：如果最终模式是 M Mode，或者 CPU 硬件不支持 MMU 特性，则不进行页表查找，直接将输入的虚拟地址 `addr` 赋值给物理地址 `*physical`，赋予所有读写执行权限，并返回翻译成功。这是系统启动早期或无操作系统环境下的默认行为，比如我们一开始在`kern_entry`中未开启虚拟地址模式时采用的是物理地址访问。
4. 如果需要进行页表翻译，则先将权限集 `*prot` 初始化为 0。

#### 2. 读取 SATP 寄存器与页表参数配置

```c++
    target_ulong base;
    int levels, ptidxbits, ptesize, vm, sum;
    int mxr = get_field(env->mstatus, MSTATUS_MXR);

    if (env->priv_ver >= PRIV_VERSION_1_10_0) {
        base = get_field(env->satp, SATP_PPN) << PGSHIFT;
        sum = get_field(env->mstatus, MSTATUS_SUM);
        vm = get_field(env->satp, SATP_MODE);
        switch (vm) {
        case VM_1_10_SV32:
          levels = 2; ptidxbits = 10; ptesize = 4; break;
        case VM_1_10_SV39:
          levels = 3; ptidxbits = 9; ptesize = 8; break;
        case VM_1_10_SV48:
          levels = 4; ptidxbits = 9; ptesize = 8; break;
        case VM_1_10_SV57:
          levels = 5; ptidxbits = 9; ptesize = 8; break;
        case VM_1_10_MBARE:
            *physical = addr;
            *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            return TRANSLATE_SUCCESS;
        default:
          g_assert_not_reached();
        }
    } else {
        base = env->sptbr << PGSHIFT;
        sum = !get_field(env->mstatus, MSTATUS_PUM);
        vm = get_field(env->mstatus, MSTATUS_VM);
        switch (vm) {
        case VM_1_09_SV32:
          levels = 2; ptidxbits = 10; ptesize = 4; break;
        case VM_1_09_SV39:
          levels = 3; ptidxbits = 9; ptesize = 8; break;
        case VM_1_09_SV48:
          levels = 4; ptidxbits = 9; ptesize = 8; break;
        case VM_1_09_MBARE:
            *physical = addr;
            *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            return TRANSLATE_SUCCESS;
        default:
          g_assert_not_reached();
        }
    }
```

**分析：**
这一段代码负责从控制寄存器中提取页表翻译所需的参数。对于较新的特权级规范（1.10.0 及以上），代码从 `satp` 寄存器中提取页表物理基址 (`base`)、SUM 位（Supervisor User Memory，允许内核访问用户页）以及分页模式 (`vm`)。

接着代码会根据 `vm` 的值配置具体的页表参数。对于本次实验关注的 **SV39** 模式，设定页表层级 `levels` 为 3，每级索引位数 `ptidxbits` 为 9，页表项大小 `ptesize` 为 8 字节，这与我们之前的实验内容相符。如果模式为 `MBARE`，则显式关闭 MMU，直接返回物理地址。此外，代码还保留了对旧版 1.09 规范的支持（`else` 分支），逻辑类似但寄存器名称不同（如使用 `sptbr` 而非 `satp`）。

#### 3. 虚拟地址规范性检查 (Canonical Address Check)

```c++
    CPUState *cs = env_cpu(env);
    int va_bits = PGSHIFT + levels * ptidxbits;
    target_ulong mask = (1L << (TARGET_LONG_BITS - (va_bits - 1))) - 1;
    target_ulong masked_msbs = (addr >> (va_bits - 1)) & mask;
    if (masked_msbs != 0 && masked_msbs != mask) {
        return TRANSLATE_FAIL;
    }
```

**分析：**
RISC-V 64位架构中的虚拟地址空间并未完全使用。对于 SV39 模式，有效地址位数为 39 位。硬件规定，虚拟地址的高位（第 63 位到第 39 位）必须与第 38 位（最高有效位）保持一致，即进行符号扩展。

这段代码计算出有效位数 `va_bits`，并通过掩码 `mask` 提取出虚拟地址的高位部分 `masked_msbs`。如果高位部分不是全 0 或全 1（即不等于 0 且不等于掩码本身），则说明该虚拟地址不符合规范，属于非法地址，函数直接返回翻译失败。

#### 4. 多级页表遍历循环 (Hardware Page Walk)

```c++
    int ptshift = (levels - 1) * ptidxbits;
    int i;

#if !TCG_OVERSIZED_GUEST
restart:
#endif
    for (i = 0; i < levels; i++, ptshift -= ptidxbits) {
        //1.索引计算与读取
        target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
                           ((1 << ptidxbits) - 1);
        target_ulong pte_addr = base + idx * ptesize;
        //1.1计算出页表项 (PTE) 的物理地址 `pte_addr`
        if (riscv_feature(env, RISCV_FEATURE_PMP) &&
            !pmp_hart_has_privs(env, pte_addr, sizeof(target_ulong),
            1 << MMU_DATA_LOAD, PRV_S)) {
            return TRANSLATE_PMP_FAIL;
        }
        //1.2 提取页表项内容 并获取物理页号PPN
#if defined(TARGET_RISCV32)
        target_ulong pte = ldl_phys(cs->as, pte_addr);
#elif defined(TARGET_RISCV64)
        target_ulong pte = ldq_phys(cs->as, pte_addr);
#endif
        target_ulong ppn = pte >> PTE_PPN_SHIFT;
	//2.有效性与目录项判断
        if (!(pte & PTE_V)) {//2.1缺页
            return TRANSLATE_FAIL;
        } else if (!(pte & (PTE_R | PTE_W | PTE_X))) {//2.2是目录项 不是叶子节点 提取出的 PPN 左移作为下一级页表的基址 base ，并继续下一次循环
            base = ppn << PGSHIFT;
        //2.3否则是叶子节点 依次进行检查
        //3.1检查是否只有写权限 ( PTE_W ) 或同时有写和执行权限 ( PTE_W | PTE_X )，这些在 RISC-V 中是保留或非法的。
        } else if ((pte & (PTE_R | PTE_W | PTE_X)) == PTE_W) {
            return TRANSLATE_FAIL;
        } else if ((pte & (PTE_R | PTE_W | PTE_X)) == (PTE_W | PTE_X)) {
            return TRANSLATE_FAIL;
      //3.2检查用户/内核权限。
        } else if ((pte & PTE_U) && ((mode != PRV_U) &&
                   (!sum || access_type == MMU_INST_FETCH))) {//如果 PTE 是用户页 ( PTE_U ) 但当前不在 U 模式（且 SUM 未开启或正在取指），则禁止访问。
            return TRANSLATE_FAIL;
        } else if (!(pte & PTE_U) && (mode != PRV_S)) {//反之，如果 PTE 是内核页但当前在 U 模式，也禁止访问。  
            return TRANSLATE_FAIL;
        } else if (ppn & ((1ULL << ptshift) - 1)) {//3.3如果是超级页（Superpage，即 ptshift > 0 的叶子节点），检查 PPN 是否对齐。
            return TRANSLATE_FAIL;
      //3.4根据具体的 access_type （读、写、取指），对比 PTE 的 R/W/X 位，确保权限足够。注意读取时如果 MXR 置位，则允许读取只执行页面。
        } else if (access_type == MMU_DATA_LOAD && !((pte & PTE_R) ||
                   ((pte & PTE_X) && mxr))) {
            return TRANSLATE_FAIL;
        } else if (access_type == MMU_DATA_STORE && !(pte & PTE_W)) {
            return TRANSLATE_FAIL;
        } else if (access_type == MMU_INST_FETCH && !(pte & PTE_X)) {
            return TRANSLATE_FAIL;
        } else {//4.如果所有检查通过，进入最后的 else 块。
            target_ulong updated_pte = pte | PTE_A |//4.1 A/D 位维护
                (access_type == MMU_DATA_STORE ? PTE_D : 0);

            if (updated_pte != pte) {//PTE 值发生了变化（例如第一次访问或第一次写），需要将其写回物理内存
                MemoryRegion *mr;
                hwaddr l = sizeof(target_ulong), addr1;
                mr = address_space_translate(cs->as, pte_addr,&addr1, &l, false, MEMTXATTRS_UNSPECIFIED);
                if (memory_region_is_ram(mr)) {
                    target_ulong *pte_pa = qemu_map_ram_ptr(mr->ram_block, addr1);
#if TCG_OVERSIZED_GUEST
                    *pte_pa = pte = updated_pte;
#else
                    target_ulong old_pte = atomic_cmpxchg(pte_pa, pte, updated_pte);
                    if (old_pte != pte) {
                        goto restart;//原子更新失败（说明期间被其他核修改），跳转回 restart 重新开始查找
                    } else {
                        pte = updated_pte;
                    }
#endif
                } else {
                    return TRANSLATE_FAIL;
                }
            }
            //物理地址计算
            target_ulong vpn = addr >> PGSHIFT;
            *physical = (ppn | (vpn & ((1L << ptshift) - 1))) << PGSHIFT;
			//TLB 填充
            if ((pte & PTE_R) || ((pte & PTE_X) && mxr)) {
                *prot |= PAGE_READ;
            }
            if ((pte & PTE_X)) {
                *prot |= PAGE_EXEC;
            }
            if ((pte & PTE_W) &&
                    (access_type == MMU_DATA_STORE || (pte & PTE_D))) {
                *prot |= PAGE_WRITE;
            }
            return TRANSLATE_SUCCESS;
        }
    }
    return TRANSLATE_FAIL;
}
```

**分析：**

这是 MMU 地址翻译的核心循环，模拟硬件逐级查找页表的过程。

1.  **索引计算与读取**：循环根据当前层级 `levels`，从虚拟地址中提取对应的索引 `idx`，计算出页表项 (PTE) 的物理地址 `pte_addr`。在进行物理内存访问前，还会进行 PMP (Physical Memory Protection) 检查。之后，使用 `ldq_phys` 读取 64 位的 PTE 内容，并提取PPN（物理页号）。
2.  **有效性与目录项判断**：
    *   首先检查 PTE 的 Valid 位 (`PTE_V`)，如果为 0，则表示缺页，返回失败。
    *   接着检查读/写/执行权限位 (`PTE_R | PTE_W | PTE_X`)。如果全为 0，说明当前的项是目录项（Pointer），代码将提取出的 PPN 左移作为下一级页表的基址 `base`，并继续下一次循环。
    *   如果不全为 0，说明找到了叶子节点（Leaf PTE），代码进入一系列 `else if` 进行严格的权限检查。
3.  **权限检查 **：
    *   **非法组合**：检查是否只有写权限 (`PTE_W`) 或同时有写和执行权限 (`PTE_W | PTE_X`)，这些在 RISC-V 中是保留或非法的。
    *   **U/S 隔离**：检查用户/内核权限。如果 PTE 是用户页 (`PTE_U`) 但当前不在 U 模式（且 `SUM` 未开启或正在取指），则禁止访问。反之，如果 PTE 是内核页但当前在 U 模式，也禁止访问。
    *   **大页对齐**：如果是超级页（`Superpage`，即 `ptshift > 0` 的叶子节点），检查 PPN 是否对齐。
    *   **访问类型检查**：根据具体的 `access_type`（读、写、取指），对比 PTE 的 `R/W/X` 位，确保权限足够。注意读取时如果 `MXR` 置位，则允许读取只执行页面。
4.  **A/D 位更新与物理地址生成**：
    *   如果所有检查通过，进入最后的 `else` 块。
    *   **A/D 位维护**：计算新的 PTE 值，设置 Accessed 位，如果是写操作则设置 Dirty 位。如果 PTE 值发生了变化（例如第一次访问或第一次写），则需要将其写回物理内存。代码使用了原子操作 `atomic_cmpxchg` 来保证多线程环境下的安全性，如果原子更新失败（说明期间被其他核修改），则跳转回 `restart` 重新开始查找。
    *   **物理地址计算**：通过公式 `(ppn | (vpn & ((1L << ptshift) - 1))) << PGSHIFT` 计算物理地址。这个公式巧妙地处理了大页映射：对于普通 4KB 页，中间的掩码部分为 0，直接使用 PPN；对于大页，则保留虚拟地址中的低位索引。
    *   **TLB 填充**：最后，根据 PTE 的属性设置返回给 TLB 的权限集 `*prot`，并返回 `TRANSLATE_SUCCESS`，标志着地址翻译成功完成。

#### 5.调试

基于此，我们来进行进一步的调试，从即将执行`sd`这条指令的时刻开始：

首先中断终端二中QEMU的执行，设置断点`break riscv_cpu_tlb_fill`。

```apl
(gdb) break riscv_cpu_tlb_fill
Breakpoint 1 at 0x5c1e399687ca: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 438.
(gdb) c
Continuing.
```

然后单步执行终端三的这条sd指令。

```apl
Thread 3 "qemu-system-ris" hit Breakpoint 1, riscv_cpu_tlb_fill (cs=0x5c1e54519fb0, address=18446744072637927416, size=8, access_type=MMU_DATA_STORE, mmu_idx=1, probe=false, retaddr=134360146673989) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:438
438     {
(gdb) p/x address
$1 = 0xffffffffc0204ff8
```

可以看到终端二停在了我们设置的断点处，且查看地址信息确实为`sd`访存的虚拟地址（`sp+8`），再设置一个断点`break get_physical_address` 。

```apl
(gdb) break get_physical_address 
Breakpoint 2 at 0x5c1e39967bb5: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 158.
(gdb) c
Continuing.
```

继续执行，断点命中。

```apl
Thread 3 "qemu-system-ris" hit Breakpoint 2, get_physical_address (env=0x5c1e545229c0, physical=0x7a3328d06220, prot=0x7a3328d06214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:158
158     {
```

此时我们进入到了get_physical_address函数中，再设置三个断点，方便我们监测在查询页表的过程中，页表项的物理地址`pte_addr`、物理页号`ppn`、以及最后计算得到的物理基址`*physical`这三个值的变化。

```apl
(gdb) break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:243
Breakpoint 3 at 0x5c1e3996805c: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 244.
(gdb) break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:255
Breakpoint 4 at 0x5c1e399680d2: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 256.
(gdb) break /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:335
Breakpoint 5 at 0x5c1e39968349: file /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c, line 337.
```

首先停在了刚更新完`pte_addr`的时刻，

```apl
(gdb) c
Continuing.

Thread 3 "qemu-system-ris" hit Breakpoint 3, get_physical_address (env=0x5c1e545229c0, physical=0x7a3328d06220, prot=0x7a3328d06214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:244
244             if (riscv_feature(env, RISCV_FEATURE_PMP) &&
(gdb) p/x pte_addr
$2 = 0x80205ff8
```

可以看到，`pte_addr`被更新为0x80205ff8，对应的上下文代码是：

```c++
        target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
                           ((1 << ptidxbits) - 1);

        /* check that physical address of PTE is legal */
        target_ulong pte_addr = base + idx * ptesize;
```

由于查看`satp`寄存器得到页表基址为0x80205000，通过页内偏移（`索引×页表项大小`）即可得到页表项的地址为80205ff8。

然后中断在了刚刚更新完物理页号PPN的位置，

```apl
(gdb) c
Continuing.

Thread 3 "qemu-system-ris" hit Breakpoint 4, get_physical_address (env=0x5c1e545229c0, physical=0x7a3328d06220, prot=0x7a3328d06214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:256
256             if (!(pte & PTE_V)) {
(gdb) p/x ppn
$3 = 0x80000
(gdb) c
Continuing.
```

对应的代码为：

```c++
#if defined(TARGET_RISCV32)
        target_ulong pte = ldl_phys(cs->as, pte_addr);
#elif defined(TARGET_RISCV64)
        target_ulong pte = ldq_phys(cs->as, pte_addr);
#endif
        target_ulong ppn = pte >> PTE_PPN_SHIFT;
```

通过页表项的地址拿到其对应的页信息，然后拿到对应的物理页号，通过调试信息可以看到此时物理页号PPN被更新为 0x80000。

然后停在了最后更新*physical物理基址的地方，只进行了一轮映射，说明这是**大页映射**，可能因为这是内核启动早期的页表，故只需一轮映射即可拿到物理基址。

```apl
Thread 3 "qemu-system-ris" hit Breakpoint 5, get_physical_address (env=0x5c1e545229c0, physical=0x7a3328d06220, prot=0x7a3328d06214, addr=18446744072637927416, access_type=1, mmu_idx=1) at /home/lin/workspace/qemu-4.1.1/target/riscv/cpu_helper.c:337
337                 if ((pte & PTE_R) || ((pte & PTE_X) && mxr)) {
(gdb) p/x *physical
$4 = 0x80204000
(gdb) p/x ppn
$5 = 0x80000
(gdb) p/x vpn
$6 = 0xffffffffc0204
(gdb) p/x ptshift
$7 = 0x12
```

并且我们可以通过 `p/x ptshift`查看到当前层级的虚拟地址位偏移量`ptshift`的值为0x12，代表了这是1GB大页映射，在**第一级页表**就找到了叶子PTE，内核早期使用1GB大页可以简化页表管理，提高性能。

对应的上下文代码为：

```c++
            target_ulong vpn = addr >> PGSHIFT;
            *physical = (ppn | (vpn & ((1L << ptshift) - 1))) << PGSHIFT;
```

通过调试信息，我们可以看到**物理页号PPN**、**虚拟页号VPN**、**当前层级的虚拟地址位偏移量`ptshift`的值**，以及我们已知的PGSHIFT，可求出物理基址*physical，通过计算验证确实为我们读出的0x80204000。

```
虚拟地址 0xffffffffc0204ff8:
63        39 38   30 29   21 20   12 11    0
+-----------+-------+-------+-------+-------+
| 符号扩展  | VPN[2]| VPN[1]| VPN[0]| offset|
+-----------+-------+-------+-------+-------+
             0x1FF   0x100   0x204   0xFF8

页表遍历:
顶级页表: base=0x80205000 + 0x1FF*8 = 0x80205FF8 → PTE
PTE的PPN=0x80000, 且是叶子节点(ptshift=18)
物理地址: (0x80000 | 0x204) << 12 = 0x80204000
```

以上所述就是QEMU进行的完整的地址翻译的过程。

