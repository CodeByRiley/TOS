#ifndef PROCESS_H
#define PROCESS_H

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 512

void user_stack_alloc(void);
long process_exec(const char *path, char *const argv[]);

#endif /* PROCESS_H */
