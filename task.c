#include "utility.h"
#include "task.h"
#include "mutex.h"
#include "queue.h"
#include "app.h"

#define MAX_TASK_NUM        16
#define MAX_RUNNING_TASK    8
#define MAX_READY_TASK      (MAX_TASK_NUM - MAX_RUNNING_TASK)
#define MAX_TASK_BUFF_NUM   (MAX_TASK_NUM + 1)
#define PID_BASE            0x10
#define MAX_TIME_SLICE      260

void (* const RunTask)(volatile Task* pt) = NULL;
void (* const LoadTask)(volatile Task* pt) = NULL;

volatile Task* gCTaskAddr = NULL; /* DO NOT USE IT DIRECTLY */

static TaskNode gTaskBuff[MAX_TASK_BUFF_NUM] = {0};
static Queue gAppToRun = {0};
static Queue gFreeTaskNode = {0};
static Queue gReadyTask = {0};
static Queue gRunningTask = {0};
static TSS gTSS = {0};
static TaskNode* gIdleTask = NULL;
static uint gPid = PID_BASE;

static void TaskEntry()
{
    if( gCTaskAddr )
    {
        gCTaskAddr->tmain();
    }
    
    // to destory current task here 处于用户态
    asm volatile(               //调用80号中断陷入内核态
        "movl  $0,  %eax \n"    // type
        "int   $0x80     \n"
    );
}

static void IdleTask()
{
    while(1);
}

static void InitTask(Task* pt, uint id, const char* name, void(*entry)(), ushort pri)
{
    pt->rv.cs = LDT_CODE32_SELECTOR;
    pt->rv.gs = LDT_VIDEO_SELECTOR;
    pt->rv.ds = LDT_DATA32_SELECTOR;
    pt->rv.es = LDT_DATA32_SELECTOR;
    pt->rv.fs = LDT_DATA32_SELECTOR;
    pt->rv.ss = LDT_DATA32_SELECTOR;
    
    pt->rv.esp = (uint)pt->stack + AppStackSize;
    pt->rv.eip = (uint)TaskEntry;
    pt->rv.eflags = 0x3202;
    
    pt->tmain = entry;
    pt->id = id;
    pt->current = 0;
    pt->total = MAX_TIME_SLICE - pri;
    pt->event = NULL;
    
    if( name )
    {
        StrCpy(pt->name, name, sizeof(pt->name)-1);
    }
    else
    {
        *(pt->name) = 0;
    }
    
    Queue_Init(&pt->wait);
    
    SetDescValue(AddrOff(pt->ldt, LDT_VIDEO_INDEX),  0xB8000, 0x07FFF, DA_DRWA + DA_32 + DA_DPL3);
    SetDescValue(AddrOff(pt->ldt, LDT_CODE32_INDEX), 0x00,    KernelHeapBase - 1, DA_C + DA_32 + DA_DPL3);
    SetDescValue(AddrOff(pt->ldt, LDT_DATA32_INDEX), 0x00,    KernelHeapBase - 1, DA_DRW + DA_32 + DA_DPL3);
    
    pt->ldtSelector = GDT_TASK_LDT_SELECTOR;
    pt->tssSelector = GDT_TASK_TSS_SELECTOR;
}

static Task* FindTaskByName(const char* name)
{
    Task* ret = NULL;
    //IdleTask任务直接返回空
    if( !StrCmp(name, "IdleTask", -1) )
    {
        int i = 0;
        //遍历gTaskBuff数组
        for(i=0; i<MAX_TASK_BUFF_NUM; i++)
        {
            TaskNode* tn = AddrOff(gTaskBuff, i);
            //id不为0且比较字符串相等
            if( tn->task.id && StrCmp(tn->task.name, name, -1) )
            {
                ret = &tn->task;
                break;
            }
        }
    }
    //返回目标任务地址
    return ret;
}

static void PrepareForRun(volatile Task* pt)
{
    pt->current++;
    
    gTSS.ss0 = GDT_DATA32_FLAT_SELECTOR;
    gTSS.esp0 = (uint)&pt->rv + sizeof(pt->rv);
    gTSS.iomb = sizeof(TSS);
    
    SetDescValue(AddrOff(gGdtInfo.entry, GDT_TASK_LDT_INDEX), (uint)&pt->ldt, sizeof(pt->ldt)-1, DA_LDT + DA_DPL0);
}

