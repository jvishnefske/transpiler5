// Companion TU for actor-plan-link.c: defines the extern global `shared`
// and a file-static `local_count` spelled EXACTLY like the main TU's own
// file-static -- the pair must remain two distinct plan elements keyed by
// their link-line TU ordinal.

static int local_count;

int shared;

int tick(void) {
  local_count += 1;
  return shared + local_count;
}
