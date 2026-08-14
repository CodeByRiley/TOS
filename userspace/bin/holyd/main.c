/* Holy D Compiler
 * A recreation of Terry A. Davis' (Rest in Peace) Holy C compiler.
 * For the D programming language.
 */

#include <include/stdio.h>
#include <include/stdlib.h>
#include <include/string.h>
#include "parser/parser.h"
#include "eval.h"
#include "compiler.h"

int check_ext(const char *filename) {
    const char *ext = strrchr(filename, '.');
    return ext && strcmp(ext, ".hd") == 0;
}

char *read_file(const char *filename) {
    FILE *file = fopen(filename, "rb"); // Use binary mode for accurate byte counts
    if (file == NULL) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        return NULL;
    }

    char *content = malloc(size + 1);
    if (content == NULL) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(content, 1, size, file);
    content[bytes_read] = '\0'; // Null-terminate at actual read position

    fclose(file);
    return content;
}

int test(void) {
    const char *source = "I64 x = 5;\n"
                         "Print(\"Hello Holy D!\");\n"
                         "auto y = 10 + x;\n"
                         "if (x < y) { Print(\"x is less!\"); }\n";

    Parser parser;
    ParserInit(&parser, source);
    ASTNode *program = ParseProgram(&parser);

    if (program != NULL) {
        printf("[Test] Parsed successfully! Found %d top-level statements.\n",
               program->stmt_count);
        HDProgram bytecode;
        HDProgramInit(&bytecode);
        if (!HDCompileProgram(program, &bytecode)) {
            printf("[Test] Compile failed.\n");
            return 1;
        }
        printf("[Test] Running compiled bytecode...\n");
        if (!HDRunProgram(&bytecode)) {
            printf("[Test] Runtime failed.\n");
            return 1;
        }
    } else {
        printf("[Test] Parse error!\n");
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: holyd [--interpret] [--dump-bytecode] <source.hd>\n");
        printf("       holyd --test\n");
        return 1;
    }

    if (strcmp(argv[1], "--test") == 0) {
        return test();
    }

    int use_interpreter = 0;
    int dump_bytecode = 0;
    const char* source_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interpret") == 0) {
            use_interpreter = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode = 1;
        } else {
            source_path = argv[i];
        }
    }

    if (source_path == NULL) {
        printf("Usage: holyd [--interpret] [--dump-bytecode] <source.hd>\n");
        return 1;
    }

    // validate Extension
    if (!check_ext(source_path)) {
        printf("Error: File '%s' must have .hd extension\n", source_path);
        return 1;
    }

    char *source = read_file(source_path);
    if (source == NULL) {
        printf("Error: Could not open or read file '%s'\n", source_path);
        return 1;
    }

    Parser parser;
    ParserInit(&parser, source);
    ASTNode *program = ParseProgram(&parser);

    if (program == NULL) {
        printf("Error: Failed to parse file.\n");
        free(source);
        return 1;
    }

    if (use_interpreter) {
        Environment env;
        EnvInit(&env);
        EvalNode(program, &env);
    } else {
        HDProgram bytecode;
        HDProgramInit(&bytecode);
        if (!HDCompileProgram(program, &bytecode)) {
            free(source);
            return 1;
        }
        if (dump_bytecode) {
            HDDumpProgram(&bytecode);
        }
        if (!HDRunProgram(&bytecode)) {
            free(source);
            return 1;
        }
    }

    free(source);
    return 0;
}
