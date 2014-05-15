// Copyright (c) 2014, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_ARM64.
#if defined(TARGET_ARCH_ARM64)

#include "vm/intermediate_language.h"

#include "vm/dart_entry.h"
#include "vm/flow_graph_compiler.h"
#include "vm/locations.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/simulator.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

#define __ compiler->assembler()->

namespace dart {

DECLARE_FLAG(int, optimization_counter_threshold);
DECLARE_FLAG(bool, use_osr);

// Generic summary for call instructions that have all arguments pushed
// on the stack and return the result in a fixed register R0.
LocationSummary* Instruction::MakeCallSummary() {
  LocationSummary* result = new LocationSummary(0, 0, LocationSummary::kCall);
  result->set_out(0, Location::RegisterLocation(R0));
  return result;
}


LocationSummary* PushArgumentInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps= 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::AnyOrConstant(value()));
  return locs;
}


void PushArgumentInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // In SSA mode, we need an explicit push. Nothing to do in non-SSA mode
  // where PushArgument is handled by BindInstr::EmitNativeCode.
  if (compiler->is_optimizing()) {
    Location value = locs()->in(0);
    if (value.IsRegister()) {
      __ Push(value.reg());
    } else if (value.IsConstant()) {
      __ PushObject(value.constant(), PP);
    } else {
      ASSERT(value.IsStackSlot());
      const intptr_t value_offset = value.ToStackSlotOffset();
      __ LoadFromOffset(TMP, FP, value_offset, PP);
      __ Push(TMP);
    }
  }
}


LocationSummary* ReturnInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RegisterLocation(R0));
  return locs;
}


// Attempt optimized compilation at return instruction instead of at the entry.
// The entry needs to be patchable, no inlined objects are allowed in the area
// that will be overwritten by the patch instructions: a branch macro sequence.
void ReturnInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->in(0).reg();
  ASSERT(result == R0);
#if defined(DEBUG)
  Label stack_ok;
  __ Comment("Stack Check");
  const intptr_t fp_sp_dist =
      (kFirstLocalSlotFromFp + 1 - compiler->StackSize()) * kWordSize;
  ASSERT(fp_sp_dist <= 0);
  // UXTX 0 on a 64-bit register (FP) is a nop, but forces R31 to be
  // interpreted as SP.
  __ sub(R2, SP, Operand(FP, UXTX, 0));
  __ CompareImmediate(R2, fp_sp_dist, PP);
  __ b(&stack_ok, EQ);
  __ hlt(0);
  __ Bind(&stack_ok);
#endif
  __ LeaveDartFrame();
  __ ret();
}


static Condition NegateCondition(Condition condition) {
  switch (condition) {
    case EQ: return NE;
    case NE: return EQ;
    case LT: return GE;
    case LE: return GT;
    case GT: return LE;
    case GE: return LT;
    case CC: return CS;
    case LS: return HI;
    case HI: return LS;
    case CS: return CC;
    default:
      UNREACHABLE();
      return EQ;
  }
}


// Detect pattern when one value is zero and another is a power of 2.
static bool IsPowerOfTwoKind(intptr_t v1, intptr_t v2) {
  return (Utils::IsPowerOfTwo(v1) && (v2 == 0)) ||
         (Utils::IsPowerOfTwo(v2) && (v1 == 0));
}


LocationSummary* IfThenElseInstr::MakeLocationSummary(bool opt) const {
  comparison()->InitializeLocationSummary(opt);
  return comparison()->locs();
}


void IfThenElseInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Register result = locs()->out(0).reg();

  Location left = locs()->in(0);
  Location right = locs()->in(1);
  ASSERT(!left.IsConstant() || !right.IsConstant());

  // Emit comparison code. This must not overwrite the result register.
  BranchLabels labels = { NULL, NULL, NULL };
  Condition true_condition = comparison()->EmitComparisonCode(compiler, labels);

  const bool is_power_of_two_kind = IsPowerOfTwoKind(if_true_, if_false_);

  intptr_t true_value = if_true_;
  intptr_t false_value = if_false_;

  if (is_power_of_two_kind) {
    if (true_value == 0) {
      // We need to have zero in result on true_condition.
      true_condition = NegateCondition(true_condition);
    }
  } else {
    if (true_value == 0) {
      // Swap values so that false_value is zero.
      intptr_t temp = true_value;
      true_value = false_value;
      false_value = temp;
    } else {
      true_condition = NegateCondition(true_condition);
    }
  }

  __ cset(result, true_condition);

  if (is_power_of_two_kind) {
    const intptr_t shift =
        Utils::ShiftForPowerOfTwo(Utils::Maximum(true_value, false_value));
    __ Lsl(result, result, shift + kSmiTagSize);
  } else {
    __ sub(result, result, Operand(1));
    const int64_t val =
        Smi::RawValue(true_value) - Smi::RawValue(false_value);
    __ AndImmediate(result, result, val, PP);
    if (false_value != 0) {
      __ AddImmediate(result, result, Smi::RawValue(false_value), PP);
    }
  }
}


LocationSummary* ClosureCallInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(R0));  // Function.
  summary->set_out(0, Location::RegisterLocation(R0));
  return summary;
}


void ClosureCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Load arguments descriptor in R4.
  int argument_count = ArgumentCount();
  const Array& arguments_descriptor =
      Array::ZoneHandle(ArgumentsDescriptor::New(argument_count,
                                                 argument_names()));
  __ LoadObject(R4, arguments_descriptor, PP);

  // R4: Arguments descriptor.
  // R0: Function.
  ASSERT(locs()->in(0).reg() == R0);
  __ LoadFieldFromOffset(R2, R0, Function::code_offset(), PP);

  // R2: code.
  // R5: Smi 0 (no IC data; the lazy-compile stub expects a GC-safe value).
  __ LoadImmediate(R5, 0, PP);
  __ LoadFieldFromOffset(R2, R2, Code::instructions_offset(), PP);
  __ AddImmediate(R2, R2, Instructions::HeaderSize() - kHeapObjectTag, PP);
  __ blr(R2);
  compiler->AddCurrentDescriptor(PcDescriptors::kClosureCall,
                                 deopt_id(),
                                 token_pos());
  compiler->RecordSafepoint(locs());
  // Marks either the continuation point in unoptimized code or the
  // deoptimization point in optimized code, after call.
  const intptr_t deopt_id_after = Isolate::ToDeoptAfter(deopt_id());
  if (compiler->is_optimizing()) {
    compiler->AddDeoptIndexAtCall(deopt_id_after, token_pos());
  } else {
    // Add deoptimization continuation point after the call and before the
    // arguments are removed.
    compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                   deopt_id_after,
                                   token_pos());
  }
  __ Drop(argument_count);
}


LocationSummary* LoadLocalInstr::MakeLocationSummary(bool opt) const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadLocalInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out(0).reg();
  __ LoadFromOffset(result, FP, local().index() * kWordSize, PP);
}


LocationSummary* StoreLocalInstr::MakeLocationSummary(bool opt) const {
  return LocationSummary::Make(1,
                               Location::SameAsFirstInput(),
                               LocationSummary::kNoCall);
}


void StoreLocalInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out(0).reg();
  ASSERT(result == value);  // Assert that register assignment is correct.
  __ StoreToOffset(value, FP, local().index() * kWordSize, PP);
}


LocationSummary* ConstantInstr::MakeLocationSummary(bool opt) const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void ConstantInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // The register allocator drops constant definitions that have no uses.
  if (!locs()->out(0).IsInvalid()) {
    Register result = locs()->out(0).reg();
    __ LoadObject(result, value(), PP);
  }
}


LocationSummary* UnboxedConstantInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 0;
  return LocationSummary::Make(kNumInputs,
                               Location::RequiresFpuRegister(),
                               LocationSummary::kNoCall);
}


void UnboxedConstantInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (!locs()->out(0).IsInvalid()) {
    const VRegister dst = locs()->out(0).fpu_reg();
    __ LoadDImmediate(dst, Double::Cast(value()).value(), PP);
  }
}


LocationSummary* AssertAssignableInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(R0));  // Value.
  summary->set_in(1, Location::RegisterLocation(R2));  // Instantiator.
  summary->set_in(2, Location::RegisterLocation(R1));  // Type arguments.
  summary->set_out(0, Location::RegisterLocation(R0));
  return summary;
}


LocationSummary* AssertBooleanInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(R0));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


static void EmitAssertBoolean(Register reg,
                              intptr_t token_pos,
                              intptr_t deopt_id,
                              LocationSummary* locs,
                              FlowGraphCompiler* compiler) {
  // Check that the type of the value is allowed in conditional context.
  // Call the runtime if the object is not bool::true or bool::false.
  ASSERT(locs->always_calls());
  Label done;
  __ CompareObject(reg, Bool::True(), PP);
  __ b(&done, EQ);
  __ CompareObject(reg, Bool::False(), PP);
  __ b(&done, EQ);

  __ Push(reg);  // Push the source object.
  compiler->GenerateRuntimeCall(token_pos,
                                deopt_id,
                                kNonBoolTypeErrorRuntimeEntry,
                                1,
                                locs);
  // We should never return here.
  __ hlt(0);
  __ Bind(&done);
}


void AssertBooleanInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register obj = locs()->in(0).reg();
  Register result = locs()->out(0).reg();

  EmitAssertBoolean(obj, token_pos(), deopt_id(), locs(), compiler);
  ASSERT(obj == result);
}


static Condition TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQ;
    case Token::kNE: return NE;
    case Token::kLT: return LT;
    case Token::kGT: return GT;
    case Token::kLTE: return LE;
    case Token::kGTE: return GE;
    default:
      UNREACHABLE();
      return VS;
  }
}


static Condition FlipCondition(Condition condition) {
  switch (condition) {
    case EQ: return EQ;
    case NE: return NE;
    case LT: return GT;
    case LE: return GE;
    case GT: return LT;
    case GE: return LE;
    case CC: return HI;
    case LS: return CS;
    case HI: return CC;
    case CS: return LS;
    default:
      UNREACHABLE();
      return EQ;
  }
}


static void EmitBranchOnCondition(FlowGraphCompiler* compiler,
                                  Condition true_condition,
                                  BranchLabels labels) {
  if (labels.fall_through == labels.false_label) {
    // If the next block is the false successor we will fall through to it.
    __ b(labels.true_label, true_condition);
  } else {
    // If the next block is not the false successor we will branch to it.
    Condition false_condition = NegateCondition(true_condition);
    __ b(labels.false_label, false_condition);

    // Fall through or jump to the true successor.
    if (labels.fall_through != labels.true_label) {
      __ b(labels.true_label);
    }
  }
}


static Condition EmitSmiComparisonOp(FlowGraphCompiler* compiler,
                                     LocationSummary* locs,
                                     Token::Kind kind) {
  Location left = locs->in(0);
  Location right = locs->in(1);
  ASSERT(!left.IsConstant() || !right.IsConstant());

  Condition true_condition = TokenKindToSmiCondition(kind);

  if (left.IsConstant()) {
    __ CompareObject(right.reg(), left.constant(), PP);
    true_condition = FlipCondition(true_condition);
  } else if (right.IsConstant()) {
    __ CompareObject(left.reg(), right.constant(), PP);
  } else {
    __ CompareRegisters(left.reg(), right.reg());
  }
  return true_condition;
}


LocationSummary* EqualityCompareInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  if (operation_cid() == kDoubleCid) {
    const intptr_t kNumTemps =  0;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RequiresFpuRegister());
    locs->set_in(1, Location::RequiresFpuRegister());
    locs->set_out(0, Location::RequiresRegister());
    return locs;
  }
  if (operation_cid() == kSmiCid) {
    const intptr_t kNumTemps = 0;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RegisterOrConstant(left()));
    // Only one input can be a constant operand. The case of two constant
    // operands should be handled by constant propagation.
    // Only right can be a stack slot.
    locs->set_in(1, locs->in(0).IsConstant()
                        ? Location::RequiresRegister()
                        : Location::RegisterOrConstant(right()));
    locs->set_out(0, Location::RequiresRegister());
    return locs;
  }
  UNREACHABLE();
  return NULL;
}


static Condition TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQ;
    case Token::kNE: return NE;
    case Token::kLT: return LT;
    case Token::kGT: return GT;
    case Token::kLTE: return LE;
    case Token::kGTE: return GE;
    default:
      UNREACHABLE();
      return VS;
  }
}


static Condition EmitDoubleComparisonOp(FlowGraphCompiler* compiler,
                                        LocationSummary* locs,
                                        Token::Kind kind) {
  const VRegister left = locs->in(0).fpu_reg();
  const VRegister right = locs->in(1).fpu_reg();
  __ fcmpd(left, right);
  Condition true_condition = TokenKindToDoubleCondition(kind);
  return true_condition;
}


Condition EqualityCompareInstr::EmitComparisonCode(FlowGraphCompiler* compiler,
                                                   BranchLabels labels) {
  if (operation_cid() == kSmiCid) {
    return EmitSmiComparisonOp(compiler, locs(), kind());
  } else {
    ASSERT(operation_cid() == kDoubleCid);
    return EmitDoubleComparisonOp(compiler, locs(), kind());
  }
}


void EqualityCompareInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT((kind() == Token::kEQ) || (kind() == Token::kNE));
  Label is_true, is_false;
  BranchLabels labels = { &is_true, &is_false, &is_false };
  Condition true_condition = EmitComparisonCode(compiler, labels);
  if ((operation_cid() == kDoubleCid) && (true_condition != NE)) {
    // Special case for NaN comparison. Result is always false unless
    // relational operator is !=.
    __ b(&is_false, VS);
  }
  EmitBranchOnCondition(compiler, true_condition, labels);
  // TODO(zra): instead of branching, use the csel instruction to get
  // True or False into result.
  Register result = locs()->out(0).reg();
  Label done;
  __ Bind(&is_false);
  __ LoadObject(result, Bool::False(), PP);
  __ b(&done);
  __ Bind(&is_true);
  __ LoadObject(result, Bool::True(), PP);
  __ Bind(&done);
}


void EqualityCompareInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                          BranchInstr* branch) {
  ASSERT((kind() == Token::kNE) || (kind() == Token::kEQ));

  BranchLabels labels = compiler->CreateBranchLabels(branch);
  Condition true_condition = EmitComparisonCode(compiler, labels);
  if ((operation_cid() == kDoubleCid) && (true_condition != NE)) {
    // Special case for NaN comparison. Result is always false unless
    // relational operator is !=.
    __ b(labels.false_label, VS);
  }
  EmitBranchOnCondition(compiler, true_condition, labels);
}


LocationSummary* TestSmiInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  // Only one input can be a constant operand. The case of two constant
  // operands should be handled by constant propagation.
  locs->set_in(1, Location::RegisterOrConstant(right()));
  return locs;
}


Condition TestSmiInstr::EmitComparisonCode(FlowGraphCompiler* compiler,
                                           BranchLabels labels) {
  Register left = locs()->in(0).reg();
  Location right = locs()->in(1);
  if (right.IsConstant()) {
    ASSERT(right.constant().IsSmi());
    const int64_t imm =
        reinterpret_cast<int64_t>(right.constant().raw());
    __ TestImmediate(left, imm, PP);
  } else {
    __ tst(left, Operand(right.reg()));
  }
  Condition true_condition = (kind() == Token::kNE) ? NE : EQ;
  return true_condition;
}


void TestSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Never emitted outside of the BranchInstr.
  UNREACHABLE();
}


void TestSmiInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                  BranchInstr* branch) {
  BranchLabels labels = compiler->CreateBranchLabels(branch);
  Condition true_condition = EmitComparisonCode(compiler, labels);
  EmitBranchOnCondition(compiler, true_condition, labels);
}


LocationSummary* TestCidsInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_temp(0, Location::RequiresRegister());
  locs->set_out(0, Location::RequiresRegister());
  return locs;
}


