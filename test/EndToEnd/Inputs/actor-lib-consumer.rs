// The hand-written consumer for actor-lib-export.c (FR-62 F2): constructs
// both exported owners through their synthesized new() — which must carry
// the C initializers — and mirrors the C `#ifdef LIB_CRATE_MAIN` driver
// call for call, printing the same bytes. The &mut arguments to
// `combined` are the intentional owner-handle API arity change.
use actor_lib_export::{combined, CounterActor, TotalActor};

fn main() {
    let mut counter_actor = CounterActor::new();
    let mut total_actor = TotalActor::new();
    println!("bump={}", counter_actor.bump(3));
    println!("scaled={}", counter_actor.bump_scaled(2));
    println!("peek={}", counter_actor.peek());
    println!("total={}", total_actor.add_total(10));
    println!("total={}", total_actor.add_total(-4));
    println!("combined={}", combined(&mut counter_actor, &mut total_actor));
}
