// FR-70: the EXTERNAL consumer of the library crate emitted from
// ../lib-crate-externals-global.c. `Host` backs the emitted getter/setter
// pair with REAL storage -- a thread-local Cell seeded with the same `7` the
// native leg's `int g_config = 7;` carries -- so the byte-diff exercises
// genuine cross-call state, not a constant: `bump`'s write through
// `set_g_config` must be visible to the following `peek`'s `g_config()`,
// exactly as the C global's store would be to its next load. The seed is
// derived from the argument count (the argc analogue) so constant folding
// cannot hide a miscompile on either leg.

use std::cell::Cell;

thread_local! {
    static G_CONFIG: Cell<i32> = const { Cell::new(7) };
}

struct Host;

impl lib_crate_externals_global::Externals for Host {
    fn host_scale(v0: i32) -> i32 {
        v0 * 3
    }
    fn g_config() -> i32 {
        G_CONFIG.with(|c| c.get())
    }
    fn set_g_config(v0: i32) {
        G_CONFIG.with(|c| c.set(v0))
    }
}

fn main() {
    let seed = std::env::args().count() as i32 * 4; // argc * 4, opaque too.
    println!("peek={}", lib_crate_externals_global::peek::<Host>());
    println!("bump={}", lib_crate_externals_global::bump::<Host>(seed + 1));
    println!("peek={}", lib_crate_externals_global::peek::<Host>());
    println!("scaled={}", lib_crate_externals_global::scaled::<Host>(seed));
    println!("plain={}", lib_crate_externals_global::plain_sum(seed, 5));
}
