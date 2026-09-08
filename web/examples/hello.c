#include <stdio.h>

int main(void) {
    printf("Hello from C, compiled to Rust.\n");
    for (int i = 1; i <= 3; i++)
        printf("  tick %d\n", i);
    return 0;
}
