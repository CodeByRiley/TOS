#include "parser.h"
#include <include/stdlib.h>
#include <include/stdio.h>

static ASTNode* ParseExpression(Parser* parser);
static ASTNode* ParseStatement(Parser* parser);
static ASTNode* ParseBlock(Parser* parser);

static int append_node(ASTNode*** items, int* count, int* capacity, ASTNode* node) {
    if (*count >= *capacity) {
        int new_capacity = *capacity ? *capacity * 2 : 8;
        ASTNode** grown = (ASTNode**)malloc(sizeof(ASTNode*) * new_capacity);
        if (grown == NULL) {
            printf("Parse error: out of memory while growing node list.\n");
            return 0;
        }

        for (int i = 0; i < *count; i++) {
            grown[i] = (*items)[i];
        }
        free(*items);
        *items = grown;
        *capacity = new_capacity;
    }

    (*items)[(*count)++] = node;
    return 1;
}

static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = LexerNextToken(&parser->lexer);
}

static int check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static int match(Parser* parser, TokenType type) {
    if (check(parser, type)) {
        advance(parser);
        return 1;
    }
    return 0;
}

static void skip_terminators(Parser* parser) {
    while (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
}

static ASTNode* ParseNumber(Parser* parser) {
    // Convert string to integer manually (no strtol in freestanding)
    long long val = 0;
    for (int i = 0; i < parser->previous.length; i++) {
        val = val * 10 + (parser->previous.start[i] - '0');
    }
    return ASTNewNumber(val);
}

static ASTNode* ParseString(Parser* parser) {
    // Strip the quotes
    const char* str = parser->previous.start + 1;
    int len = parser->previous.length - 2;
    return ASTNewString(str, len);
}

static ASTNode* ParseCall(Parser* parser) {
    const char* name = parser->previous.start;
    int len = parser->previous.length;

    advance(parser); // Consume '('

    ASTNode** args = NULL;
    int arg_count = 0;
    int arg_capacity = 0;

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (!append_node(&args, &arg_count, &arg_capacity,
                             ParseExpression(parser))) {
                break;
            }
        } while (match(parser, TOKEN_COMMA));
    }

    match(parser, TOKEN_RPAREN); // Consume ')'
    return ASTNewCall(name, len, args, arg_count);
}

static ASTNode* ParsePrimary(Parser* parser) {
    if (match(parser, TOKEN_NUMBER)) return ParseNumber(parser);
    if (match(parser, TOKEN_STRING)) return ParseString(parser);

    if (match(parser, TOKEN_IDENTIFIER)) {
        if (check(parser, TOKEN_LPAREN)) {
            return ParseCall(parser);
        }
        return ASTNewVarRef(parser->previous.start, parser->previous.length);
    }

    // Parse Array Literal: [1, 2, 3]
    if (match(parser, TOKEN_LBRACKET)) {
        ASTNode** elements = NULL;
        int count = 0;
        int capacity = 0;

        if (!check(parser, TOKEN_RBRACKET)) {
            do {
                if (!append_node(&elements, &count, &capacity,
                                 ParseExpression(parser))) {
                    break;
                }
            } while (match(parser, TOKEN_COMMA));
        }
        match(parser, TOKEN_RBRACKET); // Consume ']'

        // We will reuse AST_CALL's structure to hold the array elements,
        // but we will give it a special name so the evaluator knows it's an array.
        return ASTNewCall("[array]", 7, elements, count);
    }

    return NULL; // Error
}

static ASTNode* ParseTerm(Parser* parser) {
    ASTNode* node = ParsePrimary(parser);
    while (check(parser, TOKEN_STAR) || check(parser, TOKEN_SLASH)) {
        TokenType op = parser->current.type;
        advance(parser);
        ASTNode* right = ParsePrimary(parser);
        node = ASTNewBinaryOp(op, node, right);
    }
    return node;
}

static ASTNode* ParseExpression(Parser* parser) {
    ASTNode* node = ParseTerm(parser);
    while (check(parser, TOKEN_PLUS) || check(parser, TOKEN_MINUS) ||
           check(parser, TOKEN_TILDE) || check(parser, TOKEN_EQEQ) ||
           check(parser, TOKEN_NEQ) || check(parser, TOKEN_LT) ||
           check(parser, TOKEN_GT) || check(parser, TOKEN_LTEQ) ||
           check(parser, TOKEN_GTEQ)) {
        advance(parser);
        ASTNode* right = ParseTerm(parser);
        node = ASTNewBinaryOp(parser->previous.type, node, right);
    }
    return node;
}


static ASTNode* ParseVarDecl(Parser* parser, TokenType type, Token name) {
    ASTNode* init = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        init = ParseExpression(parser);
    }
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return ASTNewVarDecl(type, name.start, name.length, init);
}

static ASTNode* ParseIfStatement(Parser* parser) {
    advance(parser); // Consume '('
    ASTNode* cond = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* then_block = ParseBlock(parser);
    ASTNode* else_block = NULL;
    if (match(parser, TOKEN_ELSE)) {
        else_block = ParseBlock(parser);
    }
    return ASTNewIf(cond, then_block, else_block);
}