Condition TestCidsInstr::EmitComparisonCode(FlowGraphCompiler* compiler,
                                            BranchLabels labels) {
  ASSERT((kind() == Token::kIS) || (kind() == Token::kISNOT));
  Register val_reg = locs()->in(0).reg();
  Register cid_reg = locs()->temp(0).reg();

  Label* deopt = CanDeoptimize() ?
      compiler->AddDeoptStub(deopt_id(), ICData::kDeoptTestCids) : NULL;

  const intptr_t true_result = (kind() == Token::kIS) ? 1 : 0;
  const ZoneGrowableArray<intptr_t>& data = cid_results();
  ASSERT(data[0] == kSmiCid);
  bool result = data[1] == true_result;
  __ tsti(val_reg, kSmiTagMask);
  __ b(result ? labels.true_label : labels.false_label, EQ);
  __ LoadClassId(cid_reg, val_reg, PP);

  for (intptr_t i = 2; i < data.length(); i += 2) {
    const intptr_t test_cid = data[i];
    ASSERT(test_cid != kSmiCid);
    result = data[i + 1] == true_result;
    __ CompareImmediate(cid_reg, test_cid, PP);
    __ b(result ? labels.true_label : labels.false_label, EQ);
  }
  // No match found, deoptimize or false.
  if (deopt == NULL) {
    Label* target = result ? labels.false_label : labels.true_label;
    if (target != labels.fall_through) {
      __ b(target);
    }
  } else {
    __ b(deopt);
  }
  // Dummy result as the last instruction is a jump, any conditional
  // branch using the result will therefore be skipped.
  return EQ;
}


void TestCidsInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                   BranchInstr* branch) {
  BranchLabels labels = compiler->CreateBranchLabels(branch);
  EmitComparisonCode(compiler, labels);
}


void TestCidsInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result_reg = locs()->out(0).reg();
  Label is_true, is_false, done;
  BranchLabels labels = { &is_true, &is_false, &is_false };
  EmitComparisonCode(compiler, labels);
  // TODO(zra): instead of branching, use the csel instruction to get
  // True or False into result.
  __ Bind(&is_false);
  __ LoadObject(result_reg, Bool::False(), PP);
  __ b(&done);
  __ Bind(&is_true);
  __ LoadObject(result_reg, Bool::True(), PP);
  __ Bind(&done);
}


LocationSummary* RelationalOpInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  if (operation_cid() == kDoubleCid) {
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresFpuRegister());
    summary->set_in(1, Location::RequiresFpuRegister());
    summary->set_out(0, Location::RequiresRegister());
    return summary;
  }
  ASSERT(operation_cid() == kSmiCid);
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RegisterOrConstant(left()));
  // Only one input can be a constant operand. The case of two constant
  // operands should be handled by constant propagation.
  summary->set_in(1, summary->in(0).IsConstant()
                         ? Location::RequiresRegister()
                         : Location::RegisterOrConstant(right()));
  summary->set_out(0, Location::RequiresRegister());
  return summary;
}


Condition RelationalOpInstr::EmitComparisonCode(FlowGraphCompiler* compiler,
                                                BranchLabels labels) {
  if (operation_cid() == kSmiCid) {
    return EmitSmiComparisonOp(compiler, locs(), kind());
  } else {
    ASSERT(operation_cid() == kDoubleCid);
    return EmitDoubleComparisonOp(compiler, locs(), kind());
  }
}


void RelationalOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label is_true, is_false;
  BranchLabels labels = { &is_true, &is_false, &is_false };
  Condition true_condition = EmitComparisonCode(compiler, labels);
  if ((operation_cid() == kDoubleCid) && (true_condition != NE)) {
    // Special case for NaN comparison. Result is always false unless
    // relational operator is !=.
    __ b(&is_false, VS);
  }
  EmitBranchOnCondition(compiler, true_condition, labels);
  // TODO(zra): instead of branching, use the csel instruction to get
  // True or False into result.
  Register result = locs()->out(0).reg();
  Label done;
  __ Bind(&is_false);
  __ LoadObject(result, Bool::False(), PP);
  __ b(&done);
  __ Bind(&is_true);
  __ LoadObject(result, Bool::True(), PP);
  __ Bind(&done);
}


void RelationalOpInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                       BranchInstr* branch) {
  BranchLabels labels = compiler->CreateBranchLabels(branch);
  Condition true_condition = EmitComparisonCode(compiler, labels);
  if ((operation_cid() == kDoubleCid) && (true_condition != NE)) {
    // Special case for NaN comparison. Result is always false unless
    // relational operator is !=.
    __ b(labels.false_label, VS);
  }
  EmitBranchOnCondition(compiler, true_condition, labels);
}


LocationSummary* NativeCallInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 3;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(R1));
  locs->set_temp(1, Location::RegisterLocation(R2));
  locs->set_temp(2, Location::RegisterLocation(R5));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void NativeCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == R1);
  ASSERT(locs()->temp(1).reg() == R2);
  ASSERT(locs()->temp(2).reg() == R5);
  Register result = locs()->out(0).reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle(), PP);
  // Pass a pointer to the first argument in R2.
  if (!function().HasOptionalParameters()) {
    __ AddImmediate(R2, FP, (kParamEndSlotFromFp +
                             function().NumParameters()) * kWordSize, PP);
  } else {
    __ AddImmediate(R2, FP, kFirstLocalSlotFromFp * kWordSize, PP);
  }
  // Compute the effective address. When running under the simulator,
  // this is a redirection address that forces the simulator to call
  // into the runtime system.
  uword entry = reinterpret_cast<uword>(native_c_function());
  const ExternalLabel* stub_entry;
  if (is_bootstrap_native()) {
    stub_entry = &StubCode::CallBootstrapCFunctionLabel();
#if defined(USING_SIMULATOR)
    entry = Simulator::RedirectExternalReference(
        entry, Simulator::kBootstrapNativeCall, function().NumParameters());
#endif
  } else {
    // In the case of non bootstrap native methods the CallNativeCFunction
    // stub generates the redirection address when running under the simulator
    // and hence we do not change 'entry' here.
    stub_entry = &StubCode::CallNativeCFunctionLabel();
#if defined(USING_SIMULATOR)
    if (!function().IsNativeAutoSetupScope()) {
      entry = Simulator::RedirectExternalReference(
          entry, Simulator::kBootstrapNativeCall, function().NumParameters());
    }
#endif
  }
  __ LoadImmediate(R5, entry, PP);
  __ LoadImmediate(R1, NativeArguments::ComputeArgcTag(function()), PP);
  compiler->GenerateCall(token_pos(),
                         stub_entry,
                         PcDescriptors::kOther,
                         locs());
  __ Pop(result);
}


LocationSummary* StringFromCharCodeInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  // TODO(fschneider): Allow immediate operands for the char code.
  return LocationSummary::Make(kNumInputs,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void StringFromCharCodeInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Register char_code = locs()->in(0).reg();
  const Register result = locs()->out(0).reg();
  __ LoadImmediate(
      result, reinterpret_cast<uword>(Symbols::PredefinedAddress()), PP);
  __ AddImmediate(
      result, result, Symbols::kNullCharCodeSymbolOffset * kWordSize, PP);
  __ Asr(TMP, char_code, kSmiTagShift);  // Untag to use scaled adress mode.
  __ ldr(result, Address(result, TMP, UXTX, Address::Scaled));
}


LocationSummary* StringToCharCodeInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  return LocationSummary::Make(kNumInputs,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void StringToCharCodeInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(cid_ == kOneByteStringCid);
  const Register str = locs()->in(0).reg();
  const Register result = locs()->out(0).reg();
  __ LoadFieldFromOffset(result, str, String::length_offset(), PP);
  __ ldr(TMP, FieldAddress(str, OneByteString::data_offset()), kUnsignedByte);
  __ CompareImmediate(result, Smi::RawValue(1), PP);
  __ LoadImmediate(result, -1, PP);
  __ csel(result, TMP, result, EQ);
  __ SmiTag(result);
}


LocationSummary* StringInterpolateInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(R0));
  summary->set_out(0, Location::RegisterLocation(R0));
  return summary;
}


void StringInterpolateInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register array = locs()->in(0).reg();
  __ Push(array);
  const int kNumberOfArguments = 1;
  const Array& kNoArgumentNames = Object::null_array();
  compiler->GenerateStaticCall(deopt_id(),
                               token_pos(),
                               CallFunction(),
                               kNumberOfArguments,
                               kNoArgumentNames,
                               locs());
  ASSERT(locs()->out(0).reg() == R0);
}


LocationSummary* LoadUntaggedInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  return LocationSummary::Make(kNumInputs,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadUntaggedInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register object = locs()->in(0).reg();
  Register result = locs()->out(0).reg();
  __ LoadFieldFromOffset(result, object, offset(), PP);
}


LocationSummary* LoadClassIdInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  return LocationSummary::Make(kNumInputs,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadClassIdInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register object = locs()->in(0).reg();
  Register result = locs()->out(0).reg();
  Label load, done;
  __ tsti(object, kSmiTagMask);
  __ b(&load, NE);
  __ LoadImmediate(result, Smi::RawValue(kSmiCid), PP);
  __ b(&done);
  __ Bind(&load);
  __ LoadClassId(result, object, PP);
  __ SmiTag(result);
  __ Bind(&done);
}


CompileType LoadIndexedInstr::ComputeType() const {
  switch (class_id_) {
    case kArrayCid:
    case kImmutableArrayCid:
      return CompileType::Dynamic();

    case kTypedDataFloat32ArrayCid:
    case kTypedDataFloat64ArrayCid:
      return CompileType::FromCid(kDoubleCid);
    case kTypedDataFloat32x4ArrayCid:
      return CompileType::FromCid(kFloat32x4Cid);
    case kTypedDataInt32x4ArrayCid:
      return CompileType::FromCid(kInt32x4Cid);
    case kTypedDataFloat64x2ArrayCid:
      return CompileType::FromCid(kFloat64x2Cid);

    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid:
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid:
    case kOneByteStringCid:
    case kTwoByteStringCid:
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid:
      return CompileType::FromCid(kSmiCid);

    default:
      UNIMPLEMENTED();
      return CompileType::Dynamic();
  }
}


Representation LoadIndexedInstr::representation() const {
  switch (class_id_) {
    case kArrayCid:
    case kImmutableArrayCid:
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid:
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid:
    case kOneByteStringCid:
    case kTwoByteStringCid:
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid:
      return kTagged;
    case kTypedDataFloat32ArrayCid:
    case kTypedDataFloat64ArrayCid:
      return kUnboxedDouble;
    case kTypedDataInt32x4ArrayCid:
      return kUnboxedInt32x4;
    case kTypedDataFloat32x4ArrayCid:
      return kUnboxedFloat32x4;
    case kTypedDataFloat64x2ArrayCid:
      return kUnboxedFloat64x2;
    default:
      UNIMPLEMENTED();
      return kTagged;
  }
}


LocationSummary* LoadIndexedInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  // The smi index is either untagged (element size == 1), or it is left smi
  // tagged (for all element sizes > 1).
  // TODO(regis): Revisit and see if the index can be immediate.
  locs->set_in(1, Location::WritableRegister());
  if ((representation() == kUnboxedDouble)    ||
      (representation() == kUnboxedFloat32x4) ||
      (representation() == kUnboxedInt32x4)   ||
      (representation() == kUnboxedFloat64x2)) {
    locs->set_out(0, Location::RequiresFpuRegister());
  } else {
    locs->set_out(0, Location::RequiresRegister());
  }
  return locs;
}


void LoadIndexedInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register array = locs()->in(0).reg();
  ASSERT(locs()->in(1).IsRegister());  // TODO(regis): Revisit.
  Register index = locs()->in(1).reg();

  Address element_address(kNoRegister, 0);

  // The array register points to the backing store for external arrays.
  intptr_t offset = 0;
  if (!IsExternal()) {
    ASSERT(this->array()->definition()->representation() == kTagged);
    offset = FlowGraphCompiler::DataOffsetFor(class_id()) - kHeapObjectTag;
  }

  // Note that index is expected smi-tagged, (i.e, times 2) for all arrays
  // with index scale factor > 1. E.g., for Uint8Array and OneByteString the
  // index is expected to be untagged before accessing.
  ASSERT(kSmiTagShift == 1);
  switch (index_scale()) {
    case 1:
      __ add(index, array, Operand(index, ASR, kSmiTagSize));
      element_address = Address(index, offset);
      break;
    case 2:
      if (offset != 0) {
        __ add(index, array, Operand(index));
        element_address = Address(index, offset);
      } else {
        element_address = Address(array, index, UXTX, Address::Unscaled);
      }
      break;
    case 4:
      __ add(index, array, Operand(index, LSL, 1));
      element_address = Address(index, offset);
      break;
    case 8:
      __ add(index, array, Operand(index, LSL, 2));
      element_address = Address(index, offset);
      break;
    case 16:
      __ add(index, array, Operand(index, LSL, 3));
      element_address = Address(index, offset);
      break;
    default:
      UNREACHABLE();
  }

  if ((representation() == kUnboxedDouble)    ||
      (representation() == kUnboxedFloat32x4) ||
      (representation() == kUnboxedInt32x4)   ||
      (representation() == kUnboxedFloat64x2)) {
    const VRegister result = locs()->out(0).fpu_reg();
    switch (class_id()) {
      case kTypedDataFloat32ArrayCid:
        // Load single precision float.
        __ fldrs(result, element_address);
        break;
      case kTypedDataFloat64ArrayCid:
        // Load double precision float.
        __ fldrd(result, element_address);
        break;
      case kTypedDataFloat64x2ArrayCid:
      case kTypedDataInt32x4ArrayCid:
      case kTypedDataFloat32x4ArrayCid:
        __ fldrq(result, element_address);
        break;
    }
    return;
  }

  Register result = locs()->out(0).reg();
  switch (class_id()) {
    case kTypedDataInt8ArrayCid:
      ASSERT(index_scale() == 1);
      __ ldr(result, element_address, kByte);
      __ SmiTag(result);
      break;
    case kTypedDataUint8ArrayCid:
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid:
    case kOneByteStringCid:
      ASSERT(index_scale() == 1);
      __ ldr(result, element_address, kUnsignedByte);
      __ SmiTag(result);
      break;
    case kTypedDataInt16ArrayCid:
      __ ldr(result, element_address, kHalfword);
      __ SmiTag(result);
      break;
    case kTypedDataUint16ArrayCid:
    case kTwoByteStringCid:
      __ ldr(result, element_address, kUnsignedHalfword);
      __ SmiTag(result);
      break;
    case kTypedDataInt32ArrayCid:
      __ ldr(result, element_address, kWord);
      __ SmiTag(result);
      break;
    case kTypedDataUint32ArrayCid:
      __ ldr(result, element_address, kUnsignedWord);
      __ SmiTag(result);
      break;
    default:
      ASSERT((class_id() == kArrayCid) || (class_id() == kImmutableArrayCid));
      __ ldr(result, element_address);
      break;
  }
}


Representation StoreIndexedInstr::RequiredInputRepresentation(
    intptr_t idx) const {
  // Array can be a Dart object or a pointer to external data.
  if (idx == 0)  return kNoRepresentation;  // Flexible input representation.
  if (idx == 1) return kTagged;  // Index is a smi.
  ASSERT(idx == 2);
  switch (class_id_) {
    case kArrayCid:
    case kOneByteStringCid:
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid:
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid:
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid:
      return kTagged;
    case kTypedDataFloat32ArrayCid:
    case kTypedDataFloat64ArrayCid:
      return kUnboxedDouble;
    case kTypedDataFloat32x4ArrayCid:
      return kUnboxedFloat32x4;
    case kTypedDataInt32x4ArrayCid:
      return kUnboxedInt32x4;
    case kTypedDataFloat64x2ArrayCid:
      return kUnboxedFloat64x2;
    default:
      UNREACHABLE();
      return kTagged;
  }
}


