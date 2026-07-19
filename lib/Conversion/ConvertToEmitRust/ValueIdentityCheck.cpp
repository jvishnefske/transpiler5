//===- ValueIdentityCheck.cpp - Differential value-identity checker -------===//
//
// This file is part of the EmitRust project.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Implements the emitrust-value-identity-check pass: a pipeline-level
/// differential CONCRETE interpretation. The pass interprets every
/// parameterless function of the module before and after the
/// convert-to-emitrust pipeline (sharing the two-stage input scaffold of
/// emitrust-range-refinement-check) and compares the values observed at
/// `emitrust.call_opaque "print!"` operands and function return operands
/// in execution order. A divergence is a located hard error; anything the
/// bounded interpreter does not cover soft-skips that entry point silently
/// (counted in the skipped-entry-points statistic). This catches the
/// lost-copy/swap class of value-identity miscompiles that range analysis
/// is structurally blind to.
///
/// Poison policy: `ub.poison` materializes a poison marker. The marker may
/// flow through rebindings (yields, branch arguments, stores, loads, call
/// arguments) without consequence, but an entry point that actually
/// EVALUATES poison — arithmetic, comparison, cast, branching, or an
/// observation point — is soft-skipped. Interpreting poison as the
/// constant the conversion folds it to (zero) would be unsound in
/// general: it would vacuously "verify" programs whose observable
/// behavior is undefined pre-conversion.
//
//===----------------------------------------------------------------------===//

#include "EmitRust/Conversion/ValueIdentityCheck.h"

#include "DifferentialStages.h"
#include "EmitRust/EmitRustOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <vector>

namespace mlir {
namespace emitrust {
#define GEN_PASS_DEF_EMITRUSTVALUEIDENTITYCHECK
#include "EmitRust/Conversion/Passes.h.inc"
} // namespace emitrust
} // namespace mlir

using namespace mlir;
using namespace mlir::emitrust;

namespace {

/// One interpreted value: an integer of its type's storage width, or the
/// poison marker (see the poison policy in the file header).
struct InterpValue {
  bool poison = false;
  APInt value;

  static InterpValue makePoison() { return InterpValue{true, APInt()}; }
  static InterpValue makeInt(APInt v) {
    return InterpValue{false, std::move(v)};
  }
  static InterpValue makeBool(bool b) {
    InterpValue result;
    result.value = APInt(1, b ? 1 : 0);
    return result;
  }
};

/// One observation event: a `print!` call or a return terminator executed
/// during one entry-point run, with the values of all its operands. The
/// op pointer (pre-conversion side) anchors the diagnostic location.
struct TraceEvent {
  bool isReturn;
  std::string function;
  Operation *op;
  SmallVector<APInt, 2> operands;
};

using Trace = std::vector<TraceEvent>;

/// Renders `value` as the SIGNED decimal the diagnostic contract pins.
std::string formatSigned(const APInt &value) {
  std::string text;
  llvm::raw_string_ostream os(text);
  value.print(os, /*isSigned=*/true);
  return text;
}

/// Storage width of an interpretable type: the bit width for integers,
/// 64 for index (usize), 0 for anything the interpreter does not cover.
unsigned intWidth(Type type) {
  if (auto intType = dyn_cast<IntegerType>(type))
    return intType.getWidth();
  if (isa<IndexType>(type))
    return 64;
  return 0;
}

/// Whether EmitRust ops on `type` follow unsigned Rust semantics: ui<N>
/// renders as uN and index as usize. Signless/signed integers render as
/// Rust's signed iN.
bool isUnsignedLike(Type type) {
  if (auto intType = dyn_cast<IntegerType>(type))
    return intType.isUnsigned();
  return isa<IndexType>(type);
}

/// A bounded environment-based concrete interpreter over one stage module.
/// Every unsupported construct aborts the current entry point with
/// `Status::Skip`; the interpreter never diagnoses anything itself.
class Interpreter {
public:
  enum class Status { Ok, Skip };

  Interpreter(Operation *symbolScope, uint64_t stepBudget)
      : scope(symbolScope), stepsLeft(stepBudget) {}

  /// Interprets `entry` (a parameterless function) and appends every
  /// observation event to `outTrace` in execution order.
  Status run(FunctionOpInterface entry, Trace &outTrace) {
    trace = &outTrace;
    FailureOr<SmallVector<InterpValue>> result = call(entry, {});
    return failed(result) ? Status::Skip : Status::Ok;
  }

private:
  /// One function activation: SSA bindings plus the mutable slots backing
  /// `emitrust.let` and scalar `emitrust.variable` results. A slot is
  /// keyed by its defining value; reads of that value observe the current
  /// slot content (Rust `let mut` semantics after `emitrust.assign`).
  struct Frame {
    DenseMap<Value, InterpValue> env;
    DenseMap<Value, InterpValue> slots;
  };

  /// Non-terminator control effect of one executed operation.
  enum class Flow { Advance, Break, Continue };

  Operation *scope;
  uint64_t stepsLeft;
  unsigned callDepth = 0;
  Trace *trace = nullptr;
  std::string currentFunction;

  static constexpr unsigned kMaxCallDepth = 512;

  /// Charges one interpreter step; failure means the budget is exhausted.
  LogicalResult step() {
    if (stepsLeft == 0)
      return failure();
    --stepsLeft;
    return success();
  }

