
#include "mutex.h"
#include "memory.h"
#include "task.h"
#include "event.h"

static List gMList = {0};

//创建一把锁,返回锁变量的地址
static Mutex* SysCreateMutex(uint type)
{
    Mutex* ret = Malloc(sizeof(Mutex));
    
    if( ret )
    {
        Queue_Init(&ret->wait);
        
        ret->lock = 0;  
        ret->type = type;
        
        List_Add(&gMList, (ListNode*)ret);
    }
    //互斥锁的地址作为互斥锁id返回
    return ret;
}

static uint IsMutexValid(Mutex* mutex)
{
    uint ret = 0;
    ListNode* pos = NULL;
    
    List_ForEach(&gMList, pos)
    {
        if( IsEqual(pos, mutex) )
        {
            ret = 1;
            break;
        }
    }
    
    return ret;
}

static void SysDestroyMutex(Mutex* mutex, uint* result)
{
    if( mutex )
    {
        ListNode* pos = NULL;
        
        *result = 0;
        //链表遍历,地址是否相同
        List_ForEach(&gMList, pos)
        {
            if( IsEqual(pos, mutex) )
            {
                if( IsEqual(mutex->lock, 0) )
                {
                    List_DelNode(pos);
                    
                    Free(pos);
                    
                    *result = 1;
                }
                
                break;
            }
        }
    }
}

static void DoWait(Mutex* mutex, uint* wait)
{
    Event* evt = CreateEvent(MutexEvent, (uint)mutex, 0, 0);
    
    if( evt )
    {
        *wait = 1;
        
        EventSchedule(WAIT, evt);
    }
}

static void SysNormalEnter(Mutex* mutex, uint* wait)
{
    if( mutex->lock )
    {
        DoWait(mutex, wait);
    }
    else
    {
        mutex->lock = 1;
        
        *wait = 0;
    }
}

static void SysStrictEnter(Mutex* mutex,uint* wait)
{
    if( mutex->lock )
    {
        if( mutex->lock == CurrentTaskId() )
        {
            *wait = 0;
        }
        else
        {         
            DoWait(mutex, wait);
        }
    }
    else
    {
        mutex->lock = CurrentTaskId();
            
        *wait = 0;
    }
}

static void SysEnterCritical(Mutex* mutex, uint* wait)
{
    if( mutex && IsMutexValid(mutex) )
    { 
        switch(mutex->type)
        {   //占用状态,任务只能进入等待状态
            //空闲状态,任务占用临界资源并且将lock设置为已经被占用
            case Normal:
                SysNormalEnter(mutex, wait);
                break;
            case Strict:
                SysStrictEnter(mutex, wait);
                break;
            default:
                break;
        }
    }
}

static void SysNormalExit(Mutex* mutex)
{
    Event evt = {MutexEvent, (uint)mutex, 0, 0};
    
    mutex->lock = 0;
    
    EventSchedule(NOTIFY, &evt);
}

static void SysStrictExit(Mutex* mutex)
{
    if( mutex->lock == CurrentTaskId() )
    {
        SysNormalExit(mutex);
    }
    else
    {   
        KillTask();
    }
}


void SysExitCritical(Mutex* mutex)
{
    if( mutex && IsMutexValid(mutex) )
    {
        switch(mutex->type)
        {
            case Normal:
                //lock标记设置为0 表示空闲状态
                SysNormalExit(mutex);
                break;
            case Strict:
                SysStrictExit(mutex);
                break;
            default:
                break;
        }
    }
}

void MutexModInit()
{
    List_Init(&gMList);
}

//cmd子功能号
void MutexCallHandler(uint cmd, uint param1, uint param2)
{
    if( cmd == 0 )
    {
        uint* pRet = (uint*)param1;
        
        *pRet = (uint)SysCreateMutex(param2);
    }
    else if( cmd == 1 )
    {
        SysEnterCritical((Mutex*)param1, (uint*)param2);
    }
    else if( cmd == 2 )
    {
        SysExitCritical((Mutex*)param1);
    }
    else 
    {
        SysDestroyMutex((Mutex*)param1, (uint*)param2);
    }
}



