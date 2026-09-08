#include <stdio.h>
#include <setjmp.h>

/* setjmp/longjmp has no faithful Rust image, so `unwind_demo` is outside
   the supported subset. Compiled plain, the whole translation unit refuses
   with a LOCATED diagnostic and emits nothing -- the project's rule is that
   a construct it cannot model correctly must never be emitted wrongly.

   Compiled with --recover, only `unwind_demo` becomes a stub carrying its
   real signature and an unimplemented!() body, so `checksum` and `main`
   still translate and the rejection is REPORTED rather than hidden. */

static jmp_buf escape_hatch;

static int unwind_demo(int n) {
    if (setjmp(escape_hatch) != 0)
        return -1;
    if (n > 10)
        longjmp(escape_hatch, 1);
    return n * 2;
}

static int checksum(int n) {
    int acc = 0;
    for (int i = 1; i <= n; i++)
        acc += i * i;
    return acc;
}

int main(void) {
    printf("checksum   %d\n", checksum(6));
    printf("unwind     %d\n", unwind_demo(4));
    return 0;
}
