/* Actor cluster Y: a file-static plus its arms, wholly inside a
   lib-crate TU; the FR-62 F1b bin-local lift demotes it and the crate
   keeps the thread-local form. */
static int y_total = 0;

void y_add(int v) { y_total += v; }
int y_total_now(void) { return y_total; }
