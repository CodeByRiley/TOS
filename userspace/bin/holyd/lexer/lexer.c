#include "lexer.h"
#include <stddef.h>

// --- Freestanding helpers (no libc needed) ---
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int string_match(const char* a, const char* b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

void LexerInit(Lexer* lexer, const char* source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    return token;
}

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (*lexer->current == '\0') return '\0';
    return lexer->current[1];
}

static int match(Lexer* lexer, char expected) {
    if (*lexer->current == expected) {
        lexer->current++;
        return 1;
    }
    return 0;
}

static void skip_whitespace(Lexer* lexer) {
    while (1) {
        char c = peek(lexer);
        if (is_space(c)) {
            advance(lexer);
        } else if (c == '\n') {
            lexer->line++;
            advance(lexer);
        } else if (c == '/' && peek_next(lexer) == '/') {
            // Skip line comments
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else if (c == '/' && peek_next(lexer) == '*') {
            advance(lexer);
            advance(lexer);
            while (!(peek(lexer) == '*' && peek_next(lexer) == '/') && peek(lexer) != '\0') {
                if (peek(lexer) == '\n') lexer->line++;
                advance(lexer);
            }
            if (peek(lexer) == '*') {
                advance(lexer);
                advance(lexer);
            }
        } else {
            break;
        }
    }
}

static Token string(Lexer* lexer) {
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }

    if (peek(lexer) == '\0') {
        /* Unterminated strings are invalid tokens. */
        return make_token(lexer, TOKEN_UNKNOWN);
    }

    // Consume the closing quote
    advance(lexer);
    return make_token(lexer, TOKEN_STRING);
}

static Token number(Lexer* lexer) {
    if (lexer->start[0] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
        advance(lexer);
        while (is_hex_digit(peek(lexer))) {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_NUMBER);
    }

    while (is_digit(peek(lexer))) {
        advance(lexer);
    }

    /* A '.' only opens a fraction when a digit follows. That keeps the
     * number short in `1..5` (a range) and `1.length` (a property), which
     * would otherwise be swallowed as part of the literal. */
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer);
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
        return make_token(lexer, TOKEN_FLOAT);
    }

    return make_token(lexer, TOKEN_NUMBER);
}

static Token identifier(Lexer* lexer) {
    while (is_alnum(peek(lexer))) {
        advance(lexer);
    }

    int length = (int)(lexer->current - lexer->start);

    switch (length) {
      case 2:
        if (string_match(lexer->start, "U0", 2)) return make_token(lexer, TOKEN_U0);
        if (string_match(lexer->start, "U8", 2)) return make_token(lexer, TOKEN_U8);
        if (string_match(lexer->start, "I8", 2)) return make_token(lexer, TOKEN_I8);
        if (string_match(lexer->start, "if", 2)) return make_token(lexer, TOKEN_IF);

        break;
      case 3:
        if (string_match(lexer->start, "I16", 3)) return make_token(lexer, TOKEN_I16);
        if (string_match(lexer->start, "U16", 3)) return make_token(lexer, TOKEN_U16);
        if (string_match(lexer->start, "I32", 3)) return make_token(lexer, TOKEN_I32);
        if (string_match(lexer->start, "I64", 3)) return make_token(lexer, TOKEN_I64);
        if (string_match(lexer->start, "U32", 3)) return make_token(lexer, TOKEN_U32);
        if (string_match(lexer->start, "U64", 3)) return make_token(lexer, TOKEN_U64);
        if (string_match(lexer->start, "F64", 3)) return make_token(lexer, TOKEN_F64);
        if (string_match(lexer->start, "int", 3)) return make_token(lexer, TOKEN_INT);
        if (string_match(lexer->start, "for", 3)) return make_token(lexer, TOKEN_FOR);
        break;
      case 4:
        if (string_match(lexer->start, "void", 4)) return make_token(lexer, TOKEN_VOID);
        if (string_match(lexer->start, "uint", 4)) return make_token(lexer, TOKEN_UINT);
        if (string_match(lexer->start, "true", 4)) return make_token(lexer, TOKEN_TRUE);
        if (string_match(lexer->start, "long", 4)) return make_token(lexer, TOKEN_LONG);
        if (string_match(lexer->start, "auto", 4)) return make_token(lexer, TOKEN_AUTO);
        if (string_match(lexer->start, "bool", 4)) return make_token(lexer, TOKEN_BOOL);
        if (string_match(lexer->start, "else", 4)) return make_token(lexer, TOKEN_ELSE);
        break;
      case 5:
        if (string_match(lexer->start, "ulong", 5)) return make_token(lexer, TOKEN_ULONG);
        if (string_match(lexer->start, "while", 5)) return make_token(lexer, TOKEN_WHILE);
        if (string_match(lexer->start, "false", 5)) return make_token(lexer, TOKEN_FALSE);
        break;
      case 6:
        if (string_match(lexer->start, "double", 6)) return make_token(lexer, TOKEN_DOUBLE);
        if (string_match(lexer->start, "string", 6)) return make_token(lexer, TOKEN_STRING_TYPE);
        if (string_match(lexer->start, "return", 6)) return make_token(lexer, TOKEN_RETURN);
        if (string_match(lexer->start, "module", 6)) return make_token(lexer, TOKEN_MODULE);
        if (string_match(lexer->start, "import", 6)) return make_token(lexer, TOKEN_IMPORT);
        break;
      case 7:
        if (string_match(lexer->start, "foreach", 7)) return make_token(lexer, TOKEN_FOREACH);
        break;
    }

    return make_token(lexer, TOKEN_IDENTIFIER);
}

