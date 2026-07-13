# The Arith-to-EmitRust conversion library: scalar constants, signed
# arithmetic, signed/ordered comparisons, and signed casts.
#
# When EMITRUST_ENABLE_PDLL is ON (requires the mlir-pdll tool from the
# `nix develop .#pdll` shell), a small PDLL showcase compiles
# ArithToEmitRust.pdll into additional 1:1 patterns that are registered next
# to the C++ ones. With the option OFF (the default) nothing here changes:
# mlir-pdll is never invoked and the compiled code is bit-identical to the
# C++-only baseline.
set(EMITRUST_ARITH_PDLL_DEPS "")
set(EMITRUST_ARITH_PDLL_LIBS "")
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

  add_mlir_pdll_library(EmitRustArithToEmitRustPDLLPatternsIncGen
    ArithToEmitRust.pdll
    ArithToEmitRustPDLLPatterns.h.inc
    )
  set(EMITRUST_ARITH_PDLL_DEPS EmitRustArithToEmitRustPDLLPatternsIncGen)
  set(EMITRUST_ARITH_PDLL_LIBS
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

add_mlir_conversion_library(MLIRArithToEmitRust
  ArithToEmitRust.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen
  ${EMITRUST_ARITH_PDLL_DEPS}

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRArithDialect
  MLIRPass
  MLIRTransformUtils
  ${EMITRUST_ARITH_PDLL_LIBS}
  )
