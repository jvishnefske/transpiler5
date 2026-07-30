// FR-52: the EXTERNAL consumer of the library crate emitted from
// ../lib-crate-externals-trait.c. It is a separate crate compiled against the
// emitted one with `--extern`, so it can only name items the emitter marked
// `pub` -- the trait among them.
//
// This is the evidence that an emitted requirement is SATISFIABLE and not
// merely well-formed: `Host` supplies the two functions the C project left
// undefined, and instantiating the generic items with it produces the same
// bytes the native build does.

struct Host;

impl lib_crate_externals_trait::Externals for Host {
    fn host_scale(v0: i32) -> i32 {
        v0 * 3
    }
    fn host_floor(v0: i32) -> i32 {
        if v0 < 10 {
            10
        } else {
            v0
        }
    }
}

fn main() {
    println!("scaled={}", lib_crate_externals_trait::scaled::<Host>(4));
    println!("banded={}", lib_crate_externals_trait::banded::<Host>(1));
    println!("banded={}", lib_crate_externals_trait::banded::<Host>(7));
    println!("plain={}", lib_crate_externals_trait::plain_sum(2, 5));
}
