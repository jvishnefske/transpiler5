// FR-75 consumer for lib-crate-externals-slice.c: implements the
// slice-typed requirement with a helper that writes EVERY byte of the
// region it is handed — indices are relative to the slice start, exactly
// like C pointer arithmetic relative to the passed cursor — and drives
// `use_it` with a seed derived from argc, mirroring the C native oracle's
// `main`.
struct Host;

impl lib_crate_externals_slice::Externals for Host {
    fn helper(v0: &mut [u8], v1: u32) {
        let mut i: u32 = 0;
        while i < v1 {
            v0[i as usize] =
                ((v0[i as usize] as u32).wrapping_mul(2).wrapping_add(i)) as u8;
            i += 1;
        }
    }
}

fn main() {
    let argc = std::env::args().count() as i32;
    println!(
        "use_it={}",
        lib_crate_externals_slice::use_it::<Host>(argc)
    );
}
