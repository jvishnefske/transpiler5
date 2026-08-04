/* Actor cluster Y: a file-static plus its arms, wholly inside a
   lib-crate TU. The main TU only CALLS the arms, so no condensation
   pulls this TU into the binary crate -- the actor stays lib-side and
   the FR-62 F1b bin-local lift must demote it with the
   `spans workspace crates` note. */
static int y_total = 0;

void y_add(int v) { y_total += v; }
int y_total_now(void) { return y_total; }
