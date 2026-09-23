/*
 * OmniOS — os/lib/omni.h
 *
 * Shared declarations for the OmniOS userspace: common types, the
 * framebuffer (osfb), keyboard/mouse input, tiny 2-D software rasterizer
 * (raster), and the UTF-8/ASCII text drawing layer (canvas).
 *
 * Everything here links against musl (static) and talks to the Linux
 * kernel directly through /dev/fb0, /dev/tty and /dev/input/mice — there is
 * no X11/Wayland involvement.
 */
#ifndef OMNI_OS_LIB_OMNI_H
#define OMNI_OS_LIB_OMNI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Framebuffer (mmap of /dev/fb0)                                      */
/* ------------------------------------------------------------------ */

struct osfb {
    int fd;                    /* /dev/fb0 file descriptor            */
    uint32_t *mem;             /* mmap'd framebuffer (BGRA32)         */
    uint32_t  size;            /* bytes mapped                        */
    int       w, h;            /* visible width/height                */
    int       stride;          /* in pixels                           */
    int       depth;           /* bits per pixel                      */
    char     *name;            /* framebuffer id                      */
};

/* Open/mmap the framebuffer. Returns 0 on success, -1 on failure. */
int  osfb_open(struct osfb *fb, const char *dev);
void osfb_close(struct osfb *fb);

/* View the mapped framebuffer as a raster (BGRA32, possibly with stride). */
struct raster fb_raster(const struct osfb *fb);

/* Blocking wait for /dev/fb0 (kernel simpledrm can take a moment).  */
int  osfb_wait(const char *dev, int retries, int delay_ms);

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

/* Raw Linux event types (mirrors <linux/input.h>)                    */
#define OMNI_EV_KEY          0x01
#define OMNI_EV_REL          0x02
#define OMNI_KEY_RELEASED    0
#define OMNI_KEY_PRESSED     1
#define OMNI_KEY_REPEAT      2

/* Convenient key indices into the keyboard table (see omni_keys).    */
enum {
    OMNI_KEY_ESC = 1, OMNI_KEY_1, OMNI_KEY_2, OMNI_KEY_3, OMNI_KEY_4,
    OMNI_KEY_5, OMNI_KEY_6, OMNI_KEY_7, OMNI_KEY_8, OMNI_KEY_9, OMNI_KEY_0,
    OMNI_KEY_MINUS, OMNI_KEY_EQUAL, OMNI_KEY_BACKSPACE, OMNI_KEY_TAB,
    OMNI_KEY_Q, OMNI_KEY_W, OMNI_KEY_E, OMNI_KEY_R, OMNI_KEY_T, OMNI_KEY_Y,
    OMNI_KEY_U, OMNI_KEY_I, OMNI_KEY_O, OMNI_KEY_P, OMNI_KEY_LBRACE,
    OMNI_KEY_RBRACE, OMNI_KEY_ENTER, OMNI_KEY_LCTRL, OMNI_KEY_A, OMNI_KEY_S,
    OMNI_KEY_D, OMNI_KEY_F, OMNI_KEY_G, OMNI_KEY_H, OMNI_KEY_J, OMNI_KEY_K,
    OMNI_KEY_L, OMNI_KEY_SEMI, OMNI_KEY_QUOTE, OMNI_KEY_TILDE, OMNI_KEY_LSHIFT,
    OMNI_KEY_BACKSLASH, OMNI_KEY_Z, OMNI_KEY_X, OMNI_KEY_C, OMNI_KEY_V,
    OMNI_KEY_B, OMNI_KEY_N, OMNI_KEY_M, OMNI_KEY_COMMA, OMNI_KEY_DOT,
    OMNI_KEY_SLASH, OMNI_KEY_RSHIFT, OMNI_KEY_KPSTAR, OMNI_KEY_LALT,
    OMNI_KEY_SPACE, OMNI_KEY_CAPS, OMNI_KEY_F1, OMNI_KEY_F2, OMNI_KEY_F3,
    OMNI_KEY_F4, OMNI_KEY_F5, OMNI_KEY_F6, OMNI_KEY_F7, OMNI_KEY_F8,
    OMNI_KEY_F9, OMNI_KEY_F10, OMNI_KEY_NUMLOCK, OMNI_KEY_SCROLL,
    OMNI_KEY_KP7, OMNI_KEY_KP8, OMNI_KEY_KP9, OMNI_KEY_KPMINUS, OMNI_KEY_KP4,
    OMNI_KEY_KP5, OMNI_KEY_KP6, OMNI_KEY_KPPLUS, OMNI_KEY_KP1, OMNI_KEY_KP2,
    OMNI_KEY_KP3, OMNI_KEY_KP0, OMNI_KEY_KPDOT, OMNI_KEY_102ND, OMNI_KEY_F11,
    OMNI_KEY_F12, OMNI_KEY_UP = 103, OMNI_KEY_LEFT = 105,
    OMNI_KEY_RIGHT = 106, OMNI_KEY_DOWN = 108,
    OMNI_KEY_DELETE = 111
};

/* A single input unit, decoded from /dev/input/mice or /dev/tty raw. */
struct omni_input {
    int  type;                 /* 0 = none, 1 = key, 2 = motion, 3 = button */
    int  key;                  /* Linux keycode (buttons report too)       */
    int  pressed;              /* 0 released / 1 pressed / 2 repeat        */
    char text;                 /* printable char for type 1, else 0        */
    int  dx, dy;               /* relative motion                          */
};

