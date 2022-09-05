#include "hdraw.h"
#include "fs.h"
#include "utility.h"
#include "list.h"

#ifdef DTFSER
#include <malloc.h>
#define Malloc malloc
#define Free free
#else
#include "memory.h"
#endif

#define FS_MAGIC       "fengyunFS-v1.0"
#define ROOT_MAGIC     "ROOT"
#define HEADER_SCT_IDX 0
#define ROOT_SCT_IDX   1
#define FIXED_SCT_SIZE 2
#define SCT_END_FLAG   ((uint)-1)
#define FE_BYTES       sizeof(FileEntry)
#define FD_BYTES       sizeof(FileDesc)
#define FE_ITEM_CNT    (SECT_SIZE / FE_BYTES)
#define MAP_ITEM_CNT   (SECT_SIZE / sizeof(uint))

//存储于0号引导区
typedef struct
{
    byte forJmp[4];         //预留给jmp指令使用,万一0号扇区需要存储引导程序呢
    char magic[32];         //存储字符串标识现在是什么文件系统
    uint sctNum;            //多少扇区可以使用
    uint mapSize;           //扇区分配表的大小 
    uint freeNum;           //空闲链表长度
    uint freeBegin;         //空闲链表开始
} FSHeader;

//存储于1号根目录区
typedef struct
{
    char magic[32];         //"ROOT"
    uint sctBegin;          //根目录的开始扇区,本OS初始化时设置为1
    uint sctNum;            //占用多少个扇区,本OS初始化设置为0非法值,表示还没有FileEntry
    uint lastBytes;			//最后一个扇区用了多少字节
} FSRoot;

typedef struct
{
    char name[32];          //文件名
    uint sctBegin;          //文件起始扇区
    uint sctNum;            //多少个扇区
    uint lastBytes;         //
    uint type;              //是文件还是目录 存储的是用户数据还是文件相关的数据
    uint inSctIdx;          //硬盘的哪一个扇区
    uint inSctOff;			//扇区内偏移位置
    uint reserved[2];		//预留
} FileEntry;

typedef struct
{
    ListNode head;          //链表--文件描述符最后也要构成一个链表
    FileEntry fe;           //FileEntry必备
    uint objIdx;            //文件读写位置-哪一个扇区
    uint offset;            //文件读写位置-扇区内偏移位置
    uint changed;           //标志文件已经改变，关闭文件时changed若为1则将cache写入到硬盘
    byte cache[SECT_SIZE];  //文件缓冲区-512字节大小
} FileDesc;

typedef struct
{
    uint* pSct;             //指向对应管理单元所在扇区
    uint sctIdx;            //原始绝对扇区号
    uint sctOff;            //对应的分配单元(扇区分配表中)的在哪个扇区
    uint idxOff;            //扇区中偏移位置
} MapPos;

static List gFDList = {0};  //全局已经打开的文件描述符链表

void FSModInit()
{
    HDRawModInit();

    List_Init(&gFDList);
}

static void* ReadSector(uint si)
{
    void* ret = NULL;

    if( si != SCT_END_FLAG )
    {
        ret = Malloc(SECT_SIZE);

        if( !(ret && HDRawRead(si, (byte*)ret)) )
        {
            Free(ret);
            ret = NULL;
        }
    }

    return ret;
}

static MapPos FindInMap(uint si)
{
    MapPos ret = {0};
    FSHeader* header = (si != SCT_END_FLAG) ? ReadSector(HEADER_SCT_IDX) : NULL;

    if( header )
    {
        //绝对地址转换位相对地址
        uint offset = si - header->mapSize - FIXED_SCT_SIZE;
        uint sctOff = offset / MAP_ITEM_CNT;
        uint idxOff = offset % MAP_ITEM_CNT;
        uint* ps = ReadSector(sctOff + FIXED_SCT_SIZE);

        if( ps )
        {
            ret.pSct = ps;
            ret.sctIdx = si;
            ret.sctOff = sctOff;
            ret.idxOff = idxOff;
        }
    }

    Free(header);

    return ret;
}

