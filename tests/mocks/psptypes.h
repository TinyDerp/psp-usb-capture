#ifndef MOCK_PSPTYPES_H
#define MOCK_PSPTYPES_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
#ifdef _MSC_VER
#define __attribute__(attribute)
#endif
#endif
