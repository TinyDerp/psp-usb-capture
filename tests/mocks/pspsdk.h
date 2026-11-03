#ifndef MOCK_PSPSDK_H
#define MOCK_PSPSDK_H
#include "psptypes.h"
int pspSdkDisableInterrupts(void);
void pspSdkEnableInterrupts(int state);
u32 pspSdkSetK1(u32 value);
#endif
