// FR-160b fixture: the FR-62 actor lift moves `test_alpha` into
// `impl Tu0HelperCountActor` because it touches a file-local global, so no
// module-level symbol of that name exists and a module-level symbol-table
// lookup cannot see it. The struct's C initializer (`= 0`) is applied only
// inside `c_main`, which is why this arm is REPORTED and never wrapped: a
// `Default::default()` receiver would test a different program.
static int helper_count = 0;
int test_alpha(void) {
  helper_count++;
  return helper_count - 1;
}
int main(void) { return test_alpha(); }
