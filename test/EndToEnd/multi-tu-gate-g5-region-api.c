// W3.1: the intended EndToEnd differential for predicted failure #5
// (external-linkage region APIs) once G5/G6's internal-linkage checks in
// planCellSlices are relaxed by a whole-program merge (see
// test/Import/C/multi-tu-gate-g5-cellslice-global-external.c and
// multi-tu-gate-g6-cellslice-fn-external.c for the located-rejection
// oracles). Expected-reject for now: no --build, no cargo dependency
// yet. A later wave deletes this reject-pin and turns it into a cargo-
// requiring differential (clang native leg vs emitrust-cc crate build,
// matching test/EndToEnd/multi-tu.c's shape) once the shape below
// imports cleanly as a cell-slice-backed region API.
// RUN: not emitrust-cc --emit=crate %s %S/Inputs/multi-tu-gate-g5-region-api-other.c -o %t.crate --crate-name g5_region 2>&1 | FileCheck %s

int region[8];

void region_fill(int *p, int n) {
  int i;
  for (i = 0; i < n; i++)
    p[i] = i;
}

int main(void) {
  region_fill(region, 8);
  return region[3];
}

// CHECK: multi-tu-gate-g5-region-api.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function
