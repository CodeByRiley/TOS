#include "parser.h"
#include <include/stdlib.h>
#include <include/stdio.h>
#include <include/string.h>

static ASTNode* ParseExpression(Parser* parser);
static ASTNode* ParseStatement(Parser* parser);
static ASTNode* ParseBlock(Parser* parser);
static ASTNode* ParseAssignmentExpression(Parser* parser);

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

static int is_type_token(TokenType type) {
    switch (type) {
        case TOKEN_U0:
        case TOKEN_I8:
        case TOKEN_U8:
        case TOKEN_I16:
        case TOKEN_U16:
        case TOKEN_I32:
        case TOKEN_U32:
        case TOKEN_I64:
        case TOKEN_U64:
        case TOKEN_F64:
        case TOKEN_VOID:
        case TOKEN_INT:
        case TOKEN_UINT:
        case TOKEN_LONG:
        case TOKEN_ULONG:
        case TOKEN_DOUBLE:
        case TOKEN_BOOL:
        case TOKEN_STRING_TYPE:
        case TOKEN_AUTO:
            return 1;
        default:
            return 0;
    }
}

static Token consume_type(Parser* parser) {
    Token type_token = parser->current;
    if (!is_type_token(type_token.type)) {
        printf("Parse error: Expected type on line %d\n", parser->lexer.line);
        return type_token;
    }

    advance(parser);
    while (match(parser, TOKEN_LBRACKET)) {
        if (!match(parser, TOKEN_RBRACKET)) {
            printf("Parse error: Expected ']' after array type on line %d\n", parser->lexer.line);
            break;
        }
    }

    return type_token;
}

