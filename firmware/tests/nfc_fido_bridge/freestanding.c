#include <string.h>

__attribute__((weak)) void bridge_test_memory_operation(size_t length) {
    (void)length;
}

void *memcpy(void *destination, const void *source, size_t length) {
    bridge_test_memory_operation(length);
    unsigned char *to = destination;
    const unsigned char *from = source;
    for (size_t i = 0; i < length; i++) to[i] = from[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    bridge_test_memory_operation(length);
    unsigned char *to = destination;
    const unsigned char *from = source;
    if (to < from) {
        for (size_t i = 0; i < length; i++) to[i] = from[i];
    } else {
        while (length != 0u) {
            length--;
            to[length] = from[length];
        }
    }
    return destination;
}

void *memset(void *destination, int value, size_t length) {
    bridge_test_memory_operation(length);
    unsigned char *to = destination;
    for (size_t i = 0; i < length; i++) to[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    bridge_test_memory_operation(length);
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