LocationSummary* StoreIndexedInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  // The smi index is either untagged (element size == 1), or it is left smi
  // tagged (for all element sizes > 1).
  // TODO(regis): Revisit and see if the index can be immediate.
  locs->set_in(1, Location::WritableRegister());
  switch (class_id()) {
    case kArrayCid:
      locs->set_in(2, ShouldEmitStoreBarrier()
                        ? Location::WritableRegister()
                        : Location::RegisterOrConstant(value()));
      break;
    case kExternalTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid:
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kTypedDataUint8ClampedArrayCid:
    case kOneByteStringCid:
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid:
      locs->set_in(2, Location::WritableRegister());
      break;
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid:
      locs->set_in(2, Location::WritableRegister());
      break;
    case kTypedDataFloat32ArrayCid:
    case kTypedDataFloat64ArrayCid:  // TODO(srdjan): Support Float64 constants.
      locs->set_in(2, Location::RequiresFpuRegister());
      break;
    case kTypedDataInt32x4ArrayCid:
    case kTypedDataFloat32x4ArrayCid:
    case kTypedDataFloat64x2ArrayCid:
      locs->set_in(2, Location::RequiresFpuRegister());
      break;
    default:
      UNREACHABLE();
      return NULL;
  }
  return locs;
}


void StoreIndexedInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Register array = locs()->in(0).reg();
  Location index = locs()->in(1);

  Address element_address(kNoRegister, 0);
  ASSERT(index.IsRegister());  // TODO(regis): Revisit.
  // Note that index is expected smi-tagged, (i.e, times 2) for all arrays
  // with index scale factor > 1. E.g., for Uint8Array and OneByteString the
  // index is expected to be untagged before accessing.
  ASSERT(kSmiTagShift == 1);
  switch (index_scale()) {
    case 1: {
      __ SmiUntag(index.reg());
      break;
    }
    case 2: {
      break;
    }
    case 4: {
      __ Lsl(index.reg(), index.reg(), 1);
      break;
    }
    case 8: {
      __ Lsl(index.reg(), index.reg(), 2);
      break;
    }
    case 16: {
      __ Lsl(index.reg(), index.reg(), 3);
      break;
    }
    default:
      UNREACHABLE();
  }
  if (!IsExternal()) {
    ASSERT(this->array()->definition()->representation() == kTagged);
    __ AddImmediate(index.reg(), index.reg(),
        FlowGraphCompiler::DataOffsetFor(class_id()) - kHeapObjectTag, PP);
  }
  element_address = Address(array, index.reg(), UXTX, Address::Unscaled);

  switch (class_id()) {
    case kArrayCid:
      if (ShouldEmitStoreBarrier()) {
        const Register value = locs()->in(2).reg();
        __ StoreIntoObject(array, element_address, value);
      } else if (locs()->in(2).IsConstant()) {
        const Object& constant = locs()->in(2).constant();
        __ StoreIntoObjectNoBarrier(array, element_address, constant);
      } else {
        const Register value = locs()->in(2).reg();
        __ StoreIntoObjectNoBarrier(array, element_address, value);
      }
      break;
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kOneByteStringCid: {
      if (locs()->in(2).IsConstant()) {
        const Smi& constant = Smi::Cast(locs()->in(2).constant());
        __ LoadImmediate(TMP, static_cast<int8_t>(constant.Value()), PP);
        __ str(TMP, element_address, kUnsignedByte);
      } else {
        const Register value = locs()->in(2).reg();
        __ SmiUntag(value);
        __ str(value, element_address, kUnsignedByte);
      }
      break;
    }
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid: {
      if (locs()->in(2).IsConstant()) {
        const Smi& constant = Smi::Cast(locs()->in(2).constant());
        intptr_t value = constant.Value();
        // Clamp to 0x0 or 0xFF respectively.
        if (value > 0xFF) {
          value = 0xFF;
        } else if (value < 0) {
          value = 0;
        }
        __ LoadImmediate(TMP, static_cast<int8_t>(value), PP);
        __ str(TMP, element_address, kUnsignedByte);
      } else {
        const Register value = locs()->in(2).reg();
        Label store_value;
        __ SmiUntag(value);
        __ CompareImmediate(value, 0xFF, PP);
        // Clamp to 0x00 or 0xFF respectively.
        __ b(&store_value, LS);
        __ LoadImmediate(TMP, 0x00, PP);
        __ LoadImmediate(TMP2, 0xFF, PP);
        __ csel(value, TMP, value, LE);
        __ csel(value, TMP2, value, GT);
        __ Bind(&store_value);
        __ str(value, element_address, kUnsignedByte);
      }
      break;
    }
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid: {
      const Register value = locs()->in(2).reg();
      __ SmiUntag(value);
      __ str(value, element_address, kUnsignedHalfword);
      break;
    }
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid: {
      const Register value = locs()->in(2).reg();
      __ SmiUntag(value);
      __ str(value, element_address, kUnsignedWord);
      break;
    }
    case kTypedDataFloat32ArrayCid: {
      const VRegister in2 = locs()->in(2).fpu_reg();
      __ add(index.reg(), index.reg(), Operand(array));
      __ fstrs(in2, Address(index.reg()));
      break;
    }
    case kTypedDataFloat64ArrayCid: {
      const VRegister in2 = locs()->in(2).fpu_reg();
      __ add(index.reg(), index.reg(), Operand(array));
      __ StoreDToOffset(in2, index.reg(), 0, PP);
      break;
    }
    case kTypedDataFloat64x2ArrayCid:
    case kTypedDataInt32x4ArrayCid:
    case kTypedDataFloat32x4ArrayCid: {
      const VRegister in2 = locs()->in(2).fpu_reg();
      __ add(index.reg(), index.reg(), Operand(array));
      __ StoreQToOffset(in2, index.reg(), 0, PP);
      break;
    }
    default:
      UNREACHABLE();
  }
}


static void LoadValueCid(FlowGraphCompiler* compiler,
                         Register value_cid_reg,
                         Register value_reg,
                         Label* value_is_smi = NULL) {
  Label done;
  if (value_is_smi == NULL) {
    __ LoadImmediate(value_cid_reg, kSmiCid, PP);
  }
  __ tsti(value_reg, kSmiTagMask);
  if (value_is_smi == NULL) {
    __ b(&done, EQ);
  } else {
    __ b(value_is_smi, EQ);
  }
  __ LoadClassId(value_cid_reg, value_reg, PP);
  __ Bind(&done);
}


LocationSummary* GuardFieldInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, 0, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  const bool field_has_length = field().needs_length_check();
  summary->AddTemp(Location::RequiresRegister());
  summary->AddTemp(Location::RequiresRegister());
  const bool need_field_temp_reg =
      field_has_length || (field().guarded_cid() == kIllegalCid);
  if (need_field_temp_reg) {
    summary->AddTemp(Location::RequiresRegister());
  }
  return summary;
}


void GuardFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const intptr_t field_cid = field().guarded_cid();
  const intptr_t nullability = field().is_nullable() ? kNullCid : kIllegalCid;
  const intptr_t field_length = field().guarded_list_length();
  const bool field_has_length = field().needs_length_check();
  const bool needs_field_temp_reg =
      field_has_length || (field().guarded_cid() == kIllegalCid);
  if (field_has_length) {
    // Currently, we should only see final fields that remember length.
    ASSERT(field().is_final());
  }

  if (field_cid == kDynamicCid) {
    ASSERT(!compiler->is_optimizing());
    return;  // Nothing to emit.
  }

  const intptr_t value_cid = value()->Type()->ToCid();

  Register value_reg = locs()->in(0).reg();

  Register value_cid_reg = locs()->temp(0).reg();

  Register temp_reg = locs()->temp(1).reg();

  Register field_reg = needs_field_temp_reg ?
      locs()->temp(locs()->temp_count() - 1).reg() : kNoRegister;

  Label ok, fail_label;

  Label* deopt = compiler->is_optimizing() ?
      compiler->AddDeoptStub(deopt_id(), ICData::kDeoptGuardField) : NULL;

  Label* fail = (deopt != NULL) ? deopt : &fail_label;

  if (!compiler->is_optimizing() || (field_cid == kIllegalCid)) {
    if (!compiler->is_optimizing() && (field_reg == kNoRegister)) {
      // Currently we can't have different location summaries for optimized
      // and non-optimized code. So instead we manually pick up a register
      // that is known to be free because we know how non-optimizing compiler
      // allocates registers.
      field_reg = R2;
      ASSERT((field_reg != value_reg) && (field_reg != value_cid_reg));
    }

    __ LoadObject(field_reg, Field::ZoneHandle(field().raw()), PP);

    FieldAddress field_cid_operand(field_reg, Field::guarded_cid_offset());
    FieldAddress field_nullability_operand(
        field_reg, Field::is_nullable_offset());
    FieldAddress field_length_operand(
        field_reg, Field::guarded_list_length_offset());

    ASSERT(value_cid_reg != kNoRegister);
    ASSERT((value_cid_reg != value_reg) && (field_reg != value_cid_reg));

    if (value_cid == kDynamicCid) {
      LoadValueCid(compiler, value_cid_reg, value_reg);
      Label skip_length_check;
      __ ldr(TMP, field_cid_operand);
      __ CompareRegisters(value_cid_reg, TMP);
      __ b(&skip_length_check, NE);
      if (field_has_length) {
        ASSERT(temp_reg != kNoRegister);
        // Field guard may have remembered list length, check it.
        if ((field_cid == kArrayCid) || (field_cid == kImmutableArrayCid)) {
          __ LoadFieldFromOffset(
              temp_reg, value_reg, Array::length_offset(), PP);
          __ CompareImmediate(temp_reg, Smi::RawValue(field_length), PP);
        } else if (RawObject::IsTypedDataClassId(field_cid)) {
          __ LoadFieldFromOffset(
              temp_reg, value_reg, TypedData::length_offset(), PP);
          __ CompareImmediate(temp_reg, Smi::RawValue(field_length), PP);
        } else {
          ASSERT(field_cid == kIllegalCid);
          ASSERT(field_length == Field::kUnknownFixedLength);
          // At compile time we do not know the type of the field nor its
          // length. At execution time we may have set the class id and
          // list length so we compare the guarded length with the
          // list length here, without this check the list length could change
          // without triggering a deoptimization.
          Label check_array, length_compared, no_fixed_length;
          // If length is negative the length guard is either disabled or
          // has not been initialized, either way it is safe to skip the
          // length check.
          __ ldr(TMP, field_length_operand);
          __ CompareImmediate(TMP, 0, PP);
          __ b(&skip_length_check, LT);
          __ CompareImmediate(value_cid_reg, kNullCid, PP);
          __ b(&no_fixed_length, EQ);
          // Check for typed data array.
          __ CompareImmediate(value_cid_reg, kTypedDataInt32x4ArrayCid, PP);
          __ b(&no_fixed_length, GT);
          __ CompareImmediate(value_cid_reg, kTypedDataInt8ArrayCid, PP);
          // Could still be a regular array.
          __ b(&check_array, LT);
          __ LoadFieldFromOffset(
              temp_reg, value_reg, TypedData::length_offset(), PP);
          __ ldr(TMP, field_length_operand);
          __ CompareRegisters(temp_reg, TMP);
          __ b(&length_compared);
          // Check for regular array.
          __ Bind(&check_array);
          __ CompareImmediate(value_cid_reg, kImmutableArrayCid, PP);
          __ b(&no_fixed_length, GT);
          __ CompareImmediate(value_cid_reg, kArrayCid, PP);
          __ b(&no_fixed_length, LT);
          __ LoadFieldFromOffset(
              temp_reg, value_reg, Array::length_offset(), PP);
          __ ldr(TMP, field_length_operand);
          __ CompareRegisters(temp_reg, TMP);
          __ b(&length_compared);
          __ Bind(&no_fixed_length);
          __ b(fail);
          __ Bind(&length_compared);
          // Following branch cannot not occur, fall through.
        }
        __ b(fail, NE);
      }
      __ Bind(&skip_length_check);
      __ ldr(TMP, field_nullability_operand);
      __ CompareRegisters(value_cid_reg, TMP);
    } else if (value_cid == kNullCid) {
      __ ldr(value_cid_reg, field_nullability_operand);
      __ CompareImmediate(value_cid_reg, value_cid, PP);
    } else {
      Label skip_length_check;
      __ ldr(value_cid_reg, field_cid_operand);
      __ CompareImmediate(value_cid_reg, value_cid, PP);
      __ b(&skip_length_check, NE);
      if (field_has_length) {
        ASSERT(value_cid_reg != kNoRegister);
        ASSERT(temp_reg != kNoRegister);
        if ((value_cid == kArrayCid) || (value_cid == kImmutableArrayCid)) {
          __ LoadFieldFromOffset(
              temp_reg, value_reg, Array::length_offset(), PP);
          __ CompareImmediate(temp_reg, Smi::RawValue(field_length), PP);
        } else if (RawObject::IsTypedDataClassId(value_cid)) {
          __ LoadFieldFromOffset(
              temp_reg, value_reg, TypedData::length_offset(), PP);
          __ CompareImmediate(temp_reg, Smi::RawValue(field_length), PP);
        } else if (field_cid != kIllegalCid) {
          ASSERT(field_cid != value_cid);
          ASSERT(field_length >= 0);
          // Field has a known class id and length. At compile time it is
          // known that the value's class id is not a fixed length list.
          __ b(fail);
        } else {
          ASSERT(field_cid == kIllegalCid);
          ASSERT(field_length == Field::kUnknownFixedLength);
          // Following jump cannot not occur, fall through.
        }
        __ b(fail, NE);
      }
      // Not identical, possibly null.
      __ Bind(&skip_length_check);
    }
    __ b(&ok, EQ);

    __ ldr(TMP, field_cid_operand);
    __ CompareImmediate(TMP, kIllegalCid, PP);
    __ b(fail, NE);

    if (value_cid == kDynamicCid) {
      __ str(value_cid_reg, field_cid_operand);
      __ str(value_cid_reg, field_nullability_operand);
      if (field_has_length) {
        Label check_array, length_set, no_fixed_length;
        __ CompareImmediate(value_cid_reg, kNullCid, PP);
        __ b(&no_fixed_length, EQ);
        // Check for typed data array.
        __ CompareImmediate(value_cid_reg, kTypedDataInt32x4ArrayCid, PP);
        __ b(&no_fixed_length, GT);
        __ CompareImmediate(value_cid_reg, kTypedDataInt8ArrayCid, PP);
        // Could still be a regular array.
        __ b(&check_array, LT);
        // Destroy value_cid_reg (safe because we are finished with it).
        __ LoadFieldFromOffset(
            value_cid_reg, value_reg, TypedData::length_offset(), PP);
        __ str(value_cid_reg, field_length_operand);
        __ b(&length_set);  // Updated field length typed data array.
        // Check for regular array.
        __ Bind(&check_array);
        __ CompareImmediate(value_cid_reg, kImmutableArrayCid, PP);
        __ b(&no_fixed_length, GT);
        __ CompareImmediate(value_cid_reg, kArrayCid, PP);
        __ b(&no_fixed_length, LT);
        // Destroy value_cid_reg (safe because we are finished with it).
        __ LoadFieldFromOffset(
            value_cid_reg, value_reg, Array::length_offset(), PP);
        __ str(value_cid_reg, field_length_operand);
        // Updated field length from regular array.
        __ b(&length_set);
        __ Bind(&no_fixed_length);
        __ LoadImmediate(TMP, Smi::RawValue(Field::kNoFixedLength), PP);
        __ str(TMP, field_length_operand);
        __ Bind(&length_set);
      }
    } else {
      __ LoadImmediate(TMP, value_cid, PP);
      __ str(TMP, field_cid_operand);
      __ str(TMP, field_nullability_operand);
      if (field_has_length) {
        if ((value_cid == kArrayCid) || (value_cid == kImmutableArrayCid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, Array::length_offset(), PP);
          __ str(value_cid_reg, field_length_operand);
        } else if (RawObject::IsTypedDataClassId(value_cid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, TypedData::length_offset(), PP);
          __ str(value_cid_reg, field_length_operand);
        } else {
          __ LoadImmediate(TMP, Smi::RawValue(Field::kNoFixedLength), PP);
          __ str(TMP, field_length_operand);
        }
      }
    }

    if (deopt == NULL) {
      ASSERT(!compiler->is_optimizing());
      __ b(&ok);
      __ Bind(fail);

      __ LoadFieldFromOffset(TMP, field_reg, Field::guarded_cid_offset(), PP);
      __ CompareImmediate(TMP, kDynamicCid, PP);
      __ b(&ok, EQ);

      __ Push(field_reg);
      __ Push(value_reg);
      __ CallRuntime(kUpdateFieldCidRuntimeEntry, 2);
      __ Drop(2);  // Drop the field and the value.
    }
  } else {
    ASSERT(compiler->is_optimizing());
    ASSERT(deopt != NULL);
    // Field guard class has been initialized and is known.
    if (field_reg != kNoRegister) {
      __ LoadObject(field_reg, Field::ZoneHandle(field().raw()), PP);
    }
    if (value_cid == kDynamicCid) {
      // Field's guarded class id is fixed by value's class id is not known.
      __ tsti(value_reg, kSmiTagMask);

      if (field_cid != kSmiCid) {
        __ b(fail, EQ);
        __ LoadClassId(value_cid_reg, value_reg, PP);
        __ CompareImmediate(value_cid_reg, field_cid, PP);
      }

      if (field_has_length) {
        __ b(fail, NE);
        // Classes are same, perform guarded list length check.
        ASSERT(field_reg != kNoRegister);
        ASSERT(value_cid_reg != kNoRegister);
        FieldAddress field_length_operand(
            field_reg, Field::guarded_list_length_offset());
        if ((field_cid == kArrayCid) || (field_cid == kImmutableArrayCid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, Array::length_offset(), PP);
        } else if (RawObject::IsTypedDataClassId(field_cid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, TypedData::length_offset(), PP);
        }
        __ ldr(TMP, field_length_operand);
        __ CompareRegisters(value_cid_reg, TMP);
      }

      if (field().is_nullable() && (field_cid != kNullCid)) {
        __ b(&ok, EQ);
        __ CompareObject(value_reg, Object::null_object(), PP);
      }
      __ b(fail, NE);
    } else {
      // Both value's and field's class id is known.
      if ((value_cid != field_cid) && (value_cid != nullability)) {
        __ b(fail);
      } else if (field_has_length && (value_cid == field_cid)) {
        ASSERT(value_cid_reg != kNoRegister);
        if ((field_cid == kArrayCid) || (field_cid == kImmutableArrayCid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, Array::length_offset(), PP);
        } else if (RawObject::IsTypedDataClassId(field_cid)) {
          // Destroy value_cid_reg (safe because we are finished with it).
          __ LoadFieldFromOffset(
              value_cid_reg, value_reg, TypedData::length_offset(), PP);
        }
        __ CompareImmediate(value_cid_reg, field_length, PP);
        __ b(fail, NE);
      } else {
        UNREACHABLE();
      }
    }
  }
  __ Bind(&ok);
}


