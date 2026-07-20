// Companion for multi-tu-gate-g8-shared-header.c: the sole real
// definition of the shared pointer global `g`. Excluded from test
// discovery by config.excludes = ["Inputs"].
int arr[4] = {10, 20, 30, 40};
int *g = &arr[0];
