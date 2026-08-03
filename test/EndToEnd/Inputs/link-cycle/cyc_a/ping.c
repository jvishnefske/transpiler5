// Half of the FR-59 cycle-condensation pair: mutually recursive with
// cyc_b/pong.c across two directories, so the naive per-directory
// partition would produce two crates that depend on each other -- which
// cargo cannot build -- and the planner must condense them into one.

int pong(int n);

int ping(int n) { return n <= 0 ? 0 : pong(n - 1) + 1; }