class StoreInstanceFieldSlowPath : public SlowPathCode {
 public:
  StoreInstanceFieldSlowPath(StoreInstanceFieldInstr* instruction,
                             const Class& cls)
      : instruction_(instruction), cls_(cls) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    __ Comment("StoreInstanceFieldSlowPath");
    __ Bind(entry_label());

    const Code& stub =
        Code::Handle(StubCode::GetAllocationStubForClass(cls_));
    const ExternalLabel label(cls_.ToCString(), stub.EntryPoint());

    LocationSummary* locs = instruction_->locs();
    locs->live_registers()->Remove(locs->out(0));

    compiler->SaveLiveRegisters(locs);
    compiler->GenerateCall(Scanner::kNoSourcePos,  // No token position.
                           &label,
                           PcDescriptors::kOther,
                           locs);
    __ mov(locs->temp(0).reg(), R0);
    compiler->RestoreLiveRegisters(locs);

    __ b(exit_label());
  }

 private:
  StoreInstanceFieldInstr* instruction_;
  const Class& cls_;
};


LocationSummary* StoreInstanceFieldInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps,
          !field().IsNull() &&
          ((field().guarded_cid() == kIllegalCid) || is_initialization_)
          ? LocationSummary::kCallOnSlowPath
          : LocationSummary::kNoCall);

  summary->set_in(0, Location::RequiresRegister());
  if (IsUnboxedStore() && opt) {
    summary->set_in(1, Location::RequiresFpuRegister());
    summary->AddTemp(Location::RequiresRegister());
    summary->AddTemp(Location::RequiresRegister());
  } else if (IsPotentialUnboxedStore()) {
      summary->set_in(1, ShouldEmitStoreBarrier()
          ? Location::WritableRegister()
          :  Location::RequiresRegister());
      summary->AddTemp(Location::RequiresRegister());
      summary->AddTemp(Location::RequiresRegister());
      summary->AddTemp(opt ? Location::RequiresFpuRegister()
                           : Location::FpuRegisterLocation(V1));
  } else {
    summary->set_in(1, ShouldEmitStoreBarrier()
                       ? Location::WritableRegister()
                       : Location::RegisterOrConstant(value()));
  }
  return summary;
}


void StoreInstanceFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label skip_store;

  Register instance_reg = locs()->in(0).reg();

  if (IsUnboxedStore() && compiler->is_optimizing()) {
    const VRegister value = locs()->in(1).fpu_reg();
    const Register temp = locs()->temp(0).reg();
    const Register temp2 = locs()->temp(1).reg();
    const intptr_t cid = field().UnboxedFieldCid();

    if (is_initialization_) {
      const Class* cls = NULL;
      switch (cid) {
        case kDoubleCid:
          cls = &compiler->double_class();
          break;
        case kFloat32x4Cid:
          cls = &compiler->float32x4_class();
          break;
        case kFloat64x2Cid:
          cls = &compiler->float64x2_class();
          break;
        default:
          UNREACHABLE();
      }

      StoreInstanceFieldSlowPath* slow_path =
          new StoreInstanceFieldSlowPath(this, *cls);
      compiler->AddSlowPathCode(slow_path);

      __ TryAllocate(*cls,
                     slow_path->entry_label(),
                     temp,
                     temp2,
                     PP);
      __ Bind(slow_path->exit_label());
      __ mov(temp2, temp);
      __ StoreIntoObjectOffset(instance_reg, offset_in_bytes_, temp2, PP);
    } else {
      __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes_, PP);
    }
    switch (cid) {
      case kDoubleCid:
        __ Comment("UnboxedDoubleStoreInstanceFieldInstr");
        __ StoreDFieldToOffset(value, temp, Double::value_offset(), PP);
        break;
      case kFloat32x4Cid:
        __ Comment("UnboxedFloat32x4StoreInstanceFieldInstr");
        __ StoreQFieldToOffset(value, temp, Float32x4::value_offset(), PP);
        break;
      case kFloat64x2Cid:
        __ Comment("UnboxedFloat64x2StoreInstanceFieldInstr");
        __ StoreQFieldToOffset(value, temp, Float64x2::value_offset(), PP);
        break;
      default:
        UNREACHABLE();
    }

    return;
  }

  if (IsPotentialUnboxedStore()) {
    const Register value_reg = locs()->in(1).reg();
    const Register temp = locs()->temp(0).reg();
    const Register temp2 = locs()->temp(1).reg();
    const VRegister fpu_temp = locs()->temp(2).fpu_reg();

    Label store_pointer;
    Label store_double;
    Label store_float32x4;
    Label store_float64x2;

    __ LoadObject(temp, Field::ZoneHandle(field().raw()), PP);

    __ LoadFieldFromOffset(temp2, temp, Field::is_nullable_offset(), PP);
    __ CompareImmediate(temp2, kNullCid, PP);
    __ b(&store_pointer, EQ);

    __ LoadFromOffset(
        temp2, temp, Field::kind_bits_offset() - kHeapObjectTag,
        PP, kUnsignedByte);
    __ tsti(temp2, 1 << Field::kUnboxingCandidateBit);
    __ b(&store_pointer, EQ);

    __ LoadFieldFromOffset(temp2, temp, Field::guarded_cid_offset(), PP);
    __ CompareImmediate(temp2, kDoubleCid, PP);
    __ b(&store_double, EQ);

    __ LoadFieldFromOffset(temp2, temp, Field::guarded_cid_offset(), PP);
    __ CompareImmediate(temp2, kFloat32x4Cid, PP);
    __ b(&store_float32x4, EQ);

    __ LoadFieldFromOffset(temp2, temp, Field::guarded_cid_offset(), PP);
    __ CompareImmediate(temp2, kFloat64x2Cid, PP);
    __ b(&store_float64x2, EQ);

    // Fall through.
    __ b(&store_pointer);

    if (!compiler->is_optimizing()) {
        locs()->live_registers()->Add(locs()->in(0));
        locs()->live_registers()->Add(locs()->in(1));
    }

    {
      __ Bind(&store_double);
      Label copy_double;
      StoreInstanceFieldSlowPath* slow_path =
          new StoreInstanceFieldSlowPath(this, compiler->double_class());
      compiler->AddSlowPathCode(slow_path);

      __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes_, PP);
      __ CompareObject(temp, Object::null_object(), PP);
      __ b(&copy_double, NE);

      __ TryAllocate(compiler->double_class(),
                     slow_path->entry_label(),
                     temp,
                     temp2,
                     PP);
      __ Bind(slow_path->exit_label());
      __ mov(temp2, temp);
      __ StoreIntoObjectOffset(instance_reg, offset_in_bytes_, temp2, PP);
      __ Bind(&copy_double);
      __ LoadDFieldFromOffset(fpu_temp, value_reg, Double::value_offset(), PP);
      __ StoreDFieldToOffset(fpu_temp, temp, Double::value_offset(), PP);
      __ b(&skip_store);
    }

    {
      __ Bind(&store_float32x4);
      Label copy_float32x4;
      StoreInstanceFieldSlowPath* slow_path =
          new StoreInstanceFieldSlowPath(this, compiler->float32x4_class());
      compiler->AddSlowPathCode(slow_path);

      __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes_, PP);
      __ CompareObject(temp, Object::null_object(), PP);
      __ b(&copy_float32x4, NE);

      __ TryAllocate(compiler->float32x4_class(),
                     slow_path->entry_label(),
                     temp,
                     temp2,
                     PP);
      __ Bind(slow_path->exit_label());
      __ mov(temp2, temp);
      __ StoreIntoObjectOffset(instance_reg, offset_in_bytes_, temp2, PP);
      __ Bind(&copy_float32x4);
      __ LoadQFieldFromOffset(
          fpu_temp, value_reg, Float32x4::value_offset(), PP);
      __ StoreQFieldToOffset(
          fpu_temp, value_reg, Float32x4::value_offset(), PP);
      __ b(&skip_store);
    }

    {
      __ Bind(&store_float64x2);
      Label copy_float64x2;
      StoreInstanceFieldSlowPath* slow_path =
          new StoreInstanceFieldSlowPath(this, compiler->float64x2_class());
      compiler->AddSlowPathCode(slow_path);

      __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes_, PP);
      __ CompareObject(temp, Object::null_object(), PP);
      __ b(&copy_float64x2, NE);

      __ TryAllocate(compiler->float64x2_class(),
                     slow_path->entry_label(),
                     temp,
                     temp2,
                     PP);
      __ Bind(slow_path->exit_label());
      __ mov(temp2, temp);
      __ StoreIntoObjectOffset(instance_reg, offset_in_bytes_, temp2, PP);
      __ Bind(&copy_float64x2);
      __ LoadQFieldFromOffset(
          fpu_temp, value_reg, Float64x2::value_offset(), PP);
      __ StoreQFieldToOffset(
          fpu_temp, value_reg, Float64x2::value_offset(), PP);
      __ b(&skip_store);
    }

    __ Bind(&store_pointer);
  }

  if (ShouldEmitStoreBarrier()) {
    const Register value_reg = locs()->in(1).reg();
    __ StoreIntoObjectOffset(
        instance_reg, offset_in_bytes_, value_reg, PP, CanValueBeSmi());
  } else {
    if (locs()->in(1).IsConstant()) {
      __ StoreIntoObjectOffsetNoBarrier(
          instance_reg,
          offset_in_bytes_,
          locs()->in(1).constant(),
          PP);
    } else {
      const Register value_reg = locs()->in(1).reg();
      __ StoreIntoObjectOffsetNoBarrier(
          instance_reg,
          offset_in_bytes_,
          value_reg,
          PP);
    }
  }
  __ Bind(&skip_store);
}


LocationSummary* LoadStaticFieldInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_out(0, Location::RequiresRegister());
  return summary;
}


// When the parser is building an implicit static getter for optimization,
// it can generate a function body where deoptimization ids do not line up
// with the unoptimized code.
//
// This is safe only so long as LoadStaticFieldInstr cannot deoptimize.
void LoadStaticFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register field = locs()->in(0).reg();
  Register result = locs()->out(0).reg();
  __ LoadFieldFromOffset(result, field, Field::value_offset(), PP);
}


LocationSummary* StoreStaticFieldInstr::MakeLocationSummary(bool opt) const {
  LocationSummary* locs = new LocationSummary(1, 1, LocationSummary::kNoCall);
  locs->set_in(0, value()->NeedsStoreBuffer() ? Location::WritableRegister()
                                              : Location::RequiresRegister());
  locs->set_temp(0, Location::RequiresRegister());
  return locs;
}


void StoreStaticFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();

  __ LoadObject(temp, field(), PP);
  if (this->value()->NeedsStoreBuffer()) {
    __ StoreIntoObjectOffset(
        temp, Field::value_offset(), value, PP, CanValueBeSmi());
  } else {
    __ StoreIntoObjectOffsetNoBarrier(temp, Field::value_offset(), value, PP);
  }
}


LocationSummary* InstanceOfInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(R0));
  summary->set_in(1, Location::RegisterLocation(R2));
  summary->set_in(2, Location::RegisterLocation(R1));
  summary->set_out(0, Location::RegisterLocation(R0));
  return summary;
}


void InstanceOfInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->in(0).reg() == R0);  // Value.
  ASSERT(locs()->in(1).reg() == R2);  // Instantiator.
  ASSERT(locs()->in(2).reg() == R1);  // Instantiator type arguments.

  compiler->GenerateInstanceOf(token_pos(),
                               deopt_id(),
                               type(),
                               negate_result(),
                               locs());
  ASSERT(locs()->out(0).reg() == R0);
}


LocationSummary* CreateArrayInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(kElementTypePos, Location::RegisterLocation(R1));
  locs->set_in(kLengthPos, Location::RegisterLocation(R2));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void CreateArrayInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Allocate the array.  R2 = length, R1 = element type.
  ASSERT(locs()->in(kElementTypePos).reg() == R1);
  ASSERT(locs()->in(kLengthPos).reg() == R2);
  compiler->GenerateCall(token_pos(),
                         &StubCode::AllocateArrayLabel(),
                         PcDescriptors::kOther,
                         locs());
  ASSERT(locs()->out(0).reg() == R0);
}


class BoxDoubleSlowPath : public SlowPathCode {
 public:
  explicit BoxDoubleSlowPath(Instruction* instruction)
      : instruction_(instruction) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    __ Comment("BoxDoubleSlowPath");
    __ Bind(entry_label());
    const Class& double_class = compiler->double_class();
    const Code& stub =
        Code::Handle(StubCode::GetAllocationStubForClass(double_class));
    const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());

    LocationSummary* locs = instruction_->locs();
    locs->live_registers()->Remove(locs->out(0));

    compiler->SaveLiveRegisters(locs);
    compiler->GenerateCall(Scanner::kNoSourcePos,  // No token position.
                           &label,
                           PcDescriptors::kOther,
                           locs);
    __ mov(locs->out(0).reg(), R0);
    compiler->RestoreLiveRegisters(locs);

    __ b(exit_label());
  }

 private:
  Instruction* instruction_;
};


