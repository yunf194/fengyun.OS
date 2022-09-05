
#ifndef TASK_H
#define TASK_H

#include "kernel.h"
#include "queue.h"
#include "event.h"
#include "app.h"

typedef struct 
{
    uint gs;
    uint fs;
    uint es;
    uint ds;
    uint edi;
    uint esi;
    uint ebp;
    uint kesp;
    uint ebx;
    uint edx;
    uint ecx;
    uint eax;
    uint error_code;
    uint eip;           //指令寄存器
    uint cs;            
    uint eflags;
    uint esp;
    uint ss;
} RegValue;             //任务切换就是保护寄存器的值,修改寄存器的值

typedef struct
{
    uint   previous;
    uint   esp0;
    uint   ss0;
    uint   unused[22];
    ushort reserved;
    ushort iomb;
} TSS;

typedef struct
{
    RegValue   rv;                  //寄存器的值,记录任务执行状态
    Descriptor ldt[3];              //局部段描述符表,LDT描述局部于每个程序的段,包括代码段、数据段、显存段
    ushort     ldtSelector;         //LDT选择子
    ushort     tssSelector;         //TSS,用于查询内核栈
    void (*tmain)();                //任务起始地址
    uint       id;                  //任务id
    ushort     current;             //任务已经执行的时间
    ushort     total;               //执行队列中 任务总共允许执行的时间
    char       name[16];            //任务名
    Queue      wait;                //被此任务的等待队列
    byte*      stack;               //任务栈
    Event*     event;               //任务事件
} Task;

typedef struct
{
    QueueNode head;
    Task task;
} TaskNode;

typedef struct
{
    QueueNode head;
    AppInfo app;
} AppNode;

enum
{
    WAIT,
    NOTIFY
};

extern void (* const RunTask)(volatile Task* pt);
extern void (* const LoadTask)(volatile Task* pt);

void TaskModInit();
void LaunchTask();
void Schedule();
void TaskCallHandler(uint cmd, uint param1, uint param2);
void EventSchedule(uint action, Event* event);
void KillTask();
void WaitTask(const char* name);

const char* CurrentTaskName();
uint CurrentTaskId();

#endif
