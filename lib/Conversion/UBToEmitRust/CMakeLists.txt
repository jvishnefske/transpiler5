# The UB-to-EmitRust conversion library: ub.poison to emitrust.constant.
#
# When EMITRUST_ENABLE_PDLL is ON (requires the mlir-pdll tool from the
# `nix develop .#pdll` shell), UBToEmitRust.pdll compiles into a duplicate
# declarative pattern that is registered next to the C++ one. With the
# option OFF (the default) nothing here changes: mlir-pdll is never invoked
# and the compiled code is bit-identical to the C++-only baseline.
set(EMITRUST_UB_PDLL_DEPS "")
set(EMITRUST_UB_PDLL_LIBS "")
if(EMITRUST_ENABLE_PDLL)
  # MLIRConfig.cmake defaults MLIR_PDLL_TABLEGEN_EXE to the bare name
  # "mlir-pdll", which the tablegen rule also lists as a file dependency,
  # so ninja looks for it relative to the build directory. Resolve the real
  # executable from PATH (provided by the .#pdll dev shell) instead.
  find_program(EMITRUST_MLIR_PDLL_EXE mlir-pdll)
  if(NOT EMITRUST_MLIR_PDLL_EXE)
    message(FATAL_ERROR
      "EMITRUST_ENABLE_PDLL requires mlir-pdll on PATH; enter the "
      "`nix develop .#pdll` shell or turn the option OFF.")
  endif()
  set(MLIR_PDLL_TABLEGEN_EXE ${EMITRUST_MLIR_PDLL_EXE})

  add_mlir_pdll_library(EmitRustUBToEmitRustPDLLPatternsIncGen
    UBToEmitRust.pdll
    UBToEmitRustPDLLPatterns.h.inc
    )
  set(EMITRUST_UB_PDLL_DEPS EmitRustUBToEmitRustPDLLPatternsIncGen)
  set(EMITRUST_UB_PDLL_LIBS
    MLIRParser
    MLIRPDLDialect
    MLIRPDLInterpDialect
    MLIRRewrite
    )
  # Directory-scoped so the definitions and the generated-header include path
  # reach the object library that actually compiles the sources.
  add_definitions(-DEMITRUST_ENABLE_PDLL)
  include_directories(${CMAKE_CURRENT_BINARY_DIR})
endif()

add_mlir_conversion_library(MLIRUBToEmitRust
  UBToEmitRust.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen
  ${EMITRUST_UB_PDLL_DEPS}

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRUBDialect
  MLIRPass
  MLIRTransformUtils
  ${EMITRUST_UB_PDLL_LIBS}
  )
