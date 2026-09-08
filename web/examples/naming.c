#include <stdio.h>

/* Under the default idiomatic rename these become snake_case functions,
   an UpperCamelCase type and a SCREAMING_SNAKE_CASE global. Toggle
   "preserve C names" to get the verbatim C spellings back. */
struct point_pair { int first_x; int second_x; };

static int MaxRetryCount = 3;

static int ComputeDelta(struct point_pair pp) {
    return pp.second_x - pp.first_x;
}

int main(void) {
    struct point_pair pp = { 4, 19 };
    printf("delta %d over %d retries\n", ComputeDelta(pp), MaxRetryCount);
    return 0;
}
