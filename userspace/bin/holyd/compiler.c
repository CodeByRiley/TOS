#include "compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffi.h"

typedef struct {
    HDProgram* program;
} Compiler;

static void chunk_init(BytecodeChunk* chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
}

void HDProgramInit(HDProgram* program) {
    chunk_init(&program->main);
    program->functions = NULL;
    program->function_count = 0;
    program->function_capacity = 0;
    program->temp_counter = 0;
    program->had_error = 0;
}

static int grow_chunk(BytecodeChunk* chunk) {
    int new_capacity = chunk->capacity ? chunk->capacity * 2 : 32;
    Instruction* grown = (Instruction*)malloc(sizeof(Instruction) * new_capacity);
    if (!grown) return 0;
    for (int i = 0; i < chunk->count; i++) grown[i] = chunk->code[i];
    free(chunk->code);
    chunk->code = grown;
    chunk->capacity = new_capacity;
    return 1;
}

static int emit(BytecodeChunk* chunk, Instruction ins) {
    if (chunk->count >= chunk->capacity && !grow_chunk(chunk)) {
        printf("Compile error: out of memory while growing bytecode.\n");
        return -1;
    }
    chunk->code[chunk->count] = ins;
    return chunk->count++;
}

static Instruction make_ins(BytecodeOp op) {
    Instruction ins;
    ins.op = op;
    ins.i64 = 0;
    ins.f64 = 0.0;
    ins.text = NULL;
    ins.text_len = 0;
    ins.operand = 0;
    return ins;
}

static int emit_simple(BytecodeChunk* chunk, BytecodeOp op) {
    return emit(chunk, make_ins(op));
}

static int emit_text(BytecodeChunk* chunk, BytecodeOp op, const char* text, int len) {
    Instruction ins = make_ins(op);
    ins.text = text;
    ins.text_len = len;
    return emit(chunk, ins);
}

static int emit_int(BytecodeChunk* chunk, long long value) {
    Instruction ins = make_ins(BC_PUSH_INT);
    ins.i64 = value;
    return emit(chunk, ins);
}

static int emit_float(BytecodeChunk* chunk, double value) {
    Instruction ins = make_ins(BC_PUSH_FLOAT);
    ins.f64 = value;
    return emit(chunk, ins);
}

static int emit_count(BytecodeChunk* chunk, BytecodeOp op, int count) {
    Instruction ins = make_ins(op);
    ins.operand = count;
    return emit(chunk, ins);
}

static int emit_call(BytecodeChunk* chunk, const char* name, int len, int argc) {
    Instruction ins = make_ins(BC_CALL);
    ins.text = name;
    ins.text_len = len;
    ins.operand = argc;
    return emit(chunk, ins);
}

static int emit_jump(BytecodeChunk* chunk, BytecodeOp op) {
    Instruction ins = make_ins(op);
    ins.operand = -1;
    return emit(chunk, ins);
}

static void patch_jump(BytecodeChunk* chunk, int jump_at) {
    if (jump_at >= 0 && jump_at < chunk->count) {
        chunk->code[jump_at].operand = chunk->count;
    }
}

static char* make_temp_name(Compiler* compiler, const char* suffix) {
    char* name = (char*)malloc(32);
    if (!name) return NULL;
    snprintf(name, 32, "$foreach_%d_%s", compiler->program->temp_counter++, suffix);
    return name;
}

static void compile_error(Compiler* compiler, const char* message) {
    printf("Compile error: %s\n", message);
    compiler->program->had_error = 1;
}

static int is_name(const char* a, int a_len, const char* b) {
    int b_len = (int)strlen(b);
    return a_len == b_len && strncmp(a, b, (size_t)a_len) == 0;
}

