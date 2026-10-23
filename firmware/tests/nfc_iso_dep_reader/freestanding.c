#include <string.h>

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *to = destination;
    const unsigned char *from = source;
    for (size_t i = 0; i < length; i++) to[i] = from[i];
    return destination;
}

void *memset(void *destination, int value, size_t length) {
    unsigned char *to = destination;
    for (size_t i = 0; i < length; i++) to[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