LocationSummary* LoadFieldInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(
          kNumInputs, kNumTemps,
          (opt && !IsPotentialUnboxedLoad())
          ? LocationSummary::kNoCall
          : LocationSummary::kCallOnSlowPath);

  locs->set_in(0, Location::RequiresRegister());

  if (IsUnboxedLoad() && opt) {
    locs->AddTemp(Location::RequiresRegister());
  } else if (IsPotentialUnboxedLoad()) {
    locs->AddTemp(opt ? Location::RequiresFpuRegister()
                      : Location::FpuRegisterLocation(V1));
    locs->AddTemp(Location::RequiresRegister());
  }
  locs->set_out(0, Location::RequiresRegister());
  return locs;
}


void LoadFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  if (IsUnboxedLoad() && compiler->is_optimizing()) {
    const VRegister result = locs()->out(0).fpu_reg();
    const Register temp = locs()->temp(0).reg();
    __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes(), PP);
    const intptr_t cid = field()->UnboxedFieldCid();
    switch (cid) {
      case kDoubleCid:
        __ Comment("UnboxedDoubleLoadFieldInstr");
        __ LoadDFieldFromOffset(result, temp, Double::value_offset(), PP);
        break;
      case kFloat32x4Cid:
      case kFloat64x2Cid:
        UNIMPLEMENTED();
        break;
      default:
        UNREACHABLE();
    }
    return;
  }

  Label done;
  Register result_reg = locs()->out(0).reg();
  if (IsPotentialUnboxedLoad()) {
    const Register temp = locs()->temp(1).reg();
    const VRegister value = locs()->temp(0).fpu_reg();

    Label load_pointer;
    Label load_double;
    Label load_float32x4;
    Label load_float64x2;

    __ LoadObject(result_reg, Field::ZoneHandle(field()->raw()), PP);

    FieldAddress field_cid_operand(result_reg, Field::guarded_cid_offset());
    FieldAddress field_nullability_operand(result_reg,
                                           Field::is_nullable_offset());

    __ ldr(temp, field_nullability_operand);
    __ CompareImmediate(temp, kNullCid, PP);
    __ b(&load_pointer, EQ);

    __ ldr(temp, field_cid_operand);
    __ CompareImmediate(temp, kDoubleCid, PP);
    __ b(&load_double, EQ);

    __ ldr(temp, field_cid_operand);
    __ CompareImmediate(temp, kFloat32x4Cid, PP);
    __ b(&load_float32x4, EQ);

    __ ldr(temp, field_cid_operand);
    __ CompareImmediate(temp, kFloat64x2Cid, PP);
    __ b(&load_float64x2, EQ);

    // Fall through.
    __ b(&load_pointer);

    if (!compiler->is_optimizing()) {
      locs()->live_registers()->Add(locs()->in(0));
    }

    {
      __ Bind(&load_double);
      BoxDoubleSlowPath* slow_path = new BoxDoubleSlowPath(this);
      compiler->AddSlowPathCode(slow_path);

      __ TryAllocate(compiler->double_class(),
                     slow_path->entry_label(),
                     result_reg,
                     temp,
                     PP);
      __ Bind(slow_path->exit_label());
      __ LoadFieldFromOffset(temp, instance_reg, offset_in_bytes(), PP);
      __ LoadDFieldFromOffset(value, temp, Double::value_offset(), PP);
      __ StoreDFieldToOffset(value, result_reg, Double::value_offset(), PP);
      __ b(&done);
    }

    // TODO(zra): Implement these when we add simd loads and stores.
    {
      __ Bind(&load_float32x4);
      __ Stop("Float32x4 Unimplemented");
    }

    {
      __ Bind(&load_float64x2);
      __ Stop("Float64x2 Unimplemented");
    }

    __ Bind(&load_pointer);
  }
  __ LoadFieldFromOffset(result_reg, instance_reg, offset_in_bytes(), PP);
  __ Bind(&done);
}


LocationSummary* InstantiateTypeInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(R0));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void InstantiateTypeInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register result_reg = locs()->out(0).reg();

  // 'instantiator_reg' is the instantiator TypeArguments object (or null).
  // A runtime call to instantiate the type is required.
  __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
  __ PushObject(type(), PP);
  __ Push(instantiator_reg);  // Push instantiator type arguments.
  compiler->GenerateRuntimeCall(token_pos(),
                                deopt_id(),
                                kInstantiateTypeRuntimeEntry,
                                2,
                                locs());
  __ Drop(2);  // Drop instantiator and uninstantiated type.
  __ Pop(result_reg);  // Pop instantiated type.
  ASSERT(instantiator_reg == result_reg);
}


LocationSummary* InstantiateTypeArgumentsInstr::MakeLocationSummary(
    bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(R0));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void InstantiateTypeArgumentsInstr::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register result_reg = locs()->out(0).reg();
  ASSERT(instantiator_reg == R0);
  ASSERT(instantiator_reg == result_reg);

  // 'instantiator_reg' is the instantiator TypeArguments object (or null).
  ASSERT(!type_arguments().IsUninstantiatedIdentity() &&
         !type_arguments().CanShareInstantiatorTypeArguments(
             instantiator_class()));
  // If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of dynamic, then use null as
  // the type arguments.
  Label type_arguments_instantiated;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    __ CompareObject(instantiator_reg, Object::null_object(), PP);
    __ b(&type_arguments_instantiated, EQ);
  }

  __ LoadObject(R2, type_arguments(), PP);
  __ LoadFieldFromOffset(R2, R2, TypeArguments::instantiations_offset(), PP);
  __ AddImmediate(R2, R2, Array::data_offset() - kHeapObjectTag, PP);
  // The instantiations cache is initialized with Object::zero_array() and is
  // therefore guaranteed to contain kNoInstantiator. No length check needed.
  Label loop, found, slow_case;
  __ Bind(&loop);
  __ LoadFromOffset(R1, R2, 0 * kWordSize, PP);  // Cached instantiator.
  __ CompareRegisters(R1, R0);
  __ b(&found, EQ);
  __ AddImmediate(R2, R2, 2 * kWordSize, PP);
  __ CompareImmediate(R1, Smi::RawValue(StubCode::kNoInstantiator), PP);
  __ b(&loop, NE);
  __ b(&slow_case);
  __ Bind(&found);
  __ LoadFromOffset(R0, R2, 1 * kWordSize, PP);  // Cached instantiated args.
  __ b(&type_arguments_instantiated);

  __ Bind(&slow_case);
  // Instantiate non-null type arguments.
  // A runtime call to instantiate the type arguments is required.
  __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
  __ PushObject(type_arguments(), PP);
  __ Push(instantiator_reg);  // Push instantiator type arguments.
  compiler->GenerateRuntimeCall(token_pos(),
                                deopt_id(),
                                kInstantiateTypeArgumentsRuntimeEntry,
                                2,
                                locs());
  __ Drop(2);  // Drop instantiator and uninstantiated type arguments.
  __ Pop(result_reg);  // Pop instantiated type arguments.
  __ Bind(&type_arguments_instantiated);
}


LocationSummary* AllocateContextInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(R1));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void AllocateContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == R1);
  ASSERT(locs()->out(0).reg() == R0);

  __ LoadImmediate(R1, num_context_variables(), PP);
  const ExternalLabel label("alloc_context",
                            StubCode::AllocateContextEntryPoint());
  compiler->GenerateCall(token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
}


LocationSummary* CloneContextInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(R0));
  locs->set_out(0, Location::RegisterLocation(R0));
  return locs;
}


void CloneContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register context_value = locs()->in(0).reg();
  Register result = locs()->out(0).reg();

  __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
  __ Push(context_value);
  compiler->GenerateRuntimeCall(token_pos(),
                                deopt_id(),
                                kCloneContextRuntimeEntry,
                                1,
                                locs());
  __ Drop(1);  // Remove argument.
  __ Pop(result);  // Get result (cloned context).
}


LocationSummary* CatchBlockEntryInstr::MakeLocationSummary(bool opt) const {
  UNREACHABLE();
  return NULL;
}


void CatchBlockEntryInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ Bind(compiler->GetJumpLabel(this));
  compiler->AddExceptionHandler(catch_try_index(),
                                try_index(),
                                compiler->assembler()->CodeSize(),
                                catch_handler_types_,
                                needs_stacktrace());

  // Restore the pool pointer.
  __ LoadPoolPointer(PP);

  if (HasParallelMove()) {
    compiler->parallel_move_resolver()->EmitNativeCode(parallel_move());
  }

  // Restore SP from FP as we are coming from a throw and the code for
  // popping arguments has not been run.
  const intptr_t fp_sp_dist =
      (kFirstLocalSlotFromFp + 1 - compiler->StackSize()) * kWordSize;
  ASSERT(fp_sp_dist <= 0);
  __ AddImmediate(SP, FP, fp_sp_dist, PP);

  // Restore stack and initialize the two exception variables:
  // exception and stack trace variables.
  __ StoreToOffset(kExceptionObjectReg,
                   FP, exception_var().index() * kWordSize, PP);
  __ StoreToOffset(kStackTraceObjectReg,
                   FP, stacktrace_var().index() * kWordSize, PP);
}


LocationSummary* CheckStackOverflowInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs,
                          kNumTemps,
                          LocationSummary::kCallOnSlowPath);
  summary->set_temp(0, Location::RequiresRegister());
  return summary;
}


class CheckStackOverflowSlowPath : public SlowPathCode {
 public:
  explicit CheckStackOverflowSlowPath(CheckStackOverflowInstr* instruction)
      : instruction_(instruction) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    if (FLAG_use_osr) {
      uword flags_address = Isolate::Current()->stack_overflow_flags_address();
      Register value = instruction_->locs()->temp(0).reg();
      __ Comment("CheckStackOverflowSlowPathOsr");
      __ Bind(osr_entry_label());
      __ LoadImmediate(TMP, flags_address, PP);
      __ LoadImmediate(value, Isolate::kOsrRequest, PP);
      __ str(value, Address(TMP));
    }
    __ Comment("CheckStackOverflowSlowPath");
    __ Bind(entry_label());
    compiler->SaveLiveRegisters(instruction_->locs());
    // pending_deoptimization_env_ is needed to generate a runtime call that
    // may throw an exception.
    ASSERT(compiler->pending_deoptimization_env_ == NULL);
    Environment* env = compiler->SlowPathEnvironmentFor(instruction_);
    compiler->pending_deoptimization_env_ = env;
    compiler->GenerateRuntimeCall(instruction_->token_pos(),
                                  instruction_->deopt_id(),
                                  kStackOverflowRuntimeEntry,
                                  0,
                                  instruction_->locs());

    if (FLAG_use_osr && !compiler->is_optimizing() && instruction_->in_loop()) {
      // In unoptimized code, record loop stack checks as possible OSR entries.
      compiler->AddCurrentDescriptor(PcDescriptors::kOsrEntry,
                                     instruction_->deopt_id(),
                                     0);  // No token position.
    }
    compiler->pending_deoptimization_env_ = NULL;
    compiler->RestoreLiveRegisters(instruction_->locs());
    __ b(exit_label());
  }

  Label* osr_entry_label() {
    ASSERT(FLAG_use_osr);
    return &osr_entry_label_;
  }

 private:
  CheckStackOverflowInstr* instruction_;
  Label osr_entry_label_;
};


void CheckStackOverflowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  CheckStackOverflowSlowPath* slow_path = new CheckStackOverflowSlowPath(this);
  compiler->AddSlowPathCode(slow_path);

  __ LoadImmediate(TMP, Isolate::Current()->stack_limit_address(), PP);
  __ ldr(TMP, Address(TMP));
  __ CompareRegisters(SP, TMP);
  __ b(slow_path->entry_label(), LS);
  if (compiler->CanOSRFunction() && in_loop()) {
    Register temp = locs()->temp(0).reg();
    // In unoptimized code check the usage counter to trigger OSR at loop
    // stack checks.  Use progressively higher thresholds for more deeply
    // nested loops to attempt to hit outer loops with OSR when possible.
    __ LoadObject(temp, compiler->parsed_function().function(), PP);
    intptr_t threshold =
        FLAG_optimization_counter_threshold * (loop_depth() + 1);
    __ LoadFieldFromOffset(temp, temp, Function::usage_counter_offset(), PP);
    __ CompareImmediate(temp, threshold, PP);
    __ b(slow_path->osr_entry_label(), GE);
  }
  if (compiler->ForceSlowPathForStackOverflow()) {
    __ b(slow_path->entry_label());
  }
  __ Bind(slow_path->exit_label());
}


static void EmitJavascriptOverflowCheck(FlowGraphCompiler* compiler,
                                        Range* range,
                                        Label* overflow,
                                        Register result) {
  if (!range->IsWithin(-0x20000000000000LL, 0x20000000000000LL)) {
    ASSERT(overflow != NULL);
    __ CompareImmediate(result, -0x20000000000000LL, PP);
    __ b(overflow, LT);
    __ CompareImmediate(result, 0x20000000000000LL, PP);
    __ b(overflow, GT);
  }
}


static void EmitSmiShiftLeft(FlowGraphCompiler* compiler,
                             BinarySmiOpInstr* shift_left) {
  const bool is_truncating = shift_left->is_truncating();
  const LocationSummary& locs = *shift_left->locs();
  Register left = locs.in(0).reg();
  Register result = locs.out(0).reg();
  Label* deopt = shift_left->CanDeoptimize() ?
      compiler->AddDeoptStub(shift_left->deopt_id(), ICData::kDeoptBinarySmiOp)
      : NULL;
  if (locs.in(1).IsConstant()) {
    const Object& constant = locs.in(1).constant();
    ASSERT(constant.IsSmi());
    // Immediate shift operation takes 6 bits for the count.
    const intptr_t kCountLimit = 0x3F;
    const intptr_t value = Smi::Cast(constant).Value();
    if (value == 0) {
      __ mov(result, left);
    } else if ((value < 0) || (value >= kCountLimit)) {
      // This condition may not be known earlier in some cases because
      // of constant propagation, inlining, etc.
      if ((value >= kCountLimit) && is_truncating) {
        __ mov(result, ZR);
      } else {
        // Result is Mint or exception.
        __ b(deopt);
      }
    } else {
      if (!is_truncating) {
        // Check for overflow (preserve left).
        __ Lsl(TMP, left, value);
        __ cmp(left, Operand(TMP, ASR, value));
        __ b(deopt, NE);  // Overflow.
      }
      // Shift for result now we know there is no overflow.
      __ Lsl(result, left, value);
    }
    if (FLAG_throw_on_javascript_int_overflow) {
      EmitJavascriptOverflowCheck(compiler, shift_left->range(), deopt, result);
    }
    return;
  }

  // Right (locs.in(1)) is not constant.
  Register right = locs.in(1).reg();
  Range* right_range = shift_left->right()->definition()->range();
  if (shift_left->left()->BindsToConstant() && !is_truncating) {
    // TODO(srdjan): Implement code below for is_truncating().
    // If left is constant, we know the maximal allowed size for right.
    const Object& obj = shift_left->left()->BoundConstant();
    if (obj.IsSmi()) {
      const intptr_t left_int = Smi::Cast(obj).Value();
      if (left_int == 0) {
        __ CompareRegisters(right, ZR);
        __ b(deopt, MI);
        __ mov(result, ZR);
        return;
      }
      const intptr_t max_right = kSmiBits - Utils::HighestBit(left_int);
      const bool right_needs_check =
          (right_range == NULL) ||
          !right_range->IsWithin(0, max_right - 1);
      if (right_needs_check) {
        __ CompareImmediate(right,
            reinterpret_cast<int64_t>(Smi::New(max_right)), PP);
        __ b(deopt, CS);
      }
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into TMP.
      __ lslv(result, left, TMP);
    }
    if (FLAG_throw_on_javascript_int_overflow) {
      EmitJavascriptOverflowCheck(compiler, shift_left->range(), deopt, result);
    }
    return;
  }

  const bool right_needs_check =
      (right_range == NULL) || !right_range->IsWithin(0, (Smi::kBits - 1));
  if (is_truncating) {
    if (right_needs_check) {
      const bool right_may_be_negative =
          (right_range == NULL) ||
          !right_range->IsWithin(0, RangeBoundary::kPlusInfinity);
      if (right_may_be_negative) {
        ASSERT(shift_left->CanDeoptimize());
        __ CompareRegisters(right, ZR);
        __ b(deopt, MI);
      }

      __ CompareImmediate(
          right, reinterpret_cast<int64_t>(Smi::New(Smi::kBits)), PP);
      __ csel(result, ZR, result, CS);
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into TMP.
      __ lslv(TMP, left, TMP);
      __ csel(result, TMP, result, CC);
    } else {
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into TMP.
      __ lslv(result, left, TMP);
    }
  } else {
    if (right_needs_check) {
      ASSERT(shift_left->CanDeoptimize());
      __ CompareImmediate(
          right, reinterpret_cast<int64_t>(Smi::New(Smi::kBits)), PP);
      __ b(deopt, CS);
    }
    // Left is not a constant.
    // Check if count too large for handling it inlined.
    __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into IP.
    // Overflow test (preserve left, right, and IP);
    Register temp = locs.temp(0).reg();
    __ lslv(temp, left, TMP);
    __ asrv(TMP2, temp, TMP);
    __ CompareRegisters(left, TMP2);
    __ b(deopt, NE);  // Overflow.
    // Shift for result now we know there is no overflow.
    __ lslv(result, left, TMP);
  }
  if (FLAG_throw_on_javascript_int_overflow) {
    EmitJavascriptOverflowCheck(compiler, shift_left->range(), deopt, result);
  }
}


