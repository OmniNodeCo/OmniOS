/* kernel/types.h */
#ifndef TYPES_H
#define TYPES_H

// ============================================================================
// CORE TYPES (essential for freestanding mode - no glibc/headers needed)
// ============================================================================

// NULL for pointers (critical for C - no <stddef.h> needed)
#define NULL ((void*)0)

// Standard C size types (safe for 32-bit x86)
typedef unsigned int size_t;
typedef int ssize_t;  // Signed size_t (used in some APIs)

// Fixed-width integers (replace stdint.h)
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef signed   char      int8_t;
typedef signed   short     int16_t;
typedef signed   int       int32_t;

// Boolean (if you want bool later)
// typedef int _Bool; // GCC handles this automatically in C99+, so usually skipped
// #define true 1
// #define false 0

// ============================================================================
// UTILITY MACROS & INLINES (safe, no headers needed)
// ============================================================================

// MIN/MAX macros (use with caution: side-effects if argument has ++/-- )
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

// File handle type (used by file system)
typedef int file_t;

// ============================================================================
// SAFE STRING FUNCTIONS (handwritten - no <string.h> needed)
// ============================================================================

// strncpy: Copy up to n bytes from src to dest, pad with nulls
// - Always writes n bytes (null-pads if src is shorter)
// - Does NOT null-terminate if src >= n (like standard strncpy)
static inline void strncpy(char *dest, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dest[i] = src[i];
        if (src[i] == '\0') {
            // Pad remaining with nulls (this is a GNU extension deviation, but safe)
            for (size_t j = i + 1; j < n; j++) {
                dest[j] = '\0';
            }
            return;
        }
    }
}

// strncmp: Compare up to n bytes of s1 and s2
// - Returns <0, 0, or >0
// - Stops early if null terminator is hit in both strings
static inline int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

#endif // TYPES_H
