#ifndef HOLYD_EVAL_H
#define HOLYD_EVAL_H

#include "ast/ast.h"

typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_ARRAY
} ValueType;

// Forward declaration for arrays
typedef struct HDValue HDValue;

struct HDValue {
    ValueType type;
    long long i64;
    double f64;         // For VAL_FLOAT
    const char* str;
    int str_len;

    // For VAL_ARRAY
    HDValue* elements;  // Pointer to array of HDValues
    int array_len;
};

// A variable stored in memory
typedef struct EnvEntry {
    char* name;
    HDValue value;
    struct EnvEntry* next;
} EnvEntry;

typedef struct Environment {
    EnvEntry* head;
    struct Environment* parent;
} Environment;

void EnvInit(Environment* env);
void EnvInitChild(Environment* env, Environment* parent);
void EnvDefine(Environment* env, const char* name, size_t name_len, HDValue value);
void EnvSet(Environment* env, const char* name, size_t name_len, HDValue value);
HDValue* EnvGet(Environment* env, const char* name, size_t name_len);

HDValue EvalNode(ASTNode* node, Environment* env);

/* Lives in compiler.c so both execution paths format floats identically.
 * printf has no %f conversion, so this builds the digits by hand. */
void HDPrintDouble(double value);

#endif
