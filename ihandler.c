
#include "interrupt.h"
#include "keyboard.h"
#include "task.h"
#include "mutex.h"
#include "screen.h"
#include "sysinfo.h"

extern byte ReadPort(ushort port);

void TimerHandler()
{
    static uint i = 0;
    
    i = (i + 1) % 5;
    
    if( i == 0 )
    {
        Schedule();
    }
    
    SendEOI(MASTER_EOI_PORT);
}

void KeyboardHandler()
{   //读取0x60端口的数据并且将数据返回
    byte sc = ReadPort(0x60);
    //对扫描码进行编码
    PutScanCode(sc);
    //通知所有等待键盘输入的任务
    NotifyKeyCode();
    
    SendEOI(MASTER_EOI_PORT);
}

//80号中断会触发调用此中断处理函数
void SysCallHandler(uint type, uint cmd, uint param1, uint param2)   // __cdecl__
{   //type中断功能号
    switch(type)
    {
        case 0: //任务函数调用
            TaskCallHandler(cmd, param1, param2);
            break;
        case 1://cmd子功能号 互斥锁函数调用
            MutexCallHandler(cmd, param1, param2);
            break;
        case 2:
            KeyCallHandler(cmd, param1, param2);
            break;
        case 3:
            SysInfoCallHandler(cmd, param1, param2);
            break;
        default:
            break;
    }
}

void PageFaultHandler()
{
    SetPrintPos(ERR_START_W, ERR_START_H);
    
    PrintString("Page Fault: kill ");
    PrintString(CurrentTaskName());
    
    KillTask();
}

void SegmentFaultHandler()
{
    SetPrintPos(ERR_START_W, ERR_START_H);
    
    PrintString("Segment Fault: kill ");
    PrintString(CurrentTaskName());
    
    KillTask();
}
