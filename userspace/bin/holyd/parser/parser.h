#ifndef HOLYD_PARSER_H
#define HOLYD_PARSER_H

#include "../lexer/lexer.h"
#include "../ast/ast.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
} Parser;

void ParserInit(Parser* parser, const char* source);
ASTNode* ParseProgram(Parser* parser);

#endif