static ASTNode* ParseNumber(Parser* parser) {
    // Convert string to integer manually (no strtol in freestanding)
    long long val = 0;
    int base = 10;
    int i = 0;
    if (parser->previous.length > 2 &&
        parser->previous.start[0] == '0' &&
        (parser->previous.start[1] == 'x' || parser->previous.start[1] == 'X')) {
        base = 16;
        i = 2;
    }

    for (; i < parser->previous.length; i++) {
        char c = parser->previous.start[i];
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        val = val * base + digit;
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
    if (match(parser, TOKEN_TRUE)) return ASTNewNumber(1);
    if (match(parser, TOKEN_FALSE)) return ASTNewNumber(0);

    if (match(parser, TOKEN_LPAREN)) {
        ASTNode* expr = ParseExpression(parser);
        if (!match(parser, TOKEN_RPAREN)) {
            printf("Parse error: Expected ')' on line %d\n", parser->lexer.line);
        }
        return expr;
    }

    if (match(parser, TOKEN_MINUS)) {
        return ASTNewBinaryOp(TOKEN_MINUS, ASTNewNumber(0), ParsePrimary(parser));
    }

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

static int token_is_identifier_text(Token token, const char* text) {
    int len = 0;
    while (text[len]) len++;
    return token.length == len && strncmp(token.start, text, (size_t)len) == 0;
}

static ASTNode* ParsePostfix(Parser* parser) {
    ASTNode* node = ParsePrimary(parser);

    while (node != NULL) {
        if (match(parser, TOKEN_LBRACKET)) {
            ASTNode* index = ParseExpression(parser);
            if (!match(parser, TOKEN_RBRACKET)) {
                printf("Parse error: Expected ']' on line %d\n", parser->lexer.line);
            }
            node = ASTNewIndex(node, index);
        } else if (match(parser, TOKEN_DOT)) {
            if (!match(parser, TOKEN_IDENTIFIER)) {
                printf("Parse error: Expected property name after '.' on line %d\n", parser->lexer.line);
                break;
            }
            if (token_is_identifier_text(parser->previous, "length")) {
                node = ASTNewArrayLenExpr(node);
            } else {
                printf("Parse error: Unknown property '%.*s' on line %d\n",
                       parser->previous.length, parser->previous.start, parser->lexer.line);
            }
        } else {
            break;
        }
    }

    return node;
}

static ASTNode* ParseTerm(Parser* parser) {
    ASTNode* node = ParsePostfix(parser);
    while (check(parser, TOKEN_STAR) || check(parser, TOKEN_SLASH)) {
        TokenType op = parser->current.type;
        advance(parser);
        ASTNode* right = ParsePostfix(parser);
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
        TokenType op = parser->current.type;
        advance(parser);
        ASTNode* right = ParseTerm(parser);
        node = ASTNewBinaryOp(op, node, right);
    }
    return node;
}

static ASTNode* make_inc_dec(ASTNode* target, TokenType op) {
    TokenType bin_op = op == TOKEN_PLUSPLUS ? TOKEN_PLUS : TOKEN_MINUS;
    return ASTNewAssign(target->var_name, target->var_name_len,
                        ASTNewBinaryOp(bin_op,
                                       ASTNewVarRef(target->var_name, target->var_name_len),
                                       ASTNewNumber(1)));
}

static ASTNode* ParseAssignmentExpression(Parser* parser) {
    ASTNode* expr = ParseExpression(parser);
    if (expr != NULL && expr->type == AST_VAR_REF) {
        if (match(parser, TOKEN_ASSIGN)) {
            return ASTNewAssign(expr->var_name, expr->var_name_len, ParseExpression(parser));
        }
        if (match(parser, TOKEN_PLUSPLUS)) {
            return make_inc_dec(expr, TOKEN_PLUSPLUS);
        }
        if (match(parser, TOKEN_MINUSMINUS)) {
            return make_inc_dec(expr, TOKEN_MINUSMINUS);
        }
    }
    return expr;
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
    match(parser, TOKEN_LPAREN); // Consume '('
    ASTNode* cond = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* then_block = ParseBlock(parser);
    ASTNode* else_block = NULL;
    if (match(parser, TOKEN_ELSE)) {
        else_block = ParseBlock(parser);
    }
    return ASTNewIf(cond, then_block, else_block);
}

static ASTNode* ParseWhileStatement(Parser* parser) {
    match(parser, TOKEN_LPAREN);
    ASTNode* cond = ParseExpression(parser);
    match(parser, TOKEN_RPAREN);

    ASTNode* body = ParseBlock(parser);
    return ASTNewWhile(cond, body);
}

static ASTNode* ParseForStatement(Parser* parser) {
    match(parser, TOKEN_LPAREN);

    ASTNode* init = NULL;
    if (!match(parser, TOKEN_SEMICOLON)) {
        if (is_type_token(parser->current.type)) {
            Token type_token = consume_type(parser);
            if (!check(parser, TOKEN_IDENTIFIER)) {
                printf("Parse error: Expected variable name in for initializer on line %d\n", parser->lexer.line);
                return NULL;
            }
            Token name_token = parser->current;
            advance(parser);
            init = ParseVarDecl(parser, type_token.type, name_token);
        } else {
            init = ParseAssignmentExpression(parser);
            match(parser, TOKEN_SEMICOLON);
        }
    }

    ASTNode* cond = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        cond = ParseExpression(parser);
    }
    match(parser, TOKEN_SEMICOLON);

    ASTNode* inc = NULL;
    if (!check(parser, TOKEN_RPAREN)) {
        inc = ParseAssignmentExpression(parser);
    }
    match(parser, TOKEN_RPAREN);

    ASTNode* body = ParseBlock(parser);
    return ASTNewFor(init, cond, inc, body);
}

static ASTNode* ParseForeach(Parser* parser) {
    match(parser, TOKEN_LPAREN);

    if (is_type_token(parser->current.type)) {
        consume_type(parser);
    }

    if (!match(parser, TOKEN_IDENTIFIER)) {
        printf("Parse error: Expected foreach variable on line %d\n", parser->lexer.line);
        return NULL;
    }

    const char* first_name = parser->previous.start;
    int first_len = parser->previous.length;
    const char* index_name = NULL;
    int index_len = 0;
    const char* var_name = first_name;
    int var_len = first_len;

    if (match(parser, TOKEN_COMMA)) {
        index_name = first_name;
        index_len = first_len;

        if (is_type_token(parser->current.type)) {
            consume_type(parser);
        }
        if (!match(parser, TOKEN_IDENTIFIER)) {
            printf("Parse error: Expected foreach value variable on line %d\n", parser->lexer.line);
            return NULL;
        }
        var_name = parser->previous.start;
        var_len = parser->previous.length;
    }

    match(parser, TOKEN_SEMICOLON); // Consume ';'
    ASTNode* array_expr = ParseExpression(parser);
    match(parser, TOKEN_RPAREN); // Consume ')'

    ASTNode* body = ParseBlock(parser);
    return ASTNewForeach(var_name, var_len, index_name, index_len, array_expr, body);
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
            if (!is_type_token(parser->current.type)) {
                printf("Parse error: Expected parameter type on line %d\n", parser->lexer.line);
                break;
            }
            consume_type(parser);

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

static ASTNode* ParseIgnoredDirective(Parser* parser) {
    while (!check(parser, TOKEN_SEMICOLON) &&
           !check(parser, TOKEN_NEWLINE) &&
           !check(parser, TOKEN_EOF)) {
        advance(parser);
    }
    if (check(parser, TOKEN_SEMICOLON) || check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
    return ASTNewBlock(NULL, 0);
}

static ASTNode* ParseStatement(Parser* parser) {
    if (match(parser, TOKEN_MODULE) || match(parser, TOKEN_IMPORT)) {
        return ParseIgnoredDirective(parser);
    }

    if (is_type_token(parser->current.type)) {
        Token type_token = consume_type(parser);

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

    if (match(parser, TOKEN_IF)) return ParseIfStatement(parser);
    if (match(parser, TOKEN_WHILE)) return ParseWhileStatement(parser);
    if (match(parser, TOKEN_FOR)) return ParseForStatement(parser);
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
    ASTNode* expr = ParseAssignmentExpression(parser);
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
