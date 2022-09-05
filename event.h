
#ifndef EVENT_H
#define EVENT_H

#include "type.h"

enum
{
    NoneEvent,
    MutexEvent,
    KeyEvent,
    TaskEvent
};

typedef struct
{
    uint type;      //事件类型
    uint id;        //事件标识信息
    uint param1;    //事件参数1
    uint param2;    //事件参数2
} Event;

Event* CreateEvent(uint type, uint id, uint param1, uint param2);
void DestroyEvent(Event* event);


#endif
