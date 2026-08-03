// FR-58 slice 2 archive companion for ../link-merge-archive.c: defines the
// `add` the main TU calls. Compiled through the shim, then archived with
// `llvm-ar rcs`, so its shard reaches the link line as an archive member.
// Excluded from test discovery by config.excludes = ["Inputs"].

int add(int a, int b) { return a + b; }
