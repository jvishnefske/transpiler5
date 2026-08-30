// FR-159 companion 1 for ../rust-module-fnptr-e2e.c: a file-static fn-ptr
// dispatch TABLE plus a file-static accumulator -- the shape systemd's
// src/basic/rlimit-util.c carries (FR-156). Its statics are what phase 3
// will sink into `mod tu0`, and rust-module-fnptr.mlir is the already-sunk
// form of exactly this TU. Excluded from test discovery by
// config.excludes = ["Inputs"].
static int add1(int v) { return v + 1; }
static int add2(int v) { return v * 2; }
typedef int (*op_t)(int);
static op_t table[2] = {add1, add2};
static int total = 0;
int drive1(int i, int v) {
  total += table[i](v);
  return total;
}
