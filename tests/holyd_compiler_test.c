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

/* Source the parser must reject. Before ParseProgram grew an error flag it
 * printed a diagnostic and handed back an AST anyway, so broken files still
 * reached the compiler and "passed". */
static int expect_parse_failure(const char* name, const char* source) {
    Parser parser;
    ParserInit(&parser, source);
    if (ParseProgram(&parser) != NULL) {
        printf("[holyd] expected parse failure, got an AST: %s\n", name);
        return 0;
    }
    printf("[holyd] ok (rejected): %s\n", name);
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

    /* Each check divides by a comparison: if the comparison is false the VM
     * traps on division by zero and run_case fails. */
    ok &= run_case("precedence",
        "I64 i = 5;\n"
        "I64 n = 4;\n"
        "I64 lt = i < n + 1;\n"          /* 5 < 5 -> 0, not (5 < 4) + 1 -> 1 */
        "I64 c1 = 1 / (lt == 0);\n"
        "I64 mul = 1 + 2 * 3;\n"         /* 7 */
        "I64 c2 = 1 / (mul == 7);\n"
        "I64 eq = 1 == 2 - 1;\n"         /* 1 == 1 -> 1, not (1 == 2) - 1 -> -1 */
        "I64 c3 = 1 / (eq == 1);\n"
        "Print(\"precedence ok\\n\");\n");

    ok &= run_case("array assignment",
        "auto arr = [1, 2, 3];\n"
        "arr[1] = 20;\n"
        "arr[0]++;\n"
        "I64 c1 = 1 / (arr[0] == 2);\n"
        "I64 c2 = 1 / (arr[1] == 20);\n"
        "I64 c3 = 1 / (arr[2] == 3);\n"
        "Print(\"array assignment ok\\n\");\n");

    ok &= expect_parse_failure("missing initializer",
        "I64 x = ;\n");

    ok &= expect_parse_failure("unknown property",
        "auto a = [1, 2];\n"
        "Print(a.nope);\n");

    ok &= run_case("braceless bodies",
        "I64 hits = 0;\n"
        "I64 x = 0;\n"
        "if (x == 1) hits = hits + 100;\n"
        "hits = hits + 1;\n"        /* not part of the if body */
        "I64 c1 = 1 / (hits == 1);\n"
        "if (x == 0) hits = hits + 10; else hits = hits + 200;\n"
        "I64 c2 = 1 / (hits == 11);\n"
        "I64 n = 0;\n"
        "while (n < 3) n++;\n"
        "I64 c3 = 1 / (n == 3);\n"
        "Print(\"braceless ok\\n\");\n");

    ok &= run_case("else if chain",
        "I64 g = 2;\n"
        "I64 r = 0;\n"
        "if (g == 1) r = 10; else if (g == 2) r = 20; else r = 30;\n"
        "I64 c1 = 1 / (r == 20);\n"
        "Print(\"else if ok\\n\");\n");

    ok &= expect_parse_failure("unterminated block",
        "if (1) {\n"
        "    Print(\"x\\n\");\n");

    /* Every literal here is binary-exact, so == is a fair check. */
    ok &= run_case("floats",
        "F64 a = 1.5;\n"
        "F64 b = 2.25;\n"
        "I64 c1 = 1 / (a + b == 3.75);\n"
        "I64 c2 = 1 / (a * 2 == 3.0);\n"      /* int promotes to float */
        "I64 c3 = 1 / (a < 2);\n"
        "I64 c4 = 1 / (10 / 4 == 2);\n"       /* int division stays integer */
        "I64 c5 = 1 / (10.0 / 4 == 2.5);\n"   /* float division does not */
        "Print(a + b); Print(\" \"); Print(3.0); Print(\" \"); Print(0 - 0.25);\n"
        "Print(\"\\n\");\n");

    ok &= expect_parse_failure("array slice",
        "auto a = [1, 2, 3];\n"
        "auto b = a[0..2];\n");

    ok &= expect_parse_failure("variadic parameters",
        "U0 Sum(...) { }\n");

    return ok ? 0 : 1;
}
