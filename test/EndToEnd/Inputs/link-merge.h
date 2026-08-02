/* Shared header for ../link-merge-e2e.c and link-merge-lib.c (FR-58): the
 * struct/enum defined here appear in BOTH per-TU shards, so the link-step
 * merge must dedup them by symbol (spike-3 shape) rather than splice two
 * copies. */

struct Point {
  int x;
  int y;
};

enum Mode { MODE_RAW, MODE_SCALED };

int add(int a, int b);
int apply(struct Point p, enum Mode m);
extern int shared_counter;
