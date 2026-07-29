# The project-analysis library (FR-40, FR-41, FR-43): pure-AST whole-project
# analyses over the same clang ASTs the importer consumes, computed
# independently of any import. FR-43's search sits on top of them and needs an
# IMPORT to probe a candidate subset, but takes it as an injected callback
# (`ProbeFn`) rather than calling the importer here, so this library stays a
# functional core and the import stays in the driver's imperative shell. Like MLIREmitRustImportC, the clang dependencies stay PRIVATE so no
# consumer picks them up transitively; the public headers under
# include/EmitRust/Project only forward-declare clang types.
#
# It links MLIREmitRustImportC for the shared clang-driving shell
# (EmitRust/ClangProjectParser.h): that shell must be one implementation, or
# the graph and the importer could disagree about what the project contains,
# and it lives in the importer library because the configure-time
# `clang -print-resource-dir` probe defines EMITRUST_CLANG_RESOURCE_DIR on
# that target.
add_mlir_library(MLIREmitRustProject
  FrontierSearch.cpp
  ItemColoring.cpp
  ItemGraph.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Project

  LINK_LIBS PUBLIC
  MLIREmitRustImportC
  MLIRSupport
  PRIVATE
  clangAST
  clangBasic
  clangFrontend
  )
