%include "common.asm"

global _start
global TimerHandlerEntry
global KeyboardHandlerEntry
global SysCallHandlerEntry
global PageFaultHandlerEntry
global SegmentFaultHandlerEntry

global ReadPort
global WritePort
global ReadPortW
global WritePortW

extern TimerHandler
extern KeyboardHandler
extern SysCallHandler
extern PageFaultHandler
extern SegmentFaultHandler

extern gMemSize
extern gCTaskAddr
extern gGdtInfo
extern gIdtInfo
extern InitInterrupt
extern EnableTimer
extern SendEOI
extern RunTask
extern LoadTask
extern KMain
extern ClearScreen

%macro BeginFSR 0
    cli 
    
    pushad
    
    push ds
    push es
    push fs
    push gs
    
    mov si, ss
    mov ds, si
    mov es, si
    
    mov esp, BaseOfLoader
%endmacro

; 中断操作前 寄存器压栈操作用于保存上下文(寄存器的值)
; 中断操作前esp指向regvalue末尾,  ss,esp,eflag,cs,ip自动压入后esp指向raddr
%macro BeginISR 0
    cli 

    sub esp, 4  ;绕过raddr
    
    pushad
    
    push ds
    push es
    push fs
    push gs
    
    mov si, ss  ; 进入中断服务程序后，数据段ds与附加段es的选择子与堆栈段的选择子一致
    mov ds, si
    mov es, si
    
    mov esp, BaseOfLoader   ; 重新指定中断服务程序使用的内核栈
%endmacro

; 中断操作结束后 esp指向目标任务的起始位置
%macro EndISR 0
    mov esp, [gCTaskAddr]
    
    pop gs      ; 恢复上下文
    pop fs
    pop es
    pop ds
    
    popad
    
    add esp, 4  ; 跳过raddr
    
    iret        ; 必须要iret返回, 高特权级跳回低特权级
%endmacro

[section .text]
[bits 32]
_start:
    mov ebp, 0
    
    call InitGlobal
    call ClearScreen
    call KMain
    
    jmp $
    
;
;    
InitGlobal:
    push ebp
    mov ebp, esp
    
    mov eax, dword [GdtEntry]
    mov [gGdtInfo], eax
    mov eax, dword [GdtSize]
    mov [gGdtInfo + 4], eax
    
    mov eax, dword [IdtEntry]
    mov [gIdtInfo], eax
    mov eax, dword [IdtSize]
    mov [gIdtInfo + 4], eax
    
    mov eax, dword [RunTaskEntry]
    mov dword [RunTask], eax
    
    mov eax, dword [InitInterruptEntry]
    mov dword [InitInterrupt], eax
    
    mov eax, dword [SendEOIEntry]
    mov dword [SendEOI], eax
    
    mov eax, dword [LoadTaskEntry]
    mov dword [LoadTask], eax
    
    mov eax, dword [MemSize]
    mov dword [gMemSize], eax
    
    leave
    
    ret

;
; byte ReadPort(ushort port)
; 
ReadPort:
    push ebp
    mov  ebp, esp
    
    xor eax, eax
    
    mov dx, [ebp + 8]
    in  al, dx ; C语言函数返回值存储在ax中
    
    nop
    nop
    nop
    
    leave
    
    ret

;
; void WritePort(ushort port, byte value)
;
WritePort:
    push ebp
    mov  ebp, esp
    
    xor eax, eax
    
    mov dx, [ebp + 8]
    mov al, [ebp + 12]
    out dx, al
    
    nop
    nop
    nop
    
    leave
    
    ret

;
; void ReadPortW(ushort port, ushort* buf, uint n)
; 
ReadPortW:
    push ebp
    mov  ebp, esp
    
    mov edx, [ebp + 8]   ; port
    mov edi, [ebp + 12]  ; buf
    mov ecx, [ebp + 16]  ; n
    
    cld
    rep insw
    
    nop
    nop
    nop
    
    leave
    
    ret

;
; void WritePortW(ushort port, ushort* buf, uint n)
;
WritePortW:
    push ebp
    mov  ebp, esp
    
    mov edx, [ebp + 8]   ; port
    mov esi, [ebp + 12]  ; buf
    mov ecx, [ebp + 16]  ; n
    
    cld
    rep outsw
    
    nop
    nop
    nop
    
    leave
    
    ret

;
;
TimerHandlerEntry:
BeginISR 
    call TimerHandler
EndISR

;
;
KeyboardHandlerEntry:
BeginISR
    call KeyboardHandler
EndISR

;
;
SysCallHandlerEntry:
BeginISR            ; 保存当前任务上下文
    push edx
    push ecx
    push ebx
    push eax
    call SysCallHandler
    pop eax
    pop ebx
    pop ecx
    pop edx
EndISR              ; 恢复当前任务上下文

;
;
PageFaultHandlerEntry:
BeginFSR
    call PageFaultHandler
EndISR

;
;
SegmentFaultHandlerEntry:
BeginFSR
    call SegmentFaultHandler
EndISR