static uint AllocSector()
{
    uint ret = SCT_END_FLAG;
    FSHeader* header = ReadSector(HEADER_SCT_IDX);

    if( header && (header->freeBegin != SCT_END_FLAG) )
    {
        //去根目录区拿出链表头,找出分配单元的具体位置
        MapPos mp = FindInMap(header->freeBegin);

        if( mp.pSct )
        {
            uint* pInt = AddrOff(mp.pSct, mp.idxOff);   //取出分配单元
            uint next = *pInt;                          //下一个分配单元的位置
            uint flag = 1;  

            ret = header->freeBegin;
            //更新header信息 空闲扇区位置 空闲扇区数量
            header->freeBegin = next + FIXED_SCT_SIZE + header->mapSize;
            header->freeNum--;
            //当前分配单元标记为不可用
            *pInt = SCT_END_FLAG;
            //外存数据更新
            flag = flag && HDRawWrite(HEADER_SCT_IDX, (byte*)header);
            flag = flag && HDRawWrite(mp.sctOff + FIXED_SCT_SIZE, (byte*)mp.pSct);

            if( !flag )
            {
                ret = SCT_END_FLAG;
            }
        }

        Free(mp.pSct);
    }

    Free(header);

    return ret;
}

static uint FreeSector(uint si)
{
    FSHeader* header = (si != SCT_END_FLAG) ? ReadSector(HEADER_SCT_IDX) : NULL;
    uint ret = 0;

    if( header )
    {
        //获取扇区分配单元的信息
        MapPos mp = FindInMap(si);

        if( mp.pSct )
        {
            uint* pInt = AddrOff(mp.pSct, mp.idxOff);
            //此扇区分配管理单元插入到空闲扇区的头部
            *pInt = header->freeBegin - FIXED_SCT_SIZE - header->mapSize;

            header->freeBegin = si;
            header->freeNum++;

            ret = HDRawWrite(HEADER_SCT_IDX, (byte*)header) &&
                  HDRawWrite(mp.sctOff + FIXED_SCT_SIZE, (byte*)mp.pSct);
        }

        Free(mp.pSct);
    }

    Free(header);

    return ret;
}

//获取当前扇区的后继扇区 通过读取扇区分配表可以知道后继节点
static uint NextSector(uint si)
{
    FSHeader* header = (si != SCT_END_FLAG) ? ReadSector(HEADER_SCT_IDX) : NULL;
    uint ret = SCT_END_FLAG;

    if( header )
    {   //获取分配单元位置
        MapPos mp = FindInMap(si);

        if( mp.pSct )
        {
            uint* pInt = AddrOff(mp.pSct, mp.idxOff);

            if( *pInt != SCT_END_FLAG )
            {
                ret = *pInt + header->mapSize + FIXED_SCT_SIZE;
            }
        }

        Free(mp.pSct);
    }

    Free(header);

    return ret;
}

//查找链表的最后一个扇区, 空闲链表、已经分配的链表
static uint FindLast(uint sctBegin)
{
    uint ret = SCT_END_FLAG;
    uint next = sctBegin;

    while( next != SCT_END_FLAG )
    {
        ret = next;
        next = NextSector(next);
    }

    return ret;
}

//查找链表的前一个节点
static uint FindPrev(uint sctBegin, uint si)
{
    uint ret = SCT_END_FLAG;
    uint next = sctBegin;

    while( (next != SCT_END_FLAG) && (next != si) )
    {
        ret = next;
        next = NextSector(next);
    }

    if( next == SCT_END_FLAG )
    {
        ret = SCT_END_FLAG;
    }

    return ret;
}

//查找链表当中的第n号扇区
static uint FindIndex(uint sctBegin, uint idx)
{
    uint ret = sctBegin;
    uint i = 0;

    while( (i < idx) && (ret != SCT_END_FLAG) )
    {
        ret = NextSector(ret);

        i++;
    }

    return ret;
}

