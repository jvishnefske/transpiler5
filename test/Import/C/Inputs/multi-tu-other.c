// Second translation unit for multi-tu.c. Defines the external symbols the
// first TU only declares, plus its own file-static `helper` that must not
// collide with the first TU's identically named `helper`.

int shared_add(int a, int b) { return a + b; }

int shared_counter = 100;

static int helper(int x) { return x * 2; }

int use_helper(int x) { return helper(x); }
