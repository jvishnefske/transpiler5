// The other half of the FR-59 cycle-condensation pair; see cyc_a/ping.c.

int ping(int n);

int pong(int n) { return n <= 0 ? 0 : ping(n - 1) + 2; }
