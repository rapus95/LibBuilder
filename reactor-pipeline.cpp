// my-repo/reactor-pipeline.cpp
#include <stdlib.h>
#include <string>
#include <sstream>
#include <cstring>
#include <binaryen-c.h>
#include "wasm.h"
#include "pass.h"

extern "C" {
    // Fix: Changed 'unsigned int' to 'size_t' and added 'noexcept' to match WASI-SDK's libc++ headers
    void* __cxa_allocate_exception(size_t thrown_size) noexcept { __builtin_trap(); return (void*)0; }
    void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *)) { __builtin_trap(); }
    void* __cxa_begin_catch(void* exceptionObject) noexcept { __builtin_trap(); return (void*)0; }
    void __cxa_end_catch() { __builtin_trap(); }
    int __gxx_personality_v0(...) { __builtin_trap(); return 0; }

    struct OptimizeResult {
        void* ptr;
        size_t len;
    };

    __attribute__((export_name("host_malloc")))
    void* host_malloc(size_t size) { return malloc(size); }

    __attribute__((export_name("host_free")))
    void host_free(void* ptr) { free(ptr); }

    // i32 0 = WASM output, i32 1 = WAT output
    __attribute__((export_name("run_pipeline")))
    struct OptimizeResult* run_pipeline(void* input_ptr, size_t input_len, int output_wat) {
        
        BinaryenModuleRef module_ref;
        char* input = (char*)input_ptr;

        // Auto-Detect WASM vs WAT via magic header (\0asm)
        if (input_len >= 4 && input[0] == '\0' && input[1] == 'a' && input[2] == 's' && input[3] == 'm') {
            module_ref = BinaryenModuleRead(input, input_len);
        } else {
            // It's WAT! Binaryen C-API expects a null-terminated C-string for WAT parsing.
            char* wat_str = (char*)malloc(input_len + 1);
            memcpy(wat_str, input, input_len);
            wat_str[input_len] = '\0';
            module_ref = BinaryenModuleParse(wat_str);
            free(wat_str);
        }

        wasm::Module* module = (wasm::Module*)module_ref;

        // 2. Equivalent to --enable-custom-page-sizes
        module->features.enable(wasm::FeatureSet::CustomPageSizes);

        // 3. Equivalent to --pass-arg=...
        wasm::PassOptions options;
        options.arguments["asyncify-ignore-imports"] = "";
        options.arguments["asyncify-memory"] = "yield_async_mem";
        
        // 4. Setup and run PassRunner
        wasm::PassRunner runner(module, options);
        runner.add("inject-yields"); 
        runner.add("asyncify");
        runner.run();

        struct OptimizeResult* out_res = (struct OptimizeResult*)malloc(sizeof(struct OptimizeResult));

        // 5. Generate Output
        if (output_wat != 0) {
            std::ostringstream ss;
            ss << *module;
            std::string wat = ss.str();
            
            out_res->len = wat.length();
            out_res->ptr = malloc(out_res->len);
            memcpy(out_res->ptr, wat.data(), out_res->len);
        } else {
            // Equivalent to writing standard WASM format
            struct BinaryenModuleAllocateAndWriteResult res = BinaryenModuleAllocateAndWrite(module_ref, NULL);
            out_res->len = res.binaryBytes;
            out_res->ptr = res.binary; 
            if (res.sourceMap) free(res.sourceMap);
        }

        BinaryenModuleDispose(module_ref);
        return out_res;
    }
}
