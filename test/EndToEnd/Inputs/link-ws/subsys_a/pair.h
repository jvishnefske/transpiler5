// Shared record for the FR-59 workspace partition test: defined in every
// including TU's shard, dedup'd by the merge into subsys_a's crate (the
// first defining shard on the link line), and referenced cross-crate from
// subsys_b and the binary through FR-51's unconditional type export.
#ifndef LINK_WS_PAIR_H
#define LINK_WS_PAIR_H
struct Pair {
  int x;
  int y;
};
#endif