static ASTNode* ParseForeach(Parser* parser) {
    advance(parser); // Consume '('
    advance(parser); // Consume variable name identifier

    const char* var_name = parser->previous.start;
    int var_len = parser->previous.length;

    match(parser, TOKEN_SEMICOLON); // Consume ';'
    ASTNode* array_expr = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* body = ParseBlock(parser);
    return ASTNewForeach(var_name, var_len, array_expr, body);
}

static ASTNode* ParseBlock(Parser* parser) {
    match(parser, TOKEN_LBRACE); // Consume '{'
    skip_terminators(parser);    // NEW: Skip newlines after '{'

    ASTNode** stmts = NULL;
    int count = 0;
    int capacity = 0;

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        ASTNode* stmt = ParseStatement(parser);
        if (stmt != NULL) {
            if (!append_node(&stmts, &count, &capacity, stmt)) {
                break;
            }
        } else {
            printf("Parse error: Unexpected token '%.*s' on line %d\n",
                   parser->current.length, parser->current.start, parser->lexer.line);
            advance(parser);
        }
        skip_terminators(parser); // NEW: Skip newlines between statements in block
    }

    match(parser, TOKEN_RBRACE); // Consume '}'
    return ASTNewBlock(stmts, count);
}

static ASTNode* ParseFuncDecl(Parser* parser, Token name) {
    advance(parser); // Consume '('

    const char** param_names = NULL;
    int* param_lens = NULL;
    int param_count = 0;

    if (!check(parser, TOKEN_RPAREN)) {
        param_names = (const char**)malloc(sizeof(char*) * 8);
        param_lens = (int*)malloc(sizeof(int) * 8);

        do {
            if (!check(parser, TOKEN_I64) && !check(parser, TOKEN_U32) &&
                !check(parser, TOKEN_F64) && !check(parser, TOKEN_AUTO) && !check(parser, TOKEN_U0)) {
                printf("Parse error: Expected parameter type on line %d\n", parser->lexer.line);
                break;
            }
            advance(parser); // Consume type

            if (!match(parser, TOKEN_IDENTIFIER)) {
                printf("Parse error: Expected parameter name on line %d\n", parser->lexer.line);
                break;
            }

            param_names[param_count] = parser->previous.start;
            param_lens[param_count] = parser->previous.length;
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    match(parser, TOKEN_RPAREN); // Consume ')'
    skip_terminators(parser);    // Allow newline before '{'

    ASTNode* body = ParseBlock(parser);
    return ASTNewFuncDecl(name.start, name.length, param_names, param_lens, param_count, body);
}

static ASTNode* ParseStatement(Parser* parser) {
    if (check(parser, TOKEN_U0) || check(parser, TOKEN_I64) ||
        check(parser, TOKEN_U32) || check(parser, TOKEN_F64)) {

        Token type_token = parser->current;
        advance(parser); // Consume type

        if (check(parser, TOKEN_IDENTIFIER)) {
            Token name_token = parser->current;
            advance(parser); // Consume identifier

            // If next token is '(', it's a function!
            if (check(parser, TOKEN_LPAREN)) {
                return ParseFuncDecl(parser, name_token);
            }

            // Otherwise, it's a variable declaration
            return ParseVarDecl(parser, type_token.type, name_token);
        }

        printf("Parse error: Expected identifier after type on line %d\n", parser->lexer.line);
        return NULL;
    }

    if (check(parser, TOKEN_AUTO)) {
        Token type_token = parser->current;
        advance(parser); // Consume auto

        if (check(parser, TOKEN_IDENTIFIER)) {
            Token name_token = parser->current;
            advance(parser); // Consume identifier
            return ParseVarDecl(parser, type_token.type, name_token);
        }
        return NULL;
    }

    if (match(parser, TOKEN_IF)) return ParseIfStatement(parser);
    if (match(parser, TOKEN_FOREACH)) return ParseForeach(parser);
    if (check(parser, TOKEN_LBRACE)) return ParseBlock(parser);

    // Handle 'return' statement
    if (match(parser, TOKEN_RETURN)) {
        ASTNode* expr = NULL;
        if (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_NEWLINE)) {
            expr = ParseExpression(parser);
        }
        if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
            advance(parser);
        }
        return ASTNewReturn(expr);
    }

    // It's an expression statement
    ASTNode* expr = ParseExpression(parser);
    if (expr != NULL && expr->type == AST_VAR_REF && match(parser, TOKEN_ASSIGN)) {
        ASTNode* value = ParseExpression(parser);
        if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
            advance(parser);
        }
        return ASTNewAssign(expr->var_name, expr->var_name_len, value);
    }
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return expr;
}

void ParserInit(Parser* parser, const char* source) {
    LexerInit(&parser->lexer, source);
    advance(parser); // Load the first token
}

ASTNode* ParseProgram(Parser* parser) {
    ASTNode** stmts = NULL;
    int count = 0;
    int capacity = 0;

    skip_terminators(parser); // Skip blank lines at the top of the file

    while (!check(parser, TOKEN_EOF)) {
        ASTNode* stmt = ParseStatement(parser);
        if (stmt != NULL) {
            if (!append_node(&stmts, &count, &capacity, stmt)) {
                break;
            }
        } else {
            printf("Parse error: Unexpected token '%.*s' on line %d\n",
                   parser->current.length, parser->current.start, parser->lexer.line);
            advance(parser);
        }
        skip_terminators(parser); // Skip blank lines between statements
    }

    return ASTNewBlock(stmts, count);
}
