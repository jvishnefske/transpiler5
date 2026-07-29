// RealWorld C++ corpus (FR-46), project `shapes`: a virtual shape hierarchy.
//
// The HARDEST member of the corpus: an abstract base with pure virtual
// methods, three derived classes, a virtual destructor, and dynamic dispatch
// through a base-class pointer. Chosen as a shape-area dispatcher because it
// is the single most common real use of inheritance in small C++ codebases
// (a report/render loop over heterogeneous items), and because it stacks
// FOUR independent blockers at once -- base classes, virtual methods, a
// user-declared destructor, and virtual dispatch at the call site -- so its
// rejection sequence tracks progress on the whole inheritance design rather
// than on any one construct.
//
// Areas are reported in hundredths of a unit ("x100") so the program stays in
// integers and compares byte for byte against the native build.
#ifndef SHAPES_SHAPE_HPP
#define SHAPES_SHAPE_HPP

/// Abstract base of every reportable shape.
class Shape {
public:
  virtual ~Shape();

  /// Area, in hundredths of a square unit.
  virtual int area_x100() const = 0;

  /// Perimeter, in hundredths of a unit.
  virtual int perimeter_x100() const = 0;

  /// A stable short tag for the report.
  virtual const char *tag() const = 0;
};

/// An axis-aligned rectangle with integer sides.
class Rect : public Shape {
public:
  Rect(int w, int h);
  ~Rect();
  int area_x100() const;
  int perimeter_x100() const;
  const char *tag() const;

private:
  int w_;
  int h_;
};

/// A circle with an integer radius; pi is approximated as 314/100.
class Circle : public Shape {
public:
  Circle(int r);
  ~Circle();
  int area_x100() const;
  int perimeter_x100() const;
  const char *tag() const;

private:
  int r_;
};

/// A right triangle with integer legs.
class RightTriangle : public Shape {
public:
  RightTriangle(int a, int b);
  ~RightTriangle();
  int area_x100() const;
  int perimeter_x100() const;
  const char *tag() const;

private:
  int a_;
  int b_;
};

#endif
