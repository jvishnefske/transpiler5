// FR-85: the EXTERNAL consumer of the library crate emitted from
// ../lib-crate-externals-const-byteregion.c. `Host` backs the GETTER-ONLY
// byte-region requirement with the same six bytes the native leg's
// `const struct eth_addr ethbroadcast` definition carries -- a plain
// `[u8; 6]` value, which is itself part of the evidence: no struct type
// exists for the byte-region record, so the consumer supplies the region
// directly. It also backs the body-less `ethernet_output` requirement with
// the same printf the native leg's C definition performs, receiving the
// region as the `&[u8]` slice the CTS-BR parameter convention lowers a
// `const struct eth_addr *` to. The seed is derived from the argument
// count (the argc analogue) so constant folding cannot hide a miscompile
// on either leg.

struct Host;

impl lib_crate_externals_const_byteregion::Externals for Host {
    fn ethernet_output(v0: &[u8]) {
        println!("out {} {}", v0[0], v0[3]);
    }
    fn ethbroadcast() -> [u8; 6] {
        [0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa]
    }
}

fn main() {
    let argc = std::env::args().count() as i32; // 1 when run plain, opaque.
    println!(
        "{}",
        lib_crate_externals_const_byteregion::eth_sum::<Host>(argc)
    );
    println!(
        "plain={}",
        lib_crate_externals_const_byteregion::plain_sum(argc, 5)
    );
}
