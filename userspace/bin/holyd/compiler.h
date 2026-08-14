#ifndef HOLYD_COMPILER_H
#define HOLYD_COMPILER_H

#include "ast/ast.h"
#include "eval.h"

typedef enum {
    BC_PUSH_INT,
    BC_PUSH_STRING,
    BC_LOAD,
    BC_DEFINE,
    BC_STORE,
    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_EQ,
    BC_NE,
    BC_LT,
    BC_GT,
    BC_LE,
    BC_GE,
    BC_CONCAT,
    BC_MAKE_ARRAY,
    BC_ARRAY_LEN,
    BC_ARRAY_GET,
    BC_JUMP,
    BC_JUMP_IF_FALSE,
    BC_CALL,
    BC_POP,
    BC_RETURN
} BytecodeOp;

typedef struct {
    BytecodeOp op;
    long long i64;
    const char* text;
    int text_len;
    int operand;
} Instruction;

typedef struct {
    Instruction* code;
    int count;
    int capacity;
} BytecodeChunk;

typedef struct {
    const char* name;
    int name_len;
    const char** param_names;
    int* param_name_lens;
    int param_count;
    BytecodeChunk chunk;
} HDFunction;

typedef struct {
    BytecodeChunk main;
    HDFunction* functions;
    int function_count;
    int function_capacity;
    int temp_counter;
    int had_error;
} HDProgram;

void HDProgramInit(HDProgram* program);
int HDCompileProgram(ASTNode* ast, HDProgram* program);
int HDRunProgram(HDProgram* program);
void HDDumpProgram(HDProgram* program);

#endif
