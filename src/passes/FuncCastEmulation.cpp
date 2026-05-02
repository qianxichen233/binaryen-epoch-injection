/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Instruments all indirect calls so that they work even if a function
// pointer was cast incorrectly. For example, if you cast an int (int, float)
// to an int (int, float, int) and call it natively, on most archs it will
// happen to work, ignoring the extra param, whereas in wasm it will trap.
// When porting code that relies on such casts working (like e.g. Python),
// this pass may be useful. It sets a new "ABI" for indirect calls, in which
// they all return an i64 and they have a fixed number of i64 params, and
// the pass converts everything to go through that.
//
// This should work even with dynamic linking, however, the number of
// params must be identical, i.e., the "ABI" must match.
//

#include <iostream>
#include <string>

#include <ir/element-utils.h>
#include <ir/literal-utils.h>
#include <pass.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

// Converts a value to the ABI type of i64.
static Expression* toABI(Expression* value, Module* module) {
  Builder builder(*module);
  switch (value->type.getBasic()) {
    case Type::i32: {
      value = builder.makeUnary(ExtendUInt32, value);
      break;
    }
    case Type::i64: {
      // already good
      break;
    }
    case Type::f32: {
      value = builder.makeUnary(ExtendUInt32,
                                builder.makeUnary(ReinterpretFloat32, value));
      break;
    }
    case Type::f64: {
      value = builder.makeUnary(ReinterpretFloat64, value);
      break;
    }
    case Type::v128: {
      WASM_UNREACHABLE("v128 not implemented yet");
    }
    case Type::none: {
      // the value is none, but we need a value here
      value =
        builder.makeSequence(value, LiteralUtils::makeZero(Type::i64, *module));
      break;
    }
    case Type::unreachable: {
      // can leave it, the call isn't taken anyhow
      break;
    }
  }
  return value;
}

// Converts a value from the ABI type of i64 to the expected type
static Expression* fromABI(Expression* value, Type type, Module* module) {
  Builder builder(*module);
  switch (type.getBasic()) {
    case Type::i32: {
      value = builder.makeUnary(WrapInt64, value);
      break;
    }
    case Type::i64: {
      // already good
      break;
    }
    case Type::f32: {
      value = builder.makeUnary(ReinterpretInt32,
                                builder.makeUnary(WrapInt64, value));
      break;
    }
    case Type::f64: {
      value = builder.makeUnary(ReinterpretInt64, value);
      break;
    }
    case Type::v128: {
      WASM_UNREACHABLE("v128 not implemented yet");
    }
    case Type::none: {
      value = builder.makeDrop(value);
      break;
    }
    case Type::unreachable: {
      // can leave it, the call isn't taken anyhow
      break;
    }
  }
  return value;
}

struct ParallelFuncCastEmulation
  : public WalkerPass<PostWalker<ParallelFuncCastEmulation>> {
  bool isFunctionParallel() override { return true; }

  std::unique_ptr<Pass> create() override {
    return std::make_unique<ParallelFuncCastEmulation>(ABIType, numParams);
  }

  ParallelFuncCastEmulation(HeapType ABIType, Index numParams)
    : ABIType(ABIType), numParams(numParams) {}

  void visitCallIndirect(CallIndirect* curr) {
    if (curr->operands.size() > numParams) {
      Fatal() << "max-func-params needs to be at least "
              << curr->operands.size();
    }
    for (Expression*& operand : curr->operands) {
      operand = toABI(operand, getModule());
    }
    // Add extra operands as needed.
    while (curr->operands.size() < numParams) {
      curr->operands.push_back(LiteralUtils::makeZero(Type::i64, *getModule()));
    }
    // Set the new types
    curr->heapType = ABIType;
    auto oldType = curr->type;
    curr->type = Type::i64;
    curr->finalize(); // may be unreachable
    // Fix up return value
    replaceCurrent(fromABI(curr, oldType, getModule()));
  }

private:
  // The signature of a call with the right params and return
  HeapType ABIType;
  Index numParams;
};

struct FuncCastEmulation : public Pass {
  // Changes the ABI, which alters which indirect calls will trap (normally,
  // this should prevent traps, but it depends on code this is linked to at
  // runtime in the case of dynamic linking etc.).
  bool addsEffects() override { return true; }