//创建任务进入就绪队列
static void CreateTask()
{
    while( (0 < Queue_Length(&gAppToRun)) && (Queue_Length(&gReadyTask) < MAX_READY_TASK) )
    {
        TaskNode* tn = (TaskNode*)Queue_Remove(&gFreeTaskNode);
        
        if( tn )
        {
            AppNode* an = (AppNode*)Queue_Remove(&gAppToRun); 
            
            InitTask(&tn->task, gPid++, an->app.name, an->app.tmain, an->app.priority);
            
            Queue_Add(&gReadyTask, (QueueNode*)tn);
            
            Free((void*)an->app.name);
            Free(an);
        }
        else
        {
            break;
        }
    }
}

static void CheckRunningTask()
{
    if( Queue_Length(&gRunningTask) == 0 )
    {
        Queue_Add(&gRunningTask, (QueueNode*)gIdleTask);
    }
    else if( Queue_Length(&gRunningTask) > 1 )
    {
        if( IsEqual(Queue_Front(&gRunningTask), (QueueNode*)gIdleTask) )
        {
            Queue_Remove(&gRunningTask);
        }
    }
}

static void ReadyToRunning()
{
    QueueNode* node = NULL;
    
    if( Queue_Length(&gReadyTask) < MAX_READY_TASK )
    {
        CreateTask();
    }
    
    while( (Queue_Length(&gReadyTask) > 0) && (Queue_Length(&gRunningTask) < MAX_RUNNING_TASK) )
    {
        node = Queue_Remove(&gReadyTask);
        
        ((TaskNode*)node)->task.current = 0;//当前任务的current赋值为0表示刚开始
        
        Queue_Add(&gRunningTask, node);
    }
}

static void RunningToReady()
{
    if( Queue_Length(&gRunningTask) > 0 )
    {
        TaskNode* tn = (TaskNode*)Queue_Front(&gRunningTask);
        
        if( !IsEqual(tn, (QueueNode*)gIdleTask) )
        {
            if( tn->task.current == tn->task.total )
            {
                Queue_Remove(&gRunningTask);
                Queue_Add(&gReadyTask, (QueueNode*)tn);
            }
        }
    }
}

static void RunningToWaitting(Queue* wq)
{
    if( Queue_Length(&gRunningTask) > 0 )
    {
        TaskNode* tn = (TaskNode*)Queue_Front(&gRunningTask);
        
        if( !IsEqual(tn, (QueueNode*)gIdleTask) )
        {
            Queue_Remove(&gRunningTask);
            Queue_Add(wq, (QueueNode*)tn);
        }
    }
}


static void WaittingToReady(Queue* wq)
{
    while( Queue_Length(wq) > 0 )
    {
        TaskNode* tn = (TaskNode*)Queue_Front(wq);
        //销毁任务,并且任务指针赋值为空
        DestroyEvent(tn->task.event); 
        tn->task.event = NULL;
        
        Queue_Remove(wq);
        Queue_Add(&gReadyTask, (QueueNode*)tn);
    }
}

static void AppInfoToRun(const char* name, void(*tmain)(), byte pri)
{
    AppNode* an = (AppNode*)Malloc(sizeof(AppNode));
    
    if( an )
    {
        char* s = name ? (char*)Malloc(StrLen(name) + 1) : NULL;
        
        an->app.name = s ? StrCpy(s, name, -1) : NULL;
        an->app.tmain = tmain;
        an->app.priority = pri;
        
        Queue_Add(&gAppToRun, (QueueNode*)an);
    }
}

static void AppMainToRun()
{
    AppInfoToRun("AppMain", (void*)(*((uint*)AppMainEntry)), 200);
}

void TaskModInit()
{
    int i = 0;
    byte* pStack = (byte*)(AppHeapBase - (AppStackSize * MAX_TASK_BUFF_NUM));
    
    for(i=0; i<MAX_TASK_BUFF_NUM; i++)
    {
        TaskNode* tn = (void*)AddrOff(gTaskBuff, i);
        
        tn->task.stack = (void*)AddrOff(pStack, i * AppStackSize);
    }
    
    gIdleTask = (void*)AddrOff(gTaskBuff, MAX_TASK_NUM);
    
    Queue_Init(&gAppToRun);
    Queue_Init(&gFreeTaskNode);
    Queue_Init(&gRunningTask);
    Queue_Init(&gReadyTask);
    
    for(i=0; i<MAX_TASK_NUM; i++)
    {
        Queue_Add(&gFreeTaskNode, (QueueNode*)AddrOff(gTaskBuff, i));
    }
    
    SetDescValue(AddrOff(gGdtInfo.entry, GDT_TASK_TSS_INDEX), (uint)&gTSS, sizeof(gTSS)-1, DA_386TSS + DA_DPL0);
    
    InitTask(&gIdleTask->task, 0, "IdleTask", IdleTask, 255);
    
    AppMainToRun();
    
    ReadyToRunning();
    
    CheckRunningTask();
}

