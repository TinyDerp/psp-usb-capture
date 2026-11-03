#ifndef MOCK_PSPKERNEL_H
#define MOCK_PSPKERNEL_H
#include "psptypes.h"
u64 sceKernelGetSystemTimeWide(void);
void sceKernelDcacheWritebackInvalidateAll(void);
#endif