  /// Reads `v` in `frame`: the current slot content for a let/variable
  /// result, the SSA binding otherwise. Poison propagates.
  FailureOr<InterpValue> getAny(Value v, Frame &frame) {
    auto slot = frame.slots.find(v);
    if (slot != frame.slots.end())
      return slot->second;
    auto it = frame.env.find(v);
    if (it == frame.env.end())
      return failure();
    return it->second;
  }

  /// Reads `v` as a concrete integer; poison here means the entry point
  /// evaluates poison and must be skipped.
  FailureOr<APInt> getInt(Value v, Frame &frame) {
    FailureOr<InterpValue> value = getAny(v, frame);
    if (failed(value) || value->poison)
      return failure();
    return value->value;
  }

  /// Evaluates all `values`, propagating poison.
  FailureOr<SmallVector<InterpValue>> getAll(ValueRange values, Frame &frame) {
    SmallVector<InterpValue> result;
    for (Value v : values) {
      FailureOr<InterpValue> value = getAny(v, frame);
      if (failed(value))
        return failure();
      result.push_back(*value);
    }
    return result;
  }

  void bind(Value v, InterpValue value, Frame &frame) {
    frame.env[v] = std::move(value);
  }

  /// Records one observation event; a poison operand skips the entry
  /// point (poison must never reach an observation point).
  LogicalResult observe(Operation *op, bool isReturn, Frame &frame) {
    TraceEvent event;
    event.isReturn = isReturn;
    event.function = currentFunction;
    event.op = op;
    for (Value operand : op->getOperands()) {
      FailureOr<APInt> value = getInt(operand, frame);
      if (failed(value))
        return failure();
      event.operands.push_back(*value);
    }
    trace->push_back(std::move(event));
    return success();
  }

  /// Resolves `name` against the stage module's symbol table to an
  /// interpreted (non-external) function, or null.
  FunctionOpInterface resolveFunction(StringRef name) {
    Operation *symbol = SymbolTable::lookupSymbolIn(
        scope, StringAttr::get(scope->getContext(), name));
    auto fn = dyn_cast_if_present<FunctionOpInterface>(symbol);
    if (!fn || fn.isExternal())
      return nullptr;
    return fn;
  }

  /// Calls `fn` with `args`: fresh frame, CFG walk over the body blocks
  /// (cf.br / cf.cond_br), observation of the executed return.
  FailureOr<SmallVector<InterpValue>> call(FunctionOpInterface fn,
                                           ArrayRef<InterpValue> args) {
    if (callDepth >= kMaxCallDepth || fn.isExternal())
      return failure();
    Region &body = fn.getFunctionBody();
    Block *block = &body.front();
    if (block->getNumArguments() != args.size())
      return failure();

    ++callDepth;
    std::string callerFunction = currentFunction;
    currentFunction = SymbolTable::getSymbolName(fn).getValue().str();
    auto restore = llvm::make_scope_exit([&] {
      --callDepth;
      currentFunction = callerFunction;
    });

    Frame frame;
    for (auto [arg, value] : llvm::zip(block->getArguments(), args))
      bind(arg, value, frame);

    while (true) {
      Operation *terminator = block->getTerminator();
      for (Operation &op : *block) {
        if (&op == terminator)
          break;
        FailureOr<Flow> flow = execOp(&op, frame);
        if (failed(flow))
          return failure();
        // Break/continue cannot legally reach a function body block.
        if (*flow != Flow::Advance)
          return failure();
      }
      if (failed(step()))
        return failure();
      if (isa<func::ReturnOp, emitrust::ReturnOp>(terminator)) {
        if (failed(observe(terminator, /*isReturn=*/true, frame)))
          return failure();
        return getAll(terminator->getOperands(), frame);
      }
      if (auto br = dyn_cast<cf::BranchOp>(terminator)) {
        FailureOr<SmallVector<InterpValue>> operands =
            getAll(br.getDestOperands(), frame);
        if (failed(operands))
          return failure();
        block = br.getDest();
        for (auto [arg, value] : llvm::zip(block->getArguments(), *operands))
          bind(arg, value, frame);
        continue;
      }
      if (auto condBr = dyn_cast<cf::CondBranchOp>(terminator)) {
        FailureOr<APInt> condition = getInt(condBr.getCondition(), frame);
        if (failed(condition))
          return failure();
        bool taken = !condition->isZero();
        ValueRange destOperands = taken ? condBr.getTrueDestOperands()
                                        : condBr.getFalseDestOperands();
        FailureOr<SmallVector<InterpValue>> operands =
            getAll(destOperands, frame);
        if (failed(operands))
          return failure();
        block = taken ? condBr.getTrueDest() : condBr.getFalseDest();
        for (auto [arg, value] : llvm::zip(block->getArguments(), *operands))
          bind(arg, value, frame);
        continue;
      }
      return failure();
    }
  }

  /// Executes the non-terminator operations of a structured single-block
  /// region body; the caller inspects the terminator itself.
  FailureOr<Flow> execStructuredBlock(Block &block, Frame &frame) {
    Operation *terminator = block.getTerminator();
    for (Operation &op : block) {
      if (&op == terminator)
        break;
      FailureOr<Flow> flow = execOp(&op, frame);
      if (failed(flow))
        return failure();
      if (*flow != Flow::Advance)
        return *flow;
    }
    return Flow::Advance;
  }