//标记扇区目标扇区不可用，并且是最后一个扇区了
static uint MarkSector(uint si)
{
    uint ret = (si == SCT_END_FLAG) ? 1 : 0;
    MapPos mp = FindInMap(si);

    if( mp.pSct )
    {
        uint *pInt = AddrOff(mp.pSct, mp.idxOff);

        *pInt = SCT_END_FLAG;

        ret = HDRawWrite(mp.sctOff + FIXED_SCT_SIZE, (byte*)mp.pSct);
    }

    Free(mp.pSct);

    return ret;
}

static void AddToLast(uint sctBegin, uint si)
{
    //找到此文件的last扇区
    uint last = FindLast(sctBegin);

    if( last != SCT_END_FLAG )
    {
        //管理单元
        MapPos lmp = FindInMap(last);
        MapPos smp = FindInMap(si);

        if( lmp.pSct && smp.pSct )
        {
            //两个管理单元位于同一个扇区，只需要写一次硬盘即可
            if( lmp.sctOff == smp.sctOff )
            {
                //拿到last管理单元
                uint* pInt = AddrOff(lmp.pSct, lmp.idxOff);
				//si的相对地址赋给 last管理单元
                *pInt = lmp.sctOff * MAP_ITEM_CNT + smp.idxOff;
				//拿到尾部si对应的管理单元
                pInt = AddrOff(lmp.pSct, smp.idxOff);
				//赋值
                *pInt = SCT_END_FLAG;
				//仅仅写一次即可
                HDRawWrite(lmp.sctOff + FIXED_SCT_SIZE, (byte*)lmp.pSct);
            }
            else
            {
                //拿到last管理单元
                uint* pInt = AddrOff(lmp.pSct, lmp.idxOff);
				//si的相对地址赋值给 last管理单元
                *pInt = smp.sctOff * MAP_ITEM_CNT + smp.idxOff;

                pInt = AddrOff(smp.pSct, smp.idxOff);

                *pInt = SCT_END_FLAG;
				//操作了两个扇区需要写两次硬盘
                HDRawWrite(lmp.sctOff + FIXED_SCT_SIZE, (byte*)lmp.pSct);
                HDRawWrite(smp.sctOff + FIXED_SCT_SIZE, (byte*)smp.pSct);
            }
        }

        Free(lmp.pSct);
        Free(smp.pSct);
    }
}

//
static uint CheckStorage(FSRoot* fe)
{
    uint ret = 0;
    //最后一个扇区是512字节需要扩展容量
    if( fe->lastBytes == SECT_SIZE )
    {
        uint si = AllocSector();

        if( si != SCT_END_FLAG )
        {
            //当前数据链表是空链表，则新申请的扇区是链表第一个节点
            if( fe->sctBegin == SCT_END_FLAG )
            {
                fe->sctBegin = si;
            }
            else//否则加入到尾部
            {
                AddToLast(fe->sctBegin, si);
            }

            fe->sctNum++;
            fe->lastBytes = 0;

            ret = 1;
        }
    }

    return ret;
}

static uint CreateFileEntry(const char* name, uint sctBegin, uint lastBytes)
{
    uint ret = 0;
    uint last = FindLast(sctBegin);         //链表最后一个扇区
    FileEntry* feBase = NULL;               //并且将扇区从硬盘读入内存

    if( (last != SCT_END_FLAG) && (feBase = (FileEntry*)ReadSector(last)) )
    {
        //要在目标扇区写入新的FileEntry值
        //偏移位置做除法即可得到
        uint offset = lastBytes / FE_BYTES;
        FileEntry* fe = AddrOff(feBase, offset);
        //写入数据
        StrCpy(fe->name, name, sizeof(fe->name) - 1);

        fe->type = 0;
        fe->sctBegin = SCT_END_FLAG;
        fe->sctNum = 0;
        fe->inSctIdx = last;
        fe->inSctOff = offset;
        fe->lastBytes = SECT_SIZE;
        //新的FileEntry已经写入硬盘
        ret = HDRawWrite(last, (byte*)feBase);
    }

    Free(feBase);

    return ret;
}

