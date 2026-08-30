// Companion TU for link-fnptr-table-order-e2e.c and the Driver sibling: a
// file-static function-pointer dispatch table (the systemd
// src/basic/rlimit-util.c:222 shape) plus the three other carriers of a
// plain-string payload the shard retag has to agree on -- a body-level
// fn-ptr constant, a null fn-ptr global (`None`), and an enum constant
// path (`Mode::MODE_MUL`). `p_a` and `p_ab` are deliberately
// prefix-related so a substring rewrite of one cannot silently corrupt the
// other.
enum Mode { MODE_ADD, MODE_MUL };

static int p_a(int v) { return v + 1; }
static int p_ab(int v) { return v * 2; }
static int p_b(int v) { return v - 3; }

static int (*const tbl[3])(int) = {p_a, p_ab, p_b};
static int (*hook)(int) = 0;

int run_tbl(int i, int v) { return tbl[i](v); }

int run_local(int v) {
  int (*f)(int) = p_ab;
  return f(v);
}

int run_hook(int v) { return hook ? hook(v) : -1; }

int mode_of(int v) {
  enum Mode m = v > 0 ? MODE_MUL : MODE_ADD;
  return m == MODE_MUL ? 1 : 0;
}