  /// Two's-complement transfer function shared by the arith and emitrust
  /// binary operations. Signedness only matters for div/rem/shr and is
  /// passed pre-resolved. Division by zero, signed division overflow, and
  /// out-of-range shift amounts are unsupported coverage.
  enum class BinKind { Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Shr };
  static FailureOr<APInt> applyBinary(BinKind kind, bool isUnsigned,
                                      const APInt &lhs, const APInt &rhs) {
    switch (kind) {
    case BinKind::Add:
      return lhs + rhs;
    case BinKind::Sub:
      return lhs - rhs;
    case BinKind::Mul:
      return lhs * rhs;
    case BinKind::Div: {
      if (rhs.isZero())
        return failure();
      if (isUnsigned)
        return lhs.udiv(rhs);
      bool overflow = false;
      APInt result = lhs.sdiv_ov(rhs, overflow);
      if (overflow)
        return failure();
      return result;
    }
    case BinKind::Rem:
      if (rhs.isZero())
        return failure();
      if (isUnsigned)
        return lhs.urem(rhs);
      if (rhs.isAllOnes() && lhs.isMinSignedValue())
        return failure();
      return lhs.srem(rhs);
    case BinKind::And:
      return lhs & rhs;
    case BinKind::Or:
      return lhs | rhs;
    case BinKind::Xor:
      return lhs ^ rhs;
    case BinKind::Shl:
      if (rhs.uge(lhs.getBitWidth()))
        return failure();
      return lhs.shl(rhs);
    case BinKind::Shr:
      if (rhs.uge(lhs.getBitWidth()))
        return failure();
      return isUnsigned ? lhs.lshr(rhs) : lhs.ashr(rhs);
    }
    return failure();
  }

  /// Evaluates a binary op with both operands concrete and binds the
  /// result.
  FailureOr<Flow> execBinary(Operation *op, BinKind kind, bool isUnsigned,
                             Frame &frame) {
    FailureOr<APInt> lhs = getInt(op->getOperand(0), frame);
    FailureOr<APInt> rhs = getInt(op->getOperand(1), frame);
    if (failed(lhs) || failed(rhs))
      return failure();
    FailureOr<APInt> result = applyBinary(kind, isUnsigned, *lhs, *rhs);
    if (failed(result))
      return failure();
    bind(op->getResult(0), InterpValue::makeInt(*result), frame);
    return Flow::Advance;
  }

  /// Six-predicate comparison shared by emitrust.cmp; arith.cmpi carries
  /// its signedness in the predicate instead.
  static bool applyCmp(emitrust::CmpPredicate predicate, bool isUnsigned,
                       const APInt &lhs, const APInt &rhs) {
    switch (predicate) {
    case emitrust::CmpPredicate::eq:
      return lhs == rhs;
    case emitrust::CmpPredicate::ne:
      return lhs != rhs;
    case emitrust::CmpPredicate::lt:
      return isUnsigned ? lhs.ult(rhs) : lhs.slt(rhs);
    case emitrust::CmpPredicate::le:
      return isUnsigned ? lhs.ule(rhs) : lhs.sle(rhs);
    case emitrust::CmpPredicate::gt:
      return isUnsigned ? lhs.ugt(rhs) : lhs.sgt(rhs);
    case emitrust::CmpPredicate::ge:
      return isUnsigned ? lhs.uge(rhs) : lhs.sge(rhs);
    }
    return false;
  }

  /// Rust `as` cast semantics (mirrors the emitrust.cast range transfer
  /// function): same width preserves bits, narrowing truncates, widening
  /// zero-extends from ui<N>/i1/index and sign-extends from signless or
  /// signed sources.
  static FailureOr<APInt> applyCast(Type sourceType, Type resultType,
                                    const APInt &value) {
    unsigned sourceWidth = intWidth(sourceType);
    unsigned resultWidth = intWidth(resultType);
    if (sourceWidth == 0 || resultWidth == 0)
      return failure();
    if (resultWidth == sourceWidth)
      return value;
    if (resultWidth < sourceWidth)
      return value.trunc(resultWidth);
    bool zeroExtend = isUnsignedLike(sourceType) || sourceType.isInteger(1);
    return zeroExtend ? value.zext(resultWidth) : value.sext(resultWidth);
  }

  /// Interprets a call_opaque: "print!" observes, a callee resolving to an
  /// interpreted module function calls it (the post-conversion spelling of
  /// func.call), anything else is unsupported coverage.
  FailureOr<Flow> execCallOpaque(emitrust::CallOpaqueOp op, Frame &frame) {
    if (FunctionOpInterface callee = resolveFunction(op.getCallee())) {
      // A resolved call must be the plain conversion spelling: no args
      // attribute reordering, matching arity.
      if (op.getArgsAttr() ||
          callee.getNumArguments() != op->getNumOperands() ||
          callee.getResultTypes().size() != op->getNumResults())
        return failure();
      FailureOr<SmallVector<InterpValue>> args =
          getAll(op->getOperands(), frame);
      if (failed(args))
        return failure();
      FailureOr<SmallVector<InterpValue>> results = call(callee, *args);
      if (failed(results) || results->size() != op->getNumResults())
        return failure();
      for (auto [result, value] : llvm::zip(op->getResults(), *results))
        bind(result, value, frame);
      return Flow::Advance;
    }
    if (op.getCallee() == "print!" && op->getNumResults() == 0) {
      if (failed(observe(op, /*isReturn=*/false, frame)))
        return failure();
      return Flow::Advance;
    }
    return failure();
  }