static uint CreateInRoot(const char* name)
{
    FSRoot* root = (FSRoot*)ReadSector(ROOT_SCT_IDX);
    uint ret = 0;

    if( root )
    {   
        //确保root空间足够
        CheckStorage(root);
        //创建一个新文件
        if( CreateFileEntry(name, root->sctBegin, root->lastBytes) )
        {
            root->lastBytes += FE_BYTES;

            ret = HDRawWrite(ROOT_SCT_IDX, (byte*)root);
        }
    }

    Free(root);

    return ret;
}

//查找的名字,位置,次数
static FileEntry* FindInSector(const char* name, FileEntry* feBase, uint cnt)
{
    FileEntry* ret = NULL;
    uint i = 0;

    for(i=0; i<cnt; i++)
    {
        FileEntry* fe = AddrOff(feBase, i);

        if( StrCmp(fe->name, name, -1) )
        {
            ret = (FileEntry*)Malloc(FE_BYTES);

            if( ret )
            {
                *ret = *fe;
            }

            break;
        }
    }

    return ret;
}

static FileEntry* FindFileEntry(const char* name, uint sctBegin, uint sctNum, uint lastBytes)
{
    FileEntry* ret = NULL;
    uint next = sctBegin;
    uint i = 0;
    //遍历数据链表,前n-1个扇区是完全利用了
    for(i=0; i<(sctNum-1); i++)
    {
        FileEntry* feBase = (FileEntry*)ReadSector(next);

        if( feBase )
        {
            //查找的名字,位置,次数
            ret = FindInSector(name, feBase, FE_ITEM_CNT);
        }

        Free(feBase);

        if( !ret )
        {
            next = NextSector(next);
        }
        else
        {
            break;
        }
    }
    //最后一个扇区找,最后一个扇区不一定充分利用,计算一下找了多少次
    if( !ret )
    {
        uint cnt = lastBytes / FE_BYTES;
        FileEntry* feBase = (FileEntry*)ReadSector(next);

        if( feBase )
        {
            ret = FindInSector(name, feBase, cnt);
        }

        Free(feBase);
    }

    return ret;
}

static FileEntry* FindInRoot(const char* name)
{
    FileEntry* ret = NULL;
    //root读取到内存中
    FSRoot* root = (FSRoot*)ReadSector(ROOT_SCT_IDX);

    if( root && root->sctNum )
    {
        ret = FindFileEntry(name, root->sctBegin, root->sctNum, root->lastBytes);
    }

    Free(root);

    return ret;
}

//根目录区创建一个文件
uint FCreate(const char* fn)
{
    uint ret = FExisted(fn);

    if( ret == FS_NONEXISTED )
    {
        ret = CreateInRoot(fn) ? FS_SUCCEED : FS_FAILED;
    }

    return ret;
}

uint FExisted(const char* fn)
{
    uint ret = FS_FAILED;

    if( fn )
    {
        FileEntry* fe = FindInRoot(fn);

        ret = fe ? FS_EXISTED : FS_NONEXISTED;

        Free(fe);
    }

    return ret;
}

static uint IsOpened(const char* name)
{
    uint ret = 0;
    ListNode* pos = NULL;

    List_ForEach(&gFDList, pos)
    {
        FileDesc* fd = (FileDesc*)pos;

        if( StrCmp(fd->fe.name, name, -1) )
        {
            ret = 1;
            break;
        }
    }

    return ret;
}

static uint FreeFile(uint sctBegin)
{
    uint slider = sctBegin;
    uint ret = 0;
    //将此FileEntry开始到结束的扇区全都释放掉
    while( slider != SCT_END_FLAG )
    {   
        uint next = NextSector(slider);

        ret += FreeSector(slider);

        slider = next;
    }
    //返回成功释放的总数
    return ret;
}

static void MoveFileEntry(FileEntry* dst, FileEntry* src)
{
    uint inSctIdx = dst->inSctIdx;
    uint inSctOff = dst->inSctOff;

    *dst = *src;

    dst->inSctIdx = inSctIdx;       //此FileEntry位于硬盘的哪一个扇区
    dst->inSctOff = inSctOff;       //此FileEntry位于扇区的偏移位置
}

