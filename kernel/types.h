/* kernel/types.h */
#ifndef TYPES_H
#define TYPES_H

// Minimal types for freestanding OS (no system headers)
// NULL is (void*)0, size_t is unsigned int (safe for 32-bit)
#define NULL ((void*)0)
typedef unsigned int size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;

#endif // TYPES_H
