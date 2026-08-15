// FR-81: the EXTERNAL consumer of the library crate emitted from
// ../lib-crate-externals-nonconst-struct.c. `Host` backs the emitted
// whole-value getter/setter pair with REAL storage -- a thread-local
// Cell<IpGlobals> seeded with the same `{7, 40}` the native leg's
// `struct ip_globals ip_data = {7, 40u};` carries -- so the byte-diff
// exercises genuine cross-call state: every field write goes through
// `set_ip_data` as a whole value and must be visible to the next getter
// call, exactly as the C global's store would be to its next load. The seed
// is derived from the argument count (the argc analogue) so constant
// folding cannot hide a miscompile on either leg.

use std::cell::Cell;

use lib_crate_externals_nonconst_struct::IpGlobals;

thread_local! {
    static IP_DATA: Cell<IpGlobals> =
        const { Cell::new(IpGlobals { ttl: 7, addr: 40u32 }) };
}

struct Host;

impl lib_crate_externals_nonconst_struct::Externals for Host {
    fn ip_data() -> IpGlobals {
        IP_DATA.with(|c| c.get())
    }
    fn set_ip_data(v0: IpGlobals) {
        IP_DATA.with(|c| c.set(v0))
    }
}

fn main() {
    use lib_crate_externals_nonconst_struct as lib;
    let seed = std::env::args().count() as i32 * 4; // argc * 4, opaque too.
    println!(
        "get={} addr={}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    lib::set_ttl::<Host>(seed + 1);
    println!(
        "after set: {} {}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    lib::bump::<Host>();
    println!(
        "after bump: {} {}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    lib::swapish::<Host>();
    println!(
        "after swapish: {} {}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    lib::hazard::<Host>();
    println!(
        "after hazard: {} {}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    let mut s = lib::snapshot::<Host>();
    s.ttl += seed;
    lib::restore::<Host>(s);
    println!(
        "after restore: {} {}",
        lib::get_ttl::<Host>(),
        lib::get_addr::<Host>()
    );
    println!("plain={}", lib::plain_sum(seed, 5));
}