/* Keyboard keyboard-index table: index -> (shifted, unshifted) chars. */
struct omni_key {
    char lo, hi;
};

/* reliable API: raw /dev/input + /dev/tty translation behind one call */
void omni_input_init(struct omni_input *devs, int count);
int  omni_input_next(struct omni_input *e);           /* -1 none, 0 ok  */
void omni_input_push_key(int key, int pressed, int shift, char ch);
void omni_input_push_mouse(int dx, int dy);
void omni_input_push_button(int btn, int pressed);

/* Device set: aggregated /dev/input/event*, /dev/input/mice, tty fallback. */
struct pollfd;

struct omni_devs {
    int mice_fd;               /* /dev/input/mice (PS/2 3-byte protocol)  */
    int ev_fd[8];              /* /dev/input/event0..7                    */
    int ev_n;
    int tty_fd;                /* /dev/tty raw-mode keyboard fallback     */
};

void omni_devs_open(struct omni_devs *d);
void omni_devs_close(struct omni_devs *d);
int  omni_devs_nfds(struct omni_devs *d);
void omni_devs_fill(struct omni_devs *d, struct pollfd *pfds);
/* pfds[idx] is readable: drain that device into the input queue.         */
int  omni_devs_drain(struct omni_devs *d, int idx);
/* write a message to the (opened or /dev/console) console                */
void omni_console_puts(const char *s);

/* keyboard helpers */
const struct omni_key *omni_kbd_table(void);
/* translate a raw 0..127 keycode + state to a printable char (0 none)  */
char omni_key_char(unsigned code, int shift);

/* tty helpers used by the kernel-core */
int  omni_tty_open(const char *path, int *in, int *out);
int  omni_tty_read_raw(int fd);
void omni_tty_write_all(int fd, const char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* Rasterizer                                                         */
/* ------------------------------------------------------------------ */

/* BGRA8888 color (bytes memory order: B G R A)                        */
struct omni_color {
    uint8_t b, g, r, a;
};

/* color helpers */
uint32_t omni_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t omni_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t omni_mix(uint32_t dst, uint32_t src); /* src over dst         */

struct raster {
    uint32_t *bits;
    int w, h, stride;
};

void     raster_init(struct raster *r, void *bits, int w, int h, int stride);
void     raster_clear(struct raster *r, uint32_t c);
void     raster_fill(struct raster *r, int x, int y, int w, int h, uint32_t c);
void     raster_rect(struct raster *r, int x, int y, int w, int h, uint32_t c);
void     raster_hline(struct raster *r, int x0, int x1, int y, uint32_t c);
void     raster_vline(struct raster *r, int x, int y0, int y1, uint32_t c);
void     raster_px(struct raster *r, int x, int y, uint32_t c);
void     raster_blit(struct raster *dst, const struct raster *src,
                     int dx, int dy);
void     raster_blend(struct raster *dst, const struct raster *src,
                      int dx, int dy);
void     raster_blit_clip(struct raster *dst, const struct raster *src,
                          int dx, int dy, int cx, int cy, int cw, int ch);
void     raster_scroll(struct raster *r, int dy, uint32_t fill);
void     raster_gradient_v(struct raster *r, int x, int y, int w, int h,
                           uint32_t top, uint32_t bottom);

/* ------------------------------------------------------------------ */
/* Text canvas (uses the embedded 8x8 public-domain font)             */
/* ------------------------------------------------------------------ */

struct canvas {
    struct raster r;           /* target                                */
    uint32_t fg, bg;           /* colors                                */
    int      cx, cy;           /* cursor x (pixels), y (pixels)         */
    int      cellw, cellh;     /* 8x8 + padding                         */
    int      clip_x, clip_y, clip_w, clip_h;  /* redraw region          */
    int      tab;              /* tab width in cells                    */
    int      linewrap;         /* wrap at clip edge                     */
};

void canvas_init(struct canvas *c, struct raster *r, uint32_t fg, uint32_t bg);
void canvas_set_clip(struct canvas *c, int x, int y, int w, int h);
void canvas_gotoxy(struct canvas *c, int col, int row);
void canvas_draw_char(struct canvas *c, uint32_t ch, int x, int y,
                      uint32_t fg, uint32_t bg);
void canvas_puts_raw(struct canvas *c, uint32_t ch);
/* draw UTF-8 text at (x,y); returns pixels advanced                     */
int  canvas_text(struct canvas *c, const char *s, int x, int y);
int  canvas_text_len(struct canvas *c, const char *s, int x, int y, int maxw);
void canvas_clear(struct canvas *c, uint32_t bg);
void canvas_fillcell(struct canvas *c, int col, int row, int n);

/* UTF-8 decode: returns codepoint, advances *s; 0 on end/invalid          */
uint32_t omni_utf8_next(const char **s);

/* ------------------------------------------------------------------ */
/* Images (stb_image backend)                                         */
/* ------------------------------------------------------------------ */

struct osimage;

struct osimage *omni_image_load(const char *path);
struct osimage *omni_image_from_mem(const unsigned char *buf, int len);
void            omni_image_free(struct osimage *im);
void            omni_image_draw(struct raster *dst, const struct osimage *im,
                                int x, int y, int w, int h, int blend);

#ifdef __cplusplus
}
#endif

#endif /* OMNI_OS_LIB_OMNI_H */
