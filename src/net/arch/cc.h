#ifndef ARCH_CC_H
#define ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// MSVC struct packing handled via bpstruct.h / epstruct.h
#ifdef _MSC_VER
#define PACK_STRUCT_USE_INCLUDES
#endif

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("lwIP ASSERT: %s at %s:%d\n", (x), __FILE__, __LINE__); \
    abort(); \
} while(0)

#define LWIP_NO_STDDEF_H  0
#define LWIP_NO_STDINT_H  0
#define LWIP_NO_INTTYPES_H 0

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_RAND() ((u32_t)rand())

#endif
