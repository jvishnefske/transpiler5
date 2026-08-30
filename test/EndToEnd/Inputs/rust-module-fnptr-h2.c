// FR-159 companion 2 for ../rust-module-fnptr-e2e.c: a SECOND TU whose
// statics share every spelling with companion 1's (`add1`, `table`,
// `total`) and hold different values, so a module that confused the two
// paths would be immediately visible in stdout. Excluded from test
// discovery by config.excludes = ["Inputs"].
static int add1(int v) { return v * 10; }
typedef int (*op_t)(int);
static op_t table[1] = {add1};
static int total = 100;
int drive2(int v) {
  total += table[0](v);
  return total;
}
