/* TU 1 of the actor-link-lift cluster: a file-static `counter` sharing its
 * spelling with lib2's, an external global cluster {shared}, and the
 * externals {tally} co-accessed with this TU's static by bump1. */

static int counter;
int shared;
int tally;

int bump1(void) {
  counter += 3;
  tally += 1;
  return counter + tally;
}

int touch_shared(void) {
  shared += 5;
  return shared;
}