  /// Executes one non-terminator operation. Returning failure aborts the
  /// entry point as unsupported coverage.
  FailureOr<Flow> execOp(Operation *op, Frame &frame) {
    if (failed(step()))
      return failure();

    // Constants and poison.
    if (auto constant = dyn_cast<arith::ConstantOp>(op)) {
      auto attr = dyn_cast<IntegerAttr>(constant.getValue());
      if (!attr)
        return failure();
      bind(constant.getResult(), InterpValue::makeInt(attr.getValue()), frame);
      return Flow::Advance;
    }
    if (auto constant = dyn_cast<emitrust::ConstantOp>(op)) {
      auto attr = dyn_cast<IntegerAttr>(constant.getValue());
      if (!attr || intWidth(constant.getType()) == 0)
        return failure();
      bind(constant.getResult(), InterpValue::makeInt(attr.getValue()), frame);
      return Flow::Advance;
    }
    if (auto poison = dyn_cast<ub::PoisonOp>(op)) {
      bind(poison.getResult(), InterpValue::makePoison(), frame);
      return Flow::Advance;
    }

    // Arith integer arithmetic (signedness is in the opcode).
    if (isa<arith::AddIOp>(op))
      return execBinary(op, BinKind::Add, false, frame);
    if (isa<arith::SubIOp>(op))
      return execBinary(op, BinKind::Sub, false, frame);
    if (isa<arith::MulIOp>(op))
      return execBinary(op, BinKind::Mul, false, frame);
    if (isa<arith::DivSIOp>(op))
      return execBinary(op, BinKind::Div, false, frame);
    if (isa<arith::DivUIOp>(op))
      return execBinary(op, BinKind::Div, true, frame);
    if (isa<arith::RemSIOp>(op))
      return execBinary(op, BinKind::Rem, false, frame);
    if (isa<arith::RemUIOp>(op))
      return execBinary(op, BinKind::Rem, true, frame);
    if (isa<arith::AndIOp>(op))
      return execBinary(op, BinKind::And, false, frame);
    if (isa<arith::OrIOp>(op))
      return execBinary(op, BinKind::Or, false, frame);
    if (isa<arith::XOrIOp>(op))
      return execBinary(op, BinKind::Xor, false, frame);
    if (isa<arith::ShLIOp>(op))
      return execBinary(op, BinKind::Shl, false, frame);
    if (isa<arith::ShRSIOp>(op))
      return execBinary(op, BinKind::Shr, false, frame);
    if (isa<arith::ShRUIOp>(op))
      return execBinary(op, BinKind::Shr, true, frame);

    if (auto cmp = dyn_cast<arith::CmpIOp>(op)) {
      FailureOr<APInt> lhs = getInt(cmp.getLhs(), frame);
      FailureOr<APInt> rhs = getInt(cmp.getRhs(), frame);
      if (failed(lhs) || failed(rhs))
        return failure();
      bool result = false;
      switch (cmp.getPredicate()) {
      case arith::CmpIPredicate::eq:
        result = *lhs == *rhs;
        break;
      case arith::CmpIPredicate::ne:
        result = *lhs != *rhs;
        break;
      case arith::CmpIPredicate::slt:
        result = lhs->slt(*rhs);
        break;
      case arith::CmpIPredicate::sle:
        result = lhs->sle(*rhs);
        break;
      case arith::CmpIPredicate::sgt:
        result = lhs->sgt(*rhs);
        break;
      case arith::CmpIPredicate::sge:
        result = lhs->sge(*rhs);
        break;
      case arith::CmpIPredicate::ult:
        result = lhs->ult(*rhs);
        break;
      case arith::CmpIPredicate::ule:
        result = lhs->ule(*rhs);
        break;
      case arith::CmpIPredicate::ugt:
        result = lhs->ugt(*rhs);
        break;
      case arith::CmpIPredicate::uge:
        result = lhs->uge(*rhs);
        break;
      }
      bind(cmp.getResult(), InterpValue::makeBool(result), frame);
      return Flow::Advance;
    }

    if (auto select = dyn_cast<arith::SelectOp>(op)) {
      FailureOr<APInt> condition = getInt(select.getCondition(), frame);
      if (failed(condition))
        return failure();
      Value chosen = condition->isZero() ? select.getFalseValue()
                                         : select.getTrueValue();
      FailureOr<InterpValue> value = getAny(chosen, frame);
      if (failed(value))
        return failure();
      bind(select.getResult(), *value, frame);
      return Flow::Advance;
    }

    // Arith casts.
    if (isa<arith::ExtSIOp, arith::ExtUIOp, arith::TruncIOp,
            arith::IndexCastOp, arith::IndexCastUIOp>(op)) {
      FailureOr<APInt> source = getInt(op->getOperand(0), frame);
      unsigned resultWidth = intWidth(op->getResult(0).getType());
      if (failed(source) || resultWidth == 0)
        return failure();
      APInt result = *source;
      if (resultWidth < result.getBitWidth()) {
        result = result.trunc(resultWidth);
      } else if (resultWidth > result.getBitWidth()) {
        bool zeroExtend = isa<arith::ExtUIOp, arith::IndexCastUIOp>(op);
        result = zeroExtend ? result.zext(resultWidth)
                            : result.sext(resultWidth);
      }
      bind(op->getResult(0), InterpValue::makeInt(result), frame);
      return Flow::Advance;
    }

    // EmitRust arithmetic (signedness is in the type: ui<N>/index unsigned).
    if (isa<emitrust::AddOp, emitrust::SubOp, emitrust::MulOp, emitrust::DivOp,
            emitrust::RemOp, emitrust::AndOp, emitrust::OrOp, emitrust::XorOp,
            emitrust::ShlOp, emitrust::ShrOp>(op)) {
      Type type = op->getResult(0).getType();
      if (intWidth(type) == 0)
        return failure();
      bool isUnsigned = isUnsignedLike(type);
      BinKind kind = llvm::TypeSwitch<Operation *, BinKind>(op)
                         .Case<emitrust::AddOp>([](auto) { return BinKind::Add; })
                         .Case<emitrust::SubOp>([](auto) { return BinKind::Sub; })
                         .Case<emitrust::MulOp>([](auto) { return BinKind::Mul; })
                         .Case<emitrust::DivOp>([](auto) { return BinKind::Div; })
                         .Case<emitrust::RemOp>([](auto) { return BinKind::Rem; })
                         .Case<emitrust::AndOp>([](auto) { return BinKind::And; })
                         .Case<emitrust::OrOp>([](auto) { return BinKind::Or; })
                         .Case<emitrust::XorOp>([](auto) { return BinKind::Xor; })
                         .Case<emitrust::ShlOp>([](auto) { return BinKind::Shl; })
                         .Default([](auto) { return BinKind::Shr; });
      return execBinary(op, kind, isUnsigned, frame);
    }

    if (auto cmp = dyn_cast<emitrust::CmpOp>(op)) {
      Type type = cmp.getLhs().getType();
      if (intWidth(type) == 0)
        return failure();
      FailureOr<APInt> lhs = getInt(cmp.getLhs(), frame);
      FailureOr<APInt> rhs = getInt(cmp.getRhs(), frame);
      if (failed(lhs) || failed(rhs))
        return failure();
      bool result =
          applyCmp(cmp.getPredicate(), isUnsignedLike(type), *lhs, *rhs);
      bind(cmp.getResult(), InterpValue::makeBool(result), frame);
      return Flow::Advance;
    }

    if (auto cast = dyn_cast<emitrust::CastOp>(op)) {
      FailureOr<APInt> source = getInt(cast.getSource(), frame);
      if (failed(source))
        return failure();
      FailureOr<APInt> result = applyCast(cast.getSource().getType(),
                                          cast.getType(), *source);
      if (failed(result))
        return failure();
      bind(cast.getResult(), InterpValue::makeInt(*result), frame);
      return Flow::Advance;
    }

    if (auto select = dyn_cast<emitrust::SelectOp>(op)) {
      FailureOr<APInt> condition = getInt(select.getCondition(), frame);
      if (failed(condition))
        return failure();
      Value chosen = condition->isZero() ? select.getFalseValue()
                                         : select.getTrueValue();
      FailureOr<InterpValue> value = getAny(chosen, frame);
      if (failed(value))
        return failure();
      bind(select.getResult(), *value, frame);
      return Flow::Advance;
    }

    // Scalar places: variable declares a slot, assign writes it, load
    // reads it; let is the initialized mutable binding of the SCF
    // conversion. Non-scalar variables get no slot, so their uses defer
    // the skip to the first load/assign.
    if (auto variable = dyn_cast<emitrust::VariableOp>(op)) {
      auto lvalueType = cast<emitrust::LValueType>(variable.getType());
      unsigned width = intWidth(lvalueType.getValueType());
      if (width == 0)
        return Flow::Advance;
      APInt initial = APInt::getZero(width);
      if (Attribute init = variable.getInitAttr()) {
        auto attr = dyn_cast<IntegerAttr>(init);
        if (!attr)
          return Flow::Advance;
        initial = attr.getValue();
      }
      frame.slots[variable.getResult()] = InterpValue::makeInt(initial);
      return Flow::Advance;
    }
    if (auto let = dyn_cast<emitrust::LetOp>(op)) {
      FailureOr<InterpValue> init = getAny(let.getInit(), frame);
      if (failed(init))
        return failure();
      frame.slots[let.getResult()] = *init;
      return Flow::Advance;
    }
    if (auto assign = dyn_cast<emitrust::AssignOp>(op)) {
      auto slot = frame.slots.find(assign.getVar());
      if (slot == frame.slots.end())
        return failure();
      FailureOr<InterpValue> value = getAny(assign.getValue(), frame);
      if (failed(value))
        return failure();
      slot->second = *value;
      return Flow::Advance;
    }
    if (auto load = dyn_cast<emitrust::LoadOp>(op)) {
      auto slot = frame.slots.find(load.getOperand());
      if (slot == frame.slots.end())
        return failure();
      bind(load.getResult(), slot->second, frame);
      return Flow::Advance;
    }

    // Calls.
    if (auto callOpaque = dyn_cast<emitrust::CallOpaqueOp>(op))
      return execCallOpaque(callOpaque, frame);
    if (auto callOp = dyn_cast<func::CallOp>(op)) {
      FunctionOpInterface callee = resolveFunction(callOp.getCallee());
      if (!callee || callee.getNumArguments() != callOp->getNumOperands() ||
          callee.getResultTypes().size() != callOp->getNumResults())
        return failure();
      FailureOr<SmallVector<InterpValue>> args =
          getAll(callOp->getOperands(), frame);
      if (failed(args))
        return failure();
      FailureOr<SmallVector<InterpValue>> results = call(callee, *args);
      if (failed(results) || results->size() != callOp->getNumResults())
        return failure();
      for (auto [result, value] : llvm::zip(callOp->getResults(), *results))
        bind(result, value, frame);
      return Flow::Advance;
    }

    // Structured control flow: scf.
    if (auto ifOp = dyn_cast<scf::IfOp>(op))
      return execScfIf(ifOp, frame);
    if (auto whileOp = dyn_cast<scf::WhileOp>(op))
      return execScfWhile(whileOp, frame);
    if (auto forOp = dyn_cast<scf::ForOp>(op))
      return execScfFor(forOp, frame);
    if (auto switchOp = dyn_cast<scf::IndexSwitchOp>(op))
      return execScfIndexSwitch(switchOp, frame);

    // Structured control flow: emitrust.
    if (auto ifOp = dyn_cast<emitrust::IfOp>(op))
      return execEmitRustIf(ifOp, frame);
    if (auto loopOp = dyn_cast<emitrust::LoopOp>(op))
      return execEmitRustLoop(loopOp, frame);
    if (auto forOp = dyn_cast<emitrust::ForOp>(op))
      return execEmitRustFor(forOp, frame);
    if (auto switchOp = dyn_cast<emitrust::SwitchOp>(op))
      return execEmitRustSwitch(switchOp, frame);
    if (isa<emitrust::BreakOp>(op))
      return Flow::Break;
    if (isa<emitrust::ContinueOp>(op))
      return Flow::Continue;

    // Anything else — memory/place ops beyond scalar slots, cells,
    // globals, structs, floats, opaque literals, method calls — is
    // unsupported coverage.
    return failure();
  }

