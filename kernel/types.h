// kernel/types.h
#ifndef TYPES_H
#define TYPES_H

// Required for freestanding mode (-ffreestanding -nostdinc)
#define NULL ((void*)0)
typedef unsigned int size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;

// 🔥 Add MIN helper (for strncpy safety)
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

// 🔥 Handwritten strncpy (safe, no headers needed)
// Copies up to n bytes from src to dest, pads with nulls
static inline void strncpy(char *dest, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dest[i] = src[i];
        if (src[i] == '\0') {
            // Pad remaining with nulls
            for (size_t j = i + 1; j < n; j++) {
                dest[j] = '\0';
            }
            return;
        }
    }
}

// 🔥 Handwritten strncmp (safe, no headers needed)
// Compares up to n bytes of s1 and s2
static inline int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

#endif // TYPES_H
