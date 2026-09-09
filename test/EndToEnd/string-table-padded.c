// REQUIRES: cargo
// FR-217 differential end-to-end test: a `static const char *const t[N] =
// {"a", "bb", ...}` string table lowered to the PADDED two-dimensional
// byte form `[[i8; W]; N]`.
//
// The table has no pointer representation at all: `ArrayType::
// isValidElementType` (EmitRustTypes.cpp) admits no reference element, so
// hand-written IR with a `!emitrust.ref<!emitrust.slice<i8>>` element is
// refused at PARSE time by both emitrust-opt and emitrust-translate; and
// the importer's value model gives each pointer ONE base object where a
// table of N literals needs N. So the table is re-shaped, at import, into
// the same `const char t[N][W]` the emitter has always supported -- every
// row padded with NULs out to the longest element.
//
// Re-shaping storage is exactly the kind of change `cargo build` cannot
// audit: the crate compiles whether or not the rows are laid out right,
// whether or not the padding is NUL, and whether or not a row's index
// scales by W. The stdout diff against the clang-built native is the only
// oracle that sees any of that, which is why this test exists rather than
// an IR golden alone.
//
// Every index is derived from argc, so no constant fold can pre-compute a
// row and hide a miscompile; the second RUN pair moves argc off 1 so both
// the short-row and the long-row cases are reached in both orders. The
// element count comes from `sizeof(t)/sizeof(t[0])` -- the ELEMENTSOF
// idiom, which is preserved exactly by the padded lowering because the two
// sizeof operands cancel (bare `sizeof(t)` does NOT survive and is a
// located rejection; see string-table-sizeof-invalid.c).
//
// The tables are deliberately ragged (1-, 2-, 3- and 9-byte elements, plus
// an empty string), so a lowering that padded to the wrong width, dropped
// the terminator, or forgot to re-scale the row index would print visibly
// different bytes on the very first line.
//
// NOTE ON THE ONE SEMANTIC DIFFERENCE THE PADDING MAKES, and why this
// program stays clear of it: in the pointer form each literal is its own
// object, so reading PAST a short element's NUL is undefined behavior; in
// the padded form those bytes exist and read as 0. An earlier draft of
// this test read `names[0][2]` and the byte-diff caught it immediately --
// the native printed adjacent .rodata (115, 116) where the crate printed
// 0. That is a UB-only divergence, not a miscompile of a conforming
// program, but the test must not depend on it, so every subscript below
// stays at or before its element's terminator.
// --release is load-bearing: debug Rust panics on integer overflow where C
// wraps.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/string_table_padded > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/string_table_padded a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

int printf(const char *, ...);
int puts(const char *);

#define ELEMENTSOF(x) ((int)(sizeof(x) / sizeof((x)[0])))

/* A ragged file-scope table: the widest element (9 bytes + NUL) sets W. */
static const char *const names[] = {"a", "bb", "ccc", "seventeen", ""};

/* A second table with a different width, so a shared/global W would show. */
static const char *const units[] = {"s", "ms", "us"};

/* An explicitly sized table whose bound matches its initializer exactly. */
static const char *const digits[4] = {"zero", "one", "two", "three"};

/* The systemd DEFINE_STRING_TABLE_LOOKUP shape: an enum-indexed table
   whose slots are filled out of order by designated initializers, and
   whose bound is the enum's _MAX rather than a literal. */
enum LogTarget {
  LOG_TARGET_JOURNAL,
  LOG_TARGET_KMSG,
  LOG_TARGET_SYSLOG,
  LOG_TARGET_NULL,
  _LOG_TARGET_MAX,
};

static const char *const log_target_table[_LOG_TARGET_MAX] = {
    [LOG_TARGET_KMSG] = "kmsg",
    [LOG_TARGET_JOURNAL] = "journal",
    [LOG_TARGET_NULL] = "null",
    [LOG_TARGET_SYSLOG] = "syslog",
};

static void dump(int i) {
  /* %s over a row: the padded row is borrowed whole and NUL-stopped. */
  printf("names[%d]=%s|", i, names[i]);
  /* A per-character read: the second subscript must land inside the row,
     not walk a pointer. Only index 0 is read, which is in bounds for
     every element including the empty one. */
  printf("c0=%d|", (int)names[i][0]);
  /* puts takes the same shape through a different builtin. */
  puts(names[i]);
}

int main(int argc, char **argv) {
  /* argc is 1 or 4; both seeds stay in bounds of every table. */
  int seed = argc - 1;

  printf("counts=%d,%d,%d\n", ELEMENTSOF(names), ELEMENTSOF(units),
         ELEMENTSOF(digits));

  /* Walk the whole table with a runtime bound derived from ELEMENTSOF. */
  for (int i = 0; i < ELEMENTSOF(names); ++i)
    dump((i + seed) % ELEMENTSOF(names));

  /* A data-dependent row index into the narrower table. */
  printf("unit=%s|", units[seed % ELEMENTSOF(units)]);
  printf("digit=%s|", digits[(seed + 2) % ELEMENTSOF(digits)]);

  /* The terminator of the SHORTEST non-empty element is in bounds and
     must be 0 -- a lowering that dropped the NUL when padding, or that
     scaled the row index by the wrong width, would show a literal byte of
     the next row here. */
  printf("term=%d,%d|", (int)names[0][1], (int)units[0][1]);

  /* The empty element is one NUL and nothing else. */
  printf("empty=[%s]%d|", names[4], (int)names[4][0]);

  /* A branch on a row's byte, so the row contents reach control flow. */
  if (names[seed % ELEMENTSOF(names)][0] == 'a')
    printf("starts-a");
  else
    printf("starts-other");
  printf("\n");

  /* A function-local static table takes the same padded lowering. */
  {
    static const char *const local[] = {"alpha", "b"};
    printf("local=%s,%s,%d\n", local[seed % 2], local[(seed + 1) % 2],
           ELEMENTSOF(local));
  }

  /* The DOMINANT real-world spelling (systemd's DEFINE_STRING_TABLE_LOOKUP
     idiom, 466 occurrences in the tree at the time of writing): an
     enum-indexed table whose every slot is filled by a DESIGNATED
     initializer, sized by the enum's _MAX. Clang resolves the designators
     to positional slots before the importer sees them, so the padded form
     is the same -- but only if every slot is covered; a table with a gap
     is a NULL hole and refuses. */
  for (int i = 0; i < _LOG_TARGET_MAX; ++i) {
    int k = (i + seed) % _LOG_TARGET_MAX;
    printf("target[%d]=%s;", k, log_target_table[k]);
  }
  printf("|%d\n", ELEMENTSOF(log_target_table));
  return 0;
}
