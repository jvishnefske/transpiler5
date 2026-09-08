#include <stdio.h>

/* `buf` is a pointer parameter, but nothing here is emitted as a raw
   pointer: the importer proves which object it names and lowers it to a
   slice. The emitted Rust contains no `unsafe`. */
static int sum(const int *buf, int n) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total += buf[i];
    return total;
}

static void scale(int *buf, int n, int factor) {
    for (int i = 0; i < n; i++)
        buf[i] *= factor;
}

int main(void) {
    int data[5] = { 1, 2, 3, 4, 5 };
    printf("sum   %d\n", sum(data, 5));
    scale(data, 5, 3);
    printf("scaled %d\n", sum(data, 5));
    return 0;
}
