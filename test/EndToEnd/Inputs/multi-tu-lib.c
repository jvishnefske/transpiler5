// Companion translation unit for ../multi-tu.c. Excluded from test discovery
// by config.excludes = ["Inputs"]; compiled alongside multi-tu.c by both
// emitrust-cc and clang in that test's RUN lines.

int shared_counter = 7;

// Same spelling as the file-static in multi-tu.c; internal linkage keeps the
// two distinct, and the transpiler must mangle them apart per translation
// unit rather than collide in the merged crate.
static int scale(int x) { return x * 3; }

int lib_transform(int x) { return scale(x) + shared_counter; }

int add(int a, int b) { return a + b; }
