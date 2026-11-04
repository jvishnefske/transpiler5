# Hybrid-wedge spike sample (chosen before any rewrite was attempted)

Instrument: /tmp/fr264/front-head/frontier-2026-11-03-4f2a2f5-normalize.json (bx 4f2a2f5, --normalize --project-root).
Population: blocked functions (any diag or container_diag), excluding `main`, whose body extent
(clang++ AST JSON range, main file) is <= 40 lines.  274 of 352 blocked functions with a resolvable extent.
Order: sha1("hyb-2026-11-03" + module + qualified name + begin line), ascending.
Quotas per module (woff2, brunsli), each taking the first functions in hash order that fit and whose
exact tag set has not already been taken within that quota:
  method (CXXMethodDecl/ctor/dtor, any tag count): 2
  multi3 (free function, >=3 distinct tags): 3
  free12 (free function, 1-2 tags): 5
Total 20.

| # | module | function | stratum | lines | file:begin-end | tags |
|---|---|---|---|---|---|---|
| 1 | woff2 | woff2::GlyfEncoder::GetTransformedGlyfBytes | method | 26 | woff2/src/transform.cc:72-97 | struct: has members outside the subset; struct: member; typing: argument count mismatch |
| 2 | woff2 | woff2::OutputOrderedTags | method | 26 | woff2/src/font.cc:31-56 | member function |
| 3 | woff2 | woff2::RemoveDigitalSignature | multi3 | 9 | woff2/src/font.cc:392-400 | local variable of non-kernel type; member access; parameter of non-kernel type |
| 4 | woff2 | woff2::ConvertWOFF2ToTTF | multi3 | 5 | woff2/src/woff2_dec.cc:1350-1354 | implicit conversion; local variable of non-kernel type; typing: argument count mismatch |
| 5 | woff2 | woff2::ReadCompositeGlyphData | multi3 | 33 | woff2/src/glyph.cc:32-64 | call to library function; member access; member access through a pointer; parameter of non-kernel type |
| 6 | woff2 | woff2::TransformFontCollection | free12 | 12 | woff2/src/woff2_enc.cc:194-205 | member access; parameter of non-kernel type |
| 7 | woff2 | woff2::Woff2Uncompress | free12 | 11 | woff2/src/woff2_dec.cc:798-808 | indirect call; local variable of non-kernel type |
| 8 | woff2 | woff2::MaxWOFF2CompressedSize | free12 | 3 | woff2/src/woff2_enc.cc:177-179 | typing: argument count mismatch |
| 9 | woff2 | woff2::ReadBase128 | free12 | 24 | woff2/src/variable_length.cc:86-109 | member access through a pointer; parameter of non-kernel type |
| 10 | woff2 | woff2::KnownTableIndex | free12 | 6 | woff2/src/woff2_enc.cc:62-67 | array subscript |
| 11 | brunsli | brunsli::ReadSymbol | method | 14 | brunsli/c/dec/huffman_decode.cc:248-261 | member function |
| 12 | brunsli | brunsli::internal::dec::State | method | 1 | brunsli/c/dec/state.cc:19-19 | CXXConstructorDecl |
| 13 | brunsli | brunsli::BitWriterFinish | multi3 | 8 | brunsli/c/dec/jpeg_data_writer.cc:178-185 | member access; member access through a pointer; parameter of non-kernel type |
| 14 | brunsli | brunsli::EncodeOriginalJpg | multi3 | 10 | brunsli/c/enc/brunsli_encode.cc:1594-1603 | call to library function; cast to void; member access; parameter of non-kernel type; pointer: null (R6) |
| 15 | brunsli | brunsli::internal::dec::ProcessJpeg | multi3 | 38 | brunsli/c/dec/brunsli_decode.cc:2449-2486 | assignment to something other than a local variable; enumerator; local variable of non-kernel type; member access; parameter of non-kernel type; return of non-kernel type; typing: return form |
| 16 | brunsli | brunsli::EncodeEOI | free12 | 5 | brunsli/c/dec/jpeg_data_writer.cc:313-317 | member access; parameter of non-kernel type |
| 17 | brunsli | brunsli::DecodeLehmerCode | free12 | 12 | brunsli/c/common/lehmer_code.cc:31-42 | cascade: use of a rejected declaration; other: std::vector(n) without a fill value |
| 18 | brunsli | brunsli::PeekByte | free12 | 4 | brunsli/c/dec/brunsli_decode.cc:996-999 | array subscript; parameter of non-kernel type |
| 19 | brunsli | brunsli::HuffExtend | free12 | 10 | brunsli/c/enc/jpeg_data_reader.cc:572-581 | call to library function |
| 20 | brunsli | brunsli::NextTableBitSize | free12 | 10 | brunsli/c/enc/jpeg_huffman_decode.cc:17-26 | pointer: backwards (R3) |

Frozen 2026-11-03 before any harness or rewrite was written. Extents from clang AST JSON (see /tmp/hyb/extents.py, /tmp/hyb/sel.py).