static int program_has_function(HDProgram* program, const char* name, int len) {
    for (int i = 0; i < program->function_count; i++) {
        HDFunction* fn = &program->functions[i];
        if (fn->name_len == len && strncmp(fn->name, name, (size_t)len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_empty_block(ASTNode* node) {
    return node && node->type == AST_BLOCK && node->stmt_count == 0;
}

static void compile_expression(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node);
static void compile_statement(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node);

static void compile_binary_op(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node) {
    compile_expression(compiler, chunk, node->left);
    compile_expression(compiler, chunk, node->right);

    switch (node->op) {
        case TOKEN_PLUS:  emit_simple(chunk, BC_ADD); break;
        case TOKEN_MINUS: emit_simple(chunk, BC_SUB); break;
        case TOKEN_STAR:  emit_simple(chunk, BC_MUL); break;
        case TOKEN_SLASH: emit_simple(chunk, BC_DIV); break;
        case TOKEN_EQEQ:  emit_simple(chunk, BC_EQ); break;
        case TOKEN_NEQ:   emit_simple(chunk, BC_NE); break;
        case TOKEN_LT:    emit_simple(chunk, BC_LT); break;
        case TOKEN_GT:    emit_simple(chunk, BC_GT); break;
        case TOKEN_LTEQ:  emit_simple(chunk, BC_LE); break;
        case TOKEN_GTEQ:  emit_simple(chunk, BC_GE); break;
        case TOKEN_TILDE: emit_simple(chunk, BC_CONCAT); break;
        default:
            compile_error(compiler, "unsupported binary operator.");
            break;
    }
}

static void compile_expression(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node) {
    if (!node) {
        emit_int(chunk, 0);
        return;
    }

    switch (node->type) {
        case AST_NUMBER:
            emit_int(chunk, node->number_val);
            break;

        case AST_FLOAT:
            emit_float(chunk, node->float_val);
            break;

        case AST_STRING: {
            Instruction ins = make_ins(BC_PUSH_STRING);
            ins.text = node->string_val;
            ins.text_len = node->string_len;
            emit(chunk, ins);
            break;
        }

        case AST_VAR_REF:
            emit_text(chunk, BC_LOAD, node->var_name, node->var_name_len);
            break;

        case AST_BINARY_OP:
            compile_binary_op(compiler, chunk, node);
            break;

        case AST_INDEX:
            compile_expression(compiler, chunk, node->index_target);
            compile_expression(compiler, chunk, node->index_expr);
            emit_simple(chunk, BC_ARRAY_GET);
            break;

        case AST_ARRAY_LEN_EXPR:
            compile_expression(compiler, chunk, node->index_target);
            emit_simple(chunk, BC_ARRAY_LEN);
            break;

        case AST_CALL:
            if (is_name(node->callee_name, node->callee_len, "[array]")) {
                for (int i = 0; i < node->arg_count; i++) {
                    compile_expression(compiler, chunk, node->args[i]);
                }
                emit_count(chunk, BC_MAKE_ARRAY, node->arg_count);
                break;
            }

            for (int i = 0; i < node->arg_count; i++) {
                compile_expression(compiler, chunk, node->args[i]);
            }
            emit_call(chunk, node->callee_name, node->callee_len, node->arg_count);
            break;

        default:
            compile_error(compiler, "statement used where an expression was expected.");
            emit_int(chunk, 0);
            break;
    }
}

static void compile_block(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node) {
    if (!node) return;
    for (int i = 0; i < node->stmt_count; i++) {
        compile_statement(compiler, chunk, node->statements[i]);
    }
}

static void compile_foreach(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node) {
    char* array_name = make_temp_name(compiler, "array");
    char* index_name = make_temp_name(compiler, "index");
    if (!array_name || !index_name) {
        compile_error(compiler, "out of memory while naming foreach temporaries.");
        return;
    }

    compile_expression(compiler, chunk, node->array_expr);
    emit_text(chunk, BC_DEFINE, array_name, (int)strlen(array_name));
    emit_int(chunk, 0);
    emit_text(chunk, BC_DEFINE, index_name, (int)strlen(index_name));

    int loop_start = chunk->count;
    emit_text(chunk, BC_LOAD, index_name, (int)strlen(index_name));
    emit_text(chunk, BC_LOAD, array_name, (int)strlen(array_name));
    emit_simple(chunk, BC_ARRAY_LEN);
    emit_simple(chunk, BC_LT);
    int exit_jump = emit_jump(chunk, BC_JUMP_IF_FALSE);

    emit_text(chunk, BC_LOAD, array_name, (int)strlen(array_name));
    emit_text(chunk, BC_LOAD, index_name, (int)strlen(index_name));
    emit_simple(chunk, BC_ARRAY_GET);
    emit_text(chunk, BC_DEFINE, node->var_name, node->var_name_len);

    if (node->index_name != NULL) {
        emit_text(chunk, BC_LOAD, index_name, (int)strlen(index_name));
        emit_text(chunk, BC_DEFINE, node->index_name, node->index_name_len);
    }

    compile_statement(compiler, chunk, node->then_block);

    emit_text(chunk, BC_LOAD, index_name, (int)strlen(index_name));
    emit_int(chunk, 1);
    emit_simple(chunk, BC_ADD);
    emit_text(chunk, BC_STORE, index_name, (int)strlen(index_name));

    Instruction jump_back = make_ins(BC_JUMP);
    jump_back.operand = loop_start;
    emit(chunk, jump_back);
    patch_jump(chunk, exit_jump);
}

static void compile_statement(Compiler* compiler, BytecodeChunk* chunk, ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case AST_VAR_DECL:
            compile_expression(compiler, chunk, node->initializer);
            emit_text(chunk, BC_DEFINE, node->var_name, node->var_name_len);
            break;

        case AST_ASSIGN:
            compile_expression(compiler, chunk, node->initializer);
            emit_text(chunk, BC_STORE, node->var_name, node->var_name_len);
            break;

        /* Array, index, value -> BC_ARRAY_SET. Elements are reached through
         * the shared HDValue.elements pointer, so writing through the copy
         * on the stack updates the array held by the environment. */
        case AST_INDEX_ASSIGN:
            compile_expression(compiler, chunk, node->index_target);
            compile_expression(compiler, chunk, node->index_expr);
            compile_expression(compiler, chunk, node->initializer);
            emit_simple(chunk, BC_ARRAY_SET);
            break;

        case AST_BLOCK:
            compile_block(compiler, chunk, node);
            break;

        case AST_IF: {
            compile_expression(compiler, chunk, node->condition);
            int else_jump = emit_jump(chunk, BC_JUMP_IF_FALSE);
            compile_statement(compiler, chunk, node->then_block);
            int end_jump = emit_jump(chunk, BC_JUMP);
            patch_jump(chunk, else_jump);
            if (node->else_block) {
                compile_statement(compiler, chunk, node->else_block);
            }
            patch_jump(chunk, end_jump);
            break;
        }

        case AST_WHILE: {
            int loop_start = chunk->count;
            compile_expression(compiler, chunk, node->condition);
            int exit_jump = emit_jump(chunk, BC_JUMP_IF_FALSE);
            compile_statement(compiler, chunk, node->then_block);
            Instruction jump_back = make_ins(BC_JUMP);
            jump_back.operand = loop_start;
            emit(chunk, jump_back);
            patch_jump(chunk, exit_jump);
            break;
        }

        case AST_FOR: {
            if (node->init_stmt) {
                compile_statement(compiler, chunk, node->init_stmt);
            }

            int loop_start = chunk->count;
            if (node->condition) {
                compile_expression(compiler, chunk, node->condition);
            } else {
                emit_int(chunk, 1);
            }
            int exit_jump = emit_jump(chunk, BC_JUMP_IF_FALSE);

            compile_statement(compiler, chunk, node->then_block);

            if (node->increment) {
                compile_statement(compiler, chunk, node->increment);
            }

            Instruction jump_back = make_ins(BC_JUMP);
            jump_back.operand = loop_start;
            emit(chunk, jump_back);
            patch_jump(chunk, exit_jump);
            break;
        }

        case AST_FOREACH:
            compile_foreach(compiler, chunk, node);
            break;

        case AST_FUNC_DECL:
            break;

        case AST_RETURN:
            compile_expression(compiler, chunk, node->return_expr);
            emit_simple(chunk, BC_RETURN);
            break;

        case AST_STRING:
            compile_expression(compiler, chunk, node);
            emit_call(chunk, "Print", 5, 1);
            emit_simple(chunk, BC_POP);
            break;

        case AST_VAR_REF:
            if (program_has_function(compiler->program, node->var_name, node->var_name_len)) {
                emit_call(chunk, node->var_name, node->var_name_len, 0);
            } else {
                compile_expression(compiler, chunk, node);
            }
            emit_simple(chunk, BC_POP);
            break;

        default:
            compile_expression(compiler, chunk, node);
            emit_simple(chunk, BC_POP);
            break;
    }
}

static HDFunction* add_function(HDProgram* program, ASTNode* node) {
    if (program->function_count >= program->function_capacity) {
        int new_capacity = program->function_capacity ? program->function_capacity * 2 : 8;
        HDFunction* grown = (HDFunction*)malloc(sizeof(HDFunction) * new_capacity);
        if (!grown) return NULL;
        for (int i = 0; i < program->function_count; i++) grown[i] = program->functions[i];
        free(program->functions);
        program->functions = grown;
        program->function_capacity = new_capacity;
    }

    HDFunction* fn = &program->functions[program->function_count++];
    fn->name = node->var_name;
    fn->name_len = node->var_name_len;
    fn->param_names = node->param_names;
    fn->param_name_lens = node->param_name_lens;
    fn->param_count = node->param_count;
    chunk_init(&fn->chunk);
    return fn;
}

int HDCompileProgram(ASTNode* ast, HDProgram* program) {
    Compiler compiler;
    compiler.program = program;

    if (!ast || ast->type != AST_BLOCK) {
        compile_error(&compiler, "program root is not a block.");
        return 0;
    }

    for (int i = 0; i < ast->stmt_count; i++) {
        ASTNode* stmt = ast->statements[i];
        if (stmt && stmt->type == AST_FUNC_DECL) {
            HDFunction* fn = add_function(program, stmt);
            if (!fn) {
                compile_error(&compiler, "out of memory while adding function.");
                return 0;
            }
            compile_statement(&compiler, &fn->chunk, stmt->then_block);
            emit_int(&fn->chunk, 0);
            emit_simple(&fn->chunk, BC_RETURN);
        }
    }

    int main_statement_count = 0;
    for (int i = 0; i < ast->stmt_count; i++) {
        ASTNode* stmt = ast->statements[i];
        if (!stmt || stmt->type == AST_FUNC_DECL) continue;
        if (is_empty_block(stmt)) continue;
        compile_statement(&compiler, &program->main, stmt);
        main_statement_count++;
    }

    if (main_statement_count == 0) {
        if (program_has_function(program, "main", 4)) {
            emit_call(&program->main, "main", 4, 0);
            emit_simple(&program->main, BC_POP);
        } else if (program_has_function(program, "Main", 4)) {
            emit_call(&program->main, "Main", 4, 0);
            emit_simple(&program->main, BC_POP);
        }
    }
    emit_int(&program->main, 0);
    emit_simple(&program->main, BC_RETURN);

    return !program->had_error;
}

HDValue int_value(long long v) {
    HDValue value;
    value.type = VAL_INT;
    value.i64 = v;
    value.f64 = 0.0;
    value.str = NULL;
    value.str_len = 0;
    value.elements = NULL;
    value.array_len = 0;
    return value;
}

HDValue string_value(const char* s, int len) {
    HDValue value = int_value(0);
    value.type = VAL_STRING;
    value.str = s;
    value.str_len = len;
    return value;
}

HDValue float_value(double v) {
    HDValue value = int_value(0);
    value.type = VAL_FLOAT;
    value.f64 = v;
    return value;
}

static int is_numeric(HDValue value) {
    return value.type == VAL_INT || value.type == VAL_FLOAT;
}

static double as_double(HDValue value) {
    return value.type == VAL_FLOAT ? value.f64 : (double)value.i64;
}

/* printf carries no %f conversion, so build the digits here: whole part,
 * then six rounded decimals with trailing zeros trimmed. Magnitudes past
 * 64-bit say so instead of overflowing the cast. */
void HDPrintDouble(double value) {
    if (value != value) {
        printf("nan");
        return;
    }
    if (value < 0.0) {
        printf("-");
        value = -value;
    }
    if (value >= 9.0e18) {
        printf("<out of range>");
        return;
    }

    long long whole = (long long)value;
    long long frac = (long long)((value - (double)whole) * 1000000.0 + 0.5);
    if (frac >= 1000000) {
        whole++;
        frac -= 1000000;
    }

    char digits[24];
    snprintf(digits, sizeof(digits), "%06lld", frac);

    int last = 5;
    while (last > 0 && digits[last] == '0') last--;
    digits[last + 1] = '\0';

    printf("%lld.%s", whole, digits);
}

static int value_truthy(HDValue value) {
    if (value.type == VAL_INT) return value.i64 != 0;
    if (value.type == VAL_FLOAT) return value.f64 != 0.0;
    if (value.type == VAL_STRING) return value.str_len != 0;
    if (value.type == VAL_ARRAY) return value.array_len != 0;
    return 0;
}

static void print_string_escaped(const char* s, int len) {
    for (int j = 0; j < len; j++) {
        if (s[j] == '\\' && j + 1 < len) {
            char next = s[j + 1];
            if (next == 'n') {
                printf("\n");
                j++;
            } else if (next == '\\') {
                printf("\\");
                j++;
            } else if (next == '"') {
                printf("\"");
                j++;
            } else {
                printf("%c", s[j]);
            }
        } else {
            printf("%c", s[j]);
        }
    }
}

static void print_value(HDValue value) {
    if (value.type == VAL_STRING) {
        print_string_escaped(value.str, value.str_len);
    } else if (value.type == VAL_ARRAY) {
        printf("[");
        for (int i = 0; i < value.array_len; i++) {
            if (i > 0) printf(", ");
            print_value(value.elements[i]);
        }
        printf("]");
    } else if (value.type == VAL_FLOAT) {
        HDPrintDouble(value.f64);
    } else {
        printf("%lld", value.i64);
    }
}

static HDValue concat_values(HDValue left, HDValue right) {
    if (left.type == VAL_STRING && right.type == VAL_STRING) {
        int len = left.str_len + right.str_len;
        char* joined = (char*)malloc((size_t)len + 1);
        if (!joined) return int_value(0);
        memcpy(joined, left.str, (size_t)left.str_len);
        memcpy(joined + left.str_len, right.str, (size_t)right.str_len);
        joined[len] = '\0';
        return string_value(joined, len);
    }

    if (left.type == VAL_ARRAY || right.type == VAL_ARRAY) {
        int left_len = left.type == VAL_ARRAY ? left.array_len : 1;
        int right_len = right.type == VAL_ARRAY ? right.array_len : 1;
        HDValue array = int_value(0);
        array.type = VAL_ARRAY;
        array.array_len = left_len + right_len;
        array.elements = (HDValue*)malloc(sizeof(HDValue) * (size_t)array.array_len);
        if (!array.elements) return int_value(0);

        int out = 0;
        if (left.type == VAL_ARRAY) {
            for (int i = 0; i < left.array_len; i++) array.elements[out++] = left.elements[i];
        } else {
            array.elements[out++] = left;
        }
        if (right.type == VAL_ARRAY) {
            for (int i = 0; i < right.array_len; i++) array.elements[out++] = right.elements[i];
        } else {
            array.elements[out++] = right;
        }
        return array;
    }

    printf("Runtime error: ~ expects strings or arrays.\n");
    return int_value(0);
}

static int is_print_builtin(Instruction* ins, int* add_newline) {
    if (is_name(ins->text, ins->text_len, "Print") ||
        is_name(ins->text, ins->text_len, "print") ||
        is_name(ins->text, ins->text_len, "write")) {
        *add_newline = 0;
        return 1;
    }

    if (is_name(ins->text, ins->text_len, "PrintLn") ||
        is_name(ins->text, ins->text_len, "println") ||
        is_name(ins->text, ins->text_len, "writeln")) {
        *add_newline = 1;
        return 1;
    }

    return 0;
}

static HDFunction* find_function(HDProgram* program, const char* name, int len) {
    for (int i = 0; i < program->function_count; i++) {
        HDFunction* fn = &program->functions[i];
        if (fn->name_len == len && strncmp(fn->name, name, (size_t)len) == 0) {
            return fn;
        }
    }
    return NULL;
}

typedef struct {
    HDProgram* program;
    Environment* globals;
} VM;

static int run_chunk(VM* vm, BytecodeChunk* chunk, Environment* env, HDValue* out);

static int call_function(VM* vm, Instruction* ins, HDValue* stack, int* sp) {
    int add_newline = 0;
    if (is_print_builtin(ins, &add_newline)) {
        if (*sp < ins->operand) {
            printf("Runtime error: stack underflow in print builtin.\n");
            return 0;
        }
        int first = *sp - ins->operand;
        for (int i = 0; i < ins->operand; i++) {
            print_value(stack[first + i]);
        }
        if (add_newline) {
            printf("\n");
        }
        *sp = first;
        stack[(*sp)++] = int_value(0);
        return 1;
    }

    NativeFn native_fn = ffi_lookup_native(ins->text, ins->text_len);
        if (native_fn != NULL) {
            if (*sp < ins->operand) {
                printf("Runtime error: stack underflow in native call.\n");
                return 0;
            }
            int first = *sp - ins->operand;
            HDValue result = native_fn(ins->operand, &stack[first]);
            *sp = first;
            stack[(*sp)++] = result;
            return 1;
        }

    HDFunction* fn = find_function(vm->program, ins->text, ins->text_len);
    if (!fn) {
        printf("Runtime error: unknown function '%.*s'.\n", ins->text_len, ins->text);
        return 0;
    }
    if (fn->param_count != ins->operand) {
        printf("Runtime error: function '%.*s' expects %d args, got %d.\n",
               fn->name_len, fn->name, fn->param_count, ins->operand);
        return 0;
    }
    if (*sp < ins->operand) {
        printf("Runtime error: stack underflow in function call.\n");
        return 0;
    }

    Environment local;
    EnvInitChild(&local, vm->globals);

    int first = *sp - ins->operand;
    for (int i = 0; i < fn->param_count; i++) {
        EnvDefine(&local, fn->param_names[i], (size_t)fn->param_name_lens[i], stack[first + i]);
    }
    *sp = first;

    HDValue result = int_value(0);
    if (!run_chunk(vm, &fn->chunk, &local, &result)) return 0;
    stack[(*sp)++] = result;
    return 1;
}

static int pop_value(HDValue* stack, int* sp, HDValue* out) {
    if (*sp <= 0) {
        printf("Runtime error: stack underflow.\n");
        return 0;
    }
    *out = stack[--(*sp)];
    return 1;
}

static int push_value(HDValue* stack, int* sp, HDValue value) {
    if (*sp >= 256) {
        printf("Runtime error: stack overflow.\n");
        return 0;
    }
    stack[(*sp)++] = value;
    return 1;
}

static int run_chunk(VM* vm, BytecodeChunk* chunk, Environment* env, HDValue* out) {
    HDValue stack[256];
    int sp = 0;
    int ip = 0;

    while (ip < chunk->count) {
        Instruction* ins = &chunk->code[ip++];
        HDValue left, right, value;

        switch (ins->op) {
            case BC_PUSH_INT:
                if (!push_value(stack, &sp, int_value(ins->i64))) return 0;
                break;

            case BC_PUSH_FLOAT:
                if (!push_value(stack, &sp, float_value(ins->f64))) return 0;
                break;

            case BC_PUSH_STRING:
                if (!push_value(stack, &sp, string_value(ins->text, ins->text_len))) return 0;
                break;

            case BC_LOAD: {
                HDValue* found = EnvGet(env, ins->text, (size_t)ins->text_len);
                if (!found) {
                    printf("Runtime error: undefined variable '%.*s'.\n", ins->text_len, ins->text);
                    return 0;
                }
                if (!push_value(stack, &sp, *found)) return 0;
                break;
            }

            case BC_DEFINE:
                if (!pop_value(stack, &sp, &value)) return 0;
                EnvDefine(env, ins->text, (size_t)ins->text_len, value);
                break;

            case BC_STORE:
                if (!pop_value(stack, &sp, &value)) return 0;
                EnvSet(env, ins->text, (size_t)ins->text_len, value);
                break;

            case BC_ADD:
            case BC_SUB:
            case BC_MUL:
            case BC_DIV:
            case BC_EQ:
            case BC_NE:
            case BC_LT:
            case BC_GT:
            case BC_LE:
            case BC_GE:
                if (!pop_value(stack, &sp, &right) || !pop_value(stack, &sp, &left)) return 0;
                if (!is_numeric(left) || !is_numeric(right)) {
                    printf("Runtime error: numeric operator used on a non-numeric value.\n");
                    return 0;
                }

                /* A float on either side promotes both, as in C. Comparisons
                 * still yield an int so the jump ops stay integer-only. */
                if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                    double a = as_double(left);
                    double b = as_double(right);
                    switch (ins->op) {
                        case BC_ADD: value = float_value(a + b); break;
                        case BC_SUB: value = float_value(a - b); break;
                        case BC_MUL: value = float_value(a * b); break;
                        case BC_DIV:
                            if (b == 0.0) {
                                printf("Runtime error: division by zero.\n");
                                return 0;
                            }
                            value = float_value(a / b);
                            break;
                        case BC_EQ: value = int_value(a == b); break;
                        case BC_NE: value = int_value(a != b); break;
                        case BC_LT: value = int_value(a < b); break;
                        case BC_GT: value = int_value(a > b); break;
                        case BC_LE: value = int_value(a <= b); break;
                        case BC_GE: value = int_value(a >= b); break;
                        default: value = int_value(0); break;
                    }
                    if (!push_value(stack, &sp, value)) return 0;
                    break;
                }

                switch (ins->op) {
                    case BC_ADD: value = int_value(left.i64 + right.i64); break;
                    case BC_SUB: value = int_value(left.i64 - right.i64); break;
                    case BC_MUL: value = int_value(left.i64 * right.i64); break;
                    case BC_DIV:
                        if (right.i64 == 0) {
                            printf("Runtime error: division by zero.\n");
                            return 0;
                        }
                        value = int_value(left.i64 / right.i64);
                        break;
                    case BC_EQ: value = int_value(left.i64 == right.i64); break;
                    case BC_NE: value = int_value(left.i64 != right.i64); break;
                    case BC_LT: value = int_value(left.i64 < right.i64); break;
                    case BC_GT: value = int_value(left.i64 > right.i64); break;
                    case BC_LE: value = int_value(left.i64 <= right.i64); break;
                    case BC_GE: value = int_value(left.i64 >= right.i64); break;
                    default: value = int_value(0); break;
                }
                if (!push_value(stack, &sp, value)) return 0;
                break;

            case BC_CONCAT:
                if (!pop_value(stack, &sp, &right) || !pop_value(stack, &sp, &left)) return 0;
                if (!push_value(stack, &sp, concat_values(left, right))) return 0;
                break;

            case BC_MAKE_ARRAY: {
                if (sp < ins->operand) {
                    printf("Runtime error: stack underflow while creating array.\n");
                    return 0;
                }
                HDValue array = int_value(0);
                array.type = VAL_ARRAY;
                array.array_len = ins->operand;
                array.elements = (HDValue*)malloc(sizeof(HDValue) * (size_t)ins->operand);
                if (!array.elements && ins->operand > 0) return 0;
                int first = sp - ins->operand;
                for (int i = 0; i < ins->operand; i++) {
                    array.elements[i] = stack[first + i];
                }
                sp = first;
                if (!push_value(stack, &sp, array)) return 0;
                break;
            }

            case BC_ARRAY_LEN:
                if (!pop_value(stack, &sp, &value)) return 0;
                if (value.type != VAL_ARRAY) {
                    printf("Runtime error: array length requested from non-array value.\n");
                    return 0;
                }
                if (!push_value(stack, &sp, int_value(value.array_len))) return 0;
                break;

            case BC_ARRAY_GET:
                if (!pop_value(stack, &sp, &right) || !pop_value(stack, &sp, &left)) return 0;
                if (left.type != VAL_ARRAY || right.type != VAL_INT ||
                    right.i64 < 0 || right.i64 >= left.array_len) {
                    printf("Runtime error: invalid array access.\n");
                    return 0;
                }
                if (!push_value(stack, &sp, left.elements[right.i64])) return 0;
                break;

            case BC_ARRAY_SET:
                if (!pop_value(stack, &sp, &value) ||
                    !pop_value(stack, &sp, &right) ||
                    !pop_value(stack, &sp, &left)) return 0;
                if (left.type != VAL_ARRAY || right.type != VAL_INT ||
                    right.i64 < 0 || right.i64 >= left.array_len) {
                    printf("Runtime error: invalid array assignment.\n");
                    return 0;
                }
                left.elements[right.i64] = value;
                break;

            case BC_JUMP:
                ip = ins->operand;
                break;

            case BC_JUMP_IF_FALSE:
                if (!pop_value(stack, &sp, &value)) return 0;
                if (!value_truthy(value)) ip = ins->operand;
                break;

            case BC_CALL:
                if (!call_function(vm, ins, stack, &sp)) return 0;
                break;

            case BC_POP:
                if (!pop_value(stack, &sp, &value)) return 0;
                break;

            case BC_RETURN:
                if (sp > 0) {
                    *out = stack[--sp];
                } else {
                    *out = int_value(0);
                }
                return 1;
        }
    }

    *out = int_value(0);
    return 1;
}

int HDRunProgram(HDProgram* program) {
    Environment globals;
    EnvInit(&globals);

    VM vm;
    vm.program = program;
    vm.globals = &globals;

    HDValue result = int_value(0);
    return run_chunk(&vm, &program->main, &globals, &result);
}

static const char* op_name(BytecodeOp op) {
    switch (op) {
        case BC_PUSH_INT: return "PUSH_INT";
        case BC_PUSH_FLOAT: return "PUSH_FLOAT";
        case BC_PUSH_STRING: return "PUSH_STRING";
        case BC_LOAD: return "LOAD";
        case BC_DEFINE: return "DEFINE";
        case BC_STORE: return "STORE";
        case BC_ADD: return "ADD";
        case BC_SUB: return "SUB";
        case BC_MUL: return "MUL";
        case BC_DIV: return "DIV";
        case BC_EQ: return "EQ";
        case BC_NE: return "NE";
        case BC_LT: return "LT";
        case BC_GT: return "GT";
        case BC_LE: return "LE";
        case BC_GE: return "GE";
        case BC_CONCAT: return "CONCAT";
        case BC_MAKE_ARRAY: return "MAKE_ARRAY";
        case BC_ARRAY_LEN: return "ARRAY_LEN";
        case BC_ARRAY_GET: return "ARRAY_GET";
        case BC_ARRAY_SET: return "ARRAY_SET";
        case BC_JUMP: return "JUMP";
        case BC_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case BC_CALL: return "CALL";
        case BC_POP: return "POP";
        case BC_RETURN: return "RETURN";
        default: return "UNKNOWN";
    }
}

static void dump_chunk(const char* name, BytecodeChunk* chunk) {
    printf("== %s ==\n", name);
    for (int i = 0; i < chunk->count; i++) {
        Instruction* ins = &chunk->code[i];
        printf("%04d %-14s", i, op_name(ins->op));
        if (ins->op == BC_PUSH_INT) {
            printf(" %lld", ins->i64);
        } else if (ins->op == BC_PUSH_FLOAT) {
            printf(" ");
            HDPrintDouble(ins->f64);
        } else if (ins->op == BC_PUSH_STRING) {
            printf(" \"%.*s\"", ins->text_len, ins->text);
        } else if (ins->op == BC_LOAD || ins->op == BC_DEFINE ||
                   ins->op == BC_STORE || ins->op == BC_CALL) {
            printf(" %.*s", ins->text_len, ins->text);
            if (ins->op == BC_CALL) printf(" argc=%d", ins->operand);
        } else if (ins->op == BC_MAKE_ARRAY || ins->op == BC_JUMP ||
                   ins->op == BC_JUMP_IF_FALSE) {
            printf(" %d", ins->operand);
        }
        printf("\n");
    }
}

void HDDumpProgram(HDProgram* program) {
    for (int i = 0; i < program->function_count; i++) {
        char name[64];
        int len = program->functions[i].name_len;
        if (len > 48) len = 48;
        memcpy(name, program->functions[i].name, (size_t)len);
        name[len] = '\0';
        dump_chunk(name, &program->functions[i].chunk);
    }
    dump_chunk("main", &program->main);
}