  /// Runs a structured single-block region body and reads its scf.yield
  /// operands into `results` (empty region: no results, Advance).
  FailureOr<Flow> execYieldRegion(Region &region, Frame &frame,
                                  SmallVectorImpl<InterpValue> &results) {
    if (region.empty())
      return Flow::Advance;
    if (!region.hasOneBlock())
      return failure();
    Block &block = region.front();
    FailureOr<Flow> flow = execStructuredBlock(block, frame);
    if (failed(flow))
      return failure();
    if (*flow != Flow::Advance)
      return *flow;
    auto yield = dyn_cast<scf::YieldOp>(block.getTerminator());
    if (!yield)
      return failure();
    FailureOr<SmallVector<InterpValue>> values =
        getAll(yield.getOperands(), frame);
    if (failed(values))
      return failure();
    results = std::move(*values);
    return Flow::Advance;
  }

  FailureOr<Flow> execScfIf(scf::IfOp op, Frame &frame) {
    FailureOr<APInt> condition = getInt(op.getCondition(), frame);
    if (failed(condition))
      return failure();
    Region &region =
        condition->isZero() ? op.getElseRegion() : op.getThenRegion();
    SmallVector<InterpValue> results;
    FailureOr<Flow> flow = execYieldRegion(region, frame, results);
    if (failed(flow))
      return failure();
    if (*flow != Flow::Advance)
      return *flow;
    if (results.size() != op->getNumResults())
      return failure();
    for (auto [result, value] : llvm::zip(op->getResults(), results))
      bind(result, value, frame);
    return Flow::Advance;
  }

