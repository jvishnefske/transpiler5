// FR-160 slice 1 fixture -- the 'liba' workspace member.
//
// `test_alpha` is the integer-shaped entry point: its generated test is
// `assert_eq!(super::test_alpha(), 0)`, so it passes iff the C entry point
// would have exited 0, which is what meson and CTest already agree a passing
// test means.
int test_alpha(void) { return 0; }
