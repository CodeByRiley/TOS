#ifndef HOLYD_AST_H
#define HOLYD_AST_H

#include "../lexer/lexer.h"

// Forward declarations
typedef struct ASTNode ASTNode;

// All the different kinds of AST nodes
typedef enum {
    AST_NUMBER,         // 123
    AST_FLOAT,          // 1.5
    AST_STRING,         // "Hello"
    AST_VAR_DECL,       // I64 x = expr;
    AST_VAR_REF,        // x
    AST_ASSIGN,         // x = expr;
    AST_INDEX_ASSIGN,   // arr[index] = expr;
    AST_BINARY_OP,      // expr + expr
    AST_CALL,           // Print(expr)
    AST_INDEX,          // array[index]
    AST_ARRAY_LEN_EXPR, // array.length
    AST_BLOCK,          // { statement1; statement2; }
    AST_IF,             // if (cond) { block } else { block }
    AST_WHILE,          // while (cond) { block }
    AST_FOR,            // for (init; cond; inc) { block }
    AST_FOREACH,        // foreach (x; arr) { block }
    AST_FUNC_DECL,
    AST_RETURN,				  // return expr
} ASTNodeType;

struct ASTNode {
    ASTNodeType type;

    // For AST_NUMBER
    long long number_val;

    // For AST_FLOAT
    double float_val;

    // For AST_STRING
    const char* string_val;
    int string_len;

    // For AST_VAR_DECL, AST_VAR_REF
    const char* var_name;
    int var_name_len;
    TokenType var_type; // TOKEN_I64, TOKEN_U0, TOKEN_AUTO, etc.
    ASTNode* initializer; // For AST_VAR_DECL

    // For AST_ASSIGN
    ASTNode* assign_target;

    // For AST_BINARY_OP
    TokenType op;
    ASTNode* left;
    ASTNode* right;

    // For AST_INDEX / AST_ARRAY_LEN_EXPR
    ASTNode* index_target;
    ASTNode* index_expr;

    // For AST_CALL
    const char* callee_name;
    int callee_len;
    ASTNode** args;         // Array of argument expressions
    int arg_count;

    // For AST_BLOCK, AST_IF, AST_FOREACH
    ASTNode** statements;   // Array of statements in the block
    int stmt_count;

    // For AST_IF
    ASTNode* condition;
    ASTNode* then_block;
    ASTNode* else_block;

    // For AST_FOR
    ASTNode* init_stmt;
    ASTNode* increment;

    // For AST_FOREACH
    ASTNode* array_expr;
    const char* index_name;
    int index_name_len;

    // For AST_FUNC_DECL
    const char** param_names;
    int* param_name_lens;
    int param_count;

    // For AST_RETURN
    ASTNode* return_expr;
};

// Helper functions to create nodes (allocates memory)
ASTNode* ASTNewNumber(long long val);
ASTNode* ASTNewFloat(double val);
ASTNode* ASTNewString(const char* str, int len);
ASTNode* ASTNewVarRef(const char* name, int len);
ASTNode* ASTNewVarDecl(TokenType type, const char* name, int len, ASTNode* init);
ASTNode* ASTNewAssign(const char* name, int len, ASTNode* value);
ASTNode* ASTNewBinaryOp(TokenType op, ASTNode* left, ASTNode* right);
ASTNode* ASTNewIndex(ASTNode* target, ASTNode* index);
ASTNode* ASTNewIndexAssign(ASTNode* target, ASTNode* index, ASTNode* value);
ASTNode* ASTNewArrayLenExpr(ASTNode* target);
ASTNode* ASTNewCall(const char* name, int len, ASTNode** args, int arg_count);
ASTNode* ASTNewBlock(ASTNode** stmts, int count);
ASTNode* ASTNewIf(ASTNode* cond, ASTNode* then_block, ASTNode* else_block);
ASTNode* ASTNewWhile(ASTNode* cond, ASTNode* body);
ASTNode* ASTNewFor(ASTNode* init, ASTNode* cond, ASTNode* inc, ASTNode* body);
ASTNode* ASTNewForeach(const char* var_name, int var_len, const char* index_name, int index_len, ASTNode* array_expr, ASTNode* body);
ASTNode* ASTNewFuncDecl(const char* name, int len, const char** params, int* param_lens, int param_count, ASTNode* body);
ASTNode* ASTNewReturn(ASTNode* expr);

#endif
