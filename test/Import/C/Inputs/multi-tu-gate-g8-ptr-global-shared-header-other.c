// Companion for multi-tu-gate-g8-ptr-global-shared-header.c: the sole
// real definition of the shared pointer global `g`, exactly the shape a
// header-shared `extern int *g;` implies.
int arr[4] = {10, 20, 30, 40};
int *g = &arr[0];
