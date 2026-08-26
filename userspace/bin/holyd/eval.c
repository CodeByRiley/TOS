#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void EnvInit(Environment* env) {
    env->head = NULL;
    env->parent = NULL;
}

void EnvInitChild(Environment* env, Environment* parent) {
    env->head = NULL;
    env->parent = parent;
}

static EnvEntry* EnvFindLocal(Environment* env, const char* name, size_t name_len) {
    EnvEntry* entry = env->head;
    while (entry != NULL) {
        if (entry->name && strlen(entry->name) == name_len && strncmp(entry->name, name, name_len) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void EnvDefine(Environment* env, const char* name, size_t name_len, HDValue value) {
    EnvEntry* entry = EnvFindLocal(env, name, name_len);
    if (entry != NULL) {
        entry->value = value;
        return;
    }

    EnvEntry* new_entry = (EnvEntry*)malloc(sizeof(EnvEntry));
    new_entry->name = (char*)malloc(name_len + 1);
    memcpy(new_entry->name, name, name_len);
    new_entry->name[name_len] = '\0';
    new_entry->value = value;
    new_entry->next = env->head;
    env->head = new_entry;
}

void EnvSet(Environment* env, const char* name, size_t name_len, HDValue value) {
    Environment* current = env;
    while (current != NULL) {
        EnvEntry* entry = EnvFindLocal(current, name, name_len);
        if (entry != NULL) {
            entry->value = value;
            return;
        }
        current = current->parent;
    }

    EnvDefine(env, name, name_len, value);
}

HDValue* EnvGet(Environment* env, const char* name, size_t name_len) {
    Environment* current = env;
    while (current != NULL) {
        EnvEntry* entry = EnvFindLocal(current, name, name_len);
        if (entry != NULL) {
            return &entry->value;
        }
        current = current->parent;
    }
    return NULL; // Variable not found
}

static HDValue EvalExpression(ASTNode* node, Environment* env) {
    HDValue val;
    val.type = VAL_INT;
    val.i64 = 0;

    if (node == NULL) return val;

    switch (node->type) {
        case AST_NUMBER:
            val.type = VAL_INT;
            val.i64 = node->number_val;
            break;

        case AST_FLOAT:
            val.type = VAL_FLOAT;
            val.f64 = node->float_val;
            break;

        case AST_STRING:
            val.type = VAL_STRING;
            val.str = node->string_val;
            val.str_len = node->string_len;
            break;

        case AST_VAR_REF: {
            HDValue* var = EnvGet(env, node->var_name, node->var_name_len);
            if (var != NULL) {
                return *var;
            }
            printf("Error: Undefined variable '%.*s'\n", node->var_name_len, node->var_name);
            break;
        }

        case AST_BINARY_OP: {
            HDValue left = EvalExpression(node->left, env);
            HDValue right = EvalExpression(node->right, env);

            // D-style string concatenation with ~
            if (node->op == TOKEN_TILDE) {
                // Simple concatenation for demo purposes
                int new_len = left.str_len + right.str_len;
                char* new_str = (char*)malloc(new_len + 1);
                memcpy(new_str, left.str, left.str_len);
                memcpy(new_str + left.str_len, right.str, right.str_len);
                new_str[new_len] = '\0';
                val.type = VAL_STRING;
                val.str = new_str;
                val.str_len = new_len;
                return val;
            }

            // A float on either side promotes both, matching the VM.
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                double a = left.type == VAL_FLOAT ? left.f64 : (double)left.i64;
                double b = right.type == VAL_FLOAT ? right.f64 : (double)right.i64;
                val.type = VAL_FLOAT;
                switch (node->op) {
                    case TOKEN_PLUS:  val.f64 = a + b; break;
                    case TOKEN_MINUS: val.f64 = a - b; break;
                    case TOKEN_STAR:  val.f64 = a * b; break;
                    case TOKEN_SLASH: val.f64 = a / b; break;
                    case TOKEN_EQEQ:  val.type = VAL_INT; val.i64 = (a == b); break;
                    case TOKEN_NEQ:   val.type = VAL_INT; val.i64 = (a != b); break;
                    case TOKEN_LT:    val.type = VAL_INT; val.i64 = (a < b); break;
                    case TOKEN_GT:    val.type = VAL_INT; val.i64 = (a > b); break;
                    case TOKEN_LTEQ:  val.type = VAL_INT; val.i64 = (a <= b); break;
                    case TOKEN_GTEQ:  val.type = VAL_INT; val.i64 = (a >= b); break;
                    default: val.type = VAL_INT; val.i64 = 0; break;
                }
                return val;
            }

            // Integer math
            val.type = VAL_INT;
            switch (node->op) {
                case TOKEN_PLUS:  val.i64 = left.i64 + right.i64; break;
                case TOKEN_MINUS: val.i64 = left.i64 - right.i64; break;
                case TOKEN_STAR:  val.i64 = left.i64 * right.i64; break;
                case TOKEN_SLASH: val.i64 = left.i64 / right.i64; break;
                case TOKEN_EQEQ:  val.i64 = (left.i64 == right.i64); break;
                case TOKEN_NEQ:   val.i64 = (left.i64 != right.i64); break;
                case TOKEN_LT:    val.i64 = (left.i64 < right.i64); break;
                case TOKEN_GT:    val.i64 = (left.i64 > right.i64); break;
                case TOKEN_LTEQ:  val.i64 = (left.i64 <= right.i64); break;
                case TOKEN_GTEQ:  val.i64 = (left.i64 >= right.i64); break;
                default: break;
            }
            break;
        }

        case AST_CALL: {
            // Check for our hacky array literal
            if (node->callee_len == 7 && strncmp(node->callee_name, "[array]", 7) == 0) {
                HDValue val;
                val.type = VAL_ARRAY;
                val.array_len = node->arg_count;
                val.elements = (HDValue*)malloc(sizeof(HDValue) * node->arg_count);

                for (int i = 0; i < node->arg_count; i++) {
                    val.elements[i] = EvalExpression(node->args[i], env);
                }
                return val;
            }
            break;
        }

        default:
            // For statements accidentally treated as expressions
            break;
    }
    return val;
}

HDValue EvalNode(ASTNode* node, Environment* env) {
    HDValue val;
    val.type = VAL_INT;
    val.i64 = 0;

    if (node == NULL) return val;

    switch (node->type) {
        case AST_VAR_DECL: {
            if (node->initializer != NULL) {
                val = EvalExpression(node->initializer, env);
            } else {
                val.type = VAL_INT;
                val.i64 = 0;
            }
            EnvSet(env, node->var_name, node->var_name_len, val);
            break;
        }

        case AST_ASSIGN: {
            // Evaluate right side
            val = EvalExpression(node->initializer, env);
            // Assign to existing variable
            EnvSet(env, node->var_name, node->var_name_len, val);
            break;
        }

        case AST_BLOCK: {
            for (int i = 0; i < node->stmt_count; i++) {
                EvalNode(node->statements[i], env);
            }
            break;
        }

        case AST_IF: {
            HDValue cond = EvalExpression(node->condition, env);
            if (cond.i64 != 0) {
                EvalNode(node->then_block, env);
            } else if (node->else_block != NULL) {
                EvalNode(node->else_block, env);
            }
            break;
        }

        case AST_FOREACH: {
            // Evaluate the array we are looping over
            HDValue array = EvalExpression(node->array_expr, env);

            if (array.type != VAL_ARRAY) {
                printf("Error: foreach expects an array.\n");
                break;
            }

            // Loop through the elements
            for (int i = 0; i < array.array_len; i++) {
                // Set the loop variable to the current element
                EnvSet(env, node->var_name, node->var_name_len, array.elements[i]);

                // Execute the body of the loop
                EvalNode(node->then_block, env); // then_block is reused as the loop body
            }
            break;
        }

        case AST_CALL: {
            // Built-in Print function
            if (node->callee_len == 5 && strncmp(node->callee_name, "Print", 5) == 0) {
                for (int i = 0; i < node->arg_count; i++) {
                    HDValue arg = EvalExpression(node->args[i], env);
                    if (arg.type == VAL_STRING) {
                        // Process escape sequences like \n
                        for (int j = 0; j < arg.str_len; j++) {
                            if (arg.str[j] == '\\') {
                                // Look at the next character
                                if (j + 1 < arg.str_len) {
                                    char next = arg.str[j + 1];
                                    if (next == 'n') {
                                        printf("\n"); // Print actual newline
                                        j++; // Skip the 'n'
                                    } else if (next == '\\') {
                                        printf("\\"); // Print backslash
                                        j++;
                                    } else if (next == '"') {
                                        printf("\""); // Print quote
                                        j++;
                                    } else {
                                        printf("%c", arg.str[j]); // Unknown, print as is
                                    }
                                }
                            } else {
                                printf("%c", arg.str[j]);
                            }
                        }
                    } else if (arg.type == VAL_FLOAT) {
                        HDPrintDouble(arg.f64);
                    } else {
                        printf("%lld", arg.i64);
                    }
                }
            } else {
                printf("Error: Unknown function '%.*s'\n", node->callee_len, node->callee_name);
            }
            break;
        }

        case AST_INDEX_ASSIGN: {
            if (node->index_target == NULL ||
                node->index_target->type != AST_VAR_REF) {
                printf("Error: indexed assignment needs a named array.\n");
                break;
            }

            HDValue* array = EnvGet(env, node->index_target->var_name,
                                    (size_t)node->index_target->var_name_len);
            if (array == NULL || array->type != VAL_ARRAY) {
                printf("Error: indexed assignment target is not an array.\n");
                break;
            }

            HDValue index = EvalExpression(node->index_expr, env);
            if (index.type != VAL_INT || index.i64 < 0 ||
                index.i64 >= array->array_len) {
                printf("Error: array index out of range.\n");
                break;
            }

            array->elements[index.i64] = EvalExpression(node->initializer, env);
            break;
        }

        default:
            // Try evaluating as an expression (e.g. a standalone "5 + 5;")
            EvalExpression(node, env);
            break;
    }
    return val;
}
