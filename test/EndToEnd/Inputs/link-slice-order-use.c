/* FR-158 phase 2 companion TU: the CALLER, which only ever sees the
   prototype. Its string-literal argument imports to the same
   `addr_of mut (subscript lit[0])` cursor a `&arr[k]` argument does. */

void note(const char *text, int line);

int run(int k) {
  note("hi", k);
  return 0;
}
