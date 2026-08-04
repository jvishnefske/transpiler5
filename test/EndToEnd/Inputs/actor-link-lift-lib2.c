/* TU 2 of the actor-link-lift cluster: a file-static `counter` spelled
 * exactly like lib1's — the two must stay DISTINCT actors under the
 * link-line retag (TU1_COUNTER vs TU2_COUNTER). */

static int counter;

int bump2(void) {
  counter += 7;
  return counter;
}
