
#ifndef KERNEL_H
#define KERNEL_H

#include "type.h"
#include "const.h"

typedef struct {
    ushort limit1;          //段界限0~15
    ushort base1;           //基地址0~15
    byte   base2;           //基地址16~23
    byte   attr1;           //属性第1部分
    byte   attr2_limit2;    //属性第2部分和段界限第2部分16~19
    byte   base3;           //基地址24~31
} Descriptor;

typedef struct {
    Descriptor * const entry;
    const int          size;
} GdtInfo;

typedef struct {
    ushort offset1;
    ushort selector;
    byte   dcount;
    byte   attr;
    ushort offset2;
} Gate;

typedef struct {
    Gate * const entry;
    const int    size;
} IdtInfo;



extern GdtInfo gGdtInfo;
extern IdtInfo gIdtInfo;

int SetDescValue(Descriptor* pDesc, uint base, uint limit, ushort attr);
int GetDescValue(Descriptor* pDesc, uint* pBase, uint* pLimit, ushort* pAttr);
void ConfigPageTable();

#endif
