// RealWorld C++ corpus (FR-46/FR-52), project `extern-plugin`: the PLATFORM
// layer this project depends on and does not contain.
//
// WHY THIS PROJECT EXISTS. Every other project in this corpus is closed --
// every symbol it references, it also defines -- which made the single most
// common state of a real partial port invisible to the benchmark: code that
// depends on something not yet ported, or on foreign code that never will be.
// Before FR-52 such a project produced NO crate at all; the whole-program
// "referenced but not defined in any translation unit" error is not
// attributable to any one item, so `--incremental` could not stub around it
// and `--search` could only repair it by DROPPING real, translatable code.
//
// Nothing below is ever defined. The project therefore emits
// `pub trait Externals` declaring exactly these three functions, and the
// transitive closure of their callers becomes generic over it. The crate
// still has to COMPILE, which is what the LIB_BUILT outcome measures.
//
// The code shape is the same C-shaped subset `ringbuf-lib` uses -- data-only
// structs through pointer parameters, free functions in a namespace -- so the
// project's contribution is its DEPENDENCY STRUCTURE, not new constructs.
#ifndef EXTERN_PLUGIN_PLATFORM_HPP
#define EXTERN_PLUGIN_PLATFORM_HPP

namespace platform {

/// A monotonic millisecond counter supplied by the host.
int millis();

/// Reads the raw value of hardware channel `channel`.
int read_raw(int channel);

/// Reports a fault code to the host's log.
void fault(int code);

} // namespace platform

#endif
