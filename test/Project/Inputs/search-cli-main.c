// FR-43: the entry-point TU of `search-cli.c`, which is a library on its own
// and therefore has no `main` of its own to give.
int library_helper(int value);

int main(void) { return library_helper(3) - 8; }
