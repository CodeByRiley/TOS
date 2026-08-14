#include "ast.h"
#include <include/stdlib.h>

ASTNode* ASTNewNumber(long long val) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_NUMBER;
    node->number_val = val;
    return node;
}

ASTNode* ASTNewString(const char* str, int len) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_STRING;
    node->string_val = str;
    node->string_len = len;
    return node;
}

ASTNode* ASTNewVarRef(const char* name, int len) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_VAR_REF;
    node->var_name = name;
    node->var_name_len = len;
    return node;
}

ASTNode* ASTNewVarDecl(TokenType type, const char* name, int len, ASTNode* init) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_VAR_DECL;
    node->var_type = type;
    node->var_name = name;
    node->var_name_len = len;
    node->initializer = init;
    return node;
}

ASTNode* ASTNewAssign(const char* name, int len, ASTNode* value) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_ASSIGN;
    node->var_name = name;
    node->var_name_len = len;
    node->initializer = value;
    return node;
}

ASTNode* ASTNewBinaryOp(TokenType op, ASTNode* left, ASTNode* right) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_BINARY_OP;
    node->op = op;
    node->left = left;
    node->right = right;
    return node;
}

ASTNode* ASTNewCall(const char* name, int len, ASTNode** args, int arg_count) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_CALL;
    node->callee_name = name;
    node->callee_len = len;
    node->args = args;
    node->arg_count = arg_count;
    return node;
}

ASTNode* ASTNewBlock(ASTNode** stmts, int count) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_BLOCK;
    node->statements = stmts;
    node->stmt_count = count;
    return node;
}

ASTNode* ASTNewIf(ASTNode* cond, ASTNode* then_block, ASTNode* else_block) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_IF;
    node->condition = cond;
    node->then_block = then_block;
    node->else_block = else_block;
    return node;
}

ASTNode* ASTNewForeach(const char* var_name, int var_len, ASTNode* array_expr, ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FOREACH;
    node->var_name = var_name;
    node->var_name_len = var_len;
    node->array_expr = array_expr;
    node->then_block = body; // Reuse then_block for the foreach body
    return node;
}

ASTNode* ASTNewFuncDecl(const char* name, int len, const char** params, int* param_lens, int param_count, ASTNode* body) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_FUNC_DECL;
    node->var_name = name;       // Reuse var_name for function name
    node->var_name_len = len;
    node->param_names = params;
    node->param_name_lens = param_lens;
    node->param_count = param_count;
    node->then_block = body;     // Reuse then_block for function body
    return node;
}

ASTNode* ASTNewReturn(ASTNode* expr) {
    ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = AST_RETURN;
    node->return_expr = expr;
    return node;
}
