#include "oot_psp_dve.h"

#include <kubridge.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspsdk.h>
#include <stdio.h>
#include <string.h>

#define OOT_PSP_DVE_MODULE_PATH "Plugins/dvemgr.prx"
#define OOT_PSP_DVE_PATH_CAPACITY 512
#define OOT_PSP_SLIM_EDRAM_SIZE (4 * 1024 * 1024)

int pspDveMgrCheckVideoOut(void);
int pspDveMgrSetVideoOut(int output, int mode, int width, int height, int x, int y, int z);
int pspDveMgrSetEdramSize(unsigned int size);

static SceUID sOotPspDveModuleId = -1;
static int sOotPspDveModel = -1;
static int sOotPspDveCable = OOT_PSP_DVE_CABLE_NONE;
static unsigned int sOotPspDveEdramSize;

static int OotPspDve_BuildModulePath(const char* executablePath, char* modulePath, size_t modulePathCapacity) {
    const char* slash;
    const char* backslash;
    size_t rootLength;
    size_t suffixLength = sizeof(OOT_PSP_DVE_MODULE_PATH);

    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        return 0;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    rootLength = (slash != NULL) ? (size_t)(slash - executablePath) + 1 : 0;
    if ((rootLength + suffixLength) > modulePathCapacity) {
        return 0;
    }

    if (rootLength != 0) {
        memcpy(modulePath, executablePath, rootLength);
    }
    memcpy(modulePath + rootLength, OOT_PSP_DVE_MODULE_PATH, suffixLength);
    return 1;
}

static void OotPspDve_Unload(void) {
    int status = 0;

    if (sOotPspDveModuleId < 0) {
        return;
    }

    sceKernelStopModule(sOotPspDveModuleId, 0, NULL, &status, NULL);
    sceKernelUnloadModule(sOotPspDveModuleId);
    sOotPspDveModuleId = -1;
    sOotPspDveCable = OOT_PSP_DVE_CABLE_NONE;
}

int OotPspDve_Init(const char* executablePath) {
    char modulePath[OOT_PSP_DVE_PATH_CAPACITY];
    int driverEdramResult;
    int haveModulePath;
    unsigned int reportedEdramSize;
    const char* loadedModulePath = OOT_PSP_DVE_MODULE_PATH;

    OotPspDve_Unload();
    sOotPspDveEdramSize = 0;
    sOotPspDveModel = kuKernelGetModel();

    if (sOotPspDveModel <= 0) {
        printf("oot-psp dve skipped model=%d\n", sOotPspDveModel);
        return 0;
    }

    haveModulePath = OotPspDve_BuildModulePath(executablePath, modulePath, sizeof(modulePath));
    if (haveModulePath) {
        loadedModulePath = modulePath;
        sOotPspDveModuleId = pspSdkLoadStartModule(modulePath, PSP_MEMORY_PARTITION_KERNEL);
    }
    if ((sOotPspDveModuleId < 0) &&
        (!haveModulePath || (strcmp(modulePath, OOT_PSP_DVE_MODULE_PATH) != 0))) {
        if (haveModulePath) {
            printf("oot-psp dve EBOOT-relative load failed error=0x%08X path=%s; trying relative\n",
                   (unsigned int)sOotPspDveModuleId, modulePath);
        }
        loadedModulePath = OOT_PSP_DVE_MODULE_PATH;
        sOotPspDveModuleId =
            pspSdkLoadStartModule(OOT_PSP_DVE_MODULE_PATH, PSP_MEMORY_PARTITION_KERNEL);
    }
    if (sOotPspDveModuleId < 0) {
        printf("oot-psp dve load failed error=0x%08X edram_reported=%lu\n",
               (unsigned int)sOotPspDveModuleId, (unsigned long)sceGeEdramGetSize());
        return sOotPspDveModuleId;
    }

    /* This firmware does not link Daedalus's sceGe_user SetSize NID. Make the
     * same request through sceGe_driver in the loaded kernel bridge. Trust a
     * successful driver result even if the public size getter stays stale. */
    driverEdramResult = pspDveMgrSetEdramSize(OOT_PSP_SLIM_EDRAM_SIZE);
    reportedEdramSize = sceGeEdramGetSize();
    sOotPspDveEdramSize =
        (driverEdramResult >= 0) ? OOT_PSP_SLIM_EDRAM_SIZE : reportedEdramSize;
    OotPspDve_RefreshCable();
    printf("oot-psp dve ready model=%d cable=%d module=%d path=%s edram_driver=0x%08X edram_reported=%lu size=%lu\n",
           sOotPspDveModel, sOotPspDveCable, sOotPspDveModuleId, loadedModulePath,
           (unsigned int)driverEdramResult, (unsigned long)reportedEdramSize,
           (unsigned long)sOotPspDveEdramSize);
    return 1;
}

void OotPspDve_Shutdown(void) {
    if (sOotPspDveModuleId >= 0) {
        (void)OotPspDve_SetVideoOut(0, 0, 480, 272, 1, 15, 0);
        (void)sceDisplaySetMode(0, 480, 272);
    }
    OotPspDve_Unload();
}

int OotPspDve_GetModel(void) {
    return sOotPspDveModel;
}

int OotPspDve_GetCable(void) {
    return sOotPspDveCable;
}

unsigned int OotPspDve_GetEdramSize(void) {
    return sOotPspDveEdramSize;
}

int OotPspDve_RefreshCable(void) {
    int cable;

    if (sOotPspDveModuleId < 0) {
        sOotPspDveCable = OOT_PSP_DVE_CABLE_NONE;
        return OOT_PSP_DVE_CABLE_NONE;
    }

    cable = pspDveMgrCheckVideoOut();
    if (cable > OOT_PSP_DVE_CABLE_NONE) {
        sOotPspDveCable = cable;
    }
    return sOotPspDveCable;
}

int OotPspDve_IsAvailable(void) {
    return sOotPspDveModuleId >= 0;
}

int OotPspDve_SetVideoOut(int output, int mode, int width, int height, int x, int y, int z) {
    int result;

    if (sOotPspDveModuleId < 0) {
        return -1;
    }

    result = pspDveMgrSetVideoOut(output, mode, width, height, x, y, z);
    if ((result >= 0) && (mode != 0) && (sOotPspDveCable == OOT_PSP_DVE_CABLE_NONE)) {
        sOotPspDveCable =
            (output == 2) ? OOT_PSP_DVE_CABLE_COMPOSITE : OOT_PSP_DVE_CABLE_COMPONENT;
    }
    return result;
}