  FailureOr<Flow> execScfWhile(scf::WhileOp op, Frame &frame) {
    FailureOr<SmallVector<InterpValue>> carried =
        getAll(op.getInits(), frame);
    if (failed(carried))
      return failure();
    if (!op.getBefore().hasOneBlock() || !op.getAfter().hasOneBlock())
      return failure();
    Block &before = op.getBefore().front();
    Block &after = op.getAfter().front();
    while (true) {
      if (failed(step()))
        return failure();
      if (before.getNumArguments() != carried->size())
        return failure();
      for (auto [arg, value] : llvm::zip(before.getArguments(), *carried))
        bind(arg, value, frame);
      FailureOr<Flow> flow = execStructuredBlock(before, frame);
      if (failed(flow) || *flow != Flow::Advance)
        return failure();
      auto condition = dyn_cast<scf::ConditionOp>(before.getTerminator());
      if (!condition)
        return failure();
      FailureOr<APInt> conditionValue =
          getInt(condition.getCondition(), frame);
      if (failed(conditionValue))
        return failure();
      FailureOr<SmallVector<InterpValue>> forwarded =
          getAll(condition.getArgs(), frame);
      if (failed(forwarded))
        return failure();
      if (conditionValue->isZero()) {
        if (forwarded->size() != op->getNumResults())
          return failure();
        for (auto [result, value] : llvm::zip(op->getResults(), *forwarded))
          bind(result, value, frame);
        return Flow::Advance;
      }
      if (after.getNumArguments() != forwarded->size())
        return failure();
      for (auto [arg, value] : llvm::zip(after.getArguments(), *forwarded))
        bind(arg, value, frame);
      flow = execStructuredBlock(after, frame);
      if (failed(flow) || *flow != Flow::Advance)
        return failure();
      auto yield = dyn_cast<scf::YieldOp>(after.getTerminator());
      if (!yield)
        return failure();
      carried = getAll(yield.getOperands(), frame);
      if (failed(carried))
        return failure();
    }
  }