LocationSummary* BinarySmiOpInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  if (op_kind() == Token::kTRUNCDIV) {
    summary->set_in(0, Location::RequiresRegister());
    if (RightIsPowerOfTwoConstant()) {
      ConstantInstr* right_constant = right()->definition()->AsConstant();
      summary->set_in(1, Location::Constant(right_constant->value()));
    } else {
      summary->set_in(1, Location::RequiresRegister());
    }
    summary->set_out(0, Location::RequiresRegister());
    return summary;
  }
  if (op_kind() == Token::kMOD) {
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RequiresRegister());
    summary->set_out(0, Location::RequiresRegister());
    return summary;
  }
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, Location::RegisterOrSmiConstant(right()));
  if (((op_kind() == Token::kSHL) && !is_truncating()) ||
      (op_kind() == Token::kSHR)) {
    summary->AddTemp(Location::RequiresRegister());
  }
  // We make use of 3-operand instructions by not requiring result register
  // to be identical to first input register as on Intel.
  summary->set_out(0, Location::RequiresRegister());
  return summary;
}


void BinarySmiOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (op_kind() == Token::kSHL) {
    EmitSmiShiftLeft(compiler, this);
    return;
  }

  ASSERT(!is_truncating());
  const Register left = locs()->in(0).reg();
  const Register result = locs()->out(0).reg();
  Label* deopt = NULL;
  if (CanDeoptimize()) {
    deopt = compiler->AddDeoptStub(deopt_id(), ICData::kDeoptBinarySmiOp);
  }

  if (locs()->in(1).IsConstant()) {
    const Object& constant = locs()->in(1).constant();
    ASSERT(constant.IsSmi());
    const int64_t imm = reinterpret_cast<int64_t>(constant.raw());
    switch (op_kind()) {
      case Token::kADD: {
        if (deopt == NULL) {
          __ AddImmediate(result, left, imm, PP);
        } else {
          __ AddImmediateSetFlags(result, left, imm, PP);
          __ b(deopt, VS);
        }
        break;
      }
      case Token::kSUB: {
        if (deopt == NULL) {
          __ AddImmediate(result, left, -imm, PP);
        } else {
          // Negating imm and using AddImmediateSetFlags would not detect the
          // overflow when imm == kMinInt64.
          __ SubImmediateSetFlags(result, left, imm, PP);
          __ b(deopt, VS);
        }
        break;
      }
      case Token::kMUL: {
        // Keep left value tagged and untag right value.
        const intptr_t value = Smi::Cast(constant).Value();
        if (deopt == NULL) {
          if (value == 2) {
            __ Lsl(result, left, 1);
          } else {
            __ LoadImmediate(TMP, value, PP);
            __ mul(result, left, TMP);
          }
        } else {
          if (value == 2) {
            __ Asr(TMP, left, 63);  // TMP = sign of left.
            __ Lsl(result, left, 1);
            // TMP: result bits 32..63.
            __ cmp(TMP, Operand(result, ASR, 63));
            __ b(deopt, NE);
          } else {
            __ LoadImmediate(TMP, value, PP);
            __ mul(result, left, TMP);
            __ smulh(TMP, left, TMP);
            // TMP: result bits 64..127.
            __ cmp(TMP, Operand(result, ASR, 63));
            __ b(deopt, NE);
          }
        }
        break;
      }
      case Token::kTRUNCDIV: {
        const intptr_t value = Smi::Cast(constant).Value();
        if (value == 1) {
          __ mov(result, left);
          break;
        } else if (value == -1) {
          // Check the corner case of dividing the 'MIN_SMI' with -1, in which
          // case we cannot negate the result.
          __ CompareImmediate(left, 0x8000000000000000LL, kNoPP);
          __ b(deopt, EQ);
          __ sub(result, ZR, Operand(left));
          break;
        }
        ASSERT(Utils::IsPowerOfTwo(Utils::Abs(value)));
        const intptr_t shift_count =
            Utils::ShiftForPowerOfTwo(Utils::Abs(value)) + kSmiTagSize;
        ASSERT(kSmiTagSize == 1);
        __ Asr(TMP, left, 63);
        ASSERT(shift_count > 1);  // 1, -1 case handled above.
        const Register temp = TMP2;
        __ add(temp, left, Operand(TMP, LSR, 64 - shift_count));
        ASSERT(shift_count > 0);
        __ Asr(result, temp, shift_count);
        if (value < 0) {
          __ sub(result, ZR, Operand(result));
        }
        __ SmiTag(result);
        break;
      }
      case Token::kBIT_AND:
        // No overflow check.
        __ AndImmediate(result, left, imm, PP);
        break;
      case Token::kBIT_OR:
        // No overflow check.
        __ OrImmediate(result, left, imm, PP);
        break;
      case Token::kBIT_XOR:
        // No overflow check.
        __ XorImmediate(result, left, imm, PP);
        break;
      case Token::kSHR: {
        // Asr operation masks the count to 6 bits.
        const intptr_t kCountLimit = 0x3F;
        intptr_t value = Smi::Cast(constant).Value();

        if (value == 0) {
          // TODO(vegorov): should be handled outside.
          __ mov(result, left);
          break;
        } else if (value < 0) {
          // TODO(vegorov): should be handled outside.
          __ b(deopt);
          break;
        }

        value = value + kSmiTagSize;
        if (value >= kCountLimit) {
          value = kCountLimit;
        }

        __ Asr(result, left, value);
        __ SmiTag(result);
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
    if (FLAG_throw_on_javascript_int_overflow) {
      EmitJavascriptOverflowCheck(compiler, range(), deopt, result);
    }
    return;
  }

  Register right = locs()->in(1).reg();
  Range* right_range = this->right()->definition()->range();
  switch (op_kind()) {
    case Token::kADD: {
      if (deopt == NULL) {
        __ add(result, left, Operand(right));
      } else {
        __ adds(result, left, Operand(right));
        __ b(deopt, VS);
      }
      break;
    }
    case Token::kSUB: {
      if (deopt == NULL) {
        __ sub(result, left, Operand(right));
      } else {
        __ subs(result, left, Operand(right));
        __ b(deopt, VS);
      }
      break;
    }
    case Token::kMUL: {
      __ Asr(TMP, left, kSmiTagSize);  // SmiUntag left into TMP.
      if (deopt == NULL) {
        __ mul(result, TMP, right);
      } else {
          __ mul(result, TMP, right);
          __ smulh(TMP, TMP, right);
          // TMP: result bits 64..127.
          __ cmp(TMP, Operand(result, ASR, 63));
          __ b(deopt, NE);
      }
      break;
    }
    case Token::kBIT_AND: {
      // No overflow check.
      __ and_(result, left, Operand(right));
      break;
    }
    case Token::kBIT_OR: {
      // No overflow check.
      __ orr(result, left, Operand(right));
      break;
    }
    case Token::kBIT_XOR: {
      // No overflow check.
      __ eor(result, left, Operand(right));
      break;
    }
    case Token::kTRUNCDIV: {
      if ((right_range == NULL) || right_range->Overlaps(0, 0)) {
        // Handle divide by zero in runtime.
        __ CompareRegisters(right, ZR);
        __ b(deopt, EQ);
      }
      const Register temp = TMP2;
      __ Asr(temp, left, kSmiTagSize);  // SmiUntag left into temp.
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into IP.

      __ sdiv(result, temp, TMP);

      // Check the corner case of dividing the 'MIN_SMI' with -1, in which
      // case we cannot tag the result.
      __ CompareImmediate(result, 0x4000000000000000LL, kNoPP);
      __ b(deopt, EQ);
      __ SmiTag(result);
      break;
    }
    case Token::kMOD: {
      if ((right_range == NULL) || right_range->Overlaps(0, 0)) {
        // Handle divide by zero in runtime.
        __ CompareRegisters(right, ZR);
        __ b(deopt, EQ);
      }
      const Register temp = TMP2;
      __ Asr(temp, left, kSmiTagSize);  // SmiUntag left into temp.
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into IP.

      __ sdiv(result, temp, TMP);

      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into IP.
      __ msub(result, TMP, result, temp);  // result <- left - right * result
      __ SmiTag(result);
      //  res = left % right;
      //  if (res < 0) {
      //    if (right < 0) {
      //      res = res - right;
      //    } else {
      //      res = res + right;
      //    }
      //  }
      Label done;
      __ CompareRegisters(result, ZR);
      __ b(&done, GE);
      // Result is negative, adjust it.
      __ CompareRegisters(right, ZR);
      __ sub(TMP, result, Operand(right));
      __ add(result, result, Operand(right));
      __ csel(result, TMP, result, LT);
      __ Bind(&done);
      break;
    }
    case Token::kSHR: {
      if (CanDeoptimize()) {
        __ CompareRegisters(right, ZR);
        __ b(deopt, LT);
      }
      __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right into TMP.
      // sarl operation masks the count to 6 bits.
      const intptr_t kCountLimit = 0x3F;
      if ((right_range == NULL) ||
          !right_range->IsWithin(RangeBoundary::kMinusInfinity, kCountLimit)) {
        __ LoadImmediate(TMP2, kCountLimit, PP);
        __ CompareRegisters(TMP, TMP2);
        __ csel(TMP, TMP2, TMP, GT);
      }
      Register temp = locs()->temp(0).reg();
      __ Asr(temp, left, kSmiTagSize);  // SmiUntag left into temp.
      __ asrv(result, temp, TMP);
      __ SmiTag(result);
      break;
    }
    case Token::kDIV: {
      // Dispatches to 'Double./'.
      // TODO(srdjan): Implement as conversion to double and double division.
      UNREACHABLE();
      break;
    }
    case Token::kOR:
    case Token::kAND: {
      // Flow graph builder has dissected this operation to guarantee correct
      // behavior (short-circuit evaluation).
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
  if (FLAG_throw_on_javascript_int_overflow) {
    EmitJavascriptOverflowCheck(compiler, range(), deopt, result);
  }
}


LocationSummary* CheckEitherNonSmiInstr::MakeLocationSummary(bool opt) const {
  intptr_t left_cid = left()->Type()->ToCid();
  intptr_t right_cid = right()->Type()->ToCid();
  ASSERT((left_cid != kDoubleCid) && (right_cid != kDoubleCid));
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
    new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, Location::RequiresRegister());
  return summary;
}


void CheckEitherNonSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                        ICData::kDeoptBinaryDoubleOp);
  intptr_t left_cid = left()->Type()->ToCid();
  intptr_t right_cid = right()->Type()->ToCid();
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();
  if (left_cid == kSmiCid) {
    __ tsti(right, kSmiTagMask);
  } else if (right_cid == kSmiCid) {
    __ tsti(left, kSmiTagMask);
  } else {
    __ orr(TMP, left, Operand(right));
    __ tsti(TMP, kSmiTagMask);
  }
  __ b(deopt, EQ);
}


LocationSummary* BoxDoubleInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs,
                          kNumTemps,
                          LocationSummary::kCallOnSlowPath);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_temp(0, Location::RequiresRegister());
  summary->set_out(0, Location::RequiresRegister());
  return summary;
}


void BoxDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  BoxDoubleSlowPath* slow_path = new BoxDoubleSlowPath(this);
  compiler->AddSlowPathCode(slow_path);

  const Register out_reg = locs()->out(0).reg();
  const VRegister value = locs()->in(0).fpu_reg();

  __ TryAllocate(compiler->double_class(),
                 slow_path->entry_label(),
                 out_reg,
                 locs()->temp(0).reg(),
                 PP);
  __ Bind(slow_path->exit_label());
  __ StoreDFieldToOffset(value, out_reg, Double::value_offset(), PP);
}


LocationSummary* UnboxDoubleInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_out(0, Location::RequiresFpuRegister());
  return summary;
}


void UnboxDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  CompileType* value_type = value()->Type();
  const intptr_t value_cid = value_type->ToCid();
  const Register value = locs()->in(0).reg();
  const VRegister result = locs()->out(0).fpu_reg();

  if (value_cid == kDoubleCid) {
    __ LoadDFieldFromOffset(result, value, Double::value_offset(), PP);
  } else if (value_cid == kSmiCid) {
    __ Asr(TMP, value, kSmiTagSize);  // Untag input before conversion.
    __ scvtfd(result, TMP);
  } else {
    Label* deopt = compiler->AddDeoptStub(deopt_id_,
                                          ICData::kDeoptBinaryDoubleOp);
    if (value_type->is_nullable() &&
        (value_type->ToNullableCid() == kDoubleCid)) {
      __ CompareObject(value, Object::null_object(), PP);
      __ b(deopt, EQ);
      // It must be double now.
      __ LoadDFieldFromOffset(result, value, Double::value_offset(), PP);
    } else {
      Label is_smi, done;
      __ tsti(value, kSmiTagMask);
      __ b(&is_smi, EQ);
      __ CompareClassId(value, kDoubleCid, PP);
      __ b(deopt, NE);
      __ LoadDFieldFromOffset(result, value, Double::value_offset(), PP);
      __ b(&done);
      __ Bind(&is_smi);
      __ Asr(TMP, value, kSmiTagSize);  // Copy and untag.
      __ scvtfd(result, TMP);
      __ Bind(&done);
    }
  }
}


LocationSummary* BoxFloat32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BoxFloat32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* UnboxFloat32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void UnboxFloat32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BoxFloat64x2Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BoxFloat64x2Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* UnboxFloat64x2Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void UnboxFloat64x2Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BoxInt32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BoxInt32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* UnboxInt32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void UnboxInt32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BinaryDoubleOpInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_in(1, Location::RequiresFpuRegister());
  summary->set_out(0, Location::RequiresFpuRegister());
  return summary;
}


void BinaryDoubleOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const VRegister left = locs()->in(0).fpu_reg();
  const VRegister right = locs()->in(1).fpu_reg();
  const VRegister result = locs()->out(0).fpu_reg();
  switch (op_kind()) {
    case Token::kADD: __ faddd(result, left, right); break;
    case Token::kSUB: __ fsubd(result, left, right); break;
    case Token::kMUL: __ fmuld(result, left, right); break;
    case Token::kDIV: __ fdivd(result, left, right); break;
    default: UNREACHABLE();
  }
}


LocationSummary* BinaryFloat32x4OpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BinaryFloat32x4OpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BinaryFloat64x2OpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BinaryFloat64x2OpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Simd32x4ShuffleInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Simd32x4ShuffleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Simd32x4ShuffleMixInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Simd32x4ShuffleMixInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Simd32x4GetSignMaskInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Simd32x4GetSignMaskInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ConstructorInstr::MakeLocationSummary(
    bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ConstructorInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ZeroInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ZeroInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4SplatInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4SplatInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ComparisonInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ComparisonInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4MinMaxInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4MinMaxInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4SqrtInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4SqrtInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ScaleInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ScaleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ZeroArgInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ZeroArgInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ClampInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ClampInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4WithInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4WithInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ToInt32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ToInt32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Simd64x2ShuffleInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Simd64x2ShuffleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2ZeroInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2ZeroInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2SplatInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2SplatInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2ConstructorInstr::MakeLocationSummary(
    bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2ConstructorInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2ToFloat32x4Instr::MakeLocationSummary(
    bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2ToFloat32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float32x4ToFloat64x2Instr::MakeLocationSummary(
    bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float32x4ToFloat64x2Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2ZeroArgInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2ZeroArgInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Float64x2OneArgInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Float64x2OneArgInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Int32x4BoolConstructorInstr::MakeLocationSummary(
    bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Int32x4BoolConstructorInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Int32x4GetFlagInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Int32x4GetFlagInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Int32x4SelectInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Int32x4SelectInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Int32x4SetFlagInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Int32x4SetFlagInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* Int32x4ToFloat32x4Instr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void Int32x4ToFloat32x4Instr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BinaryInt32x4OpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BinaryInt32x4OpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* MathUnaryInstr::MakeLocationSummary(bool opt) const {
  if ((kind() == MathUnaryInstr::kSin) || (kind() == MathUnaryInstr::kCos)) {
    const intptr_t kNumInputs = 1;
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
    summary->set_in(0, Location::FpuRegisterLocation(V0));
    summary->set_out(0, Location::FpuRegisterLocation(V0));
    return summary;
  }
  ASSERT((kind() == MathUnaryInstr::kSqrt) ||
         (kind() == MathUnaryInstr::kDoubleSquare));
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_out(0, Location::RequiresFpuRegister());
  return summary;
}


void MathUnaryInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (kind() == MathUnaryInstr::kSqrt) {
    VRegister val = locs()->in(0).fpu_reg();
    VRegister result = locs()->out(0).fpu_reg();
    __ fsqrtd(result, val);
  } else if (kind() == MathUnaryInstr::kDoubleSquare) {
    VRegister val = locs()->in(0).fpu_reg();
    VRegister result = locs()->out(0).fpu_reg();
    __ fmuld(result, val, val);
  } else {
    ASSERT((kind() == MathUnaryInstr::kSin) ||
           (kind() == MathUnaryInstr::kCos));
    __ CallRuntime(TargetFunction(), InputCount());
  }
}


LocationSummary* MathMinMaxInstr::MakeLocationSummary(bool opt) const {
  if (result_cid() == kDoubleCid) {
    const intptr_t kNumInputs = 2;
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresFpuRegister());
    summary->set_in(1, Location::RequiresFpuRegister());
    // Reuse the left register so that code can be made shorter.
    summary->set_out(0, Location::SameAsFirstInput());
    return summary;
  }
  ASSERT(result_cid() == kSmiCid);
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, Location::RequiresRegister());
  // Reuse the left register so that code can be made shorter.
  summary->set_out(0, Location::SameAsFirstInput());
  return summary;
}


void MathMinMaxInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT((op_kind() == MethodRecognizer::kMathMin) ||
         (op_kind() == MethodRecognizer::kMathMax));
  const intptr_t is_min = (op_kind() == MethodRecognizer::kMathMin);
  if (result_cid() == kDoubleCid) {
    Label done, returns_nan, are_equal;
    const VRegister left = locs()->in(0).fpu_reg();
    const VRegister right = locs()->in(1).fpu_reg();
    const VRegister result = locs()->out(0).fpu_reg();
    __ fcmpd(left, right);
    __ b(&returns_nan, VS);
    __ b(&are_equal, EQ);
    const Condition double_condition =
        is_min ? TokenKindToDoubleCondition(Token::kLTE)
               : TokenKindToDoubleCondition(Token::kGTE);
    ASSERT(left == result);
    __ b(&done, double_condition);
    __ fmovdd(result, right);
    __ b(&done);

    __ Bind(&returns_nan);
    __ LoadDImmediate(result, NAN, PP);
    __ b(&done);

    __ Bind(&are_equal);
    // Check for negative zero: -0.0 is equal 0.0 but min or max must return
    // -0.0 or 0.0 respectively.
    // Check for negative left value (get the sign bit):
    // - min -> left is negative ? left : right.
    // - max -> left is negative ? right : left
    // Check the sign bit.
    __ fmovrd(TMP, left);  // Sign bit is in bit 63 of TMP.
    __ CompareImmediate(TMP, 0, PP);
    if (is_min) {
      ASSERT(left == result);
      __ b(&done, LT);
      __ fmovdd(result, right);
    } else {
      __ b(&done, GE);
      __ fmovdd(result, right);
      ASSERT(left == result);
    }
    __ Bind(&done);
    return;
  }

  ASSERT(result_cid() == kSmiCid);
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();
  Register result = locs()->out(0).reg();
  __ CompareRegisters(left, right);
  ASSERT(result == left);
  if (is_min) {
    __ csel(result, right, left, GT);
  } else {
    __ csel(result, right, left, LT);
  }
}


LocationSummary* UnarySmiOpInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  // We make use of 3-operand instructions by not requiring result register
  // to be identical to first input register as on Intel.
  summary->set_out(0, Location::RequiresRegister());
  return summary;
}


void UnarySmiOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out(0).reg();
  switch (op_kind()) {
    case Token::kNEGATE: {
      Label* deopt = compiler->AddDeoptStub(deopt_id(), ICData::kDeoptUnaryOp);
      __ subs(result, ZR, Operand(value));
      __ b(deopt, VS);
      if (FLAG_throw_on_javascript_int_overflow) {
        EmitJavascriptOverflowCheck(compiler, range(), deopt, value);
      }
      break;
    }
    case Token::kBIT_NOT:
      __ mvn(result, value);
      // Remove inverted smi-tag.
      __ andi(result, result, ~kSmiTagMask);
      break;
    default:
      UNREACHABLE();
  }
}


LocationSummary* UnaryDoubleOpInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_out(0, Location::RequiresFpuRegister());
  return summary;
}


void UnaryDoubleOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  VRegister result = locs()->out(0).fpu_reg();
  VRegister value = locs()->in(0).fpu_reg();
  __ fnegd(result, value);
}


LocationSummary* SmiToDoubleInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::WritableRegister());
  result->set_out(0, Location::RequiresFpuRegister());
  return result;
}


void SmiToDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  VRegister result = locs()->out(0).fpu_reg();
  __ SmiUntag(value);
  __ scvtfd(result, value);
}


LocationSummary* DoubleToIntegerInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  result->set_in(0, Location::RegisterLocation(R1));
  result->set_out(0, Location::RegisterLocation(R0));
  return result;
}


void DoubleToIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Register result = locs()->out(0).reg();
  const Register value_obj = locs()->in(0).reg();
  ASSERT(result == R0);
  ASSERT(result != value_obj);
  __ LoadDFieldFromOffset(VTMP, value_obj, Double::value_offset(), PP);

  Label do_call, done;
  // First check for NaN. Checking for minint after the conversion doesn't work
  // on ARM64 because fcvtzds gives 0 for NaN.
  __ fcmpd(VTMP, VTMP);
  __ b(&do_call, VS);

  __ fcvtzds(result, VTMP);
  // Overflow is signaled with minint.

  // Check for overflow and that it fits into Smi.
  __ CompareImmediate(result, 0xC000000000000000, PP);
  __ b(&do_call, MI);
  __ SmiTag(result);
  if (FLAG_throw_on_javascript_int_overflow) {
    EmitJavascriptOverflowCheck(compiler, range(), &do_call, result);
  }
  __ b(&done);
  __ Bind(&do_call);
  __ Push(value_obj);
  ASSERT(instance_call()->HasICData());
  const ICData& ic_data = *instance_call()->ic_data();
  ASSERT((ic_data.NumberOfChecks() == 1));
  const Function& target = Function::ZoneHandle(ic_data.GetTargetAt(0));

  const intptr_t kNumberOfArguments = 1;
  compiler->GenerateStaticCall(deopt_id(),
                               instance_call()->token_pos(),
                               target,
                               kNumberOfArguments,
                               Object::null_array(),  // No argument names.,
                               locs());
  __ Bind(&done);
}


LocationSummary* DoubleToSmiInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result = new LocationSummary(
      kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::RequiresFpuRegister());
  result->set_out(0, Location::RequiresRegister());
  return result;
}


void DoubleToSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(), ICData::kDeoptDoubleToSmi);
  const Register result = locs()->out(0).reg();
  const VRegister value = locs()->in(0).fpu_reg();
  // First check for NaN. Checking for minint after the conversion doesn't work
  // on ARM64 because fcvtzds gives 0 for NaN.
  // TODO(zra): Check spec that this is true.
  __ fcmpd(value, value);
  __ b(deopt, VS);

  __ fcvtzds(result, value);
  // Check for overflow and that it fits into Smi.
  __ CompareImmediate(result, 0xC000000000000000, PP);
  __ b(deopt, MI);
  __ SmiTag(result);
  if (FLAG_throw_on_javascript_int_overflow) {
    EmitJavascriptOverflowCheck(compiler, range(), deopt, result);
  }
}


LocationSummary* DoubleToDoubleInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void DoubleToDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* DoubleToFloatInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::RequiresFpuRegister());
  result->set_out(0, Location::RequiresFpuRegister());
  return result;
}


void DoubleToFloatInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const VRegister value = locs()->in(0).fpu_reg();
  const VRegister result = locs()->out(0).fpu_reg();
  __ fcvtsd(result, value);
}


LocationSummary* FloatToDoubleInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::RequiresFpuRegister());
  result->set_out(0, Location::RequiresFpuRegister());
  return result;
}


void FloatToDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const VRegister value = locs()->in(0).fpu_reg();
  const VRegister result = locs()->out(0).fpu_reg();
  __ fcvtds(result, value);
}


LocationSummary* InvokeMathCFunctionInstr::MakeLocationSummary(bool opt) const {
  ASSERT((InputCount() == 1) || (InputCount() == 2));
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(InputCount(), kNumTemps, LocationSummary::kCall);
  result->set_in(0, Location::FpuRegisterLocation(V0));
  if (InputCount() == 2) {
    result->set_in(1, Location::FpuRegisterLocation(V1));
  }
  if (recognized_kind() == MethodRecognizer::kMathDoublePow) {
    result->AddTemp(Location::FpuRegisterLocation(V30));
  }
  result->set_out(0, Location::FpuRegisterLocation(V0));
  return result;
}


// Pseudo code:
// if (exponent == 0.0) return 1.0;
// // Speed up simple cases.
// if (exponent == 1.0) return base;
// if (exponent == 2.0) return base * base;
// if (exponent == 3.0) return base * base * base;
// if (base == 1.0) return 1.0;
// if (base.isNaN || exponent.isNaN) {
//    return double.NAN;
// }
// if (base != -Infinity && exponent == 0.5) {
//   if (base == 0.0) return 0.0;
//   return sqrt(value);
// }
// TODO(srdjan): Move into a stub?
static void InvokeDoublePow(FlowGraphCompiler* compiler,
                            InvokeMathCFunctionInstr* instr) {
  ASSERT(instr->recognized_kind() == MethodRecognizer::kMathDoublePow);
  const intptr_t kInputCount = 2;
  ASSERT(instr->InputCount() == kInputCount);
  LocationSummary* locs = instr->locs();

  const VRegister base = locs->in(0).fpu_reg();
  const VRegister exp = locs->in(1).fpu_reg();
  const VRegister result = locs->out(0).fpu_reg();
  const VRegister saved_base = locs->temp(0).fpu_reg();
  ASSERT((base == result) && (result != saved_base));

  Label skip_call, try_sqrt, check_base, return_nan, do_pow;
  __ fmovdd(saved_base, base);
  __ LoadDImmediate(result, 1.0, PP);
  // exponent == 0.0 -> return 1.0;
  __ fcmpdz(exp);
  __ b(&check_base, VS);  // NaN -> check base.
  __ b(&skip_call, EQ);  // exp is 0.0, result is 1.0.

  // exponent == 1.0 ?
  __ fcmpd(exp, result);
  Label return_base;
  __ b(&return_base, EQ);

  // exponent == 2.0 ?
  __ LoadDImmediate(VTMP, 2.0, PP);
  __ fcmpd(exp, VTMP);
  Label return_base_times_2;
  __ b(&return_base_times_2, EQ);

  // exponent == 3.0 ?
  __ LoadDImmediate(VTMP, 3.0, PP);
  __ fcmpd(exp, VTMP);
  __ b(&check_base, NE);

  // base_times_3.
  __ fmuld(result, saved_base, saved_base);
  __ fmuld(result, result, saved_base);
  __ b(&skip_call);

  __ Bind(&return_base);
  __ fmovdd(result, saved_base);
  __ b(&skip_call);

  __ Bind(&return_base_times_2);
  __ fmuld(result, saved_base, saved_base);
  __ b(&skip_call);

  __ Bind(&check_base);
  // Note: 'exp' could be NaN.
  // base == 1.0 -> return 1.0;
  __ fcmpd(saved_base, result);
  __ b(&return_nan, VS);
  __ b(&skip_call, EQ);  // base is 1.0, result is 1.0.

  __ fcmpd(saved_base, exp);
  __ b(&try_sqrt, VC);  // // Neither 'exp' nor 'base' is NaN.

  __ Bind(&return_nan);
  __ LoadDImmediate(result, NAN, PP);
  __ b(&skip_call);

  Label return_zero;
  __ Bind(&try_sqrt);

  // Before calling pow, check if we could use sqrt instead of pow.
  __ LoadDImmediate(result, -INFINITY, PP);

  // base == -Infinity -> call pow;
  __ fcmpd(saved_base, result);
  __ b(&do_pow, EQ);

  // exponent == 0.5 ?
  __ LoadDImmediate(result, 0.5, PP);
  __ fcmpd(exp, result);
  __ b(&do_pow, NE);

  // base == 0 -> return 0;
  __ fcmpdz(saved_base);
  __ b(&return_zero, EQ);

  __ fsqrtd(result, saved_base);
  __ b(&skip_call);

  __ Bind(&return_zero);
  __ LoadDImmediate(result, 0.0, PP);
  __ b(&skip_call);

  __ Bind(&do_pow);
  __ fmovdd(base, saved_base);  // Restore base.

  __ CallRuntime(instr->TargetFunction(), kInputCount);
  __ Bind(&skip_call);
}


void InvokeMathCFunctionInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (recognized_kind() == MethodRecognizer::kMathDoublePow) {
    InvokeDoublePow(compiler, this);
    return;
  }
  __ CallRuntime(TargetFunction(), InputCount());
}


