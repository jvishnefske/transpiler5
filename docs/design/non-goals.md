## Non-Goals for the MVP

Generics, lifetimes beyond simple references, traits and impls, pattern
matching beyond literal match arms, data-carrying enums (C-like unit-variant
enums are supported), error-handling sugar, and expression trees (every
value is a named let binding; no inlining of subexpressions). On the C side
the importer rejects, with located diagnostics: computed goto (plain
goto/labels are supported per C99-33), unions outside the one-slot
model (CTS-R3), bit-fields outside the C99-45 backing-run accessor
model,
int-to-enum conversions, pointer-to-pointer values, pointer struct fields,
NULL data pointers, void* casts, malloc and friends
(pointer arithmetic, pointer locals, and pointer/array parameters are now
supported through the FR-28 decomposition; pointer-typed globals with one
global region base are supported per CTS-P4, including its carve-out
promoting a single constant-size calloc/malloc site bound to a global
pointer into a static backing array),
multi-dimensional arrays, sizeof/_Alignof of
variable-length-array/incomplete/function operands, conditional operators
with non-scalar results, va_list-using variadic definitions (a variadic
definition whose body never touches va_list imports as its fixed
prototype per CTS-F1, with effect-free trailing extras dropped at call
sites), and `char *` variables
bound to string literals (aggregate initializer lists are supported per
C99-11/12, `char s[] = "..."` and the printf/puts %s shapes per
C99-28/47). These are natural follow-ons; the emitter's
statement-per-op model is chosen precisely so expression inlining can be
layered in later, as EmitC did.