  FailureOr<Flow> execScfFor(scf::ForOp op, Frame &frame) {
    FailureOr<APInt> lower = getInt(op.getLowerBound(), frame);
    FailureOr<APInt> upper = getInt(op.getUpperBound(), frame);
    FailureOr<APInt> stepValue = getInt(op.getStep(), frame);
    FailureOr<SmallVector<InterpValue>> iterValues =
        getAll(op.getInitArgs(), frame);
    if (failed(lower) || failed(upper) || failed(stepValue) ||
        failed(iterValues))
      return failure();
    if (!stepValue->isStrictlyPositive())
      return failure();
    Block &body = *op.getBody();
    if (body.getNumArguments() != iterValues->size() + 1)
      return failure();
    for (APInt iv = *lower; iv.slt(*upper);) {
      if (failed(step()))
        return failure();
      bind(body.getArgument(0), InterpValue::makeInt(iv), frame);
      for (auto [arg, value] :
           llvm::zip(body.getArguments().drop_front(), *iterValues))
        bind(arg, value, frame);
      FailureOr<Flow> flow = execStructuredBlock(body, frame);
      if (failed(flow) || *flow != Flow::Advance)
        return failure();
      auto yield = dyn_cast<scf::YieldOp>(body.getTerminator());
      if (!yield)
        return failure();
      iterValues = getAll(yield.getOperands(), frame);
      if (failed(iterValues))
        return failure();
      bool overflow = false;
      iv = iv.sadd_ov(*stepValue, overflow);
      if (overflow)
        break;
    }
    if (iterValues->size() != op->getNumResults())
      return failure();
    for (auto [result, value] : llvm::zip(op->getResults(), *iterValues))
      bind(result, value, frame);
    return Flow::Advance;
  }

  FailureOr<Flow> execScfIndexSwitch(scf::IndexSwitchOp op, Frame &frame) {
    FailureOr<APInt> discriminator = getInt(op.getArg(), frame);
    if (failed(discriminator))
      return failure();
    Region *chosen = &op.getDefaultRegion();
    for (auto [index, caseValue] : llvm::enumerate(op.getCases())) {
      if (*discriminator ==
          APInt(discriminator->getBitWidth(), caseValue, /*isSigned=*/true)) {
        chosen = &op.getCaseRegions()[index];
        break;
      }
    }
    SmallVector<InterpValue> results;
    FailureOr<Flow> flow = execYieldRegion(*chosen, frame, results);
    if (failed(flow))
      return failure();
    if (*flow != Flow::Advance)
      return *flow;
    if (results.size() != op->getNumResults())
      return failure();
    for (auto [result, value] : llvm::zip(op->getResults(), results))
      bind(result, value, frame);
    return Flow::Advance;
  }

  FailureOr<Flow> execEmitRustIf(emitrust::IfOp op, Frame &frame) {
    FailureOr<APInt> condition = getInt(op.getCondition(), frame);
    if (failed(condition))
      return failure();
    Region &region =
        condition->isZero() ? op.getElseRegion() : op.getThenRegion();
    if (region.empty())
      return Flow::Advance;
    if (!region.hasOneBlock())
      return failure();
    return execStructuredBlock(region.front(), frame);
  }

  FailureOr<Flow> execEmitRustLoop(emitrust::LoopOp op, Frame &frame) {
    if (!op.getRegion().hasOneBlock())
      return failure();
    Block &body = op.getRegion().front();
    while (true) {
      if (failed(step()))
        return failure();
      FailureOr<Flow> flow = execStructuredBlock(body, frame);
      if (failed(flow))
        return failure();
      if (*flow == Flow::Break)
        return Flow::Advance;
      // Advance and Continue both re-enter the loop body.
    }
  }

  FailureOr<Flow> execEmitRustFor(emitrust::ForOp op, Frame &frame) {
    FailureOr<APInt> lower = getInt(op.getLowerBound(), frame);
    FailureOr<APInt> upper = getInt(op.getUpperBound(), frame);
    FailureOr<APInt> stepValue = getInt(op.getStep(), frame);
    if (failed(lower) || failed(upper) || failed(stepValue))
      return failure();
    Type type = op.getLowerBound().getType();
    bool isUnsigned = isUnsignedLike(type);
    // Rust's (lb..ub).step_by(step) requires a positive step; step_by(0)
    // panics and a negative step is a shape the conversion never emits.
    if (isUnsigned ? stepValue->isZero() : !stepValue->isStrictlyPositive())
      return failure();
    Block &body = op.getRegion().front();
    for (APInt iv = *lower;
         isUnsigned ? iv.ult(*upper) : iv.slt(*upper);) {
      if (failed(step()))
        return failure();
      bind(op.getInductionVar(), InterpValue::makeInt(iv), frame);
      FailureOr<Flow> flow = execStructuredBlock(body, frame);
      if (failed(flow))
        return failure();
      if (*flow == Flow::Break)
        break;
      bool overflow = false;
      iv = isUnsigned ? iv.uadd_ov(*stepValue, overflow)
                      : iv.sadd_ov(*stepValue, overflow);
      if (overflow)
        break; // The Rust range iterator ends at the type's edge.
    }
    return Flow::Advance;
  }

