#ifndef FFI_H
#define FFI_H

#include "compiler.h" // Gets HDValue from here

// Define the function pointer type
typedef HDValue (*NativeFn)(int arg_count, HDValue* args);

// Expose a function that compiler.c can call to find a native function
NativeFn ffi_lookup_native(const char* name, int len);

#endif