static uint AdjustStorage(FSRoot* fe)
{
    uint ret = 0;

    if( !fe->lastBytes )            //最后一个扇区是否完全空闲
    {   //查找最后一个扇区和倒数第二个扇区
        uint last = FindLast(fe->sctBegin);
        uint prev = FindPrev(fe->sctBegin, last);
        //释放最后一个扇区并且标记倒数第二个扇区为最后一个扇区
        if( FreeSector(last) && MarkSector(prev) )
        {
            fe->sctNum--;
            fe->lastBytes = SECT_SIZE;

            if( !fe->sctNum )
            {
                fe->sctBegin = SCT_END_FLAG;
            }

            ret = 1;
        }
    }

    return ret;
}

//数据链表中要抹除的最后n个字节，对FileEntry的lastbyte操作
static uint EraseLast(FSRoot* fe, uint bytes)
{
    uint ret = 0;

    while( fe->sctNum && bytes )
    {
        //小于最后扇区数据，直接抹除
        if( bytes < fe->lastBytes )
        {
            fe->lastBytes -= bytes;

            ret += bytes;

            bytes = 0;
        }
        else
        {
            //最后一个扇区的数据要全部抹除
            bytes -= fe->lastBytes;

            ret += fe->lastBytes;

            fe->lastBytes = 0;
            //将最后一个扇区归还到空闲扇区区
            AdjustStorage(fe);
        }
    }

    return ret;
}

static uint DeleteInRoot(const char* name)
{
    FSRoot* root = (FSRoot*)ReadSector(ROOT_SCT_IDX);
    //查找到对应的FileEntry
    FileEntry* fe = FindInRoot(name);
    uint ret = 0;

    if( root && fe )
    {   
        //查找最后一个扇区并且将最后一个扇区读取到内存中
        uint last = FindLast(root->sctBegin);
        //目标FileEntry所在的扇区也要读取到内存中
        FileEntry* feTarget = ReadSector(fe->inSctIdx);
        //将最后一个扇区读到内存中
        FileEntry* feLast = (last != SCT_END_FLAG) ? ReadSector(last) : NULL;

        if( feTarget && feLast )
        {
            //定位最后一个FileEntry
            uint lastOff = root->lastBytes / FE_BYTES - 1;
            //读取最后一个扇区的最后一个FileEntry和目标FileEntry
            FileEntry* lastItem = AddrOff(feLast, lastOff);
            FileEntry* targetItem = AddrOff(feTarget, fe->inSctOff);
            //将数据链表的每一个扇区释放扇区，本质是链表遍历
            FreeFile(targetItem->sctBegin);
            //移动FileEntry的值
            MoveFileEntry(targetItem, lastItem);
            //FileEntry对应的数据链表中要抹除的最后n个字节，是对FileEntry的lastbyte操作
            EraseLast(root, FE_BYTES);
            //一定要写回硬盘
            ret = HDRawWrite(ROOT_SCT_IDX, (byte*)root) &&
                    HDRawWrite(fe->inSctIdx, (byte*)feTarget);
        }

        Free(feTarget);
        Free(feLast);
    }

    Free(root);
    Free(fe);

    return ret;
}

uint FOpen(const char *fn)
{
    FileDesc* ret = NULL;
    //文件名不为空且文件未被打开
    if( fn && !IsOpened(fn) )
    {
        FileEntry* fe = NULL;
        //分配文件描述符并且获取FileEntry
        ret = (FileDesc*)Malloc(FD_BYTES);
        fe = ret ? FindInRoot(fn) : NULL;

        if( ret && fe )
        {
            ret->fe = *fe;
            ret->objIdx = SCT_END_FLAG;
            ret->offset = SECT_SIZE;
            ret->changed = 0;

            List_Add(&gFDList, (ListNode*)ret);
        }

        Free(fe);
    }

    return (uint)ret;
}

static uint IsFDValid(FileDesc* fd)
{
    uint ret = 0;
    ListNode* pos = NULL;

    List_ForEach(&gFDList, pos)
    {
        if( IsEqual(pos, fd) )
        {
            ret = 1;
            break;
        }
    }

    return ret;
}

