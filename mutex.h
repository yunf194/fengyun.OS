
#ifndef MUTEX_H
#define MUTEX_H

#include "type.h"
#include "queue.h"

enum
{
    Normal,
    Strict
};

typedef struct 
{
    ListNode head;      //链表
    Queue wait;         //
    uint type;          //
    uint lock;          //0表示占用 1表示空闲
} Mutex;

void MutexModInit();
void MutexCallHandler(uint cmd, uint param1, uint param2);


#endif