  FailureOr<Flow> execEmitRustSwitch(emitrust::SwitchOp op, Frame &frame) {
    FailureOr<APInt> discriminator = getInt(op.getDiscriminator(), frame);
    if (failed(discriminator))
      return failure();
    unsigned width = discriminator->getBitWidth();
    Region *chosen = &op.getDefaultRegion();
    for (auto [index, caseValue] : llvm::enumerate(op.getCases())) {
      // The stored 64-bit case value compares as a bit pattern in the
      // discriminator's width (matching the emitted match arm literal).
      APInt pattern(64, static_cast<uint64_t>(caseValue));
      if (width < 64)
        pattern = pattern.trunc(width);
      else if (width > 64)
        pattern = pattern.sext(width);
      if (*discriminator == pattern) {
        chosen = &op.getCaseRegions()[index];
        break;
      }
    }
    if (!chosen->hasOneBlock())
      return failure();
    // Break/continue are transparent with respect to an enclosing loop,
    // so the flow propagates unchanged.
    return execStructuredBlock(chosen->front(), frame);
  }
};

/// The emitrust-value-identity-check pass. See the TableGen description in
/// Passes.td for the input modes, skip semantics, poison policy, and
/// diagnostic contract.
struct EmitRustValueIdentityCheck
    : public emitrust::impl::EmitRustValueIdentityCheckBase<
          EmitRustValueIdentityCheck> {
  /// Generous per-entry-point step budget: adversarial IR cannot hang the
  /// checker, while everyday staged loops (thousands of iterations) still
  /// verify.
  static constexpr uint64_t kStepBudget = 10'000'000;

  void runOnOperation() override {
    ModuleOp module = getOperation();

    ModuleOp preModule;
    ModuleOp postModule;
    OwningOpRef<ModuleOp> converted;
    if (std::optional<StagedModulePair> staged =
            detectStagedModulePair(module)) {
      preModule = staged->before;
      postModule = staged->after;
    } else {
      preModule = module;
      FailureOr<OwningOpRef<ModuleOp>> convertedOrFailure =
          cloneAndConvert(module);
      if (failed(convertedOrFailure)) {
        module.emitError("value identity check: internal "
                         "convert-to-emitrust pipeline failed");
        return signalPassFailure();
      }
      converted = std::move(*convertedOrFailure);
      postModule = *converted;
    }

    // Entry points: the parameterless functions of the pre-conversion
    // stage, each interpreted independently on both sides.
    SmallVector<FunctionOpInterface> entryPoints;
    for (Operation &op : preModule.getBody()->getOperations()) {
      auto fn = dyn_cast<FunctionOpInterface>(&op);
      if (fn && !fn.isExternal() && fn.getNumArguments() == 0)
        entryPoints.push_back(fn);
    }

    for (FunctionOpInterface preFn : entryPoints) {
      StringRef symbol = SymbolTable::getSymbolName(preFn).getValue();
      auto postFn = dyn_cast_if_present<FunctionOpInterface>(
          SymbolTable::lookupSymbolIn(
              postModule, StringAttr::get(&getContext(), symbol)));
      if (!postFn || postFn.isExternal() || postFn.getNumArguments() != 0) {
        ++numSkippedEntryPoints;
        continue;
      }
      Trace preTrace;
      Trace postTrace;
      Interpreter preInterpreter(preModule, kStepBudget);
      Interpreter postInterpreter(postModule, kStepBudget);
      if (preInterpreter.run(preFn, preTrace) != Interpreter::Status::Ok ||
          postInterpreter.run(postFn, postTrace) != Interpreter::Status::Ok) {
        ++numSkippedEntryPoints;
        continue;
      }
      compare(preFn, symbol, preTrace, postTrace);
    }
    markAllAnalysesPreserved();
  }

  /// Compares the two observation traces of one entry point in execution
  /// order and reports the first divergence with the pinned diagnostic.
  void compare(FunctionOpInterface entry, StringRef symbol,
               const Trace &preTrace, const Trace &postTrace) {
    unsigned printOccurrence = 0;
    unsigned returnOccurrence = 0;
    size_t common = std::min(preTrace.size(), postTrace.size());
    for (size_t i = 0; i < common; ++i) {
      const TraceEvent &pre = preTrace[i];
      const TraceEvent &post = postTrace[i];
      if (pre.isReturn != post.isReturn ||
          pre.operands.size() != post.operands.size()) {
        emitSequenceDivergence(entry, symbol, preTrace, postTrace, i);
        return;
      }
      unsigned occurrence =
          pre.isReturn ? returnOccurrence++ : printOccurrence++;
      for (auto [k, values] :
           llvm::enumerate(llvm::zip(pre.operands, post.operands))) {
        auto [preValue, postValue] = values;
        if (preValue.getBitWidth() == postValue.getBitWidth() &&
            preValue == postValue)
          continue;
        pre.op->emitError()
            << "value identity violation: '" << pre.function << "' "
            << (pre.isReturn ? "return value " : "print operand ") << k
            << " at occurrence " << occurrence << " observed "
            << formatSigned(preValue) << " pre-conversion but "
            << formatSigned(postValue) << " post-conversion";
        signalPassFailure();
        return;
      }
    }
    if (preTrace.size() != postTrace.size())
      emitSequenceDivergence(entry, symbol, preTrace, postTrace, common);
  }

  /// Structural divergence (different observation sequences): also a
  /// value-identity violation, reported without the operand-value form
  /// because there is no matched pair to print.
  void emitSequenceDivergence(FunctionOpInterface entry, StringRef symbol,
                              const Trace &preTrace, const Trace &postTrace,
                              size_t index) {
    entry->emitError() << "value identity violation: '" << symbol
                       << "' observation sequence diverged at event " << index
                       << " (" << preTrace.size() << " events pre-conversion, "
                       << postTrace.size() << " post-conversion)";
    signalPassFailure();
  }
};

} // namespace
