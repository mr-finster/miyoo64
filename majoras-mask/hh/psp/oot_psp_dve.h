#ifndef OOT_PSP_DVE_H
#define OOT_PSP_DVE_H

typedef enum OotPspDveCable {
    OOT_PSP_DVE_CABLE_NONE = 0,
    OOT_PSP_DVE_CABLE_COMPOSITE = 1,
    OOT_PSP_DVE_CABLE_COMPONENT = 2,
} OotPspDveCable;

/* Returns 1 when the DVE bridge is loaded, 0 on a PSP-1000, and a negative PSP
 * error when the bridge could not be loaded. Cable detection is independent
 * of module lifetime so a Slim can detect a cable connected after startup. */
int OotPspDve_Init(const char* executablePath);
void OotPspDve_Shutdown(void);

int OotPspDve_GetModel(void);
int OotPspDve_GetCable(void);
unsigned int OotPspDve_GetEdramSize(void);
int OotPspDve_RefreshCable(void);
int OotPspDve_IsAvailable(void);
int OotPspDve_SetVideoOut(int output, int mode, int width, int height, int x, int y, int z);

#endif