LocationSummary* ExtractNthOutputInstr::MakeLocationSummary(bool opt) const {
  // Only use this instruction in optimized code.
  ASSERT(opt);
  const intptr_t kNumInputs = 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, 0, LocationSummary::kNoCall);
  if (representation() == kUnboxedDouble) {
    if (index() == 0) {
      summary->set_in(0, Location::Pair(Location::RequiresFpuRegister(),
                                        Location::Any()));
    } else {
      ASSERT(index() == 1);
      summary->set_in(0, Location::Pair(Location::Any(),
                                        Location::RequiresFpuRegister()));
    }
    summary->set_out(0, Location::RequiresFpuRegister());
  } else {
    ASSERT(representation() == kTagged);
    if (index() == 0) {
      summary->set_in(0, Location::Pair(Location::RequiresRegister(),
                                        Location::Any()));
    } else {
      ASSERT(index() == 1);
      summary->set_in(0, Location::Pair(Location::Any(),
                                        Location::RequiresRegister()));
    }
    summary->set_out(0, Location::RequiresRegister());
  }
  return summary;
}


void ExtractNthOutputInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->in(0).IsPairLocation());
  PairLocation* pair = locs()->in(0).AsPairLocation();
  Location in_loc = pair->At(index());
  if (representation() == kUnboxedDouble) {
    VRegister out = locs()->out(0).fpu_reg();
    VRegister in = in_loc.fpu_reg();
    __ fmovdd(out, in);
  } else {
    ASSERT(representation() == kTagged);
    Register out = locs()->out(0).reg();
    Register in = in_loc.reg();
    __ mov(out, in);
  }
}


LocationSummary* MergedMathInstr::MakeLocationSummary(bool opt) const {
  if (kind() == MergedMathInstr::kTruncDivMod) {
    const intptr_t kNumInputs = 2;
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RequiresRegister());
    // Output is a pair of registers.
    summary->set_out(0, Location::Pair(Location::RequiresRegister(),
                                       Location::RequiresRegister()));
    return summary;
  }
  UNIMPLEMENTED();
  return NULL;
}


void MergedMathInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = NULL;
  if (CanDeoptimize()) {
    deopt = compiler->AddDeoptStub(deopt_id(), ICData::kDeoptBinarySmiOp);
  }
  if (kind() == MergedMathInstr::kTruncDivMod) {
    const Register left = locs()->in(0).reg();
    const Register right = locs()->in(1).reg();
    ASSERT(locs()->out(0).IsPairLocation());
    const PairLocation* pair = locs()->out(0).AsPairLocation();
    const Register result_div = pair->At(0).reg();
    const Register result_mod = pair->At(1).reg();
    const Range* right_range = InputAt(1)->definition()->range();
    if ((right_range == NULL) || right_range->Overlaps(0, 0)) {
      // Handle divide by zero in runtime.
      __ CompareRegisters(right, ZR);
      __ b(deopt, EQ);
    }

    __ Asr(result_mod, left, kSmiTagSize);  // SmiUntag left.
    __ Asr(TMP, right, kSmiTagSize);  // SmiUntag right.

    __ sdiv(result_div, result_mod, TMP);

    // Check the corner case of dividing the 'MIN_SMI' with -1, in which
    // case we cannot tag the result.
    __ CompareImmediate(result_div, 0x4000000000000000, PP);
    __ b(deopt, EQ);
    // result_mod <- left - right * result_div.
    __ msub(result_mod, TMP, result_div, result_mod);
    __ SmiTag(result_div);
    __ SmiTag(result_mod);
    // Correct MOD result:
    //  res = left % right;
    //  if (res < 0) {
    //    if (right < 0) {
    //      res = res - right;
    //    } else {
    //      res = res + right;
    //    }
    //  }
    Label done;
    __ CompareRegisters(result_mod, ZR);;
    __ b(&done, GE);
    // Result is negative, adjust it.
    __ CompareRegisters(right, ZR);
    __ sub(TMP2, result_mod, Operand(right));
    __ add(TMP, result_mod, Operand(right));
    __ csel(result_mod, TMP, TMP2, GE);
    __ Bind(&done);
    // FLAG_throw_on_javascript_int_overflow: not needed.
    // Note that the result of an integer division/modulo of two
    // in-range arguments, cannot create out-of-range result.
    return;
  }
  if (kind() == MergedMathInstr::kSinCos) {
    UNIMPLEMENTED();
  }
  UNIMPLEMENTED();
}


LocationSummary* PolymorphicInstanceCallInstr::MakeLocationSummary(
    bool opt) const {
  return MakeCallSummary();
}


void PolymorphicInstanceCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(
      deopt_id(), ICData::kDeoptPolymorphicInstanceCallTestFail);
  if (ic_data().NumberOfChecks() == 0) {
    __ b(deopt);
    return;
  }
  ASSERT(ic_data().NumArgsTested() == 1);
  if (!with_checks()) {
    ASSERT(ic_data().HasOneTarget());
    const Function& target = Function::ZoneHandle(ic_data().GetTargetAt(0));
    compiler->GenerateStaticCall(deopt_id(),
                                 instance_call()->token_pos(),
                                 target,
                                 instance_call()->ArgumentCount(),
                                 instance_call()->argument_names(),
                                 locs());
    return;
  }

  // Load receiver into R0.
  __ LoadFromOffset(
      R0, SP, (instance_call()->ArgumentCount() - 1) * kWordSize, PP);

  LoadValueCid(compiler, R2, R0,
               (ic_data().GetReceiverClassIdAt(0) == kSmiCid) ? NULL : deopt);

  compiler->EmitTestAndCall(ic_data(),
                            R2,  // Class id register.
                            instance_call()->ArgumentCount(),
                            instance_call()->argument_names(),
                            deopt,
                            deopt_id(),
                            instance_call()->token_pos(),
                            locs());
}


LocationSummary* BranchInstr::MakeLocationSummary(bool opt) const {
  comparison()->InitializeLocationSummary(opt);
  // Branches don't produce a result.
  comparison()->locs()->set_out(0, Location::NoLocation());
  return comparison()->locs();
}


void BranchInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  comparison()->EmitBranchCode(compiler, this);
}


LocationSummary* CheckClassInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  if (!IsNullCheck()) {
    summary->AddTemp(Location::RequiresRegister());
  }
  return summary;
}


void CheckClassInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const ICData::DeoptReasonId deopt_reason = licm_hoisted_ ?
      ICData::kDeoptHoistedCheckClass : ICData::kDeoptCheckClass;
  if (IsNullCheck()) {
    Label* deopt = compiler->AddDeoptStub(deopt_id(), deopt_reason);
    __ CompareObject(locs()->in(0).reg(), Object::null_object(), PP);
    __ b(deopt, EQ);
    return;
  }

  ASSERT((unary_checks().GetReceiverClassIdAt(0) != kSmiCid) ||
         (unary_checks().NumberOfChecks() > 1));
  Register value = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(deopt_id(), deopt_reason);
  Label is_ok;
  intptr_t cix = 0;
  if (unary_checks().GetReceiverClassIdAt(cix) == kSmiCid) {
    __ tsti(value, kSmiTagMask);
    __ b(&is_ok, EQ);
    cix++;  // Skip first check.
  } else {
    __ tsti(value, kSmiTagMask);
    __ b(deopt, EQ);
  }
  __ LoadClassId(temp, value, PP);
  const intptr_t num_checks = unary_checks().NumberOfChecks();
  for (intptr_t i = cix; i < num_checks; i++) {
    ASSERT(unary_checks().GetReceiverClassIdAt(i) != kSmiCid);
    __ CompareImmediate(temp, unary_checks().GetReceiverClassIdAt(i), PP);
    if (i == (num_checks - 1)) {
      __ b(deopt, NE);
    } else {
      __ b(&is_ok, EQ);
    }
  }
  __ Bind(&is_ok);
}


LocationSummary* CheckSmiInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  return summary;
}


void CheckSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Label* deopt = compiler->AddDeoptStub(deopt_id(), ICData::kDeoptCheckSmi);
  __ tsti(value, kSmiTagMask);
  __ b(deopt, NE);
}


LocationSummary* CheckArrayBoundInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(kLengthPos, Location::RegisterOrSmiConstant(length()));
  locs->set_in(kIndexPos, Location::RegisterOrSmiConstant(index()));
  return locs;
}


void CheckArrayBoundInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                        ICData::kDeoptCheckArrayBound);

  Location length_loc = locs()->in(kLengthPos);
  Location index_loc = locs()->in(kIndexPos);

  if (length_loc.IsConstant() && index_loc.IsConstant()) {
    // TODO(srdjan): remove this code once failures are fixed.
    if ((Smi::Cast(length_loc.constant()).Value() >
         Smi::Cast(index_loc.constant()).Value()) &&
        (Smi::Cast(index_loc.constant()).Value() >= 0)) {
      // This CheckArrayBoundInstr should have been eliminated.
      return;
    }
    ASSERT((Smi::Cast(length_loc.constant()).Value() <=
            Smi::Cast(index_loc.constant()).Value()) ||
           (Smi::Cast(index_loc.constant()).Value() < 0));
    // Unconditionally deoptimize for constant bounds checks because they
    // only occur only when index is out-of-bounds.
    __ b(deopt);
    return;
  }

  if (index_loc.IsConstant()) {
    Register length = length_loc.reg();
    const Smi& index = Smi::Cast(index_loc.constant());
    __ CompareImmediate(length, reinterpret_cast<int64_t>(index.raw()), PP);
    __ b(deopt, LS);
  } else if (length_loc.IsConstant()) {
    const Smi& length = Smi::Cast(length_loc.constant());
    Register index = index_loc.reg();
    __ CompareImmediate(index, reinterpret_cast<int64_t>(length.raw()), PP);
    __ b(deopt, CS);
  } else {
    Register length = length_loc.reg();
    Register index = index_loc.reg();
    __ CompareRegisters(index, length);
    __ b(deopt, CS);
  }
}


LocationSummary* UnboxIntegerInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void UnboxIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BoxIntegerInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BoxIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* BinaryMintOpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void BinaryMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* ShiftMintOpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void ShiftMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* UnaryMintOpInstr::MakeLocationSummary(bool opt) const {
  UNIMPLEMENTED();
  return NULL;
}


void UnaryMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  UNIMPLEMENTED();
}


LocationSummary* ThrowInstr::MakeLocationSummary(bool opt) const {
  return new LocationSummary(0, 0, LocationSummary::kCall);
}


void ThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->GenerateRuntimeCall(token_pos(),
                                deopt_id(),
                                kThrowRuntimeEntry,
                                1,
                                locs());
  __ hlt(0);
}


LocationSummary* ReThrowInstr::MakeLocationSummary(bool opt) const {
  return new LocationSummary(0, 0, LocationSummary::kCall);
}


void ReThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->SetNeedsStacktrace(catch_try_index());
  compiler->GenerateRuntimeCall(token_pos(),
                                deopt_id(),
                                kReThrowRuntimeEntry,
                                2,
                                locs());
  __ hlt(0);
}


void GraphEntryInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (!compiler->CanFallThroughTo(normal_entry())) {
    __ b(compiler->GetJumpLabel(normal_entry()));
  }
}


void TargetEntryInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ Bind(compiler->GetJumpLabel(this));
  if (!compiler->is_optimizing()) {
    compiler->EmitEdgeCounter();
    // Add an edge counter.
    // On ARM64 the deoptimization descriptor points after the edge counter
    // code so that we can reuse the same pattern matching code as at call
    // sites, which matches backwards from the end of the pattern.
    compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                   deopt_id_,
                                   Scanner::kNoSourcePos);
  }
  if (HasParallelMove()) {
    compiler->parallel_move_resolver()->EmitNativeCode(parallel_move());
  }
}


LocationSummary* GotoInstr::MakeLocationSummary(bool opt) const {
  return new LocationSummary(0, 0, LocationSummary::kNoCall);
}


void GotoInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (!compiler->is_optimizing()) {
    compiler->EmitEdgeCounter();
    // Add a deoptimization descriptor for deoptimizing instructions that
    // may be inserted before this instruction.  On ARM64 this descriptor
    // points after the edge counter code so that we can reuse the same
    // pattern matching code as at call sites, which matches backwards from
    // the end of the pattern.
    compiler->AddCurrentDescriptor(PcDescriptors::kDeopt,
                                   GetDeoptId(),
                                   Scanner::kNoSourcePos);
  }
  if (HasParallelMove()) {
    compiler->parallel_move_resolver()->EmitNativeCode(parallel_move());
  }

  // We can fall through if the successor is the next block in the list.
  // Otherwise, we need a jump.
  if (!compiler->CanFallThroughTo(successor())) {
    __ b(compiler->GetJumpLabel(successor()));
  }
}


LocationSummary* CurrentContextInstr::MakeLocationSummary(bool opt) const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void CurrentContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ mov(locs()->out(0).reg(), CTX);
}


LocationSummary* StrictCompareInstr::MakeLocationSummary(bool opt) const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  if (needs_number_check()) {
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
    locs->set_in(0, Location::RegisterLocation(R0));
    locs->set_in(1, Location::RegisterLocation(R1));
    locs->set_out(0, Location::RegisterLocation(R0));
    return locs;
  }
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RegisterOrConstant(left()));
  // Only one of the inputs can be a constant. Choose register if the first one
  // is a constant.
  locs->set_in(1, locs->in(0).IsConstant()
                      ? Location::RequiresRegister()
                      : Location::RegisterOrConstant(right()));
  locs->set_out(0, Location::RequiresRegister());
  return locs;
}


Condition StrictCompareInstr::EmitComparisonCode(FlowGraphCompiler* compiler,
                                                 BranchLabels labels) {
  Location left = locs()->in(0);
  Location right = locs()->in(1);
  ASSERT(!left.IsConstant() || !right.IsConstant());
  if (left.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(right.reg(),
                                          left.constant(),
                                          needs_number_check(),
                                          token_pos());
  } else if (right.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(left.reg(),
                                          right.constant(),
                                          needs_number_check(),
                                          token_pos());
  } else {
    compiler->EmitEqualityRegRegCompare(left.reg(),
                                       right.reg(),
                                       needs_number_check(),
                                       token_pos());
  }
  Condition true_condition = (kind() == Token::kEQ_STRICT) ? EQ : NE;
  return true_condition;
}


void StrictCompareInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ Comment("StrictCompareInstr");
  ASSERT(kind() == Token::kEQ_STRICT || kind() == Token::kNE_STRICT);

  Label is_true, is_false;
  BranchLabels labels = { &is_true, &is_false, &is_false };
  Condition true_condition = EmitComparisonCode(compiler, labels);
  EmitBranchOnCondition(compiler, true_condition, labels);

  Register result = locs()->out(0).reg();
  Label done;
  __ Bind(&is_false);
  __ LoadObject(result, Bool::False(), PP);
  __ b(&done);
  __ Bind(&is_true);
  __ LoadObject(result, Bool::True(), PP);
  __ Bind(&done);
}


void StrictCompareInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                        BranchInstr* branch) {
  ASSERT(kind() == Token::kEQ_STRICT || kind() == Token::kNE_STRICT);

  BranchLabels labels = compiler->CreateBranchLabels(branch);
  Condition true_condition = EmitComparisonCode(compiler, labels);
  EmitBranchOnCondition(compiler, true_condition, labels);
}


LocationSummary* BooleanNegateInstr::MakeLocationSummary(bool opt) const {
  return LocationSummary::Make(1,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void BooleanNegateInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out(0).reg();

  __ LoadObject(result, Bool::True(), PP);
  __ LoadObject(TMP, Bool::False(), PP);
  __ CompareRegisters(result, value);
  __ csel(result, TMP, result, EQ);
}


LocationSummary* AllocateObjectInstr::MakeLocationSummary(bool opt) const {
  return MakeCallSummary();
}


void AllocateObjectInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Code& stub = Code::Handle(StubCode::GetAllocationStubForClass(cls()));
  const ExternalLabel label(cls().ToCString(), stub.EntryPoint());
  compiler->GenerateCall(token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
  __ Drop(ArgumentCount());  // Discard arguments.
}

}  // namespace dart

#endif  // defined TARGET_ARCH_ARM64
