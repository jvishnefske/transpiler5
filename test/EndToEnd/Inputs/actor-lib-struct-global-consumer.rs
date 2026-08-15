// The hand-written consumer for actor-lib-struct-global.c (FR-84):
// constructs the exported owner through its synthesized new() — which
// must carry the C struct's aggregate initializer, including the nested
// array's implicit zero tail — and mirrors the C `#ifdef LIB_CRATE_MAIN`
// driver call for call, printing the same bytes. Inputs are seeded from
// args().count() (1 when run bare), matching the native driver's argc, so
// neither compiler can constant-fold a miscompile out of sight.
use actor_lib_struct_global::CalActor;

fn main() {
    let seed = std::env::args().count() as i32;
    let mut cal_actor = CalActor::new();
    println!("base={}", cal_actor.shift_base(seed * 5));
    println!("base={}", cal_actor.shift_base(-2 * seed));
    println!("mask={}", cal_actor.fold_mask(3855u32 * seed as u32));
    // C's %f prints six decimals; the values are exactly representable.
    println!("scaled={:.6}", cal_actor.scaled(3 * seed));
    println!("taps={}", cal_actor.tap_sum());
}
