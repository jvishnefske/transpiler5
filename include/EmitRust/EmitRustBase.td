//===- EmitRustBase.td - EmitRust dialect ------------------*- tablegen -*-===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
// Defines the MLIR EmitRust dialect and the base classes shared by its
// operation, type, and attribute definitions.
//
//===----------------------------------------------------------------------===//

#ifndef EMITRUST_EMITRUSTBASE
#define EMITRUST_EMITRUSTBASE

include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// EmitRust dialect definition
//===----------------------------------------------------------------------===//

def EmitRust_Dialect : Dialect {
  let name = "emitrust";
  let cppNamespace = "::mlir::emitrust";

  let summary = "Dialect to generate Rust from MLIR.";
  let description = [{
    The EmitRust dialect models Rust language constructs so that a direct,
    syntax-directed translation can emit readable Rust source code. It is
    modeled on the upstream EmitC dialect: values are SSA, statements are
    ops, and everything the emitter cannot represent is rejected with a
    located diagnostic.
  }];

  let useDefaultTypePrinterParser = 1;
  let useDefaultAttributePrinterParser = 1;

  let extraClassDeclaration = [{
    /// Registers the dialect types; defined in EmitRustTypes.cpp alongside
    /// the generated type storage definitions.
    void registerTypes();
    /// Registers the dialect attributes; defined in EmitRustAttributes.cpp
    /// alongside the generated attribute storage definitions.
    void registerAttributes();
  }];
}

//===----------------------------------------------------------------------===//
// Base classes
//===----------------------------------------------------------------------===//

// Base class for EmitRust dialect ops.
class EmitRust_Op<string mnemonic, list<Trait> traits = []>
    : Op<EmitRust_Dialect, mnemonic, traits>;

#endif // EMITRUST_EMITRUSTBASE
