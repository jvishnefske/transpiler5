// Adversarial enum probe: every use the brunsli/woff2 frontier makes of an enum.
#include <cstdio>
#include <cstddef>
namespace st {
typedef enum { OK = 0, NONREP, MEM, NOT_ENOUGH = 7 } Status;   // anonymous + typedef, gap
enum Dup { A = 1, B = 1, C = A + B, D };                         // duplicate + dependent values
enum class Stage { SIGNATURE = 0, HEADER, DONE, ERROR };         // scoped, int underlying
enum Mask { INIT = 0, HUFF_MASK = 0x10, READ_LAST, SCAN_MASK = 0x40, READ_SCAN };
enum Neg { M1 = -1, Z, P1 };                                      // negative -> underlying int
Status check(int x) { if (x > 2) return NOT_ENOUGH; return x == 0 ? OK : NONREP; }
Stage fail(Status s) { if (s != OK) return Stage::ERROR; return Stage::DONE; }
int promote(Status s) { return s + 1; }                          // enum -> int arithmetic
int is_zero(Status s) { return s == 0; }                          // enum compared with int literal
size_t stored(size_t stage) { return (stage & HUFF_MASK) ? 1 : 0; } // enumerator as size_t constant
int scoped_to_int(Stage s) { return static_cast<int>(s); }
Stage int_to_scoped(int v) { return static_cast<Stage>(v); }      // any int is a valid Stage
}
int main() {
  using namespace st;
  printf("%d %d %d\n", check(0), check(1), check(5));
  printf("%d %d\n", (int)fail(OK), (int)fail(MEM));
  printf("%d %d\n", promote(NOT_ENOUGH), is_zero(OK));
  printf("%d %d %d %d\n", A, B, C, D);
  printf("%zu %zu\n", stored(READ_LAST), stored(READ_SCAN));
  printf("%d %d\n", scoped_to_int(Stage::ERROR), scoped_to_int(int_to_scoped(42))); // out of range of enumerators, in range of int
  printf("%d %d %d\n", M1, Z, P1);
  Status s = check(3); s = check(0);
  printf("%d\n", s == OK);
  return 0;
}
