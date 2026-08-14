#ifndef HOLYD_LEXER_H
#define HOLYD_LEXER_H

#include <stddef.h>

// All the types of tokens our language understands
typedef enum {
    TOKEN_EOF,          // End of file
    TOKEN_NEWLINE,      // \n (useful for REPL)

    // Literals and Identifiers
    TOKEN_NUMBER,       // 123
    TOKEN_STRING,       // "Hello"
    TOKEN_IDENTIFIER,   // variable names like x, Print, myVar

    // Holy D Keywords
    TOKEN_U0,           // U0
    TOKEN_I64,          // I64
    TOKEN_U32,          // U32
    TOKEN_F64,          // F64
    TOKEN_AUTO,         // auto
    TOKEN_FOREACH,      // foreach
    TOKEN_IF,           // if
    TOKEN_ELSE,         // else
    TOKEN_WHILE,        // while
    TOKEN_RETURN,       // return

    // Operators
    TOKEN_ASSIGN,       // =
    TOKEN_PLUS,         // +
    TOKEN_MINUS,        // -
    TOKEN_STAR,         // *
    TOKEN_SLASH,        // /
    TOKEN_TILDE,        // ~ (D-style concatenation)
    TOKEN_AMPERSAND,    // & (address-of)
    TOKEN_BANG,         // !

    TOKEN_EQEQ,         // ==
    TOKEN_NEQ,          // !=
    TOKEN_LT,           // <
    TOKEN_GT,           // >
    TOKEN_LTEQ,         // <=
    TOKEN_GTEQ,         // >=

    // Punctuation
    TOKEN_SEMICOLON,    // ;
    TOKEN_COLON,        // :
    TOKEN_COMMA,        // ,
    TOKEN_LPAREN,       // (
    TOKEN_RPAREN,       // )
    TOKEN_LBRACE,       // {
    TOKEN_RBRACE,       // }
    TOKEN_LBRACKET,     // [
    TOKEN_RBRACKET,     // ]

    TOKEN_UNKNOWN       // Anything we don't recognize
} TokenType;

typedef struct {
    TokenType type;
    const char* start;  // Pointer to the start of the token in source
    int length;         // Length of the token
} Token;

typedef struct {
    const char* start;  // Start of the whole source code
    const char* current; // Current scanning position
    int line;           // Current line number (for error messages)
} Lexer;

void LexerInit(Lexer* lexer, const char* source);
Token LexerNextToken(Lexer* lexer);

// Helper to print tokens (for debugging)
const char* TokenTypeToString(TokenType type);

#endif
