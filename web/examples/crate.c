#include <stdio.h>

/* Emitted as a complete cargo crate: Cargo.toml plus src/main.rs, ready to
   `cargo run`. This shape is what the project's end-to-end oracle builds and
   byte-diffs against the clang-built native binary, byte for byte. */
static unsigned fnv1a(const char *s) {
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
        s++;
    }
    return h;
}

int main(void) {
    printf("%-6s %08x\n", "emit", fnv1a("emit"));
    printf("%-6s %08x\n", "rust", fnv1a("rust"));
    printf("%-6s %08x\n", "from", fnv1a("from"));
    printf("%-6s %08x\n", "c",    fnv1a("c"));
    return 0;
}