  void run(Module* module) override {
    bool need_relocate = hasArgument("relocatable-fpcast");

    Index numParams = std::stoul(getArgumentOrDefault("max-func-params", "18"));
    // we just need the one ABI function type for all indirect calls
    HeapType ABIType(
      Signature(Type(std::vector<Type>(numParams, Type::i64)), Type::i64));
    // Add a thunk for each function in the table, and do the call through it.
    std::unordered_map<Name, Function*> funcThunks;

    auto getOrCreateThunk = [&](Name name) -> Function* {
      auto [iter, inserted] = funcThunks.insert({name, nullptr});
      if (inserted) {
        iter->second = makeThunk(name, module, numParams);
      }
      return iter->second;
    };

    for (auto& segment : module->elementSegments) {
      if (!segment->type.isFunction()) {
        continue;
      }
      for (Index i = 0; i < segment->data.size(); ++i) {
        auto* ref = segment->data[i]->dynCast<RefFunc>();
        if (!ref) {
          continue;
        }
        
        auto* thunk = getOrCreateThunk(ref->func);
        if (!thunk) {
          continue;
        }
        ref->func = thunk->name;
        ref->finalize(*module);
      }
    }

    if (need_relocate) {
      struct PendingExport {
        Name externalName;
        Name internalName;
      };
      std::vector<PendingExport> newExports;

      // add thunk for each function in export section
      for (auto& exp : module->exports) {
        if (exp->kind != ExternalKind::Function) {
          continue;
        }
        
        Name* internal = exp->getInternalName();
        if(!internal) {
          continue;
        }

        auto* thunk = getOrCreateThunk(*internal);
        if (!thunk) {
          continue;
        }

        Name thunkExportName = "$fpcast_emu$" + std::string(exp->name.str);
        
        // Avoid duplicate exports if one already exists.
        if (!module->getExportOrNull(thunkExportName)) {
          newExports.push_back(PendingExport {thunkExportName, thunk->name});
        }
      }

      // append the new thunk function into export section
      for (auto& info : newExports) {
        auto exportPtr = std::make_unique<Export>(
          info.externalName,
          ExternalKind::Function,
          info.internalName
        );
        
        module->addExport(std::move(exportPtr));
      }
    }

    // update call_indirects
    ParallelFuncCastEmulation(ABIType, numParams).run(getPassRunner(), module);
  }

private:
  // Creates a thunk for a function, casting args and return value as needed.
  // Returns nullptr if the function has more params than numParams (cannot
  // build a valid thunk without referencing non-existent local indices).
  Function* makeThunk(Name name, Module* module, Index numParams) {
    Name thunk = std::string("byn$fpcast-emu$") + name.toString();
    if (module->getFunctionOrNull(thunk)) {
      Fatal() << "FuncCastEmulation::makeThunk seems a thunk name already in "
                 "use. Was the pass already run on this code?";
    }
    // The item in the table may be a function or a function import.
    auto* func = module->getFunction(name);
    // A thunk has exactly numParams i64 locals. If the original function has
    // more parameters, local indices in the thunk body would exceed the
    // function's local count, producing invalid IR and heap corruption in
    // downstream passes. Skip thunk creation for such functions.
    if (func->getParams().size() > numParams) {
      std::cerr << "warning: FuncCastEmulation: skipping thunk for '"
                << name.toString() << "' (" << func->getParams().size()
                << " params exceeds max-func-params=" << numParams << ")\n";
      return nullptr;
    }
    Type type = func->getResults();
    Builder builder(*module);
    std::vector<Expression*> callOperands;
    Index i = 0;
    for (const auto& param : func->getParams()) {
      callOperands.push_back(
        fromABI(builder.makeLocalGet(i++, Type::i64), param, module));
    }
    auto* call = builder.makeCall(name, callOperands, type);
    std::vector<Type> thunkParams;
    for (Index i = 0; i < numParams; i++) {
      thunkParams.push_back(Type::i64);
    }
    auto thunkFunc = builder.makeFunction(
      thunk,
      Type(Signature(Type(thunkParams), Type::i64), NonNullable, Exact),
      {}, // no vars
      toABI(call, module));
    thunkFunc->hasExplicitName = true;
    return module->addFunction(std::move(thunkFunc));
  }
};

Pass* createFuncCastEmulationPass() { return new FuncCastEmulation(); }

} // namespace wasm
