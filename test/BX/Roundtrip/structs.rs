// FR-257: Rust structs and impls -> C++, law 2 at the CLI from the Rust side
// (the C++ printed for this file, fed back in, prints byte-identical C++; its
// Rust is this file's canonical Rust). Pins that rs_parse resolves struct
// TYPES and literals with Rust's path rules like calls (`super::Frame`,
// `shapes::Frame` relative to the root, `crate::shapes::Size` -- all one
// absolute Kernel struct), field-init shorthand `Size { w, h: .. }` /
// `Frame { .., title, .. }`, a struct literal nested in another, `S::new(..)`
// <=> `S(..)` (a constructor whose member-initializer list takes a moved
// parameter of the field's own name: `title(std::move(title))`), a call
// with a struct-valued argument inside an `if` condition (rustc forbids a
// bare struct literal there; the parser follows it), and a whole-member
// store `all[0].size = Size { .. }`.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
// CHECK:       mod shapes {
// CHECK-NEXT:      #[derive(Clone)]
// CHECK-NEXT:      pub struct Size {
// CHECK-NEXT:          pub w: i32,
// CHECK-NEXT:          pub h: i32,
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      #[derive(Clone)]
// CHECK-NEXT:      pub struct Frame {
// CHECK-NEXT:          pub size: Size,
// CHECK-NEXT:          pub title: String,
// CHECK-NEXT:          pub tags: Vec<String>,
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      impl Frame {
// CHECK-NEXT:          pub fn new(mut title: String, w: i32) -> Frame {
// CHECK-NEXT:              return Frame { size: Size { w, h: w * 2 }, title, tags: Vec::new() };
// CHECK-NEXT:          }
// CHECK-EMPTY:
// CHECK-NEXT:          pub fn area(&self) -> i32 {
// CHECK-NEXT:              return self.size.w * self.size.h;
// CHECK-NEXT:          }
// CHECK-EMPTY:
// CHECK-NEXT:          pub fn tag(&mut self, t: &String) {
// CHECK-NEXT:              self.tags.push(t.clone());
// CHECK-NEXT:          }
// CHECK-NEXT:      }
// CHECK-EMPTY:
// CHECK-NEXT:      pub mod util {
// CHECK-NEXT:          pub fn wider(a: &crate::shapes::Frame, b: &crate::shapes::Frame) -> bool {
// CHECK-NEXT:              return a.size.w > b.size.w;
// CHECK-NEXT:          }
// CHECK-NEXT:      }
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  fn big(mut f: crate::shapes::Frame) -> bool {
// CHECK-NEXT:      return f.area() > 50;
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  fn main() {
// CHECK-NEXT:      let mut a: crate::shapes::Frame = crate::shapes::Frame::new(String::from("a"), 4);
// CHECK-NEXT:      let b: crate::shapes::Frame = crate::shapes::Frame::new(String::from("b"), 6);
// CHECK-NEXT:      a.tag(&String::from("x"));
// CHECK-NEXT:      a.size.h += 1;
// CHECK-NEXT:      if big(crate::shapes::Frame::new(String::from("c"), 9)) && crate::shapes::util::wider(&b, &a) {
// CHECK-NEXT:          print!("{} {} {}\n", a.area(), b.area(), a.tags.len());
// CHECK-NEXT:      }
// CHECK-NEXT:      let mut all: Vec<crate::shapes::Frame> = vec![a.clone(), b.clone()];
// CHECK-NEXT:      all[0usize].size = crate::shapes::Size { w: 1, h: 1 };
// CHECK-NEXT:      print!("{} {}\n", all[0usize].area(), a.title);
// CHECK-NEXT:  }
// CPP:         #include <cstdio>
// CPP-NEXT:    #include <string>
// CPP-NEXT:    #include <utility>
// CPP-NEXT:    #include <vector>
// CPP-EMPTY:
// CPP-NEXT:    namespace shapes {
// CPP-EMPTY:
// CPP-NEXT:    struct Size {
// CPP-NEXT:        int w;
// CPP-NEXT:        int h;
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    struct Frame {
// CPP-NEXT:        Size size;
// CPP-NEXT:        std::string title;
// CPP-NEXT:        std::vector<std::string> tags;
// CPP-NEXT:        Frame(std::string title, const int w);
// CPP-NEXT:        int area() const;
// CPP-NEXT:        void tag(const std::string& t);
// CPP-NEXT:    };
// CPP-EMPTY:
// CPP-NEXT:    }  // namespace shapes
// CPP-EMPTY:
// CPP-NEXT:    namespace shapes {
// CPP-EMPTY:
// CPP-NEXT:    Frame::Frame(std::string title, const int w) : size(Size{w, w * 2}), title(std::move(title)), tags(std::vector<std::string>{}) {}
// CPP-EMPTY:
// CPP-NEXT:    int Frame::area() const {
// CPP-NEXT:        return this->size.w * this->size.h;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    void Frame::tag(const std::string& t) {
// CPP-NEXT:        this->tags.push_back(t);
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    namespace util {
// CPP-EMPTY:
// CPP-NEXT:    bool wider(const ::shapes::Frame& a, const ::shapes::Frame& b) {
// CPP-NEXT:        return a.size.w > b.size.w;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    }  // namespace util
// CPP-EMPTY:
// CPP-NEXT:    }  // namespace shapes
// CPP-EMPTY:
// CPP-NEXT:    bool big(::shapes::Frame f) {
// CPP-NEXT:        return f.area() > 50;
// CPP-NEXT:    }
// CPP-EMPTY:
// CPP-NEXT:    int main() {
// CPP-NEXT:        ::shapes::Frame a = ::shapes::Frame(std::string("a"), 4);
// CPP-NEXT:        const ::shapes::Frame b = ::shapes::Frame(std::string("b"), 6);
// CPP-NEXT:        a.tag(std::string("x"));
// CPP-NEXT:        ++a.size.h;
// CPP-NEXT:        if (big(::shapes::Frame(std::string("c"), 9)) && ::shapes::util::wider(b, a)) {
// CPP-NEXT:            std::printf("%d %d %zu\n", a.area(), b.area(), a.tags.size());
// CPP-NEXT:        }
// CPP-NEXT:        std::vector<::shapes::Frame> all = {a, b};
// CPP-NEXT:        all[static_cast<std::size_t>(0)].size = ::shapes::Size{1, 1};
// CPP-NEXT:        std::printf("%d %s\n", all[static_cast<std::size_t>(0)].area(), a.title.c_str());
// CPP-NEXT:        return 0;
// CPP-NEXT:    }
mod shapes {
    #[derive(Clone)]
    pub struct Size {
        pub w: i32,
        pub h: i32,
    }

    #[derive(Clone)]
    pub struct Frame {
        pub size: Size,
        pub title: String,
        pub tags: Vec<String>,
    }

    impl Frame {
        pub fn new(mut title: String, w: i32) -> Frame {
            return Frame { size: Size { w, h: w * 2 }, title, tags: Vec::new() };
        }

        pub fn area(&self) -> i32 {
            return self.size.w * self.size.h;
        }

        pub fn tag(&mut self, t: &String) {
            self.tags.push(t.clone());
        }
    }

    pub mod util {
        pub fn wider(a: &super::Frame, b: &crate::shapes::Frame) -> bool {
            return a.size.w > b.size.w;
        }
    }
}

fn big(mut f: shapes::Frame) -> bool {
    return f.area() > 50;
}

fn main() {
    let mut a: crate::shapes::Frame = crate::shapes::Frame::new(String::from("a"), 4);
    let b: shapes::Frame = shapes::Frame::new(String::from("b"), 6);
    a.tag(&String::from("x"));
    a.size.h += 1;
    if big(shapes::Frame::new(String::from("c"), 9)) && shapes::util::wider(&b, &a) {
        print!("{} {} {}\n", a.area(), b.area(), a.tags.len());
    }
    let mut all: Vec<crate::shapes::Frame> = vec![a.clone(), b];
    all[0usize].size = crate::shapes::Size { w: 1, h: 1 };
    print!("{} {}\n", all[0usize].area(), a.title);
}