static uint FlushCache(FileDesc* fd)
{
    uint ret = 1;

    if( fd->changed )
    {
        uint sctIdx = FindIndex(fd->fe.sctBegin, fd->objIdx);

        ret = 0;
        //找到写入的硬盘的第几个扇区并且写入扇区
        if( (sctIdx != SCT_END_FLAG) && (ret = HDRawWrite(sctIdx, fd->cache)) )
        {
            fd->changed = 0;
        }
    }

    return ret;
}

static uint FlushFileEntry(FileEntry* fe)
{
    uint ret = 0;
    //将FileEntry读取到内存中
    FileEntry* feBase = ReadSector(fe->inSctIdx);
    FileEntry* feInSct = AddrOff(feBase, fe->inSctOff);

    *feInSct = *fe;
    //修改并且写回硬盘
    ret = HDRawWrite(fe->inSctIdx, (byte*)feBase);

    Free(feBase);

    return ret;
}

static uint ToFlush(FileDesc* fd)
{
    return FlushCache(fd) && FlushFileEntry(&fd->fe);
}

void FClose(uint fd)
{
    FileDesc* pf = (FileDesc*)fd;
    //文件描述符是否合法
    if( IsFDValid(pf) )
    {   //写到硬盘上
        ToFlush(pf);
        //链表删除
        List_DelNode((ListNode*)pf);

        Free(pf);
    }
}
//链表中的第idx个扇区数据读入缓冲区中
static uint ReadToCache(FileDesc* fd, uint idx)
{
    uint ret = 0;

    if( idx < fd->fe.sctNum )
    {
        uint sctIdx = FindIndex(fd->fe.sctBegin, idx);

        ToFlush(fd);

        if( (sctIdx != SCT_END_FLAG) && (ret = HDRawRead(sctIdx, fd->cache)) )
        {
            fd->objIdx = idx;
            fd->offset = 0;
            fd->changed = 0;
        }
    }

    return ret;
}

static uint PrepareCache(FileDesc* fd, uint objIdx)
{
    //文件是否需要扩容
    CheckStorage(&fd->fe);
    //指定扇区数据读入缓冲区中
    return ReadToCache(fd, objIdx);
}

static uint CopyToCache(FileDesc* fd, byte* buf, uint len)
{
    uint ret = -1;

    if( fd->objIdx != SCT_END_FLAG )
    {   //计算能向缓冲区写入的最大数据量
        uint n = SECT_SIZE - fd->offset;
        byte* p = AddrOff(fd->cache, fd->offset);

        n = (n < len) ? n : len;

        MemCpy(p, buf, n);

        fd->offset += n;
        fd->changed = 1;
        //更新数据链表的最后一个扇区的数据总量，如果offset超过lastbyte更新lastbyte
        if( ((fd->fe.sctNum - 1) == fd->objIdx) && (fd->fe.lastBytes < fd->offset) )
        {
            fd->fe.lastBytes = fd->offset;
        }

        ret = n;
    }

    return ret;
}

static uint ToWrite(FileDesc* fd, byte* buf, uint len)
{
    uint ret = 1;
    uint i = 0;
    uint n = 0;

    while( (i < len) && ret )
    {
        //p初始时指向buf开始位置
        byte* p = AddrOff(buf, i);

        if( fd->offset == SECT_SIZE )
        {
            //文件要写入的扇区内offset=512时，需要扩容一个新扇区，读取文件数据链表的下一个扇区
            ret = PrepareCache(fd, fd->objIdx + 1);
        }

        if( ret )
        {   //数据写入缓冲区
            n = CopyToCache(fd, p, len - i);

            i += n;
        }
    }

    ret = i;

    return ret;
}

uint FWrite(uint fd, byte* buf, uint len)
{
    uint ret = -1;

    if( IsFDValid((FileDesc*)fd) && buf )
    {
        ret = ToWrite((FileDesc*)fd, buf, len);
    }

    return ret;
}

uint FDelete(const char* fn)
{
    return fn && !IsOpened(fn) && DeleteInRoot(fn) ? FS_SUCCEED : FS_FAILED;
}

