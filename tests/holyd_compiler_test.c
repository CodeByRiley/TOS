#include <include/stdio.h>

#include "compiler.h"
#include "parser/parser.h"

static int run_case(const char* name, const char* source) {
    Parser parser;
    ParserInit(&parser, source);
    ASTNode* ast = ParseProgram(&parser);
    if (!ast) {
        printf("[holyd] parse failed: %s\n", name);
        return 0;
    }

    HDProgram program;
    HDProgramInit(&program);
    if (!HDCompileProgram(ast, &program)) {
        printf("[holyd] compile failed: %s\n", name);
        return 0;
    }

    if (!HDRunProgram(&program)) {
        printf("[holyd] runtime failed: %s\n", name);
        return 0;
    }

    printf("[holyd] ok: %s\n", name);
    return 1;
}

int main(void) {
    int ok = 1;

    ok &= run_case("math",
        "I64 a = 10;\n"
        "I64 b = 20;\n"
        "auto sum = a + b;\n"
        "auto product = a * b;\n"
        "auto complex = (a + b) * 2 - 5;\n"
        "Print(\"sum=\"); Print(sum); Print(\"\\n\");\n"
        "Print(\"product=\"); Print(product); Print(\"\\n\");\n"
        "Print(\"complex=\"); Print(complex); Print(\"\\n\");\n");

    ok &= run_case("strings",
        "auto first = \"Terry\";\n"
        "auto last = \"Davis\";\n"
        "auto full = first ~ \" \" ~ last;\n"
        "Print(full); Print(\"\\n\");\n");

    ok &= run_case("conditionals",
        "I64 age = 25;\n"
        "if (age >= 18) { Print(\"adult\\n\"); } else { Print(\"minor\\n\"); }\n"
        "auto score = 150;\n"
        "if (score == 100) { Print(\"perfect\\n\"); } else { if (score > 100) { Print(\"high\\n\"); } }\n");

    ok &= run_case("arrays",
        "auto numbers = [10, 20, 30, 40, 50];\n"
        "I64 sum = 0;\n"
        "foreach (n; numbers) { sum = sum + n; }\n"
        "Print(\"sum=\"); Print(sum); Print(\"\\n\");\n");

    ok &= run_case("functions",
        "U0 Greet(I64 age) {\n"
        "    Print(\"age=\"); Print(age); Print(\"\\n\");\n"
        "}\n"
        "I64 Add(I64 a, I64 b) {\n"
        "    return a + b;\n"
        "}\n"
        "Greet(25);\n"
        "auto result = Add(10, 20);\n"
        "Print(result); Print(\"\\n\");\n");

    ok &= run_case("no_semis",
        "I64 x = 10\n"
        "I64 y = 20\n"
        "if (x < y) {\n"
        "    Print(\"x<y\\n\")\n"
        "}\n"
        "auto nums = [1, 2, 3]\n"
        "foreach (n; nums) {\n"
        "    Print(n)\n"
        "}\n");

    ok &= run_case("holyc_d_style",
        "module demo;\n"
        "import std.stdio;\n"
        "U0 Banner() {\n"
        "    \"HolyC-style string statement\\n\";\n"
        "}\n"
        "void main() {\n"
        "    I64[] nums = [0x10, 20, 30];\n"
        "    long total = 0;\n"
        "    foreach (i, I64 n; nums) {\n"
        "        writeln(\"nums[\", i, \"] = \", n);\n"
        "        total = total + nums[i];\n"
        "    }\n"
        "    nums = nums ~ [40, 50];\n"
        "    for (I64 j = 3; j < nums.length; j++) {\n"
        "        total = total + nums[j];\n"
        "    }\n"
        "    I64 countdown = 2;\n"
        "    while (countdown > 0) {\n"
        "        countdown--;\n"
        "    }\n"
        "    Banner;\n"
        "    writeln(\"len=\", nums.length, \" total=\", total, \" countdown=\", countdown);\n"
        "}\n");

    return ok ? 0 : 1;
}
