// RealWorld C++ corpus (FR-46), project `shapes`: hierarchy bodies.
// See shape.hpp for the shape rationale.
#include "shape.hpp"

Shape::~Shape() {}

Rect::Rect(int w, int h) : w_(w), h_(h) {}
Rect::~Rect() {}
int Rect::area_x100() const { return w_ * h_ * 100; }
int Rect::perimeter_x100() const { return (w_ + h_) * 2 * 100; }
const char *Rect::tag() const { return "rect"; }

Circle::Circle(int r) : r_(r) {}
Circle::~Circle() {}
int Circle::area_x100() const { return r_ * r_ * 314; }
int Circle::perimeter_x100() const { return 2 * r_ * 314; }
const char *Circle::tag() const { return "circle"; }

/// Integer square root, floor. Used for the triangle's hypotenuse so the
/// program stays in integers.
static int isqrt(int v) {
  if (v <= 0)
    return 0;
  int r = 0;
  while ((r + 1) * (r + 1) <= v)
    r = r + 1;
  return r;
}

RightTriangle::RightTriangle(int a, int b) : a_(a), b_(b) {}
RightTriangle::~RightTriangle() {}
int RightTriangle::area_x100() const { return a_ * b_ * 50; }
int RightTriangle::perimeter_x100() const {
  int hyp = isqrt(a_ * a_ + b_ * b_);
  return (a_ + b_ + hyp) * 100;
}
const char *RightTriangle::tag() const { return "tri"; }
