// FR-43: the shared header of `search-backtrack.cpp`.
//
// `external_scale` is the load-bearing declaration: the project CALLS it and
// no translation unit of the project DEFINES it, because in the original C++
// program it comes from a prebuilt library outside the transpiled set. That is
// an entirely ordinary thing for a header to say, and nothing about it is
// outside the supported subset -- the coloring probe has no grounds to call it
// anything but Green, and it is right that it does not.
#ifndef SEARCH_BACKTRACK_H
#define SEARCH_BACKTRACK_H

int external_scale(int value);

int read_scaled(int value);
int triple(int value);
int shared_util(int value);

#endif
