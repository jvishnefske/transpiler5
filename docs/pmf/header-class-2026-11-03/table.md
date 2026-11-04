| TU | function | class | n | strict blocker set (own body + class rows) | extra rows via callees |
|---|---|---|--:|---|---|
| font.cc | FindTable | Font | 5 | ?: of non-scalar type, local non-kernel, return of non-kernel type, cls:map-field, cls:nested-class |  |
| font.cc | FindTable | Font | 6 | ?: of non-scalar type, local non-kernel, method-overloading, return of non-kernel type, cls:map-field, cls:nested-class |  |
| font.cc | OutputOrderedTags | Font | 5 | local non-kernel, other: std::vector::erase (iterators and pointers are outside the subset), other: std::vector::insert (iterators and pointers are outside the subset), cls:map-field, cls:nested-class |  |
| font.cc | IsReused | Font::Table (nested; hand-derived) | 4 | pointer: null (R6), cls:map-field, cls:nested-class, cls:pointer-field |  |
| woff2_out.cc | WOFF2StringOut() | WOFF2StringOut | 4 | ctor-body, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | Write | WOFF2StringOut | 5 | method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private | member access through a pointer |
| woff2_out.cc | Write | WOFF2StringOut | 6 | member access through a pointer, method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | SetMaxSize | WOFF2StringOut | 3 | cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | WOFF2MemoryOut() | WOFF2MemoryOut | 4 | ctor-body, cls:inheritance, cls:pointer-field, cls:private |  |
| woff2_out.cc | Write | WOFF2MemoryOut | 5 | method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private | libcall:memcpy |
| woff2_out.cc | Write | WOFF2MemoryOut | 6 | libcall:memcpy, method-overloading, param non-kernel, cls:inheritance, cls:pointer-field, cls:private |  |
| context.cc | InitAll | ComponentStateDC | 8 | typing: counting-loop canonical form, cls:ctor-body(inline), cls:field-of-unimported-class, cls:inline-body:member access through a pointer, cls:inline-body:object construction, cls:inline-body:other: std::vector::resize without a fill value, cls:private, cls:vector<non-kernel>-field |  |
| context.cc | InitAll | ComponentState | 12 | array-subscript, DCHECK-abort, local non-kernel, typing: counting-loop canonical form, cls:array-field, cls:ctor-body(inline), cls:inline-body:member access through a pointer, cls:inline-body:object construction, cls:inline-body:other: std::vector::resize without a fill value, cls:inline-body:sizeof/alignof, cls:private, cls:vector<non-kernel>-field |  |
| ans_decode.cc | Init | ANSDecodingData | 5 | comma operator, member access(unimported class), cls:array-field, cls:ctor-body(inline), cls:inline-body:object construction |  |
| brunsli_decode.cc | BrunsliDecoder() | BrunsliDecoder | 9 | ctor-body, member access through a pointer, object construction, param non-kernel, cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field |  |
| brunsli_decode.cc | ~BrunsliDecoder() | BrunsliDecoder | 5 | cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field |  |
| brunsli_decode.cc | Decode | BrunsliDecoder | 14 | dereference, enumerator, DCHECK-abort, local non-kernel, member access(unimported class), param non-kernel, pointer: null (R6), return of non-kernel type, switch statement, cls:ctor-overload/copy, cls:destructor, cls:enum-in-class, cls:private, cls:unique_ptr-field | 45 rows |
| huffman_decode.cc | ReadFromBitStream | HuffmanDecodingData | 16 | array-subscript, array-to-pointer decay, assignment to something other than a local variable, compound assignment to something other than a local variable, declaration without an initializer, default argument, hdr-fn-rejected, libcall:BrunsliBitReaderIsHealthy, libcall:BuildHuffmanTable, local non-kernel, member access(unimported class), member access through a pointer, param non-kernel, pointer: null (R6), static local variable, cls:vector<non-kernel>-field | libcall:memcpy, libcall:memset, switch statement, typing: counting-loop canonical form |
| huffman_decode.cc | ReadSymbol | HuffmanDecodingData | 8 | assignment to something other than a local variable, compound assignment to something other than a local variable, declaration without an initializer, hdr-fn-rejected, local non-kernel, member access(unimported class), param non-kernel, cls:vector<non-kernel>-field |  |
| state.cc | State() | State | 9 | ctor-body, object construction, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | State() | State | 8 | struct: definition of a rejected member, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ~State() | State | 7 | cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ApplyOptions | State | 8 | param non-kernel, cls:NSDMI, cls:ctor-overload/copy, cls:destructor, cls:enum-field, cls:pointer-field, cls:unique_ptr-field, cls:vector<non-kernel>-field |  |
| state.cc | ~MetadataState() | MetadataState | 8 | libcall:BrotliDecoderDestroyInstance, pointer: null (R6), cls:NSDMI, cls:destructor, cls:enum-field, cls:enum-in-class, cls:field-of-unimported-class, cls:pointer-field |  |
| brunsli_encode.cc | Histogram() | Histogram | 3 | ctor-body, member access through a pointer, cls:array-field | libcall:memset |
| brunsli_encode.cc | Clear | Histogram | 2 | libcall:memset, cls:array-field |  |
| brunsli_encode.cc | AddHistogram | Histogram | 2 | array-subscript, cls:array-field |  |
| brunsli_encode.cc | Add | Histogram | 3 | array-subscript, DCHECK-abort, cls:array-field |  |
| brunsli_encode.cc | Merge | Histogram | 2 | array-subscript, cls:array-field |  |
| brunsli_encode.cc | Resize | EntropySource | 3 | other: std::vector::resize without a fill value, cls:array-field, cls:private |  |
| brunsli_encode.cc | AddCode | EntropySource | 2 | cls:array-field, cls:private | array-subscript, DCHECK-abort |
| brunsli_encode.cc | Finish | EntropySource | 5 | explicit cast to non-kernel type, other: std::vector::swap, return of non-kernel type, cls:array-field, cls:private |  |
| brunsli_encode.cc | Merge | EntropySource | 3 | DCHECK-abort, cls:array-field, cls:private | array-subscript |
| brunsli_encode.cc | EntropyCodes() | EntropyCodes | 7 | ctor-body, libcall:ClusterHistograms, object construction, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | EncodeContextMap | EntropyCodes | 6 | libcall:EncodeContextMap, param non-kernel, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | BuildAndStoreEntropyCodes | EntropyCodes | 6 | libcall:BuildAndStoreANSEncodingData, param non-kernel, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | GetANSTable | EntropyCodes | 5 | return of non-kernel type, cls:array-field, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | DataStream() | DataStream | 6 | ctor-body, object construction, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | Resize | DataStream | 4 | cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | ResizeForBlock | DataStream | 6 | global variable, static local variable, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | AddCode | DataStream | 8 | DCHECK-abort, local non-kernel, member access(unimported class), normalize: incdec operand, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field | array-subscript, cls:array-field |
| brunsli_encode.cc | AddBits | DataStream | 6 | local non-kernel, member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | FlushArithmeticCoder | DataStream | 5 | member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | FlushBitWriter | DataStream | 5 | member access(unimported class), cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | AddBit | DataStream | 7 | member access(unimported class), member access through a pointer, param non-kernel, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field |  |
| brunsli_encode.cc | EncodeCodeWords | DataStream | 12 | hdr-fn-rejected, implicit conversion, local non-kernel, member access(unimported class), param non-kernel, reinterpret_cast, sizeof/alignof, typing: counting-loop canonical form, cls:nested-class, cls:private, cls:static-data-member, cls:vector<non-kernel>-field | return of non-kernel type, cls:array-field |
| write_bits.cc | Storage() | Storage | 6 | ctor-body, DCHECK-abort, member access through a pointer, cls:const-field, cls:destructor, cls:pointer-field |  |
| write_bits.cc | AppendBytes | Storage | 5 | DCHECK-abort, libcall:memcpy, cls:const-field, cls:destructor, cls:pointer-field |  |
| write_bits.cc | ~Storage() | Storage | 4 | DCHECK-abort, cls:const-field, cls:destructor, cls:pointer-field |  |
