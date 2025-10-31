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

## 拓展练习Challenge3：完善异常中断