uint FSFormat()
{
    FSHeader* header = (FSHeader*)Malloc(SECT_SIZE);        //引导区
    FSRoot* root = (FSRoot*)Malloc(SECT_SIZE);              //根目录区
    uint* p = (uint*)Malloc(MAP_ITEM_CNT * sizeof(uint));   //操作扇区分配表的每一个分配单元
    uint ret = 0;

    if( header && root && p )
    {
        uint i = 0;
        uint j = 0;
        uint current = 0;

        //给引导区的内容赋值
        StrCpy(header->magic, FS_MAGIC, sizeof(header->magic)-1);
        header->sctNum = HDRawSectors();
        header->mapSize = (header->sctNum - FIXED_SCT_SIZE) / 129 + !!((header->sctNum - FIXED_SCT_SIZE) % 129);
        header->freeNum = header->sctNum - header->mapSize - FIXED_SCT_SIZE;
        header->freeBegin = FIXED_SCT_SIZE + header->mapSize;
        //注意一定要写回硬盘
        ret = HDRawWrite(HEADER_SCT_IDX, (byte*)header);

        //给根目录区的相关成员赋值
        StrCpy(root->magic, ROOT_MAGIC, sizeof(root->magic)-1);
        root->sctNum = 0;               //根目录占用的扇区数目为0
        root->sctBegin = SCT_END_FLAG;  //标记为非法扇区
        root->lastBytes = SECT_SIZE;
        //注意一定要写回硬盘
        ret = ret && HDRawWrite(ROOT_SCT_IDX, (byte*)root);

        //针对于扇区分配表(2~n 的扇区)的每个分配单元赋值
        for(i=0; ret && (i<header->mapSize) && (current<header->freeNum); i++)
        {   
            //每个扇区的128个的每个分配单元赋值
            for(j=0; j<MAP_ITEM_CNT; j++)
            {
                uint* pInt = AddrOff(p, j);

                if( current < header->freeNum )
                {
                    *pInt = current + 1;

                    if( current == (header->freeNum - 1) )
                    {
                        *pInt = SCT_END_FLAG;
                    }

                    current++;
                }
                else
                {
                    break;
                }
            }
            //写回硬盘
            ret = ret && HDRawWrite(i + FIXED_SCT_SIZE, (byte*)p);
        }
    }

    Free(header);
    Free(root);
    Free(p);

    return ret;
}

uint FSIsFormatted()
{
    uint ret = 0;
    FSHeader* header = (FSHeader*)ReadSector(HEADER_SCT_IDX);
    FSRoot* root = (FSRoot*)ReadSector(ROOT_SCT_IDX);

    if( header && root )
    {
        ret = StrCmp(header->magic, FS_MAGIC, -1) &&
                (header->sctNum == HDRawSectors()) &&
                StrCmp(root->magic, ROOT_MAGIC, -1);
    }

    Free(header);
    Free(root);

    return ret;
}

uint FRename(const char* ofn, const char* nfn)
{
    uint ret = FS_FAILED;
    //未被打开且新名字不为空
    if( ofn && !IsOpened(ofn) && nfn )
    {   
        FileEntry* ofe = FindInRoot(ofn);
        FileEntry* nfe = FindInRoot(nfn);
        //目标文件存在且新名字文件名也没被占用
        if( ofe && !nfe )
        {   //拷贝名字
            StrCpy(ofe->name, nfn, sizeof(ofe->name) - 1);
            //写回硬盘
            if( FlushFileEntry(ofe) )
            {
                ret = FS_SUCCEED;
            }
        }

        Free(ofe);
        Free(nfe);
    }

    return ret;
}

//文件长度 (n-1)*512+lastbytes
static uint GetFileLen(FileDesc* fd)
{
    uint ret = 0;

    if( fd->fe.sctBegin != SCT_END_FLAG )
    {
        ret = (fd->fe.sctNum - 1) * SECT_SIZE + fd->fe.lastBytes;
    }

    return ret;
}

