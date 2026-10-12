#include <pspsdk.h>
#include <pspkernel.h>
#include <pspsysevent.h>

#include <stdio.h>
#include <string.h>
#include <pspge.h>

PSP_MODULE_INFO("pspDveManager_Module", 0x1006, 1, 0);

int sceHprmIsHeadphoneExist();
int sceImposeSetVideoOutMode(int, int, int);
int sceDve_driver_DEB2F80C(int);
int sceDve_driver_93828323(int);
int sceDve_driver_0B85524C(int);
int sceDve_driver_A265B504(int, int, int);
int sceGeEdramSetSizeDriver(int);
int sceGeEdramSetSizeUser(int);

#define PSP_SLIM_EDRAM_SIZE (4 * 1024 * 1024)

static unsigned int sEdramRequestSize;
static int sEdramRequestResult = -1;

#define RETURN(x) res = x; pspSdkSetK1(k1); return x

int pspDveMgrCheckVideoOut() {
    int k1 = pspSdkSetK1(0);
    int intr = sceKernelCpuSuspendIntr();

    /* Warning: NID changed between 3.60 and 3.71. */
    int cable = sceHprmIsHeadphoneExist();

    sceKernelCpuResumeIntr(intr);
    pspSdkSetK1(k1);
    return cable;
}

int pspDveMgrSetVideoOut(int u, int mode, int width, int height, int x, int y, int z) {
    int k1 = pspSdkSetK1(0);
    int res = sceDve_driver_DEB2F80C(u);
    if (res < 0) {
        RETURN(-1);
    }

    /* These parameters end up in sceDisplaySetMode. */
    res = sceImposeSetVideoOutMode(mode, width, height);
    if (res < 0) {
        RETURN(-2);
    }

    res = sceDve_driver_93828323(0);
    if (res < 0) {
        RETURN(-3);
    }

    res = sceDve_driver_0B85524C(1);
    if (res < 0) {
        RETURN(-4);
    }

    res = sceDve_driver_A265B504(x, y, z);
    if (res < 0) {
        RETURN(-5);
    }

    pspSdkSetK1(k1);
    return res;
}

static int pspDveMgrSetEdramSizeKernel(unsigned int size) {
    int res = sceGeEdramSetSizeUser((int)size);

    if (res < 0) {
        res = sceGeEdramSetSizeDriver((int)size);
    }
    return res;
}

int pspDveMgrSetEdramSize(unsigned int size) {
    int k1 = pspSdkSetK1(0);
    int res;

    if ((sEdramRequestSize == size) && (sEdramRequestResult >= 0)) {
        res = sEdramRequestResult;
    } else {
        res = pspDveMgrSetEdramSizeKernel(size);
        sEdramRequestSize = size;
        sEdramRequestResult = res;
    }

    pspSdkSetK1(k1);
    return res;
}

int module_start(SceSize args, void* argp) {
    int k1 = pspSdkSetK1(0);

    /* Do this on the kernel module's own start thread.  A user-mode EBOOT
     * calling the exported wrapper later is too late to change that thread's
     * privilege context on some standalone CFW configurations. */
    sEdramRequestSize = PSP_SLIM_EDRAM_SIZE;
    sEdramRequestResult = pspDveMgrSetEdramSizeKernel(PSP_SLIM_EDRAM_SIZE);
    pspSdkSetK1(k1);
    return 0;
}

int module_stop(SceSize args, void* argp) {
    return 0;
}
