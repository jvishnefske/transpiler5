// RealWorld C++ corpus (FR-46), project `shapes`: the dispatch loop.
// See report.hpp for why this lives in its own TU.
#include <cstdio>

#include "report.hpp"
#include "shape.hpp"

int report(Shape *const *shapes, int count) {
  int total = 0;
  for (int i = 0; i < count; i = i + 1) {
    const Shape *s = shapes[i];
    int area = s->area_x100();
    printf("%-6s area=%d.%02d perim=%d.%02d\n", s->tag(), area / 100,
           area % 100, s->perimeter_x100() / 100, s->perimeter_x100() % 100);
    total = total + area;
  }
  return total;
}
