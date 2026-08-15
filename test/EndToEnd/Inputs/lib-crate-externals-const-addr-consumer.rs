// FR-80 consumer: satisfies the address-carrying const-struct requirement
// by lending ONE static item -- the `&'static IpAddr` every
// `E::ip_addr_any()` call returns, which is what gives the requirement a
// stable, unique address. The requirement FUNCTIONS answer with pointer
// IDENTITY (`core::ptr::eq`), mirroring the native leg's C definitions
// (`p == &ip_addr_any`) exactly: a lowering that handed out per-call
// copies, or that erased identity into value equality, diverges in the
// byte-diff.
use lib_crate_externals_const_addr::{
    check_any, check_other, member_chain, member_dot, mixed, read_through,
    via_local, Externals, Ip4, IpAddr,
};

static IP_ADDR_ANY: IpAddr = IpAddr {
    u: Ip4 { a: 0u32 },
    kind: 4,
};

struct Host;
impl Externals for Host {
    fn is_any(p: &IpAddr) -> i32 {
        core::ptr::eq(p, Host::ip_addr_any()) as i32
    }
    fn ip4_get(q: &Ip4) -> u32 {
        q.a
    }
    fn ip_addr_any() -> &'static IpAddr {
        &IP_ADDR_ANY
    }
}

fn main() {
    let argc = std::env::args().count() as i32;
    println!("check={}", check_any::<Host>());
    println!("other={}", check_other::<Host>((argc - 1) as u32));
    println!("chain={}", member_chain::<Host>().wrapping_add(argc as u32));
    println!("dot={}", member_dot::<Host>().wrapping_mul(argc as u32));
    println!("local={}", via_local::<Host>());
    println!("read={}", read_through::<Host>().wrapping_add(argc as u32));
    println!("mixed={}", mixed::<Host>(argc));
}
