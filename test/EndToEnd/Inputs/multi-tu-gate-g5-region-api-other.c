// Companion for multi-tu-gate-g5-region-api.c: a realistic second
// consumer of the same externally visible region API from a different
// translation unit. Excluded from test discovery by
// config.excludes = ["Inputs"].
extern int region[8];
void region_fill(int *p, int n);

int reset_region(void) {
  region_fill(region, 8);
  return region[0];
}
