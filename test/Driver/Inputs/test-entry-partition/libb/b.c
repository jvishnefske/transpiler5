// FR-160 slice 1 fixture -- the 'libb' workspace member.
//
// `test_beta` is the void-shaped entry point, run for its panics.
//
// `test_fails` is the NON-VACUITY control: it returns a nonzero exit code, so
// its generated test must FAIL. A green `cargo test` that cannot go red is
// exactly the vacuous pass FR-160 exists to forbid.
//
// `test_takes` cannot be wrapped at all, and it lives HERE, in a member that
// is neither the first shard nor the binary, so the "takes arguments" warning
// has an OWNING member whose source location it must be reported against.
void test_beta(void) {}

int test_fails(void) { return 7; }

int test_takes(int x) { return x; }
