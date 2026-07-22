// RealWorld corpus (Track 4), multi-TU program `calc`: the operations TU.
// Defines the arithmetic primitives called across the TU boundary by
// calc_main.c.
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }
