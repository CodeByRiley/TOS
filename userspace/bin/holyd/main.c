/* Holy D Compiler
 * A recreation of Terry A. Davis' (Rest in Peace) Holy C compiler.
 * For the D programming language.
 */

#include "compiler.h"
#include "eval.h"
#include "parser/parser.h"
#include <lib/syscall.h>   /* readdir_path: TOS-only, not in libc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BUF_SIZE 1024

/* FLAGS

        This block is the design intent for the full hcc-style CLI. What is
  actually wired up is whatever print_usage() lists; every other flag below
  is rejected with the reason it cannot work yet. Keep the two in sync as
  flags land.

        Immediate Your File: -run
        Immediately run your code. This is smoke and mirrors, under the hood it
  will create the assembly file, use gcc to assemble it and then run the file.
  So is more of a convenience.

  ----------------------------------
        Create Control Flow Graph: -cfg
        Creates control flow graph of your code as a .dot file that can be used
  by graphviz to create a graphical representation of the flow of your program.

  ----------------------------------

        -cfg-png
        Immediately creates a .png of your program using graphviz. You must have
  graphviz installed for this to work correctly.

  ----------------------------------

        -cfg-svg
        Immediately creates a .svg of your program using graphviz. You must have
  graphviz installed for this to work correctly.

  ----------------------------------

        Print tokens: -tokens
        Prints all of the tokens from your program to stdout

  ----------------------------------


        Create Assembly File: -S
        Create assembly code and write to a file.

  ----------------------------------

        If you are wanting to then compile the assembly use the following:

        gcc -lsqlite3 -ltos -I/usr/local/include -L/usr/local/lib ./<file>.s

        Presently the libraries are created as a dynamic library which are then
  linked at compile time.

        Create Object File: -obj
        Creates an object file.

  ----------------------------------


        Create Library: -lib
        Creates a dynamic library and shared object file from your code,
  treating it as position independent. You cannot have a Main or main function
  nor can you have any top level executing code in your file.

  ----------------------------------

        Link C-Libraries: -clibs
        Links in c libraries, you have to declare the function prototypes
  manually in your code. Note that there are no F32's yet so libraries requiring
  float will need you to figure out a workaround.

  ----------------------------------

        Rename Binary: -o
        Change the output name of your executable from the default a.out

  ----------------------------------

        Define Preprocessor Variable: -D<var>
        Define a #define, this does not yet accept a value.

  ----------------------------------

        Print Help: --help
        Display all the above options with a small description
 */

int check_ext(const char *filename) {
  const char *ext = strrchr(filename, '.');
  return ext && strcmp(ext, ".hd") == 0;
}

char *read_file(const char *filename) {
  FILE *file =
      fopen(filename, "rb"); // Use binary mode for accurate byte counts
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

// Returns a packed buffer of filenames (each null-terminated, double-null at
// end). Caller must free() this buffer when done.
char *get_test_files(const char *dir) {
  unsigned index = 0;
  char buf[1024];
  long bytes_read;

  size_t total_capacity = 4096;
  char *out_buf = malloc(total_capacity);
  if (!out_buf)
    return NULL;

  size_t out_len = 0;

  while ((bytes_read = readdir_path(dir, &index, buf, sizeof(buf) - 1)) > 0) {
    buf[bytes_read] = '\0'; // Null-terminate what we just read

    for (long i = 0; i < bytes_read;) {
      if (buf[i] == '\0') {
        i++;
        continue;
      }

      char *filename = &buf[i];
      size_t len = strlen(filename);

      /* Append .hd names to a NUL-separated list. */
      if (len >= 3 && strcmp(filename + len - 3, ".hd") == 0) {
        if (out_len + len + 2 > total_capacity) {
          total_capacity *= 2;
          char *new_buf = realloc(out_buf, total_capacity);
          if (!new_buf) {
            free(out_buf);
            return NULL;
          }
          out_buf = new_buf;
        }

        strcpy(out_buf + out_len, filename);
        out_len += len + 1;
      }

      i += len; // Skip to the end of the current filename
    }
  }

  // Double null-terminate the end of the buffer so the consumer knows to stop
  if (out_len + 1 > total_capacity) {
    out_buf = realloc(out_buf, out_len + 1);
  }
  out_buf[out_len] = '\0';

  return out_buf;
}

int test(void) {
  int success = 0, failure = 0;

  /* NUL-separated .hd paths. */
  char *files = get_test_files("holyd/tests");
  if (files == NULL || files[0] == '\0') {
    printf("holyd: no .hd test files found in holyd/tests/\n");
    if (files)
      free(files);
    return 1;
  }

  char *current = files;
  while (*current != '\0') {
    // Construct the full path: "holyd/tests/filename.hd"
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "holyd/tests/%s", current);

    printf("holyd: running %s... ", full_path);
    fflush(stdout);

    // Use full_path instead of current
    char *source = read_file(full_path);
    if (source == NULL) {
      printf("holyd: failed to read %s\n", full_path);
      failure++;
      current += strlen(current) + 1; // Move to next string
      continue;
    }

    Parser parser;
    ParserInit(&parser, source);
    ASTNode *program = ParseProgram(&parser);

    if (program == NULL) {
      printf("holyd: failed to parse %s\n", full_path);
      failure++;
    } else {
      HDProgram bytecode;
      HDProgramInit(&bytecode);

      if (!HDCompileProgram(program, &bytecode)) {
        printf("holyd: failed to compile %s\n", full_path);
        failure++;
      } else if (!HDRunProgram(&bytecode)) {
        printf("holyd: failed to run %s\n", full_path);
        failure++;
      } else {
        printf("holyd: passed %s\n", full_path);
        success++;
      }
    }

    free(source);
    current += strlen(current) + 1; // Move to next string
  }

  free(files);

  printf("\n--- Results ---\n");
  printf("holyd: success: %d, Failure: %d\n", success, failure);
  return failure > 0 ? 1 : 0;
}

static void print_usage(void) {
  printf("Usage: holyd [options] <source.hd>\n");
  printf("       holyd --test\n");
  printf("\nImplemented:\n");
  printf("  %-16s %s\n", "--help", "Show this list and exit");
  printf("  %-16s %s\n", "--test", "Run every .hd file under holyd/tests/");
  printf("  %-16s %s\n", "-tokens", "Print the token stream and exit");
  printf("  %-16s %s\n", "--interpret", "Walk the AST instead of running bytecode");
  printf("  %-16s %s\n", "--dump-bytecode", "Disassemble the program before running it");
  printf("\nNot implemented yet:\n");
  printf("  %-16s %s\n", "-run -S -obj", "need a native code backend");
  printf("  %-16s %s\n", "-lib -clibs -o", "need a native code backend");
  printf("  %-16s %s\n", "-D<var>", "needs a preprocessor");
  printf("  %-16s %s\n", "-cfg", "control flow graphs are not built yet");
  printf("  %-16s %s\n", "-cfg-png", "not built, and needs graphviz on the host");
  printf("  %-16s %s\n", "-cfg-svg", "not built, and needs graphviz on the host");
}

// Why a documented flag still cannot run. NULL means we do not know the flag
// at all, which is a different message.
static const char *unimplemented_reason(const char *arg) {
  static const char *needs_backend[] = {"-run", "-S",     "-obj",
                                        "-lib", "-clibs", "-o"};
  for (unsigned i = 0; i < sizeof(needs_backend) / sizeof(needs_backend[0]); i++) {
    if (strcmp(arg, needs_backend[i]) == 0) {
      return "it needs a native code backend";
    }
  }

  if (strncmp(arg, "-D", 2) == 0) {
    return "it needs a preprocessor";
  }
  if (strcmp(arg, "-cfg") == 0) {
    return "control flow graphs are not built yet";
  }
  if (strcmp(arg, "-cfg-png") == 0 || strcmp(arg, "-cfg-svg") == 0) {
    return "control flow graphs are not built yet, and these also need graphviz";
  }
  return NULL;
}

// Runs the lexer alone, so this still works on a file the parser rejects.
static void dump_tokens(const char *source) {
  Lexer lexer;
  LexerInit(&lexer, source);

  for (;;) {
    Token token = LexerNextToken(&lexer);

    // A newline token would otherwise break the one-token-per-line layout,
    // and EOF has no text to show at all.
    if (token.type == TOKEN_NEWLINE) {
      printf("%4d  %-16s '\\n'\n", lexer.line, TokenTypeToString(token.type));
    } else if (token.type == TOKEN_EOF) {
      printf("%4d  %-16s\n", lexer.line, TokenTypeToString(token.type));
      break;
    } else {
      printf("%4d  %-16s '%.*s'\n", lexer.line, TokenTypeToString(token.type),
             token.length, token.start);
    }
  }
}

int main(int argc, char **argv) {
  int use_interpreter = 0;
  int dump_bytecode = 0;
  int tokens_only = 0;
  int run_tests = 0;
  const char *source_path = NULL;

  if (argc < 2) {
    print_usage();
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0) {
      print_usage();
      return 0;
    }
    if (strcmp(arg, "--test") == 0) {
      run_tests = 1;
      continue;
    }
    if (strcmp(arg, "--interpret") == 0) {
      use_interpreter = 1;
      continue;
    }
    if (strcmp(arg, "--dump-bytecode") == 0) {
      dump_bytecode = 1;
      continue;
    }
    if (strcmp(arg, "-tokens") == 0) {
      tokens_only = 1;
      continue;
    }

    // Anything else starting with '-' is a flag we do not run. Saying so beats
    // the old behaviour, where an unknown flag was quietly taken as the source
    // path and then overwritten by the real one.
    if (arg[0] == '-') {
      const char *reason = unimplemented_reason(arg);
      if (reason != NULL) {
        printf("holyd: '%s' is not implemented: %s\n", arg, reason);
      } else {
        printf("holyd: unknown option '%s'\n", arg);
      }
      printf("Run 'holyd --help' for the full list.\n");
      return 1;
    }

    if (source_path != NULL) {
      printf("holyd: one source file at a time (already have '%s')\n",
             source_path);
      return 1;
    }
    source_path = arg;
  }

  if (run_tests) {
    return test();
  }

  if (source_path == NULL) {
    print_usage();
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

  if (tokens_only) {
    dump_tokens(source);
    free(source);
    return 0;
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
