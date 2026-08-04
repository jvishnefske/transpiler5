/* Minimal lwIP architecture port (compiler abstraction) for the transpile
 * corpus. lwIP 2.x derives its integer types from <stdint.h> on its own; a port
 * only has to supply the diagnostic/assert hooks and the packing macros. This
 * is a host build (glibc), so the platform hooks route to printf/abort. */
#ifndef LWIP_CORPUS_ARCH_CC_H
#define LWIP_CORPUS_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("Assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
    fflush(NULL); abort(); } while (0)

#endif /* LWIP_CORPUS_ARCH_CC_H */