//文件读写指针的位置 
static uint GetFilePos(FileDesc* fd)
{
    uint ret = 0;

    if( fd->objIdx != SCT_END_FLAG )
    {
        ret = fd->objIdx * SECT_SIZE + fd->offset;
    }

    return ret;
}

static uint CopyFromCache(FileDesc* fd, byte* buf, uint len)
{
    uint ret = (fd->objIdx != SCT_END_FLAG);

    if( ret )
    {   //计算当前缓冲区可提供的最大数据量以及数据起始位置
        uint n = SECT_SIZE - fd->offset;
        byte* p = AddrOff(fd->cache, fd->offset);

        n = (n < len) ? n : len;

        MemCpy(buf, p, n);

        fd->offset += n;

        ret = n;
    }

    return ret;
}

static uint ToRead(FileDesc* fd, byte* buf, uint len)
{
    //计算最大可读取数据量
    uint ret = -1;
    uint n = GetFileLen(fd) - GetFilePos(fd);
    uint i = 0;

    len = (len < n) ? len : n;
    //循环读取
    while( (i < len) && ret )
    {
        byte* p = AddrOff(buf, i);
        //缓冲区数据读完了，就需要从硬盘读入文件数据链表的下一个扇区到缓冲区中
        if( fd->offset == SECT_SIZE )
        {
            ret = PrepareCache(fd, fd->objIdx + 1);
        }

        if( ret )
        {   //从缓冲区读取数据
            n = CopyFromCache(fd, p, len - i); //len-i 还需要读取多少数据
        }

        i += n;
    }

    ret = i;

    return ret;
}

uint FRead(uint fd, byte* buf, uint len)
{
    uint ret = -1;

    if( IsFDValid((FileDesc*)fd) && buf )
    {
        ret = ToRead((FileDesc*)fd, buf, len);
    }

    return ret;
}

static uint ToLocate(FileDesc* fd, uint pos)
{
    uint ret = -1;
    uint len = GetFileLen(fd);
    //计算移动后读写指针的位置，不可超出文件长度
    pos = (pos < len) ? pos : len;

    {   //计算新位置在哪里
        uint objIdx = pos / SECT_SIZE;
        uint offset = pos % SECT_SIZE;
        uint sctIdx = FindIndex(fd->fe.sctBegin, objIdx);//文件系统中的位置

        ToFlush(fd);
        //flush后在读取数据
        if( (sctIdx != SCT_END_FLAG) && HDRawRead(sctIdx, fd->cache) )
        {
            fd->objIdx = objIdx;
            fd->offset = offset;

            ret = pos;
        }
    }

    return ret;
}

//擦除文件尾部n个字节
uint FErase(uint fd, uint bytes)
{
    uint ret = 0;
    FileDesc* pf = (FileDesc*)fd;

    if( IsFDValid(pf) )
    {
        uint pos = GetFilePos(pf);
        uint len = GetFileLen(pf);

        ret = EraseLast(&pf->fe, bytes);

        len -= ret;

        if( ret && (pos > len) )
        {
            ToLocate(pf, len);
        }
    }

    return ret;
}
//移动文件指针
uint FSeek(uint fd, uint pos)
{
    uint ret = -1;
    FileDesc* pf = (FileDesc*)fd;

    if( IsFDValid(pf) )
    {
        ret = ToLocate(pf, pos);
    }

    return ret;
}
//文件长度
uint FLength(uint fd)
{
    uint ret = -1;
    FileDesc* pf = (FileDesc*)fd;

    if( IsFDValid(pf) )
    {
        ret = GetFileLen(pf);
    }

    return ret;
}
//读写指针位置
uint FTell(uint fd)
{
    uint ret = -1;
    FileDesc* pf = (FileDesc*)fd;

    if( IsFDValid(pf) )
    {
        ret = GetFilePos(pf);
    }

    return ret;
}
//缓冲区数据写入硬盘
uint FFlush(uint fd)
{
    uint ret = -1;
    FileDesc* pf = (FileDesc*)fd;

    if( IsFDValid(pf) )
    {
        ret = ToFlush(pf);
    }

    return ret;
}
