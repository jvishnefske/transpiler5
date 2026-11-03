// Project header for ../normalize-header-reject.cpp (FR-254 R4). Not a test:
// lit excludes every Inputs/ directory.
#pragma once
#include <cstdio>

inline int Counter() {
    static int n = 0;
    n = n + 1;
    return n;
}

inline int Store16(int v) {
    return v & 0xffff;
}

inline int Store16(int v, int w) {
    return (v + w) & 0xffff;
}

inline int Scale(int v) {
    return v * 3;
}

inline int Inner(int v) {
    int t[2] = {v, v};
    return t[0];
}

inline int Outer(int v) {
    return Inner(v) + 1;
}

constexpr int Twice(int v) {
    return 2 * v;
}

template <typename T>
inline T Id(T v) {
    return v;
}

inline int Plus1(int v) {
    return v + 1;
}

inline int Unreached(int v) {
    int t[2] = {v, v};
    return t[1];
}
