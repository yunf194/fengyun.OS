
#include "memory.h"
#include "utility.h"
#include "list.h"

#define FM_ALLOC_SIZE    32
#define FM_NODE_SIZE     sizeof(FMemNode)
#define VM_HEAD_SIZE     sizeof(VMemHead)

typedef byte(FMemUnit)[FM_ALLOC_SIZE];  //内存分配单元大小是FM_ALLOC_SIZE即32bytes
typedef union _FMemNode  FMemNode;	    //内存管理单元的别名 

union _FMemNode			//管理单元4bytes 注意是union尽量少地使用内存
{
    FMemNode* next;		//管理单元间链表形式连接起来
    FMemUnit* ptr;      //标记
};

typedef struct			
{
    FMemNode* node;	    //指向内存管理链表的头节点
    FMemNode* nbase;    //内存管理单元起始地址
    FMemUnit* ubase;    //内存分配单元起始地址
    uint max;		    //记录还有多少内存分配单元可以用
} FMemList;


typedef struct
{
    ListNode head;      //ListNode双向链表节点
    uint used;          //使用区域长度
    uint free;          //空闲区域长度
    byte* ptr;          //指向已用区域起始位置
} VMemHead;


static FMemList gFMemList = {0};        //定长内存管理单元链表 --表头
static List gVMemList = {0};

//初始化内存管理,从mem中构建管理单元和分配单元
static void FMemInit(byte* mem, uint size)
{
    FMemNode* p = NULL;
    int i = 0;
    uint max = 0;
    
    max = size / (FM_NODE_SIZE + FM_ALLOC_SIZE);
    //gFMemList赋值--内存管理单元群殴起始位置和内存分配单元起始位置
    gFMemList.max = max;
    gFMemList.nbase = (FMemNode*)mem;
    gFMemList.ubase = (FMemUnit*)((uint)mem + max * FM_NODE_SIZE);
    gFMemList.node = (FMemNode*)mem;
    
    p = gFMemList.node;
    //从管理单元0~n-1，每个管理单元的next指针指向下一个管理单元
    for(i=0; i<max-1; i++)
    {
        FMemNode* current = (FMemNode*)AddrOff(p, i);
        FMemNode* next = (FMemNode*)AddrOff(p, i+1);
        
        current->next = next;
    }
    //最后一个管理单元指向null
    ((FMemNode*)AddrOff(p, i))->next = NULL;
}

static void* FMemAlloc()
{
    void* ret = NULL;
    //gFMemList.node指向管理单元链表的第一个节点,将管理单元第一个节点分配出去
    if( gFMemList.node )
    {
        //取出来的管理单元相对于nbase(管理单元头部)的编号是多少
        FMemNode* alloc = gFMemList.node;
        int index = AddrIndex(alloc, gFMemList.nbase);
        //取出对应的内存分配单元
        ret = AddrOff(gFMemList.ubase, index);
        //更新链表的头节点为下一个相邻节点
        gFMemList.node = alloc->next;
        //标记内存管理单元指向分配单元,归还内存的时候可以验证一下
        alloc->ptr = ret;
    }
    //返回值为分配单元地址
    return ret;
}

//传入参数是分配单元的地址
static int FMemFree(void* ptr)
{
    int ret = 0;
    
    if( ptr )
    {   //取出来的分配单元相对于ubase的(分配单元头部)的编号是多少
        uint index = AddrIndex((FMemUnit*)ptr, gFMemList.ubase);
        //根据编号与全局nbase取出管理单元来
        FMemNode* node = AddrOff(gFMemList.nbase, index);
        //判断一下管理单元下标和ptr的值是否有问题
        if( (index < gFMemList.max) && IsEqual(node->ptr, ptr) )
        {   //将此node插入的链表头部
            node->next = gFMemList.node;
            
            gFMemList.node = node;
            
            ret = 1;
        }
    }
    
    return ret;
}

static void VMemInit(byte* mem, uint size)
{
    List_Init((List*)&gVMemList);       //初始化一个双向链表节点,next和prev都指向gVMemList
    VMemHead* head = (VMemHead*)mem;    //创建管理头
    
    head->used = 0;                     //已用区域初始化为0
    head->free = size - VM_HEAD_SIZE;   //除了管理头都是空闲区域
    head->ptr = AddrOff(head, 1);       //ptr指向userd的开头
    
    List_AddTail(&gVMemList, (ListNode*)head);//插入到全局链表头
} 

static void* VMemAlloc(uint size)
{
    ListNode* pos = NULL;
    VMemHead* ret = NULL;
    uint alloc = size + VM_HEAD_SIZE;
    //for(pos=(&gVMemList)->next; !IsEqual(&gVMemList, pos); pos=pos->next)
    List_ForEach(&gVMemList, pos)
    {
        VMemHead* current = (VMemHead*)pos;
        //满足需要的大小
        if( current->free >= alloc )
        {   //(uint)current->ptr + (current->used + current->free)当前管理节点的空闲区域的末尾
            //然后再减去alloc 需要申请的内存大小，意味着从空闲内存尾部分割
            byte* mem = (byte*)((uint)current->ptr + (current->used + current->free) - alloc);
            
            ret = (VMemHead*)mem;
            ret->used = size;           //新的管理节点的used为申请的size大小内存
            ret->free = 0;              //free成员为0
            ret->ptr = AddrOff(ret, 1); //ptr指针指向used起始位置即 管理头后面
            
            current->free -= alloc;     //原先的节点减去被划分的大小
            
            List_AddAfter((ListNode*)current, (ListNode*)ret);//中部节点插入
            
            break;
        }
    }
    //返回used地址即管理头后面的地址
    return ret ? ret->ptr : NULL;
}

static int VMemFree(void* ptr)
{
    int ret = 0;
    
    if( ptr )
    {
        ListNode* pos = NULL;
        //遍历管理链表
        List_ForEach(&gVMemList, pos)
        {
            VMemHead* current = (VMemHead*)pos;
            //used地址相同
            if( IsEqual(current->ptr, ptr) )
            {
                VMemHead* prev = (VMemHead*)(current->head.prev);
                
                prev->free += current->used + current->free + VM_HEAD_SIZE;
                //双向链表删除节点，注意prev节点要么是VMemAlloc(cur)之后再进行VMemAlloc(prev)
                //要么prev节点是最初的节点
                List_DelNode((ListNode*)current);
                
                ret = 1;
                
                break;
            }
        }
    }
    
    return ret;
}

void MemModInit(byte* mem, uint size)
{
    byte* fmem = mem;
    uint fsize = size / 2;
    byte* vmem = AddrOff(fmem, fsize);
    uint vsize = size - fsize;
    
    FMemInit(fmem, fsize);
    VMemInit(vmem, vsize);
}

void* Malloc(uint size)
{
    void* ret = NULL;
    
    if( size <= FM_ALLOC_SIZE )
    {
        ret = FMemAlloc();
    }
    
    if( !ret ) 
    {
        ret = VMemAlloc(size);
    }
    
    return ret;
}

void Free(void* ptr)
{
    if( ptr )
    {
        if( !FMemFree(ptr) )
        {
            VMemFree(ptr);
        }
    }
}


