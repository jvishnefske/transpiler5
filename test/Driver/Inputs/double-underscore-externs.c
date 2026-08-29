// FR-140 companion input for test/Driver/double-underscore-names.c: a
// LIBRARY translation unit whose one undefined symbol carries an interior
// double underscore, so FR-52's requirement trait declares a method under
// that spelling. Not a test on its own (it lives under Inputs/).
extern int host__scale(int v);

int compute(int x) { return host__scale(x) + 1; }
