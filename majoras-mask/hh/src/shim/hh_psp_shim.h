/* POSIX stand-ins for the small PSP SDK surface used by the oot-PSP port files the
 * handheld build reuses (libultra, game glue, scheduler, asset loader, mixer).
 * Every <psp*.h> include in those files resolves here (Makefile.handheld adds
 * -Isrc/port/hh/shim). Semantics are the subset those callers rely on. */
#ifndef HH_PSP_SHIM_H
#define HH_PSP_SHIM_H

#include <stdint.h>
#include <stddef.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef unsigned int SceUInt32;
typedef int SceInt32;
typedef long long SceInt64;
typedef unsigned long long SceUInt64;
typedef int SceOff;
typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);

typedef struct SceKernelThreadInfo {
    SceSize size;
    char name[32];
    int status;
    int currentPriority;
    void* stack;
    int stackSize;
} SceKernelThreadInfo;

#define THREAD_ATTR_USER 0x80000000u
#define THREAD_ATTR_VFPU 0x00004000u
#define PSP_THREAD_ATTR_USER THREAD_ATTR_USER
#define PSP_THREAD_ATTR_VFPU THREAD_ATTR_VFPU

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR 0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
#define PSP_SEEK_SET 0
#define PSP_SEEK_CUR 1
#define PSP_SEEK_END 2
#define PSP_SMEM_Low 0
#define PSP_SMEM_L PSP_SMEM_Low

/* Controller: filled by the handheld input layer (hh_input.c). */
typedef struct SceCtrlData {
    unsigned int TimeStamp;
    unsigned int Buttons;
    unsigned char Lx;
    unsigned char Ly;
    unsigned char Rsrv[6];
} SceCtrlData;

enum {
    PSP_CTRL_SELECT = 0x000001, PSP_CTRL_START = 0x000008, PSP_CTRL_UP = 0x000010,
    PSP_CTRL_RIGHT = 0x000020, PSP_CTRL_DOWN = 0x000040, PSP_CTRL_LEFT = 0x000080,
    PSP_CTRL_LTRIGGER = 0x000100, PSP_CTRL_RTRIGGER = 0x000200, PSP_CTRL_TRIANGLE = 0x001000,
    PSP_CTRL_CIRCLE = 0x002000, PSP_CTRL_CROSS = 0x004000, PSP_CTRL_SQUARE = 0x008000,
    PSP_CTRL_HOME = 0x010000,
};
#define PSP_CTRL_MODE_DIGITAL 0
#define PSP_CTRL_MODE_ANALOG 1

/* threads */
SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, int stackSize,
                             SceUInt attr, void* option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp);
int sceKernelExitDeleteThread(int status);
int sceKernelTerminateDeleteThread(SceUID thid);
int sceKernelDeleteThread(SceUID thid);
int sceKernelChangeThreadPriority(SceUID thid, int priority);
SceUID sceKernelGetThreadId(void);
int sceKernelReferThreadStatus(SceUID thid, SceKernelThreadInfo* info);
int sceKernelDelayThread(SceUInt delay);
/* semaphores */
SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initVal, int maxVal, void* option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout);
int sceKernelPollSema(SceUID semaid, int signal);
/* time */
SceUInt32 sceKernelGetSystemTimeLow(void);
SceInt64 sceKernelGetSystemTimeWide(void);
int sceRtcGetCurrentTick(SceUInt64* tick);
/* memory */
SceUID sceKernelAllocPartitionMemory(SceUID partitionid, const char* name, int type, SceSize size, void* addr);
int sceKernelFreePartitionMemory(SceUID blockid);
void* sceKernelGetBlockHeadAddr(SceUID blockid);
int sceKernelVolatileMemLock(int unk, void** ptr, int* size);
int sceKernelVolatileMemUnlock(int unk);
void sceKernelDcacheWritebackRange(const void* p, unsigned int size);
void sceKernelDcacheInvalidateRange(const void* p, unsigned int size);
void sceKernelDcacheWritebackInvalidateAll(void);
int sceDmacTryMemcpy(void* dst, const void* src, SceSize size);
int scePowerLock(int unk);
int scePowerUnlock(int unk);
/* io */
SceUID sceIoOpen(const char* file, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void* data, SceSize size);
int sceIoWrite(SceUID fd, const void* data, SceSize size);
int sceIoLseek32(SceUID fd, int offset, int whence);
SceInt64 sceIoLseek(SceUID fd, SceInt64 offset, int whence);
int sceIoRemove(const char* file);
int sceIoRename(const char* oldname, const char* newname);
/* ctrl */
int sceCtrlSetSamplingCycle(int cycle);
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(SceCtrlData* pad, int count);
int sceCtrlReadBufferPositive(SceCtrlData* pad, int count);
/* process */
void sceKernelExitGame(void);

#endif
