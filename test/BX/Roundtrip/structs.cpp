// FR-257: structs and member functions, C++ -> canonical Rust under
// --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++. Pins:
// `#[derive(Clone)] struct` with `pub` fields (`pub struct` inside a `mod`);
// `impl S { pub fn .. }` with `&self` (const), `&mut self` (non-const), an
// associated fn (static) and `new` (a member-initializer-list constructor);
// implicit member access / `this->f` <=> `self.f`, `*this = o;` <=>
// `*self = o.clone();`; aggregate init `{a, b}` / `S{a, b}` / `S p{0, 0};`
// <=> `S { x: a, y: b }`; `S c(a, b);` <=> `S::new(a, b)`; normal form N15
// (struct definitions in topological-then-path order, whatever the source
// order: the canonical C++ forward-declares `Node` because `Graph`, printed
// first, holds a `std::vector<Node>`), member functions defined out of line
// after every class (`int Pen::count() const {`), and `::zoo::Pen` /
// `crate::zoo::Pen` across namespaces.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:       #[derive(Clone)]
// CHECK-NEXT:  struct Graph {
// CHECK-NEXT:      pub nodes: Vec<Node>,
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  impl Graph {
// CHECK-NEXT:      pub fn weight(&self, n: &Node) -> i64 {
// CHECK-NEXT:          return (n.id as i64) * 3i64;
// CHECK-NEXT:      }
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  #[derive(Clone)]
// CHECK-NEXT:  struct Node {
// CHECK-NEXT:      pub id: i32,
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  #[derive(Clone)]
// CHECK-NEXT:  struct Point {
// CHECK-NEXT:      pub x: i32,
// CHECK-NEXT:      pub y: i32,
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  impl Point {
// CHECK-NEXT:      pub fn flip(&mut self) {
// CHECK-NEXT:          let t: i32 = self.x;
// CHECK-NEXT:          self.x = self.y;
// CHECK-NEXT:          self.y = t;
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      pub fn plus(&self, o: &Point) -> Point {
// CHECK-NEXT:          return Point { x: self.x + o.x, y: self.y + o.y };
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      pub fn origin() -> Point {
// CHECK-NEXT:          let mut p: Point = Point { x: 0, y: 0 };
// CHECK-NEXT:          return p;
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      pub fn copy_from(&mut self, o: &Point) {
// CHECK-NEXT:          *self = o.clone();
// CHECK-NEXT:      }
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  mod zoo {
// CHECK-NEXT:      #[derive(Clone)]
// CHECK-NEXT:      pub struct Animal {
// CHECK-NEXT:          pub name: String,
// CHECK-NEXT:          pub legs: u32,
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      impl Animal {
// CHECK-NEXT:          pub fn new(mut n: String, mut l: u32) -> Animal {
// CHECK-NEXT:              return Animal { name: n, legs: l.wrapping_mul(1u32) };
// CHECK-NEXT:          }
// CHECK-EMPTY:
// CHECK-NEXT:          pub fn walks(&self) -> bool {
// CHECK-NEXT:              return self.legs > 0u32;
// CHECK-NEXT:          }
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      #[derive(Clone)]
// CHECK-NEXT:      pub struct Pen {
// CHECK-NEXT:          pub animals: Vec<Animal>,
// CHECK-NEXT:          pub pen_id: i32,
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      impl Pen {
// CHECK-NEXT:          pub fn count(&self) -> i32 {
// CHECK-NEXT:              return self.animals.len() as i32;
// CHECK-NEXT:          }
// CHECK-EMPTY:
// CHECK-NEXT:          pub fn admit(&mut self, a: &Animal) {
// CHECK-NEXT:              self.animals.push(a.clone());
// CHECK-NEXT:              self.pen_id += 1;
// CHECK-NEXT:          }
// CHECK-NEXT:      }
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  fn main() {
// CHECK-NEXT:      let mut pen: crate::zoo::Pen = crate::zoo::Pen { animals: Vec::new(), pen_id: 7 };
// CHECK-NEXT:      let mut cat: crate::zoo::Animal = crate::zoo::Animal::new(String::from("cat"), 4u32);
// CHECK-NEXT:      pen.admit(&cat);
// CHECK-NEXT:      pen.admit(&crate::zoo::Animal::new(String::from("fish"), 0u32));
// CHECK-NEXT:      let mut p: Point = Point::origin();
// CHECK-NEXT:      let mut q: Point = Point { x: 3, y: 4 };
// CHECK-NEXT:      p = p.plus(&q);
// CHECK-NEXT:      p.flip();
// CHECK-NEXT:      q.x -= 1;
// CHECK-NEXT:      let mut r: Point = q.clone();
// CHECK-NEXT:      r.copy_from(&p);
// CHECK-NEXT:      let mut g: Graph = Graph { nodes: vec![Node { id: 5 }, Node { id: 6 }] };
// CHECK-NEXT:      g.nodes.push(Node { id: 7 });
// CHECK-NEXT:      let w: i64 = g.weight(&g.nodes[2usize]);
// CHECK-NEXT:      print!("{} {} {} {} {} {}\n", pen.count(), p.x, p.y, r.y, pen.animals[1usize].walks() as i32, w);
// CHECK-NEXT:  }
// CPP:         #include <cstdio>
// CPP-NEXT:    #include <string>
// CPP-NEXT:    #include <utility>
// CPP-NEXT:    #include <vector>
// CPP-EMPTY:
// CPP-NEXT:    struct Node;
// CPP-EMPTY:
// CPP-NEXT:    struct Graph {
// CPP-NEXT:        std::vector<Node> nodes;
// CPP-NEXT:        long long weight(const Node& n) const;
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    struct Node {
// CPP-NEXT:        int id;
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    struct Point {
// CPP-NEXT:        int x;
// CPP-NEXT:        int y;
// CPP-NEXT:        void flip();
// CPP-NEXT:        Point plus(const Point& o) const;
// CPP-NEXT:        static Point origin();
// CPP-NEXT:        void copy_from(const Point& o);
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    namespace zoo {
// CPP-EMPTY:
// CPP-NEXT:    struct Animal {
// CPP-NEXT:        std::string name;
// CPP-NEXT:        unsigned legs;
// CPP-NEXT:        Animal(std::string n, unsigned l);
// CPP-NEXT:        bool walks() const;
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    struct Pen {
// CPP-NEXT:        std::vector<Animal> animals;
// CPP-NEXT:        int pen_id;
// CPP-NEXT:        int count() const;
// CPP-NEXT:        void admit(const Animal& a);
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    }  // namespace zoo
// CPP-EMPTY:
// CPP-NEXT:    long long Graph::weight(const Node& n) const {
// CPP-NEXT:        return static_cast<long long>(n.id) * 3LL;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    void Point::flip() {
// CPP-NEXT:        const int t = this->x;
// CPP-NEXT:        this->x = this->y;
// CPP-NEXT:        this->y = t;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    Point Point::plus(const Point& o) const {
// CPP-NEXT:        return Point{this->x + o.x, this->y + o.y};
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    Point Point::origin() {
// CPP-NEXT:        Point p = {0, 0};
// CPP-NEXT:        return p;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    void Point::copy_from(const Point& o) {
// CPP-NEXT:        *this = o;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    namespace zoo {
// CPP-EMPTY:
// CPP-NEXT:    Animal::Animal(std::string n, unsigned l) : name(std::move(n)), legs(l * 1U) {}
// CPP-EMPTY:
// CPP-NEXT:    bool Animal::walks() const {
// CPP-NEXT:        return this->legs > 0U;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    int Pen::count() const {
// CPP-NEXT:        return static_cast<int>(this->animals.size());
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    void Pen::admit(const Animal& a) {
// CPP-NEXT:        this->animals.push_back(a);
// CPP-NEXT:        ++this->pen_id;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    }  // namespace zoo
// CPP-EMPTY:
// CPP-NEXT:    int main() {
// CPP-NEXT:        ::zoo::Pen pen = {std::vector<::zoo::Animal>{}, 7};
// CPP-NEXT:        ::zoo::Animal cat = ::zoo::Animal(std::string("cat"), 4U);
// CPP-NEXT:        pen.admit(cat);
// CPP-NEXT:        pen.admit(::zoo::Animal(std::string("fish"), 0U));
// CPP-NEXT:        Point p = Point::origin();
// CPP-NEXT:        Point q = {3, 4};
// CPP-NEXT:        p = p.plus(q);
// CPP-NEXT:        p.flip();
// CPP-NEXT:        --q.x;
// CPP-NEXT:        Point r = q;
// CPP-NEXT:        r.copy_from(p);
// CPP-NEXT:        Graph g = {std::vector<Node>{Node{5}, Node{6}}};
// CPP-NEXT:        g.nodes.push_back(Node{7});
// CPP-NEXT:        const long long w = g.weight(g.nodes[static_cast<std::size_t>(2)]);
// CPP-NEXT:        std::printf("%d %d %d %d %d %lld\n", pen.count(), p.x, p.y, r.y, static_cast<int>(pen.animals[static_cast<std::size_t>(1)].walks()), w);
// CPP-NEXT:        return 0;
// CPP-NEXT:    }
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace zoo {

struct Animal;

struct Pen {
    std::vector<Animal> animals;
    int pen_id;
    int count() const;
    void admit(const Animal& a);
};

struct Animal {
    std::string name;
    unsigned legs;
    Animal(std::string n, unsigned l) : name(std::move(n)), legs(l * 1U) {}
    bool walks() const {
        return legs > 0U;
    }
};

int Pen::count() const {
    return static_cast<int>(animals.size());
}

void Pen::admit(const Animal& a) {
    this->animals.push_back(a);
    pen_id += 1;
}

}  // namespace zoo

struct Node {
    int id;
};

struct Graph {
    std::vector<Node> nodes;
    long long weight(const Node& n) const {
        return static_cast<long long>(n.id) * 3LL;
    }
};

struct Point {
    int x;
    int y;
    void flip() {
        const int t = x;
        x = y;
        this->y = t;
    }
    Point plus(const Point& o) const {
        return Point{x + o.x, y + o.y};
    }
    static Point origin() {
        Point p{0, 0};
        return p;
    }
    void copy_from(const Point& o) {
        *this = o;
    }
};

int main() {
    zoo::Pen pen = {{}, 7};
    zoo::Animal cat(std::string("cat"), 4U);
    pen.admit(cat);
    pen.admit(zoo::Animal(std::string("fish"), 0U));
    Point p = Point::origin();
    Point q = {3, 4};
    p = p.plus(q);
    p.flip();
    q.x -= 1;
    Point r = q;
    r.copy_from(p);
    Graph g = {{Node{5}, {6}}};
    g.nodes.push_back(Node{7});
    const long long w = g.weight(g.nodes[2]);
    std::printf("%d %d %d %d %d %lld\n", pen.count(), p.x, p.y, r.y, static_cast<int>(pen.animals[1].walks()), w);
    return 0;
}
