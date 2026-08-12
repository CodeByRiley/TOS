#ifndef ASSERT_H
#define ASSERT_H

/* If you have a panic function, you can declare it here */
// extern void panic(const char *msg);

/* In a freestanding environment, we define assert to halt if it fails */
#define assert(x) \
    do { \
        if (!(x)) { \
            /* panic("Assertion failed: " #x); */ \
            while (1) { /* Halt CPU */ } \
        } \
    } while (0)

#endif /* ASSERT_H */