Token LexerNextToken(Lexer* lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (peek(lexer) == '\0') return make_token(lexer, TOKEN_EOF);

    char c = advance(lexer);

    if (c == '\n') {
        lexer->line++;
        // Consume any consecutive newlines so we only emit ONE TOKEN_NEWLINE
        while (peek(lexer) == '\n') {
            lexer->line++;
            advance(lexer);
        }
        return make_token(lexer, TOKEN_NEWLINE);
    }

    if (is_digit(c)) return number(lexer);
    if (is_alpha(c)) return identifier(lexer);

    switch (c) {
        // case '\\': return make_token(lexer, TOKEN_BACKSLASH);
        // case '"': return make_token(lexer, TOKEN_QUOTE);
        case '(': return make_token(lexer, TOKEN_LPAREN);
        case ')': return make_token(lexer, TOKEN_RPAREN);
        case '{': return make_token(lexer, TOKEN_LBRACE);
        case '}': return make_token(lexer, TOKEN_RBRACE);
        case '[': return make_token(lexer, TOKEN_LBRACKET);
        case ']': return make_token(lexer, TOKEN_RBRACKET);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case ':': return make_token(lexer, TOKEN_COLON);
        case ',': return make_token(lexer, TOKEN_COMMA);
        case '.':
            /* Longest run wins: '...' before '..' before a lone '.'. */
            if (peek(lexer) == '.' && peek_next(lexer) == '.') {
                advance(lexer);
                advance(lexer);
                return make_token(lexer, TOKEN_DOTDOTDOT);
            }
            if (peek(lexer) == '.') {
                advance(lexer);
                return make_token(lexer, TOKEN_DOTDOT);
            }
            return make_token(lexer, TOKEN_DOT);
        case '+': return make_token(lexer, match(lexer, '+') ? TOKEN_PLUSPLUS : TOKEN_PLUS);
        case '-': return make_token(lexer, match(lexer, '-') ? TOKEN_MINUSMINUS : TOKEN_MINUS);
        case '*': return make_token(lexer, TOKEN_STAR);
        case '/': return make_token(lexer, TOKEN_SLASH);
        case '~': return make_token(lexer, TOKEN_TILDE);
        case '&': return make_token(lexer, TOKEN_AMPERSAND);
        case '!': return make_token(lexer, match(lexer, '=') ? TOKEN_NEQ : TOKEN_BANG);
        case '=': return make_token(lexer, match(lexer, '=') ? TOKEN_EQEQ : TOKEN_ASSIGN);
        case '<': return make_token(lexer, match(lexer, '=') ? TOKEN_LTEQ : TOKEN_LT);
        case '>': return make_token(lexer, match(lexer, '=') ? TOKEN_GTEQ : TOKEN_GT);
        case '"': return string(lexer);
    }

    return make_token(lexer, TOKEN_UNKNOWN);
}

const char* TokenTypeToString(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_STRING: return "STRING";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_U0: return "U0";
        case TOKEN_I8: return "I8";
        case TOKEN_U8: return "U8";
        case TOKEN_I16: return "I16";
        case TOKEN_U16: return "U16";
        case TOKEN_I32: return "I32";
        case TOKEN_I64: return "I64";
        case TOKEN_U32: return "U32";
        case TOKEN_U64: return "U64";
        case TOKEN_F64: return "F64";
        case TOKEN_VOID: return "VOID";
        case TOKEN_INT: return "INT";
        case TOKEN_UINT: return "UINT";
        case TOKEN_LONG: return "LONG";
        case TOKEN_ULONG: return "ULONG";
        case TOKEN_DOUBLE: return "DOUBLE";
        case TOKEN_BOOL: return "BOOL";
        case TOKEN_STRING_TYPE: return "STRING_TYPE";
        case TOKEN_AUTO: return "AUTO";
        case TOKEN_FOREACH: return "FOREACH";
        case TOKEN_FOR: return "FOR";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_TRUE: return "TRUE";
        case TOKEN_FALSE: return "FALSE";
        case TOKEN_MODULE: return "MODULE";
        case TOKEN_IMPORT: return "IMPORT";
        case TOKEN_ASSIGN: return "ASSIGN";
        case TOKEN_PLUSPLUS: return "PLUSPLUS";
        case TOKEN_MINUSMINUS: return "MINUSMINUS";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_TILDE: return "TILDE";
        case TOKEN_AMPERSAND: return "AMPERSAND";
        case TOKEN_BANG: return "BANG";
        case TOKEN_EQEQ: return "EQEQ";
        case TOKEN_NEQ: return "NEQ";
        case TOKEN_LT: return "LT";
        case TOKEN_GT: return "GT";
        case TOKEN_LTEQ: return "LTEQ";
        case TOKEN_GTEQ: return "GTEQ";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COLON: return "COLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_DOT: return "DOT";
        case TOKEN_DOTDOT: return "DOTDOT";
        case TOKEN_DOTDOTDOT: return "DOTDOTDOT";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_UNKNOWN: return "UNKNOWN";
        default: return "UNHANDLED";
    }
}
