// RealWorld C++ corpus (FR-46), project `polygon`: driver TU.
//
// Exercises: `std::vector<Point>` as a local, `push_back` of a struct value,
// `const std::vector<T> &` and `std::vector<T> &` PARAMETERS across a TU
// boundary, `Point &` out-parameters, and a range-for over the container in
// both const and mutating form. Everything here is a construct the W2.3 STL
// wave explicitly left OUT (references, vector-typed parameters, range-for,
// struct element type), so this project is the corpus's standing demand for
// the next container wave.
#include <cstdio>
#include <vector>

#include "geom.hpp"

int main(void) {
  std::vector<Point> poly;
  poly.push_back(Point{0, 0});
  poly.push_back(Point{6, 0});
  poly.push_back(Point{6, 4});
  poly.push_back(Point{3, 7});
  poly.push_back(Point{0, 4});

  long long area2 = shoelace_twice(poly);
  long long perim = perimeter_manhattan(poly);

  Point lo;
  Point hi;
  bounding_box(poly, lo, hi);

  Point delta;
  delta.x = 2;
  delta.y = -1;
  translate(poly, delta);

  Point lo2;
  Point hi2;
  bounding_box(poly, lo2, hi2);

  // A box strictly inside the shifted bounding box, so the count is a real
  // data-dependent number rather than a vacuous 0.
  Point probe_lo;
  probe_lo.x = lo2.x - 1;
  probe_lo.y = lo2.y - 1;
  Point probe_hi;
  probe_hi.x = hi2.x - 2;
  probe_hi.y = hi2.y;
  int inner = count_strictly_inside(poly, probe_lo, probe_hi);

  printf("n=%d area2=%lld perim=%lld\n", (int)poly.size(), area2, perim);
  printf("bbox=(%d,%d)-(%d,%d) shifted=(%d,%d)-(%d,%d) inner=%d\n", lo.x, lo.y,
         hi.x, hi.y, lo2.x, lo2.y, hi2.x, hi2.y, inner);
  return 0;
}
