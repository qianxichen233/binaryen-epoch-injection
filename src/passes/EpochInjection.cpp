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
// Instruments the build with code to log execution at each function
// entry, loop header, and return. This can be useful in debugging, to log out
// a trace, and diff it to another (running in another browser, to
// check for bugs, for example).
//
// The logging is performed by calling an ffi with an id for each
// call site. You need to provide that import on the JS side.
//
// This pass is more effective on flat IR (--flatten) since when it
// instruments say a return, there will be no code run in the return's
// value.
//

#include "asmjs/shared-constants.h"
#include "shared-constants.h"
#include <pass.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

Name EPOCH_CALLBACK("epoch_callback");
Name SIGNAL_CALLBACK("signal_callback");
Name EPOCH("epoch");

Name LIND_NAMESPACE("lind");

struct EpochInjection : public WalkerPass<PostWalker<EpochInjection>> {
  // The module name the epoch function is imported from.
  IString epochModule;
  bool importedEpoch;
  bool isMainModule;

  // Adds calls to new imports.
  bool addsEffects() override { return true; }

  void run(Module* module) override {
    epochModule = getArgumentOrDefault("epoch_callback", "");
    importedEpoch = hasArgument("epoch-import");
    isMainModule = hasArgument("epoch-main-module");
    Super::run(module);
  }

  void visitLoop(Loop* curr) { curr->body = injectEpoch(curr->body); }

  void visitFunction(Function* curr) {
    if (curr->imported()) {
      return;
    }
    if (auto* block = curr->body->dynCast<Block>()) {
      if (!block->list.empty()) {
        block->list.back() = injectEpoch(block->list.back());
      }
    }
    curr->body = injectEpoch(curr->body);
  }

  void visitModule(Module* curr) {
    Builder builder(*getModule());

    // Add the import
    auto import =
      Builder::makeFunction(
        EPOCH_CALLBACK, Type(Signature(Type::none, Type::none), NonNullable, Inexact), {});

    auto epoch = Builder::makeGlobal(EPOCH,
                                            Type::i64,
                                            builder.makeConst(int64_t(0)),
                                            Builder::Mutable);
    if (importedEpoch) {
      epoch->module = LIND_NAMESPACE;
      epoch->base = EPOCH;
    }

    if (isMainModule) {
      auto signal_callback =
        Builder::makeFunction(
          SIGNAL_CALLBACK, Type(Signature(Type({Type::i32, Type::i32}), Type::none), NonNullable, Inexact), {});
      signal_callback->module = ENV;
      signal_callback->base = SIGNAL_CALLBACK;
      curr->addFunction(std::move(signal_callback));
      curr->addExport(builder.makeExport(SIGNAL_CALLBACK, SIGNAL_CALLBACK, ExternalKind::Function));
    }

    import->module = LIND_NAMESPACE;
    import->base = EPOCH_CALLBACK;
    curr->addFunction(std::move(import));
    curr->addGlobal(std::move(epoch));
    if (!importedEpoch) {
      curr->addExport(builder.makeExport(EPOCH, EPOCH, ExternalKind::Global));
    }
  }

private:
  Expression* injectEpoch(Expression* curr) {
    Builder builder(*getModule());
    return builder.makeSequence(
      builder.makeIf(
          builder.makeBinary(
            GtUInt64, builder.makeGlobalGet(EPOCH, Type::i64), builder.makeConst(Literal((int64_t)0))),
          builder.makeCall(EPOCH_CALLBACK, {}, Type::none)),
      curr);
  }
};

Pass* createEpochInjectionPass() { return new EpochInjection(); }

} // namespace wasm
