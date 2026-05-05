#include "pass.h"
#include "wasm.h"
#include "wasm-builder.h"
#include "ir/find_all.h"

namespace wasm {

struct InjectYields : public Pass {
  bool isFunctionParallel() override { return false; } 

  void run(Module* module) override {
    Builder builder(*module);

    // 1. Ensure the global yield flag exists
    if (!module->getGlobalOrNull("HOST_YIELD_REQUEST")) {
      auto global = builder.makeGlobal(
          "HOST_YIELD_REQUEST", Type::i32, builder.makeConst(int32_t(0)), Builder::Mutable
      );
      module->addGlobal(std::move(global));
    }

    // 2. Automatically create the Isolated Asyncify Memory (1KB footprint!)
    Name asyncMemName = "yield_async_mem";
    if (!module->getMemoryOrNull(asyncMemName)) {
      auto mem = std::make_unique<Memory>();
      mem->name = asyncMemName;
      mem->addressType = Type::i32; // Force 32-bit pointers for dataPtr=0
      
      // EXPLICITLY ENABLE CUSTOM PAGE SIZES
      module->features.setCustomPageSizes();
      
      // Set the page size to 1 byte (2^0)
      mem->pageSizeLog2 = 0; 
      
      // Allocate exactly 1,024 pages (1,024 * 1 byte = 1 KB)
      mem->initial = 1024;
      mem->max = 1024;     

      module->addMemory(std::move(mem));
      
      auto exp = std::make_unique<Export>();
      exp->name = asyncMemName;
      exp->value = asyncMemName;
      exp->kind = ExternalKind::Memory;
      module->addExport(std::move(exp));
    }

    // Explicitly enable Multi-Memory feature on the Wasm module
    module->features.setMultiMemory();

    // 3. Generate the $handle_yield helper
    generateHandlerFunction(module, builder);

    // 4. Walk functions and inject the polling checks
    for (auto& func : module->functions) {
      if (func->imported() || func->name == "handle_yield" || func->module == "asyncify") {
        continue;
      }

      func->body = builder.makeSequence(makeCheck(builder), func->body);

      FindAll<Loop> loops(func->body);
      for (Loop* loop : loops.list) {
        loop->body = builder.makeSequence(makeCheck(builder), loop->body);
      }
    }
  }

private:
  Expression* makeCheck(Builder& builder) {
    return builder.makeIf(
        builder.makeGlobalGet("HOST_YIELD_REQUEST", Type::i32),
        builder.makeCall("handle_yield", {}, Type::none)
    );
  }

  void generateHandlerFunction(Module* module, Builder& builder) {
    if (module->getFunctionOrNull("handle_yield")) return;

    // Helper to create the proper WebAssembly imports
    auto ensureImport = [&](Name mod, Name base, Name internalName, Signature sig) {
      if (!module->getFunctionOrNull(internalName)) {
        // Omitting the body argument makes it an import in Binaryen
        auto func = builder.makeFunction(internalName, sig, {});
        func->module = mod;
        func->base = base;
        module->addFunction(std::move(func));
      }
    };

    // 1. Create the imports using the exact "asyncify.fun_name" syntax for internal names!
    ensureImport("asyncify", "get_state",    "asyncify.get_state",    Signature(Type::none, Type::i32));
    ensureImport("asyncify", "stop_rewind",  "asyncify.stop_rewind",  Signature(Type::none, Type::none));
    ensureImport("asyncify", "start_unwind", "asyncify.start_unwind", Signature(Type::i32, Type::none));

    // 2. Call those imports using the dot-syntax names
    auto* isRewinding = builder.makeBinary(EqInt32,
        builder.makeCall("asyncify.get_state", {}, Type::i32),
        builder.makeConst(int32_t(2))
    );

    auto* stopRewind = builder.makeCall("asyncify.stop_rewind", {}, Type::none);

    // dataPtr is ALWAYS exactly 0!
    auto* triggerUnwind = builder.makeBlock({
        builder.makeGlobalSet("HOST_YIELD_REQUEST", builder.makeConst(int32_t(0))),
        builder.makeCall("asyncify.start_unwind", { builder.makeConst(int32_t(0)) }, Type::none)
    });

    auto* body = builder.makeIf(isRewinding, stopRewind, triggerUnwind);

    auto func = builder.makeFunction("handle_yield", Signature(Type::none, Type::none), {}, body);
    module->addFunction(std::move(func));
  }
};

Pass* createInjectYieldsPass() { return new InjectYields(); }

} // namespace wasm
