/* POSIX implementation of the PSP SDK subset in shim/hh_psp_shim.h. */
#include "shim/hh_psp_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- threads ------------------------------------------------------------ */

#define HH_MAX_THREADS 16

typedef struct {
    int used;
    int started;
    pthread_t thread;
    SceKernelThreadEntry entry;
    int priority;
    int stackSize;
    char name[32];
    void* args;
    SceSize argLen;
} HhThread;

static HhThread sThreads[HH_MAX_THREADS]; /* id = index + 1; the main thread is HH_MAX_THREADS + 1 */
static pthread_mutex_t sThreadLock = PTHREAD_MUTEX_INITIALIZER;
static __thread SceUID tCurrentId;

static void* HhThread_Trampoline(void* arg) {
    HhThread* t = arg;

    tCurrentId = (SceUID)(t - sThreads) + 1;
    t->entry(t->argLen, t->args);
    return NULL;
}

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, int stackSize,
                             SceUInt attr, void* option) {
    int i;

    (void)attr;
    (void)option;
    pthread_mutex_lock(&sThreadLock);
    for (i = 0; i < HH_MAX_THREADS; i++) {
        if (!sThreads[i].used) {
            memset(&sThreads[i], 0, sizeof(sThreads[i]));
            sThreads[i].used = 1;
            sThreads[i].entry = entry;
            sThreads[i].priority = initPriority;
            sThreads[i].stackSize = stackSize;
            snprintf(sThreads[i].name, sizeof(sThreads[i].name), "%s", name ? name : "");
            pthread_mutex_unlock(&sThreadLock);
            return i + 1;
        }
    }
    pthread_mutex_unlock(&sThreadLock);
    return -1;
}

int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp) {
    HhThread* t;
    pthread_attr_t attr;
    int ret;

    if (thid < 1 || thid > HH_MAX_THREADS || !sThreads[thid - 1].used) {
        return -1;
    }
    t = &sThreads[thid - 1];
    /* Like the PSP kernel, the argument block is copied for the new thread. */
    t->argLen = arglen;
    t->args = NULL;
    if (arglen != 0 && argp != NULL) {
        t->args = malloc(arglen);
        memcpy(t->args, argp, arglen);
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, t->stackSize < 0x40000 ? 0x40000 : (size_t)t->stackSize);
    ret = pthread_create(&t->thread, &attr, HhThread_Trampoline, t);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        return -1;
    }
    pthread_detach(t->thread);
    t->started = 1;
    return 0;
}

int sceKernelExitDeleteThread(int status) {
    SceUID id = tCurrentId;

    (void)status;
    if (id >= 1 && id <= HH_MAX_THREADS) {
        pthread_mutex_lock(&sThreadLock);
        free(sThreads[id - 1].args);
        sThreads[id - 1].used = 0;
        pthread_mutex_unlock(&sThreadLock);
    }
    pthread_exit(NULL);
    return 0;
}

int sceKernelTerminateDeleteThread(SceUID thid) {
    /* Only used on shutdown paths; the callers stop their loops by flag first. */
    if (thid >= 1 && thid <= HH_MAX_THREADS) {
        pthread_mutex_lock(&sThreadLock);
        sThreads[thid - 1].used = 0;
        pthread_mutex_unlock(&sThreadLock);
    }
    return 0;
}

int sceKernelDeleteThread(SceUID thid) {
    return sceKernelTerminateDeleteThread(thid);
}

int sceKernelChangeThreadPriority(SceUID thid, int priority) {
    if (thid >= 1 && thid <= HH_MAX_THREADS) {
        sThreads[thid - 1].priority = priority;
    }
    return 0;
}

SceUID sceKernelGetThreadId(void) {
    return (tCurrentId != 0) ? tCurrentId : HH_MAX_THREADS + 1;
}

int sceKernelReferThreadStatus(SceUID thid, SceKernelThreadInfo* info) {
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    if (thid >= 1 && thid <= HH_MAX_THREADS && sThreads[thid - 1].used) {
        snprintf(info->name, sizeof(info->name), "%s", sThreads[thid - 1].name);
        info->currentPriority = sThreads[thid - 1].priority;
        info->stackSize = sThreads[thid - 1].stackSize;
        return 0;
    }
    if (thid == HH_MAX_THREADS + 1) {
        snprintf(info->name, sizeof(info->name), "main");
        info->currentPriority = 0x20;
        return 0;
    }
    return -1;
}

