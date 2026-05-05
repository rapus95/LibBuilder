#include "pass.h"
#include "wasm.h"
#include "wasm-builder.h"
#include "ir/find_all.h" // Binaryen's magic AST finder

namespace wasm {

struct InjectYields : public Pass {
  // This tells Binaryen the pass doesn't mutate globals in a way that breaks parallelization
  bool isFunctionParallel() override { return false; } 

  void run(Module* module) override {
    // A Builder is Binaryen's tool for creating new AST nodes (Expressions)
    Builder builder(*module);

    // 1. Ensure the global flag exists
    if (!module->getGlobalOrNull("HOST_YIELD_REQUEST")) {
      auto global = builder.makeGlobal(
          "HOST_YIELD_REQUEST",
          Type::i32,
          builder.makeConst(int32_t(0)),
          Builder::Mutable
      );
      module->addGlobal(std::move(global));
    }

    // 2. Generate the $handle_yield helper function
    generateHandlerFunction(module, builder);

    // 3. Walk all functions and inject the polling check
    for (auto& func : module->functions) {
      // Don't instrument our own handler, imported functions, or Asyncify internals
      if (func->imported() || func->name == "handle_yield" || func->module == "asyncify") {
        continue;
      }

      // Inject at the very beginning of the function (Prologue)
      func->body = builder.makeSequence(makeCheck(builder), func->body);

      // Find all Loop blocks in this function and inject at the top of the loop
      FindAll<Loop> loops(func->body);
      for (Loop* loop : loops.list) {
        loop->body = builder.makeSequence(makeCheck(builder), loop->body);
      }
    }
  }

private:
  // Creates the AST: if (HOST_YIELD_REQUEST) call handle_yield()
  Expression* makeCheck(Builder& builder) {
    return builder.makeIf(
        builder.makeGlobalGet("HOST_YIELD_REQUEST", Type::i32),
        builder.makeCall("handle_yield", {}, Type::none)
    );
  }

  // Generates our flawless Rewind/Unwind router
  void generateHandlerFunction(Module* module, Builder& builder) {
    if (module->getFunctionOrNull("handle_yield")) return;

    // if (asyncify_get_state() == 2)
    auto* isRewinding = builder.makeBinary(EqInt32,
        builder.makeCall("asyncify_get_state", {}, Type::i32),
        builder.makeConst(int32_t(2))
    );

    // then: asyncify_stop_rewind()
    auto* stopRewind = builder.makeCall("asyncify_stop_rewind", {}, Type::none);

    // else: HOST_YIELD_REQUEST = 0; asyncify_start_unwind(16);
    auto* triggerUnwind = builder.makeBlock({
        builder.makeGlobalSet("HOST_YIELD_REQUEST", builder.makeConst(int32_t(0))),
        // Note: 16 is the memory address of the Asyncify Data Struct. Adjust as needed!
        builder.makeCall("asyncify_start_unwind", { builder.makeConst(int32_t(16)) }, Type::none)
    });

    auto* body = builder.makeIf(isRewinding, stopRewind, triggerUnwind);

    // Create and add the function
    auto func = builder.makeFunction("handle_yield", Signature(Type::none, Type::none), {}, body);
    module->addFunction(std::move(func));
  }
};

// This registers the pass with Binaryen's CLI tools
Pass* createInjectYieldsPass() { return new InjectYields(); }

} // namespace wasm
