#include <cstdio>
int bad3(int x) { switch (x) { case 1: return 2; default: return 3; } }
int g3() { return bad3(3); }
int main() { printf("%d\n", g3()); return 0; }