int sceKernelDelayThread(SceUInt delay) {
    struct timespec ts = { (time_t)(delay / 1000000u), (long)(delay % 1000000u) * 1000L };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    return 0;
}

/* ---- semaphores --------------------------------------------------------- */

#define HH_MAX_SEMAS 32

typedef struct {
    int used;
    int count;
    int max;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} HhSema;

static HhSema sSemas[HH_MAX_SEMAS];
static pthread_mutex_t sSemaLock = PTHREAD_MUTEX_INITIALIZER;

SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initVal, int maxVal, void* option) {
    int i;

    (void)name;
    (void)attr;
    (void)option;
    pthread_mutex_lock(&sSemaLock);
    for (i = 0; i < HH_MAX_SEMAS; i++) {
        if (!sSemas[i].used) {
            sSemas[i].used = 1;
            sSemas[i].count = initVal;
            sSemas[i].max = maxVal;
            pthread_mutex_init(&sSemas[i].lock, NULL);
            pthread_cond_init(&sSemas[i].cond, NULL);
            pthread_mutex_unlock(&sSemaLock);
            return i + 1;
        }
    }
    pthread_mutex_unlock(&sSemaLock);
    return -1;
}

int sceKernelDeleteSema(SceUID semaid) {
    if (semaid < 1 || semaid > HH_MAX_SEMAS) {
        return -1;
    }
    pthread_mutex_lock(&sSemaLock);
    sSemas[semaid - 1].used = 0;
    pthread_mutex_unlock(&sSemaLock);
    return 0;
}

