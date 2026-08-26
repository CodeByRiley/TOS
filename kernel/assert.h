#ifndef ASSERT_H
#define ASSERT_H

/* TODO: Route assertion failures through panic(). */
#define assert(x) \
    do { \
        if (!(x)) { \
            while (1) { } \
        } \
    } while (0)

#endif /* ASSERT_H */
