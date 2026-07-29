// RealWorld C++ corpus (FR-46), project `polygon`: geometry body.
// See geom.hpp for the shape rationale.
#include "geom.hpp"

long long shoelace_twice(const std::vector<Point> &pts) {
  long long acc = 0;
  int n = (int)pts.size();
  for (int i = 0; i < n; i = i + 1) {
    int j = i + 1;
    if (j == n)
      j = 0;
    acc = acc + (long long)pts[i].x * (long long)pts[j].y;
    acc = acc - (long long)pts[j].x * (long long)pts[i].y;
  }
  if (acc < 0)
    acc = -acc;
  return acc;
}

static int abs_int(int v) { return v < 0 ? -v : v; }

long long perimeter_manhattan(const std::vector<Point> &pts) {
  long long acc = 0;
  int n = (int)pts.size();
  for (int i = 0; i < n; i = i + 1) {
    int j = i + 1;
    if (j == n)
      j = 0;
    acc = acc + abs_int(pts[j].x - pts[i].x);
    acc = acc + abs_int(pts[j].y - pts[i].y);
  }
  return acc;
}

void bounding_box(const std::vector<Point> &pts, Point &lo, Point &hi) {
  bool first = true;
  for (const Point &p : pts) {
    if (first) {
      lo = p;
      hi = p;
      first = false;
      continue;
    }
    if (p.x < lo.x)
      lo.x = p.x;
    if (p.y < lo.y)
      lo.y = p.y;
    if (p.x > hi.x)
      hi.x = p.x;
    if (p.y > hi.y)
      hi.y = p.y;
  }
  if (first) {
    lo.x = 0;
    lo.y = 0;
    hi.x = 0;
    hi.y = 0;
  }
}

void translate(std::vector<Point> &pts, const Point &delta) {
  for (Point &p : pts) {
    p.x = p.x + delta.x;
    p.y = p.y + delta.y;
  }
}

int count_strictly_inside(const std::vector<Point> &pts, const Point &lo,
                          const Point &hi) {
  int n = 0;
  for (const Point &p : pts) {
    if (p.x > lo.x && p.x < hi.x && p.y > lo.y && p.y < hi.y)
      n = n + 1;
  }
  return n;
}
