// RealWorld C++ corpus (FR-46), project `polygon`: integer lattice geometry.
//
// The REFERENCE + `std::vector` + RANGE-FOR member of the corpus, solidly
// OUTSIDE today's subset by construction. Chosen over a synthetic container
// exercise because a shoelace-area / bounding-box kernel over a point list is
// the smallest genuinely useful computational-geometry program there is, and
// it reaches for exactly the three constructs idiomatic modern C++ uses to
// pass a collection around: `const std::vector<T> &` in, `std::vector<T> &`
// out, and `for (const T &x : xs)` to walk it. All coordinates are integers
// so the native and transpiled legs compare byte for byte with no
// floating-point rounding in the way.
#ifndef POLYGON_GEOM_HPP
#define POLYGON_GEOM_HPP

#include <vector>

/// An integer lattice point.
struct Point {
  int x;
  int y;
};

/// Twice the signed area of the closed polygon through `pts` (the shoelace
/// sum). Twice, so the result stays exact in integers.
long long shoelace_twice(const std::vector<Point> &pts);

/// Manhattan perimeter of the closed polygon through `pts`.
long long perimeter_manhattan(const std::vector<Point> &pts);

/// Axis-aligned bounding box of `pts`, written through `lo`/`hi`.
void bounding_box(const std::vector<Point> &pts, Point &lo, Point &hi);

/// Translates every point of `pts` in place by `delta`.
void translate(std::vector<Point> &pts, const Point &delta);

/// Number of points of `pts` strictly inside the box `lo`..`hi`.
int count_strictly_inside(const std::vector<Point> &pts, const Point &lo,
                          const Point &hi);

#endif
