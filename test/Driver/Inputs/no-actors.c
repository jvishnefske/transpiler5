/* FR-62 slice 5c: a program with no globals and therefore no actor plan
   clusters — under --actor-mode=async nothing is spawned, no async anchor
   exists, and the emitted crate must keep the DEFAULT manifest (no tokio)
   and the synchronous fn main wrapper. */
int add(int a, int b) { return a + b; }

int main(void) { return add(20, 22) - 42; }