int sceKernelSignalSema(SceUID semaid, int signal) {
    HhSema* s;

    if (semaid < 1 || semaid > HH_MAX_SEMAS) {
        return -1;
    }
    s = &sSemas[semaid - 1];
    pthread_mutex_lock(&s->lock);
    s->count += signal;
    if (s->max > 0 && s->count > s->max) {
        s->count = s->max;
    }
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout) {
    HhSema* s;
    struct timespec deadline;
    int ret = 0;

    if (semaid < 1 || semaid > HH_MAX_SEMAS) {
        return -1;
    }
    s = &sSemas[semaid - 1];
    if (timeout != NULL) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += *timeout / 1000000u;
        deadline.tv_nsec += (long)(*timeout % 1000000u) * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&s->lock);
    while (s->count < signal) {
        if (timeout != NULL) {
            if (pthread_cond_timedwait(&s->cond, &s->lock, &deadline) == ETIMEDOUT) {
                ret = -1;
                break;
            }
        } else {
            pthread_cond_wait(&s->cond, &s->lock);
        }
    }
    if (ret == 0) {
        s->count -= signal;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

int sceKernelPollSema(SceUID semaid, int signal) {
    HhSema* s;
    int ret = -1;

    if (semaid < 1 || semaid > HH_MAX_SEMAS) {
        return -1;
    }
    s = &sSemas[semaid - 1];
    pthread_mutex_lock(&s->lock);
    if (s->count >= signal) {
        s->count -= signal;
        ret = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

/* ---- time --------------------------------------------------------------- */

static SceInt64 HhNowUsec(void) {
    extern uint64_t port_time_us64(void); /* hh_port.c: one syscall, not two */

    return (SceInt64)port_time_us64();
}

SceUInt32 sceKernelGetSystemTimeLow(void) {
    return (SceUInt32)HhNowUsec();
}

SceInt64 sceKernelGetSystemTimeWide(void) {
    return HhNowUsec();
}

int sceRtcGetCurrentTick(SceUInt64* tick) {
    *tick = (SceUInt64)HhNowUsec();
    return 0;
}

/* ---- memory ------------------------------------------------------------- */

#define HH_MAX_BLOCKS 16
static void* sBlocks[HH_MAX_BLOCKS];

SceUID sceKernelAllocPartitionMemory(SceUID partitionid, const char* name, int type, SceSize size, void* addr) {
    int i;

    (void)partitionid;
    (void)name;
    (void)type;
    (void)addr;
    for (i = 0; i < HH_MAX_BLOCKS; i++) {
        if (sBlocks[i] == NULL) {
            sBlocks[i] = malloc(size);
            return (sBlocks[i] != NULL) ? i + 1 : -1;
        }
    }
    return -1;
}

int sceKernelFreePartitionMemory(SceUID blockid) {
    if (blockid < 1 || blockid > HH_MAX_BLOCKS) {
        return -1;
    }
    free(sBlocks[blockid - 1]);
    sBlocks[blockid - 1] = NULL;
    return 0;
}

void* sceKernelGetBlockHeadAddr(SceUID blockid) {
    return (blockid >= 1 && blockid <= HH_MAX_BLOCKS) ? sBlocks[blockid - 1] : NULL;
}

/* The PSP's "volatile" 4 MB partition does not exist here: report failure so
 * callers take their normal-heap path. */
int sceKernelVolatileMemLock(int unk, void** ptr, int* size) {
    (void)unk;
    (void)ptr;
    (void)size;
    return -1;
}

int sceKernelVolatileMemUnlock(int unk) {
    (void)unk;
    return 0;
}

void sceKernelDcacheWritebackRange(const void* p, unsigned int size) {
    (void)p;
    (void)size;
}

void sceKernelDcacheInvalidateRange(const void* p, unsigned int size) {
    (void)p;
    (void)size;
}

void sceKernelDcacheWritebackInvalidateAll(void) {
}

int sceDmacTryMemcpy(void* dst, const void* src, SceSize size) {
    memcpy(dst, src, size);
    return 0;
}

int scePowerLock(int unk) {
    (void)unk;
    return 0;
}

int scePowerUnlock(int unk) {
    (void)unk;
    return 0;
}

/* ---- io ----------------------------------------------------------------- */

SceUID sceIoOpen(const char* file, int flags, int mode) {
    int oflags = 0;
    int fd;

    switch (flags & PSP_O_RDWR) {
        case PSP_O_WRONLY:
            oflags = O_WRONLY;
            break;
        case PSP_O_RDWR:
            oflags = O_RDWR;
            break;
        default:
            oflags = O_RDONLY;
            break;
    }
    if (flags & PSP_O_CREAT) {
        oflags |= O_CREAT;
    }
    if (flags & PSP_O_TRUNC) {
        oflags |= O_TRUNC;
    }
    if (flags & PSP_O_APPEND) {
        oflags |= O_APPEND;
    }
    fd = open(file, oflags, mode ? mode : 0644);
    return (fd >= 0) ? fd : -1;
}

int sceIoClose(SceUID fd) {
    return close(fd);
}

int sceIoRead(SceUID fd, void* data, SceSize size) {
    return (int)read(fd, data, size);
}

int sceIoWrite(SceUID fd, const void* data, SceSize size) {
    return (int)write(fd, data, size);
}

int sceIoLseek32(SceUID fd, int offset, int whence) {
    return (int)lseek(fd, offset, whence);
}

SceInt64 sceIoLseek(SceUID fd, SceInt64 offset, int whence) {
    return (SceInt64)lseek(fd, (off_t)offset, whence);
}

int sceIoRemove(const char* file) {
    return unlink(file);
}

int sceIoRename(const char* oldname, const char* newname) {
    return rename(oldname, newname);
}

/* ---- ctrl: hh_input.c owns the real state ---------------------------------- */

int sceCtrlSetSamplingCycle(int cycle) {
    (void)cycle;
    return 0;
}

int sceCtrlSetSamplingMode(int mode) {
    (void)mode;
    return 0;
}

void HhInput_Read(SceCtrlData* pad);

int sceCtrlPeekBufferPositive(SceCtrlData* pad, int count) {
    (void)count;
    HhInput_Read(pad);
    return 1;
}

int sceCtrlReadBufferPositive(SceCtrlData* pad, int count) {
    return sceCtrlPeekBufferPositive(pad, count);
}

void sceKernelExitGame(void) {
    exit(0);
}