static void ScheduleNext()
{
    //若执行队列任务数未达到最大任务数，则从就绪队列中移入任务到执行队列
    ReadyToRunning();
    //判断是否有任务用于移动idle任务
    CheckRunningTask();
    //执行队列队首元素移动到队尾
    Queue_Rotate(&gRunningTask);
    //取出队首元素修改任务指针指向新任务
    gCTaskAddr = &((TaskNode*)Queue_Front(&gRunningTask))->task;
    ////初始化好此任务的内核栈TSS信息和设置ldt信息
    PrepareForRun(gCTaskAddr);
    //加载队ldt信息到内存中
    LoadTask(gCTaskAddr);
}

void LaunchTask()
{
    gCTaskAddr = &((TaskNode*)Queue_Front(&gRunningTask))->task;
    
    PrepareForRun(gCTaskAddr);
    
    RunTask(gCTaskAddr);
}

void Schedule()
{
    RunningToReady();
    ScheduleNext();
}

static void WaitEvent(Queue* wait, Event* event)
{
    //当前任务记录事件
    gCTaskAddr->event = event;
    //当前任务放入目标等待队列
    RunningToWaitting(wait);
    //调度下一个任务执行
    ScheduleNext();
}

static void TaskSchedule(uint action, Event* event)
{
    Task* task = (Task*)event->id;
    
    if( action == NOTIFY )
    {
        WaittingToReady(&task->wait);
    }
    else if( action == WAIT )
    {
        //gCTask 进入 目标任务的等待队列
        WaitEvent(&task->wait, event);
    }
}

static void MutexSchedule(uint action, Event* event)
{
    Mutex* mutex = (Mutex*)event->id;
    
    if( action == NOTIFY )
    {
        WaittingToReady(&mutex->wait);
    }
    else if( action == WAIT )
    {
        WaitEvent(&mutex->wait, event);
    }
}

//action == NOTIFY 任务从等待队列调度到就绪队列
//action == WAIT   当前任务进入等待队列
static void KeySchedule(uint action, Event* event)
{
    Queue* wait = (Queue*)event->id;    //等待队列的地址 (uint)&gKeyWait
    
    if( action == NOTIFY )
    {
        uint kc = event->param1;        //拿到keycode
        ListNode* pos = NULL;
        
        List_ForEach((List*)wait, pos)  //通知遍历等待队列中的每个任务
        {
            TaskNode* tn = (TaskNode*)pos;
            Event* we = tn->task.event;   //拿到任务的等待事件
            uint* ret = (uint*)we->param1;//ReadKey函数中的ret
            
            *ret = kc;                  //用户的摁键编码返回给用户
        }
        
        WaittingToReady(wait);
    }
    else if( action == WAIT )
    {
        WaitEvent(wait, event);
    }
}

//根据事件把任务从等待状态调度为就绪状态还是从就绪状态调度为等待状态
// typedef struct
// {
//     uint type;
//     uint id;
//     uint param1;
//     uint param2;
// } Event;
void EventSchedule(uint action, Event* event)
{
    switch(event->type)
    {
        case KeyEvent:
            KeySchedule(action, event);
            break;
        case TaskEvent:
            TaskSchedule(action, event);
            break;
        case MutexEvent:
            MutexSchedule(action, event);
            break;
        default:
            break;
    }
}

void KillTask()
{
    QueueNode* node = Queue_Remove(&gRunningTask);
    Task* task = &((TaskNode*)node)->task;
    Event evt = {TaskEvent, (uint)task, 0, 0};
    //被本任务阻塞的其他任务都给唤醒
    EventSchedule(NOTIFY, &evt);
    
    task->id = 0;
    
    Queue_Add(&gFreeTaskNode, node);
    
    Schedule();
}

void WaitTask(const char* name)
{
    Task* task = FindTaskByName(name);
    
    if( task )
    {
        Event* evt = CreateEvent(TaskEvent, (uint)task, 0, 0);
        
        if( evt )
        {   
            //当前任务进入 目标任务的等待队列
            EventSchedule(WAIT, evt);
        }
    }
}

void TaskCallHandler(uint cmd, uint param1, uint param2)
{
    switch(cmd)
    {
        case 0:
            KillTask();
            break;
        case 1:
            WaitTask((char*)param1);
            break;
        case 2:
            AppInfoToRun(((AppInfo*)param1)->name, ((AppInfo*)param1)->tmain, ((AppInfo*)param1)->priority);
            break;
        default:
            break;
    }
}

const char* CurrentTaskName()
{
    return (const char*)gCTaskAddr->name;
}

uint CurrentTaskId()
{
    return gCTaskAddr->id;
}


