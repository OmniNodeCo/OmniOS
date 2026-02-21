/* kernel/video.h */
#ifndef VIDEO_H
#define VIDEO_H

#include "types.h"   // ← Use our own types.h

void clear_screen(void);
void print_char(char c);
void print_str(const char *s);

#endif // VIDEO_H
