- [x] FR-3 Op verifiers reject malformed IR with precise diagnostics:
  assign to a non-let value, assign to a non-mut let, func with more than
  one result, cmp/binary type mismatches, empty opaque type string.
  (test/Dialect/EmitRust/invalid.mlir)
