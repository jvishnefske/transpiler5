// FR-79: the EXTERNAL consumer of the library crate emitted from
// ../lib-crate-externals-const-struct.c. `Host` backs the GETTER-ONLY
// requirement by constructing the same `{0u, 4}` value the native leg's
// `const struct ip_addr ip_addr_any` definition carries -- through the
// struct's `pub` fields, which is itself part of the evidence: a consumer
// crate really can build the by-value struct the getter must return. The
// seed is derived from the argument count (the argc analogue) so constant
// folding cannot hide a miscompile on either leg.

struct Host;

impl lib_crate_externals_const_struct::Externals for Host {
    fn ip_addr_any() -> lib_crate_externals_const_struct::IpAddr {
        lib_crate_externals_const_struct::IpAddr { addr: 0u32, kind: 4 }
    }
}

fn main() {
    let argc = std::env::args().count() as i32; // 1 when run plain, opaque.
    println!(
        "is_any={}",
        lib_crate_externals_const_struct::is_any::<Host>((argc - 1) as u32)
    );
    let c = lib_crate_externals_const_struct::copy_any::<Host>();
    println!("copy={},{}", c.addr.wrapping_add(argc as u32), c.kind);
    println!(
        "kind={}",
        lib_crate_externals_const_struct::kind_plus::<Host>(argc)
    );
    println!(
        "plain={}",
        lib_crate_externals_const_struct::plain_sum(argc, 5)
    );
}
