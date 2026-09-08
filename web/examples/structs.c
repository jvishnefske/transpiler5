#include <stdio.h>

/* `type` and `ref` are Rust keywords. They are mangled into the emitted
   struct's own field namespace, not rejected -- member names are the one
   identifier class that mangles rather than refusing. */
struct Sample {
    int type;
    int ref;
    int weight;
};

static int score(struct Sample s) {
    return s.type * 10 + s.ref;
}

int main(void) {
    struct Sample samples[3];
    int total = 0;

    for (int i = 0; i < 3; i++) {
        samples[i].type   = i + 1;
        samples[i].ref    = (i + 1) * 2;
        samples[i].weight = 100 - i;
    }

    for (int i = 0; i < 3; i++) {
        int s = score(samples[i]);
        printf("sample %d -> score %d weight %d\n", i, s, samples[i].weight);
        total += s;
    }
    printf("total %d\n", total);
    return 0;
}
