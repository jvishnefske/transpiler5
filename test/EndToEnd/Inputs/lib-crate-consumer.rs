// FR-51: an EXTERNAL consumer of the library crate emitted from
// ../lib-crate-external-caller.c. It is a separate crate, compiled against
// the emitted one with `--extern`, so nothing here can reach a private item:
// every call below succeeds only because the emitter marked that item `pub`.
//
// It prints the same lines, in the same order, as the C `main` in
// lib-crate-external-caller.c, which is the native oracle the test diffs
// against. That makes this a real differential check of a LIBRARY, recovering
// the evidence a library normally cannot provide (a lib crate has no entry
// point, which is exactly why the RealWorld harness scores such a project
// LIB_BUILT rather than TRANSPILED).

fn main() {
    let mut acc = lib_crate_external_caller::Acc::default();
    lib_crate_external_caller::acc_reset(&mut acc);
    let values: [i32; 4] = [7, -3, 20, 5];
    for v in values {
        lib_crate_external_caller::acc_push(&mut acc, v);
    }
    println!("count={}", lib_crate_external_caller::acc_count(&mut acc));
    println!("total={}", lib_crate_external_caller::acc_total(&mut acc));
    println!("scaled={}", lib_crate_external_caller::scale_total(&mut acc, 3));
    println!("clamped={}", lib_crate_external_caller::clamped(-9));
    println!("clamped={}", lib_crate_external_caller::clamped(11));
}
