#include <stdio.h>

/* Mutable file-scope state. In most C-to-Rust output this becomes
   `static mut` behind `unsafe`, or a thread_local. Here the co-access
   clustering lifts it into a struct owned by main, and the three functions
   below become &mut self methods on it. */
static int counter    = 0;
static int high_water = 0;

static void bump(int by) {
    counter += by;
    if (counter > high_water)
        high_water = counter;
}

static void drain(void) {
    counter = 0;
}

static int peek(void) {
    return counter;
}

int main(void) {
    bump(5);
    bump(12);
    printf("counter %d high_water %d\n", peek(), high_water);
    drain();
    bump(3);
    printf("counter %d high_water %d\n", peek(), high_water);
    return 0;
}
