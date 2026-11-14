/*
 * OmniOS — os/apps/oms.c
 *
 * The OmniScript engine (see oms.h): a lexer and a one-pass compiler to
 * bytecode, a stack virtual machine with a mark-and-sweep garbage
 * collector, serialization of programs (the inside of .omx files) and the
 * core library (text, lists, maps, maths, time).
 *
 * Bytecode loaded from a file is checked before it runs (operands, jumps,
 * constants), and the VM checks its stack, so a damaged or hostile
 * program can fail but cannot corrupt memory.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "oms.h"

#define STACK_MAX  16384
#define FRAMES_MAX 400
#define MAX_LOCALS 250

/* ---- objects ------------------------------------------------------------------ */
struct oms_obj { unsigned char type, mark; struct oms_obj *next; };
typedef struct { struct oms_obj h; uint32_t hash; int len; char s[]; } ostr;
typedef struct { struct oms_obj h; int n, cap; oms_value *v; } olist;
typedef struct { ostr *k; oms_value v; } oentry;
typedef struct { struct oms_obj h; int n, used, cap; oentry *e; } omap;
typedef struct {
    struct oms_obj h;
    ostr *name;
    int arity, ncode, capcode, nk, capk;
    uint8_t *code;
    uint16_t *lines;
    oms_value *k;
} ofunc;
typedef struct { struct oms_obj h; ostr *name; oms_native fn; } onative;

#define TOMB ((ostr *)1)

struct frame { ofunc *f; uint8_t *ip; oms_value *slots; };

struct oms_vm {
    oms_value stack[STACK_MAX];
    oms_value *sp;
    struct frame frames[FRAMES_MAX];
    int nframes;
    omap *globals;
    ostr **strs;
    int nstrs, capstrs;
    struct oms_obj *objs;
    size_t bytes, next_gc;
    struct oms_obj **gray;
    int ngray, capgray;
    ofunc *main;
    char file[128];
    char err[600];
    int exiting, exit_code;
    jmp_buf jb;
    int in_run;
    struct oms_io io;
    void *user;
};

enum {
    OP_CONST, OP_NIL, OP_TRUE, OP_FALSE, OP_POP, OP_DUP, OP_DUP2,
    OP_GETL, OP_SETL, OP_GETG, OP_SETG, OP_DEFG, OP_INDEX, OP_SETINDEX,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_NOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_JMP, OP_JF, OP_JFK, OP_JTK, OP_LOOP, OP_CALL, OP_RET,
    OP_LIST, OP_MAP, OP_ITER, OP_RANGE, OP_SWAP, OP_COUNT
};

/* operand bytes of each opcode (for the loader's check) */
static const unsigned char g_oplen[OP_COUNT] = {
    [OP_CONST] = 2, [OP_GETL] = 1, [OP_SETL] = 1, [OP_GETG] = 2, [OP_SETG] = 2,
    [OP_DEFG] = 2, [OP_JMP] = 2, [OP_JF] = 2, [OP_JFK] = 2, [OP_JTK] = 2,
    [OP_LOOP] = 2, [OP_CALL] = 1, [OP_LIST] = 2, [OP_MAP] = 2, [OP_ITER] = 3,
    [OP_RANGE] = 3,
};

static void *vmem(struct oms_vm *vm, void *p, size_t oldsz, size_t newsz)
{
    void *q;
    vm->bytes += newsz - oldsz;
    if (newsz == 0) {
        free(p);
        return NULL;
    }
    q = realloc(p, newsz);
    if (!q) {
        fprintf(stderr, "OmniScript: out of memory\n");
        exit(1);
    }
    return q;
}

static struct oms_obj *new_obj(struct oms_vm *vm, int type, size_t size)
{
    struct oms_obj *o = vmem(vm, NULL, 0, size);
    memset(o, 0, size);
    o->type = (unsigned char)type;
    o->next = vm->objs;
    vm->objs = o;
    return o;
}

static uint32_t hash_str(const char *s, int n)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* interned strings: equal text, same object */
static ostr *intern(struct oms_vm *vm, const char *s, int n)
{
    uint32_t h = hash_str(s, n);
    int i, tomb = -1;
    ostr *o;
    if (vm->nstrs + 1 > vm->capstrs * 3 / 4) {
        int oc = vm->capstrs, nc = oc ? oc * 2 : 256;
        ostr **old = vm->strs;
        vm->strs = calloc((size_t)nc, sizeof(ostr *));
        vm->capstrs = nc;
        vm->nstrs = 0;
        for (i = 0; i < oc; i++)
            if (old[i] && old[i] != TOMB) {
                int j = (int)(old[i]->hash & (uint32_t)(nc - 1));
                while (vm->strs[j])
                    j = (j + 1) & (nc - 1);
                vm->strs[j] = old[i];
                vm->nstrs++;
            }
        free(old);
    }
    for (i = (int)(h & (uint32_t)(vm->capstrs - 1));; i = (i + 1) & (vm->capstrs - 1)) {
        ostr *e = vm->strs[i];
        if (!e)
            break;
        if (e == TOMB) {
            if (tomb < 0)
                tomb = i;
            continue;
        }
        if (e->hash == h && e->len == n && memcmp(e->s, s, (size_t)n) == 0)
            return e;
    }
    o = (ostr *)new_obj(vm, OBJ_STR, sizeof(ostr) + (size_t)n + 1);
    o->hash = h;
    o->len = n;
    memcpy(o->s, s, (size_t)n);
    o->s[n] = '\0';
    if (tomb >= 0)
        i = tomb;
    else
        vm->nstrs++;
    vm->strs[i] = o;
    return o;
}

#define OBJ(v)   ((v).u.o)
#define IS(v, T) ((v).t == OMS_OBJ && OBJ(v)->type == (T))
#define STR(v)   ((ostr *)OBJ(v))
#define LIST(v)  ((olist *)OBJ(v))
#define MAP(v)   ((omap *)OBJ(v))

oms_value oms_nil(void) { oms_value v; v.t = OMS_NIL; v.u.n = 0; return v; }
oms_value oms_bool(int b) { oms_value v; v.t = OMS_BOOL; v.u.n = 0; v.u.b = b != 0; return v; }
oms_value oms_num(double n) { oms_value v; v.t = OMS_NUM; v.u.n = n; return v; }
static oms_value objv(void *o) { oms_value v; v.t = OMS_OBJ; v.u.o = o; return v; }

oms_value oms_str(struct oms_vm *vm, const char *s, size_t n)
{
    return objv(intern(vm, s, (int)n));
}

oms_value oms_cstr(struct oms_vm *vm, const char *s)
{
    return oms_str(vm, s, strlen(s));
}

int oms_is_str(oms_value v) { return IS(v, OBJ_STR); }
int oms_is_list(oms_value v) { return IS(v, OBJ_LIST); }
int oms_is_map(oms_value v) { return IS(v, OBJ_MAP); }

const char *oms_str_of(oms_value v, size_t *n)
{
    if (!IS(v, OBJ_STR))
        return NULL;
    if (n)
        *n = (size_t)STR(v)->len;
    return STR(v)->s;
}

oms_value oms_list_new(struct oms_vm *vm)
{
    return objv(new_obj(vm, OBJ_LIST, sizeof(olist)));
}

void oms_list_push(struct oms_vm *vm, oms_value list, oms_value v)
{
    olist *l = LIST(list);
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        l->v = vmem(vm, l->v, sizeof(oms_value) * (size_t)l->cap, sizeof(oms_value) * (size_t)nc);
        l->cap = nc;
    }
    l->v[l->n++] = v;
}

int oms_list_len(oms_value v) { return IS(v, OBJ_LIST) ? LIST(v)->n : 0; }

oms_value oms_list_get(oms_value v, int i)
{
    return (IS(v, OBJ_LIST) && i >= 0 && i < LIST(v)->n) ? LIST(v)->v[i] : oms_nil();
}

/* ---- maps (string keys) -------------------------------------------------------- */
static oentry *map_find(oentry *e, int cap, ostr *k)
{
    oentry *tomb = NULL;
    int i = (int)(k->hash & (uint32_t)(cap - 1));
    for (;;) {
        oentry *x = &e[i];
        if (x->k == NULL)
            return tomb ? tomb : x;
        if (x->k == TOMB) {
            if (!tomb)
                tomb = x;
        } else if (x->k == k) {
            return x;
        }
        i = (i + 1) & (cap - 1);
    }
}

static void map_put(struct oms_vm *vm, omap *m, ostr *k, oms_value v)
{
    oentry *x;
    if (m->used + 1 > m->cap * 3 / 4) {
        int oc = m->cap, nc = oc ? oc * 2 : 8, i;
        oentry *old = m->e;
        m->e = vmem(vm, NULL, 0, sizeof(oentry) * (size_t)nc);
        memset(m->e, 0, sizeof(oentry) * (size_t)nc);
        m->cap = nc;
        m->used = m->n = 0;
        for (i = 0; i < oc; i++)
            if (old[i].k && old[i].k != TOMB) {
                oentry *y = map_find(m->e, nc, old[i].k);
                *y = old[i];
                m->n++;
                m->used++;
            }
        vmem(vm, old, sizeof(oentry) * (size_t)oc, 0);
    }
    x = map_find(m->e, m->cap, k);
    if (x->k == NULL || x->k == TOMB) {
        if (x->k == NULL)
            m->used++;
        m->n++;
        x->k = k;
    }
    x->v = v;
}

static int map_get(omap *m, ostr *k, oms_value *out)
{
    oentry *x;
    if (!m->cap)
        return 0;
    x = map_find(m->e, m->cap, k);
    if (x->k == NULL || x->k == TOMB)
        return 0;
    *out = x->v;
    return 1;
}

static int map_del(omap *m, ostr *k)
{
    oentry *x;
    if (!m->cap)
        return 0;
    x = map_find(m->e, m->cap, k);
    if (x->k == NULL || x->k == TOMB)
        return 0;
    x->k = TOMB;
    x->v = oms_nil();
    m->n--;
    return 1;
}

oms_value oms_map_new(struct oms_vm *vm)
{
    return objv(new_obj(vm, OBJ_MAP, sizeof(omap)));
}

void oms_map_set(struct oms_vm *vm, oms_value map, const char *key, oms_value v)
{
    map_put(vm, MAP(map), intern(vm, key, (int)strlen(key)), v);
}

/* ---- text of values ------------------------------------------------------------ */
struct buf { char *s; size_t n, cap; };

static void bput(struct buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->n + n + 1)
            nc *= 2;
        b->s = realloc(b->s, nc);
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void num_text(double d, char *out, size_t n)
{
    if (isnan(d))
        snprintf(out, n, "nan");
    else if (isinf(d))
        snprintf(out, n, d > 0 ? "inf" : "-inf");
    else if (d == floor(d) && fabs(d) < 1e15)
        snprintf(out, n, "%.0f", d);
    else
        snprintf(out, n, "%.14g", d);
}

static void value_text(struct buf *b, oms_value v, int quote, int depth)
{
    char tmp[64];
    switch (v.t) {
    case OMS_NIL: bput(b, "nil", 3); return;
    case OMS_BOOL: bput(b, v.u.b ? "true" : "false", v.u.b ? 4 : 5); return;
    case OMS_NUM: num_text(v.u.n, tmp, sizeof(tmp)); bput(b, tmp, strlen(tmp)); return;
    default: break;
    }
    if (depth > 8) {
        bput(b, "...", 3);
        return;
    }
    switch (OBJ(v)->type) {
    case OBJ_STR:
        if (quote) bput(b, "\"", 1);
        bput(b, STR(v)->s, (size_t)STR(v)->len);
        if (quote) bput(b, "\"", 1);
        break;
    case OBJ_LIST: {
        int i;
        bput(b, "[", 1);
        for (i = 0; i < LIST(v)->n; i++) {
            if (i) bput(b, ", ", 2);
            value_text(b, LIST(v)->v[i], 1, depth + 1);
        }
        bput(b, "]", 1);
        break;
    }
    case OBJ_MAP: {
        int i, k = 0;
        bput(b, "{", 1);
        for (i = 0; i < MAP(v)->cap; i++) {
            oentry *e = &MAP(v)->e[i];
            if (!e->k || e->k == TOMB) continue;
            if (k++) bput(b, ", ", 2);
            bput(b, e->k->s, (size_t)e->k->len);
            bput(b, ": ", 2);
            value_text(b, e->v, 1, depth + 1);
        }
        bput(b, "}", 1);
        break;
    }
    case OBJ_FUNC:
        snprintf(tmp, sizeof(tmp), "<fn %s>", ((ofunc *)OBJ(v))->name ? ((ofunc *)OBJ(v))->name->s : "?");
        bput(b, tmp, strlen(tmp));
        break;
    case OBJ_NATIVE:
        snprintf(tmp, sizeof(tmp), "<builtin %s>", ((onative *)OBJ(v))->name->s);
        bput(b, tmp, strlen(tmp));
        break;
    }
}

char *oms_to_text(oms_value v)
{
    struct buf b = { NULL, 0, 0 };
    value_text(&b, v, 0, 0);
    if (!b.s)
        bput(&b, "", 0);
    return b.s;
}

static oms_value to_str(struct oms_vm *vm, oms_value v)
{
    char *t;
    oms_value r;
    if (IS(v, OBJ_STR))
        return v;
    t = oms_to_text(v);
    r = oms_cstr(vm, t);
    free(t);
    return r;
}

int oms_truthy(oms_value v)
{
    switch (v.t) {
    case OMS_NIL: return 0;
    case OMS_BOOL: return v.u.b;
    case OMS_NUM: return v.u.n != 0;
    default:
        if (IS(v, OBJ_STR)) return STR(v)->len > 0;
        if (IS(v, OBJ_LIST)) return LIST(v)->n > 0;
        if (IS(v, OBJ_MAP)) return MAP(v)->n > 0;
        return 1;
    }
}

static int equal(oms_value a, oms_value b)
{
    if (a.t != b.t)
        return 0;
    switch (a.t) {
    case OMS_NIL: return 1;
    case OMS_BOOL: return a.u.b == b.u.b;
    case OMS_NUM: return a.u.n == b.u.n;
    default: return OBJ(a) == OBJ(b);           /* strings are interned */
    }
}

static const char *type_name(oms_value v)
{
    switch (v.t) {
    case OMS_NIL: return "nil";
    case OMS_BOOL: return "bool";
    case OMS_NUM: return "number";
    default:
        switch (OBJ(v)->type) {
        case OBJ_STR: return "string";
        case OBJ_LIST: return "list";
        case OBJ_MAP: return "map";
        default: return "function";
        }
    }
}

/* ---- garbage collector ------------------------------------------------------------ */
static void mark_obj(struct oms_vm *vm, struct oms_obj *o)
{
    if (!o || o->mark)
        return;
    o->mark = 1;
    if (vm->ngray == vm->capgray) {
        vm->capgray = vm->capgray ? vm->capgray * 2 : 256;
        vm->gray = realloc(vm->gray, sizeof(*vm->gray) * (size_t)vm->capgray);
        if (!vm->gray)
            exit(1);
    }
    vm->gray[vm->ngray++] = o;
}

static void mark_val(struct oms_vm *vm, oms_value v)
{
    if (v.t == OMS_OBJ)
        mark_obj(vm, OBJ(v));
}

static void blacken(struct oms_vm *vm, struct oms_obj *o)
{
    int i;
    switch (o->type) {
    case OBJ_LIST:
        for (i = 0; i < ((olist *)o)->n; i++)
            mark_val(vm, ((olist *)o)->v[i]);
        break;
    case OBJ_MAP:
        for (i = 0; i < ((omap *)o)->cap; i++) {
            oentry *e = &((omap *)o)->e[i];
            if (e->k && e->k != TOMB) {
                mark_obj(vm, &e->k->h);
                mark_val(vm, e->v);
            }
        }
        break;
    case OBJ_FUNC: {
        ofunc *f = (ofunc *)o;
        if (f->name) mark_obj(vm, &f->name->h);
        for (i = 0; i < f->nk; i++)
            mark_val(vm, f->k[i]);
        break;
    }
    case OBJ_NATIVE:
        mark_obj(vm, &((onative *)o)->name->h);
        break;
    }
}

static void free_obj(struct oms_vm *vm, struct oms_obj *o)
{
    switch (o->type) {
    case OBJ_STR: vmem(vm, o, sizeof(ostr) + (size_t)((ostr *)o)->len + 1, 0); return;
    case OBJ_LIST:
        vmem(vm, ((olist *)o)->v, sizeof(oms_value) * (size_t)((olist *)o)->cap, 0);
        vmem(vm, o, sizeof(olist), 0);
        return;
    case OBJ_MAP:
        vmem(vm, ((omap *)o)->e, sizeof(oentry) * (size_t)((omap *)o)->cap, 0);
        vmem(vm, o, sizeof(omap), 0);
        return;
    case OBJ_FUNC: {
        ofunc *f = (ofunc *)o;
        vmem(vm, f->code, (size_t)f->capcode, 0);
        vmem(vm, f->lines, sizeof(uint16_t) * (size_t)f->capcode, 0);
        vmem(vm, f->k, sizeof(oms_value) * (size_t)f->capk, 0);
        vmem(vm, o, sizeof(ofunc), 0);
        return;
    }
    case OBJ_NATIVE: vmem(vm, o, sizeof(onative), 0); return;
    }
}

static void gc(struct oms_vm *vm)
{
    oms_value *v;
    struct oms_obj **pp;
    int i;
    for (v = vm->stack; v < vm->sp; v++)
        mark_val(vm, *v);
    for (i = 0; i < vm->nframes; i++)
        mark_obj(vm, &vm->frames[i].f->h);
    mark_obj(vm, &vm->globals->h);
    if (vm->main)
        mark_obj(vm, &vm->main->h);
    while (vm->ngray > 0)
        blacken(vm, vm->gray[--vm->ngray]);
    for (i = 0; i < vm->capstrs; i++)        /* the intern table is weak */
        if (vm->strs[i] && vm->strs[i] != TOMB && !vm->strs[i]->h.mark)
            vm->strs[i] = TOMB;
    pp = &vm->objs;
    while (*pp) {
        struct oms_obj *o = *pp;
        if (o->mark) {
            o->mark = 0;
            pp = &o->next;
        } else {
            *pp = o->next;
            free_obj(vm, o);
        }
    }
    vm->next_gc = vm->bytes * 2 < (size_t)1 << 20 ? (size_t)1 << 20 : vm->bytes * 2;
}

/* ---- errors -------------------------------------------------------------------- */
static void say_err(struct oms_vm *vm, const char *s)
{
    if (vm->io.err)
        vm->io.err(vm->io.ctx, s, strlen(s));
    else
        fputs(s, stderr);
}

int oms_error(struct oms_vm *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err, sizeof(vm->err), fmt, ap);
    va_end(ap);
    return -1;
}

void oms_exit(struct oms_vm *vm, int code)
{
    vm->exiting = 1;
    vm->exit_code = code;
}

static int cur_line(struct oms_vm *vm, struct frame *fr)
{
    long at = (long)(fr->ip - fr->f->code) - 1;
    if (at < 0) at = 0;
    if (at >= fr->f->ncode) at = fr->f->ncode - 1;
    return fr->f->ncode > 0 ? fr->f->lines[at] : 0;
    (void)vm;
}

static void vm_raise(struct oms_vm *vm, const char *fmt, ...) __attribute__((noreturn));
static void vm_raise(struct oms_vm *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err, sizeof(vm->err), fmt, ap);
    va_end(ap);
    longjmp(vm->jb, 1);
}

/* ---- lexer ----------------------------------------------------------------------- */
enum {
    T_EOF, T_ERR, T_ID, T_NUM, T_STR, T_LP, T_RP, T_LB, T_RB, T_LC, T_RC,
    T_COMMA, T_DOT, T_COLON, T_SEMI, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_EQ, T_EQEQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_PLUSEQ, T_MINUSEQ, T_STAREQ,
    T_SLASHEQ, T_NOT, T_AND, T_OR,
    K_LET, K_FN, K_IF, K_ELIF, K_ELSE, K_WHILE, K_FOR, K_IN, K_RETURN,
    K_BREAK, K_CONTINUE, K_TRUE, K_FALSE, K_NIL
};

struct tok { int t; const char *s; int len, line; double num; };

struct loop { int depth, nbrk, start; int brk[128]; };

struct local { const char *s; int len, depth; };

struct fstate {
    struct fstate *up;
    ofunc *f;
    struct local loc[MAX_LOCALS + 4];
    int nloc, depth;
    struct loop *loops[16];
    int nloops;
};

struct parser {
    struct oms_vm *vm;
    const char *src, *p;
    int line;
    struct tok cur, prev;
    int err;
    char *errbuf;
    size_t errn;
    struct fstate *fs;
};

static void perr_at(struct parser *P, const struct tok *t, const char *msg)
{
    if (P->err)
        return;
    P->err = 1;
    if (t->t == T_EOF)
        snprintf(P->errbuf, P->errn, "%s:%d: %s (at the end)", P->vm->file, t->line, msg);
    else if (t->t == T_ERR)
        snprintf(P->errbuf, P->errn, "%s:%d: %s", P->vm->file, t->line, msg);
    else
        snprintf(P->errbuf, P->errn, "%s:%d: %s (at '%.*s')", P->vm->file, t->line, msg,
                 t->len > 30 ? 30 : t->len, t->s);
}

static void perr(struct parser *P, const char *msg) { perr_at(P, &P->prev, msg); }

static const struct { const char *s; int t; } g_kw[] = {
    { "let", K_LET }, { "fn", K_FN }, { "if", K_IF }, { "elif", K_ELIF },
    { "else", K_ELSE }, { "while", K_WHILE }, { "for", K_FOR }, { "in", K_IN },
    { "return", K_RETURN }, { "break", K_BREAK }, { "continue", K_CONTINUE },
    { "true", K_TRUE }, { "false", K_FALSE }, { "nil", K_NIL }, { "and", T_AND },
    { "or", T_OR }, { "not", T_NOT },
};

static struct tok lex(struct parser *P)
{
    struct tok t;
    const char *p = P->p;
    for (;;) {                                   /* spaces and comments */
        if (*p == '\n') { P->line++; p++; }
        else if (*p == ' ' || *p == '\t' || *p == '\r') p++;
        else if (*p == '#' || (p[0] == '/' && p[1] == '/')) { while (*p && *p != '\n') p++; }
        else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') P->line++; p++; }
            if (*p) p += 2;
        } else break;
    }
    t.s = p;
    t.line = P->line;
    t.num = 0;
    t.len = 1;
    if (!*p) { t.t = T_EOF; t.len = 0; P->p = p; return t; }
    if (isalpha((unsigned char)*p) || *p == '_') {
        size_t i;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        t.len = (int)(p - t.s);
        t.t = T_ID;
        for (i = 0; i < sizeof(g_kw) / sizeof(g_kw[0]); i++)
            if ((int)strlen(g_kw[i].s) == t.len && !memcmp(g_kw[i].s, t.s, (size_t)t.len))
                t.t = g_kw[i].t;
        P->p = p;
        return t;
    }
    if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *e;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            t.num = (double)strtoull(p, &e, 16);
        else
            t.num = strtod(p, &e);
        t.t = T_NUM;
        t.len = (int)(e - p);
        P->p = e;
        return t;
    }
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q) {
            if (*p == '\\' && p[1]) p++;
            if (*p == '\n') P->line++;
            p++;
        }
        if (!*p) {
            t.t = T_ERR;
            P->p = p;
            perr_at(P, &t, "this text has no closing quote");
            return t;
        }
        p++;
        t.t = T_STR;
        t.len = (int)(p - t.s);
        P->p = p;
        return t;
    }
    P->p = p + 1;
#define TWO(c, a, b) if (p[1] == (c)) { P->p = p + 2; t.len = 2; t.t = (a); } else t.t = (b); break
    switch (*p) {
    case '(': t.t = T_LP; break;
    case ')': t.t = T_RP; break;
    case '[': t.t = T_LB; break;
    case ']': t.t = T_RB; break;
    case '{': t.t = T_LC; break;
    case '}': t.t = T_RC; break;
    case ',': t.t = T_COMMA; break;
    case '.': t.t = T_DOT; break;
    case ':': t.t = T_COLON; break;
    case ';': t.t = T_SEMI; break;
    case '%': t.t = T_PCT; break;
    case '+': TWO('=', T_PLUSEQ, T_PLUS);
    case '-': TWO('=', T_MINUSEQ, T_MINUS);
    case '*': TWO('=', T_STAREQ, T_STAR);
    case '/': TWO('=', T_SLASHEQ, T_SLASH);
    case '=': TWO('=', T_EQEQ, T_EQ);
    case '!': TWO('=', T_NE, T_NOT);
    case '<': TWO('=', T_LE, T_LT);
    case '>': TWO('=', T_GE, T_GT);
    case '&': if (p[1] == '&') { P->p = p + 2; t.len = 2; t.t = T_AND; break; } /* fall through */
    case '|': if (p[1] == '|') { P->p = p + 2; t.len = 2; t.t = T_OR; break; } /* fall through */
    default:
        t.t = T_ERR;
        perr_at(P, &t, "a character OmniScript doesn't know");
        break;
    }
#undef TWO
    return t;
}

static void next(struct parser *P)
{
    P->prev = P->cur;
    P->cur = lex(P);
}

static int check(struct parser *P, int t) { return P->cur.t == t; }

static int accept(struct parser *P, int t)
{
    if (!check(P, t))
        return 0;
    next(P);
    return 1;
}

static void expect(struct parser *P, int t, const char *msg)
{
    if (!accept(P, t))
        perr_at(P, &P->cur, msg);
}

/* ---- code generation -------------------------------------------------------------- */
static ofunc *new_func(struct oms_vm *vm)
{
    return (ofunc *)new_obj(vm, OBJ_FUNC, sizeof(ofunc));
}

static void emit(struct parser *P, int b)
{
    ofunc *f = P->fs->f;
    if (f->ncode == f->capcode) {
        int nc = f->capcode ? f->capcode * 2 : 64;
        f->code = vmem(P->vm, f->code, (size_t)f->capcode, (size_t)nc);
        f->lines = vmem(P->vm, f->lines, sizeof(uint16_t) * (size_t)f->capcode,
                        sizeof(uint16_t) * (size_t)nc);
        f->capcode = nc;
    }
    f->code[f->ncode] = (uint8_t)b;
    f->lines[f->ncode++] = (uint16_t)(P->prev.line > 65535 ? 65535 : P->prev.line);
}

static void emit2(struct parser *P, int op, int u16)
{
    emit(P, op);
    emit(P, (u16 >> 8) & 255);
    emit(P, u16 & 255);
}

static int add_const(struct parser *P, oms_value v)
{
    ofunc *f = P->fs->f;
    int i;
    for (i = 0; i < f->nk; i++)                  /* reuse equal constants */
        if (f->k[i].t == v.t && v.t != OMS_OBJ ? equal(f->k[i], v) :
            (f->k[i].t == OMS_OBJ && v.t == OMS_OBJ && OBJ(f->k[i]) == OBJ(v)))
            return i;
    if (f->nk >= 65535) {
        perr(P, "too many constants in one function");
        return 0;
    }
    if (f->nk == f->capk) {
        int nc = f->capk ? f->capk * 2 : 16;
        f->k = vmem(P->vm, f->k, sizeof(oms_value) * (size_t)f->capk, sizeof(oms_value) * (size_t)nc);
        f->capk = nc;
    }
    f->k[f->nk] = v;
    return f->nk++;
}

static int name_const(struct parser *P, const struct tok *t)
{
    return add_const(P, objv(intern(P->vm, t->s, t->len)));
}

static int emit_jump(struct parser *P, int op)
{
    emit2(P, op, 0xffff);
    return P->fs->f->ncode - 2;
}

static void patch(struct parser *P, int at)
{
    int off = P->fs->f->ncode - at - 2;
    if (off > 65535) {
        perr(P, "this block is too long");
        return;
    }
    P->fs->f->code[at] = (uint8_t)(off >> 8);
    P->fs->f->code[at + 1] = (uint8_t)off;
}

static void emit_loop(struct parser *P, int start)
{
    int off;
    emit(P, OP_LOOP);
    off = P->fs->f->ncode - start + 2;
    if (off > 65535)
        perr(P, "this loop is too long");
    emit(P, (off >> 8) & 255);
    emit(P, off & 255);
}

static int find_local(struct parser *P, const struct tok *t)
{
    int i;
    for (i = P->fs->nloc - 1; i >= 0; i--)
        if (P->fs->loc[i].len == t->len && !memcmp(P->fs->loc[i].s, t->s, (size_t)t->len))
            return i;
    return -1;
}

static int add_local(struct parser *P, const char *s, int len)
{
    struct fstate *fs = P->fs;
    if (fs->nloc >= MAX_LOCALS) {
        perr(P, "too many variables in one function");
        return 0;
    }
    fs->loc[fs->nloc].s = s;
    fs->loc[fs->nloc].len = len;
    fs->loc[fs->nloc].depth = fs->depth;
    return fs->nloc++;
}

static void begin_scope(struct parser *P) { P->fs->depth++; }

static void end_scope(struct parser *P)
{
    struct fstate *fs = P->fs;
    fs->depth--;
    while (fs->nloc > 0 && fs->loc[fs->nloc - 1].depth > fs->depth) {
        emit(P, OP_POP);
        fs->nloc--;
    }
}

/* ---- expressions ------------------------------------------------------------------- */
enum { PR_NONE, PR_ASSIGN, PR_OR, PR_AND, PR_EQ, PR_CMP, PR_TERM, PR_FACTOR, PR_UNARY, PR_CALL };

static void expr(struct parser *P);
static void expr_prec(struct parser *P, int prec);
static void block_body(struct parser *P);
static void function(struct parser *P, const struct tok *name);

static oms_value unescape(struct parser *P, const struct tok *t)
{
    char *o = malloc((size_t)t->len + 1);
    int i, k = 0;
    oms_value v;
    for (i = 1; i < t->len - 1; i++) {
        char c = t->s[i];
        if (c == '\\' && i + 1 < t->len - 1) {
            c = t->s[++i];
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            case 'e': c = 27; break;
            default: break;
            }
        }
        o[k++] = c;
    }
    v = oms_str(P->vm, o, (size_t)k);
    free(o);
    return v;
}

static int compound_op(int t)
{
    return t == T_PLUSEQ ? OP_ADD : t == T_MINUSEQ ? OP_SUB : t == T_STAREQ ? OP_MUL :
           t == T_SLASHEQ ? OP_DIV : -1;
}

static void variable(struct parser *P, struct tok name, int can_assign)
{
    int slot = find_local(P, &name), op;
    int getop = slot >= 0 ? OP_GETL : OP_GETG, setop = slot >= 0 ? OP_SETL : OP_SETG;
    int arg = slot >= 0 ? slot : name_const(P, &name);
#define EMIT_VAR(o) do { if (slot >= 0) { emit(P, (o)); emit(P, arg); } else emit2(P, (o), arg); } while (0)
    if (can_assign && accept(P, T_EQ)) {
        expr(P);
        EMIT_VAR(setop);
    } else if (can_assign && (op = compound_op(P->cur.t)) >= 0) {
        next(P);
        EMIT_VAR(getop);
        expr(P);
        emit(P, op);
        EMIT_VAR(setop);
    } else {
        EMIT_VAR(getop);
    }
#undef EMIT_VAR
}

static int args(struct parser *P, int close)
{
    int n = 0;
    if (!check(P, close)) {
        do {
            if (check(P, close))
                break;                            /* trailing comma */
            expr(P);
            if (++n > 255)
                perr(P, "too many values");
        } while (accept(P, T_COMMA));
    }
    expect(P, close, close == T_RP ? "expected ')'" : "expected ']'");
    return n;
}

static void index_tail(struct parser *P, int can_assign)
{
    int op;
    if (can_assign && accept(P, T_EQ)) {
        expr(P);
        emit(P, OP_SETINDEX);
    } else if (can_assign && (op = compound_op(P->cur.t)) >= 0) {
        next(P);
        emit(P, OP_DUP2);
        emit(P, OP_INDEX);
        expr(P);
        emit(P, op);
        emit(P, OP_SETINDEX);
    } else {
        emit(P, OP_INDEX);
    }
}

static void primary(struct parser *P, int can_assign)
{
    struct tok t = P->prev;
    switch (t.t) {
    case T_NUM: emit2(P, OP_CONST, add_const(P, oms_num(t.num))); break;
    case T_STR: emit2(P, OP_CONST, add_const(P, unescape(P, &t))); break;
    case K_TRUE: emit(P, OP_TRUE); break;
    case K_FALSE: emit(P, OP_FALSE); break;
    case K_NIL: emit(P, OP_NIL); break;
    case T_ID: variable(P, t, can_assign); break;
    case T_LP:
        expr(P);
        expect(P, T_RP, "expected ')'");
        break;
    case T_LB: emit2(P, OP_LIST, args(P, T_RB)); break;
    case T_LC: {                                  /* { key: value, ... } */
        int n = 0;
        if (!check(P, T_RC)) {
            do {
                if (check(P, T_RC))
                    break;
                if (accept(P, T_ID) || accept(P, T_STR)) {
                    struct tok k = P->prev;
                    emit2(P, OP_CONST, add_const(P, k.t == T_STR ? unescape(P, &k)
                                                    : objv(intern(P->vm, k.s, k.len))));
                } else {
                    perr_at(P, &P->cur, "a map key must be a name or text");
                    return;
                }
                expect(P, T_COLON, "expected ':' after the key");
                expr(P);
                n++;
            } while (accept(P, T_COMMA));
        }
        expect(P, T_RC, "expected '}' to close the map");
        emit2(P, OP_MAP, n);
        break;
    }
    case K_FN: function(P, NULL); break;
    case T_MINUS: expr_prec(P, PR_UNARY); emit(P, OP_NEG); break;
    case T_NOT: expr_prec(P, PR_UNARY); emit(P, OP_NOT); break;
    default:
        perr_at(P, &t, "expected a value");
        break;
    }
}

static int infix_prec(int t)
{
    switch (t) {
    case T_OR: return PR_OR;
    case T_AND: return PR_AND;
    case T_EQEQ: case T_NE: return PR_EQ;
    case T_LT: case T_LE: case T_GT: case T_GE: return PR_CMP;
    case T_PLUS: case T_MINUS: return PR_TERM;
    case T_STAR: case T_SLASH: case T_PCT: return PR_FACTOR;
    case T_LP: case T_LB: case T_DOT: return PR_CALL;
    default: return PR_NONE;
    }
}

static void expr_prec(struct parser *P, int prec)
{
    int can_assign = prec <= PR_ASSIGN;
    next(P);
    primary(P, can_assign);
    while (!P->err && prec <= infix_prec(P->cur.t)) {
        int t = P->cur.t, j;
        next(P);
        switch (t) {
        case T_OR:
            j = emit_jump(P, OP_JTK);
            emit(P, OP_POP);
            expr_prec(P, PR_AND);
            patch(P, j);
            break;
        case T_AND:
            j = emit_jump(P, OP_JFK);
            emit(P, OP_POP);
            expr_prec(P, PR_EQ);
            patch(P, j);
            break;
        case T_LP: {
            int n = args(P, T_RP);                    /* the arguments first */
            emit(P, OP_CALL);
            emit(P, n);
            break;
        }
        case T_LB:
            expr(P);
            expect(P, T_RB, "expected ']'");
            index_tail(P, can_assign);
            break;
        case T_DOT:
            expect(P, T_ID, "expected a name after '.'");
            emit2(P, OP_CONST, name_const(P, &P->prev));
            index_tail(P, can_assign);
            break;
        default: {
            static const unsigned char ops[] = {
                [T_EQEQ] = OP_EQ, [T_NE] = OP_NE, [T_LT] = OP_LT, [T_LE] = OP_LE,
                [T_GT] = OP_GT, [T_GE] = OP_GE, [T_PLUS] = OP_ADD, [T_MINUS] = OP_SUB,
                [T_STAR] = OP_MUL, [T_SLASH] = OP_DIV, [T_PCT] = OP_MOD };
            expr_prec(P, infix_prec(t) + 1);
            emit(P, ops[t]);
            break;
        }
        }
    }
    if (can_assign && (check(P, T_EQ) || compound_op(P->cur.t) >= 0))
        perr_at(P, &P->cur, "can't assign to this");
}

static void expr(struct parser *P) { expr_prec(P, PR_ASSIGN); }

/* ---- statements --------------------------------------------------------------------- */
static void statement(struct parser *P);

static void declare_global_or_local(struct parser *P, const struct tok *name)
{
    if (P->fs->up == NULL && P->fs->depth == 0)
        emit2(P, OP_DEFG, name_const(P, name));
    else
        add_local(P, name->s, name->len);
}

static void let_stmt(struct parser *P)
{
    do {
        struct tok name;
        expect(P, T_ID, "expected a variable name after 'let'");
        name = P->prev;
        if (accept(P, T_EQ))
            expr(P);
        else
            emit(P, OP_NIL);
        declare_global_or_local(P, &name);
    } while (accept(P, T_COMMA));
}

static void function(struct parser *P, const struct tok *name)
{
    struct fstate fs;
    ofunc *f = new_func(P->vm);
    memset(&fs, 0, sizeof(fs));
    fs.up = P->fs;
    fs.f = f;
    f->name = name ? intern(P->vm, name->s, name->len) : intern(P->vm, "fn", 2);
    P->fs = &fs;
    add_local(P, "", 0);                         /* slot 0: the function itself */
    expect(P, T_LP, "expected '(' after the function name");
    if (!check(P, T_RP)) {
        do {
            expect(P, T_ID, "expected a parameter name");
            add_local(P, P->prev.s, P->prev.len);
            f->arity++;
        } while (accept(P, T_COMMA));
    }
    expect(P, T_RP, "expected ')'");
    expect(P, T_LC, "expected '{' to start the function");
    block_body(P);
    emit(P, OP_NIL);
    emit(P, OP_RET);
    P->fs = fs.up;
    emit2(P, OP_CONST, add_const(P, objv(f)));
}

static void block_body(struct parser *P)
{
    while (!check(P, T_RC) && !check(P, T_EOF) && !P->err)
        statement(P);
    expect(P, T_RC, "expected '}'");
}

static void block(struct parser *P)
{
    expect(P, T_LC, "expected '{'");
    begin_scope(P);
    block_body(P);
    end_scope(P);
}

static void if_stmt(struct parser *P)
{
    int jf, jend[64], ne = 0, i;
    expr(P);
    jf = emit_jump(P, OP_JF);
    block(P);
    while (check(P, K_ELIF) || check(P, K_ELSE)) {
        if (ne < 64)
            jend[ne++] = emit_jump(P, OP_JMP);
        patch(P, jf);
        if (accept(P, K_ELIF)) {
            expr(P);
            jf = emit_jump(P, OP_JF);
            block(P);
        } else {
            next(P);
            if (accept(P, K_IF)) {                /* else if */
                expr(P);
                jf = emit_jump(P, OP_JF);
                block(P);
            } else {
                block(P);
                jf = -1;
                break;
            }
        }
    }
    if (jf >= 0)
        patch(P, jf);
    for (i = 0; i < ne; i++)
        patch(P, jend[i]);
}

static void loop_begin(struct parser *P, struct loop *L, int start)
{
    memset(L, 0, sizeof(*L));
    L->depth = P->fs->depth;
    L->start = start;
    if (P->fs->nloops < 16)
        P->fs->loops[P->fs->nloops++] = L;
    else
        perr(P, "loops nested too deep");
}

static void loop_end(struct parser *P, struct loop *L)
{
    int i;
    for (i = 0; i < L->nbrk; i++)
        patch(P, L->brk[i]);
    P->fs->nloops--;
}

static void while_stmt(struct parser *P)
{
    struct loop L;
    int start = P->fs->f->ncode, jf;
    loop_begin(P, &L, start);
    expr(P);
    jf = emit_jump(P, OP_JF);
    block(P);
    emit_loop(P, start);
    patch(P, jf);
    loop_end(P, &L);
}

static void for_stmt(struct parser *P)
{
    struct loop L;
    struct tok var;
    int base, start, exitj, is_range = 0;
    expect(P, T_ID, "expected a variable name after 'for'");
    var = P->prev;
    expect(P, K_IN, "expected 'in'");
    begin_scope(P);
    /* for x in range(...): counted, without making a list */
    if (check(P, T_ID) && P->cur.len == 5 && !memcmp(P->cur.s, "range", 5)) {
        const char *save = P->p;
        int line = P->line;
        struct tok cur = P->cur, prev = P->prev;
        next(P);
        if (check(P, T_LP)) {
            int n;
            next(P);
            n = args(P, T_RP);
            if (n < 1 || n > 3)
                perr(P, "range takes 1 to 3 numbers");
            if (n == 1) {                             /* range(b): 0 .. b */
                emit2(P, OP_CONST, add_const(P, oms_num(0)));
                emit(P, OP_SWAP);
            }
            if (n <= 2)
                emit2(P, OP_CONST, add_const(P, oms_num(1)));
            is_range = 1;
        } else {                                       /* a variable named range */
            P->p = save;
            P->line = line;
            P->cur = cur;
            P->prev = prev;
        }
    }
    base = P->fs->nloc;
    if (is_range) {                                   /* slots: i, end, step */
        add_local(P, "(i)", 3);
        add_local(P, "(end)", 5);
        add_local(P, "(step)", 6);
    } else {                                          /* slots: sequence, index */
        expr(P);
        emit2(P, OP_CONST, add_const(P, oms_num(0)));
        add_local(P, "(seq)", 5);
        add_local(P, "(idx)", 5);
    }
    emit(P, OP_NIL);
    add_local(P, var.s, var.len);
    start = P->fs->f->ncode;
    loop_begin(P, &L, start);
    emit(P, is_range ? OP_RANGE : OP_ITER);
    emit(P, base);
    exitj = P->fs->f->ncode;
    emit(P, 0xff);
    emit(P, 0xff);
    emit(P, OP_SETL);
    emit(P, P->fs->nloc - 1);
    emit(P, OP_POP);
    expect(P, T_LC, "expected '{'");
    begin_scope(P);
    block_body(P);
    end_scope(P);
    emit_loop(P, start);
    patch(P, exitj);
    loop_end(P, &L);
    end_scope(P);
}

/* break / continue: drop what the loop body declared, then jump */
static void jump_out(struct parser *P, int cont)
{
    struct loop *L;
    int i;
    if (P->fs->nloops == 0) {
        perr(P, cont ? "'continue' outside a loop" : "'break' outside a loop");
        return;
    }
    L = P->fs->loops[P->fs->nloops - 1];
    for (i = P->fs->nloc - 1; i >= 0 && P->fs->loc[i].depth > L->depth; i--)
        emit(P, OP_POP);
    if (cont)
        emit_loop(P, L->start);                       /* OP_ITER/OP_RANGE step on */
    else if (L->nbrk < 128)
        L->brk[L->nbrk++] = emit_jump(P, OP_JMP);
    else
        perr(P, "too many 'break's in one loop");
}

static void statement(struct parser *P)
{
    if (accept(P, T_SEMI))
        return;
    if (accept(P, K_LET)) {
        let_stmt(P);
    } else if (accept(P, K_FN)) {
        if (accept(P, T_ID)) {
            struct tok name = P->prev;
            if (P->fs->up == NULL && P->fs->depth == 0) {
                function(P, &name);                   /* a global function */
                emit2(P, OP_DEFG, name_const(P, &name));
            } else {
                add_local(P, name.s, name.len);       /* local to this block */
                function(P, &name);
            }
        } else {
            function(P, NULL);                        /* an unnamed function */
            emit(P, OP_POP);
        }
    } else if (accept(P, K_IF)) {
        if_stmt(P);
    } else if (accept(P, K_WHILE)) {
        while_stmt(P);
    } else if (accept(P, K_FOR)) {
        for_stmt(P);
    } else if (accept(P, K_RETURN)) {
        if (P->fs->up == NULL)
            perr(P, "'return' outside a function");
        if (check(P, T_RC) || check(P, T_SEMI) || check(P, T_EOF) || P->cur.line != P->prev.line)
            emit(P, OP_NIL);
        else
            expr(P);
        emit(P, OP_RET);
    } else if (accept(P, K_BREAK)) {
        jump_out(P, 0);
    } else if (accept(P, K_CONTINUE)) {
        jump_out(P, 1);
    } else if (check(P, T_LC)) {
        block(P);
    } else {
        expr(P);
        emit(P, OP_POP);
    }
    if (P->err) {                                 /* skip to a likely restart */
        while (!check(P, T_EOF) && !check(P, T_RC))
            next(P);
    }
}

int oms_compile(struct oms_vm *vm, const char *src, const char *file, char *err, size_t errn)
{
    struct parser P;
    struct fstate fs;
    const char *base = strrchr(file ? file : "program", '/');
    snprintf(vm->file, sizeof(vm->file), "%s", base ? base + 1 : (file ? file : "program"));
    memset(&P, 0, sizeof(P));
    memset(&fs, 0, sizeof(fs));
    P.vm = vm;
    P.src = P.p = src;
    P.line = 1;
    P.errbuf = err;
    P.errn = errn;
    if (err && errn)
        err[0] = '\0';
    fs.f = new_func(vm);
    fs.f->name = intern(vm, "main", 4);
    P.fs = &fs;
    vm->main = fs.f;                     /* reachable for the collector */
    add_local(&P, "", 0);
    next(&P);
    while (!check(&P, T_EOF) && !P.err)
        statement(&P);
    emit(&P, OP_NIL);
    emit(&P, OP_RET);
    if (P.err) {
        vm->main = NULL;
        return -1;
    }
    return 0;
}

/* ---- the virtual machine ------------------------------------------------------------ */
#define PUSH(v) do { if (vm->sp >= vm->stack + STACK_MAX - 1) vm_raise(vm, "out of stack space"); \
                     *vm->sp++ = (v); } while (0)
#define POP()   (vm->sp > vm->stack ? *--vm->sp : (vm_raise(vm, "stack underflow"), oms_nil()))
#define PEEK(k) (vm->sp[-1 - (k)])

static oms_value concat(struct oms_vm *vm, oms_value a, oms_value b)
{
    oms_value sa = to_str(vm, a), sb;
    char *buf;
    oms_value r;
    PUSH(sa);                                     /* keep it while b converts */
    sb = to_str(vm, b);
    buf = malloc((size_t)STR(sa)->len + (size_t)STR(sb)->len + 1);
    memcpy(buf, STR(sa)->s, (size_t)STR(sa)->len);
    memcpy(buf + STR(sa)->len, STR(sb)->s, (size_t)STR(sb)->len);
    r = oms_str(vm, buf, (size_t)STR(sa)->len + (size_t)STR(sb)->len);
    free(buf);
    vm->sp--;
    return r;
}

static int index_of(struct oms_vm *vm, oms_value idx, int len)
{
    int i;
    if (idx.t != OMS_NUM)
        vm_raise(vm, "a position must be a number, not %s", type_name(idx));
    i = (int)idx.u.n;
    if (i < 0)
        i += len;                                 /* -1 = the last */
    return i;
}

static oms_value get_index(struct oms_vm *vm, oms_value obj, oms_value idx)
{
    if (IS(obj, OBJ_LIST)) {
        int i = index_of(vm, idx, LIST(obj)->n);
        if (i < 0 || i >= LIST(obj)->n)
            vm_raise(vm, "position %d is outside the list (length %d)", (int)idx.u.n, LIST(obj)->n);
        return LIST(obj)->v[i];
    }
    if (IS(obj, OBJ_STR)) {
        int i = index_of(vm, idx, STR(obj)->len);
        if (i < 0 || i >= STR(obj)->len)
            vm_raise(vm, "position %d is outside the text (length %d)", (int)idx.u.n, STR(obj)->len);
        return oms_str(vm, STR(obj)->s + i, 1);
    }
    if (IS(obj, OBJ_MAP)) {
        oms_value v;
        ostr *k = IS(idx, OBJ_STR) ? STR(idx) : STR(to_str(vm, idx));
        return map_get(MAP(obj), k, &v) ? v : oms_nil();
    }
    vm_raise(vm, "can't look inside %s", type_name(obj));
}

static void set_index(struct oms_vm *vm, oms_value obj, oms_value idx, oms_value v)
{
    if (IS(obj, OBJ_LIST)) {
        int i = index_of(vm, idx, LIST(obj)->n);
        if (i < 0 || i >= LIST(obj)->n)
            vm_raise(vm, "position %d is outside the list (length %d)", (int)idx.u.n, LIST(obj)->n);
        LIST(obj)->v[i] = v;
        return;
    }
    if (IS(obj, OBJ_MAP)) {
        map_put(vm, MAP(obj), IS(idx, OBJ_STR) ? STR(idx) : STR(to_str(vm, idx)), v);
        return;
    }
    vm_raise(vm, "can't change %s", type_name(obj));
}

static int compare(struct oms_vm *vm, oms_value a, oms_value b)
{
    if (a.t == OMS_NUM && b.t == OMS_NUM)
        return a.u.n < b.u.n ? -1 : a.u.n > b.u.n ? 1 : 0;
    if (IS(a, OBJ_STR) && IS(b, OBJ_STR))
        return strcmp(STR(a)->s, STR(b)->s);
    vm_raise(vm, "can't compare %s with %s", type_name(a), type_name(b));
}

static void call_value(struct oms_vm *vm, oms_value callee, int argc)
{
    if (IS(callee, OBJ_FUNC)) {
        ofunc *f = (ofunc *)OBJ(callee);
        struct frame *fr;
        if (argc > f->arity)
            vm_raise(vm, "%s takes %d value%s, not %d", f->name->s, f->arity, f->arity == 1 ? "" : "s", argc);
        while (argc < f->arity) {                 /* missing ones are nil */
            PUSH(oms_nil());
            argc++;
        }
        if (vm->nframes >= FRAMES_MAX)
            vm_raise(vm, "too much recursion (functions calling functions %d deep)", FRAMES_MAX);
        fr = &vm->frames[vm->nframes++];
        fr->f = f;
        fr->ip = f->code;
        fr->slots = vm->sp - argc - 1;
        return;
    }
    if (IS(callee, OBJ_NATIVE)) {
        onative *n = (onative *)OBJ(callee);
        oms_value ret = oms_nil();
        vm->err[0] = '\0';
        if (n->fn(vm, argc, vm->sp - argc, &ret) != 0)
            vm_raise(vm, "%s: %s", n->name->s, vm->err[0] ? vm->err : "failed");
        vm->sp -= argc + 1;
        PUSH(ret);
        return;
    }
    vm_raise(vm, "%s isn't a function", type_name(callee));
}

static int execute(struct oms_vm *vm)
{
    struct frame *fr = &vm->frames[vm->nframes - 1];
    uint8_t *ip = fr->ip;
#define U8()   (*ip++)
#define U16()  (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define SYNC() (fr->ip = ip)
#define LOAD() (fr = &vm->frames[vm->nframes - 1], ip = fr->ip)
    for (;;) {
        int op = U8();
        switch (op) {
        case OP_CONST: { int k = U16(); PUSH(fr->f->k[k]); break; }
        case OP_NIL: PUSH(oms_nil()); break;
        case OP_TRUE: PUSH(oms_bool(1)); break;
        case OP_FALSE: PUSH(oms_bool(0)); break;
        case OP_POP: (void)POP(); break;
        case OP_DUP: { oms_value v = PEEK(0); PUSH(v); break; }
        case OP_DUP2: { oms_value a = PEEK(1), b = PEEK(0); PUSH(a); PUSH(b); break; }
        case OP_SWAP: { oms_value t = PEEK(0); PEEK(0) = PEEK(1); PEEK(1) = t; break; }
        case OP_GETL: {
            int sl = U8();
            if (fr->slots + sl >= vm->sp) { SYNC(); vm_raise(vm, "bad local variable"); }
            PUSH(fr->slots[sl]);
            break;
        }
        case OP_SETL: {
            int sl = U8();
            if (fr->slots + sl >= vm->sp) { SYNC(); vm_raise(vm, "bad local variable"); }
            fr->slots[sl] = PEEK(0);
            break;
        }
        case OP_GETG: {
            ostr *name = STR(fr->f->k[U16()]);
            oms_value v;
            if (!map_get(vm->globals, name, &v)) {
                SYNC();
                vm_raise(vm, "'%s' isn't defined (misspelled, or used before 'let'?)", name->s);
            }
            PUSH(v);
            break;
        }
        case OP_SETG: {
            ostr *name = STR(fr->f->k[U16()]);
            oms_value old;
            if (!map_get(vm->globals, name, &old)) {
                SYNC();
                vm_raise(vm, "'%s' isn't defined: write 'let %s = ...' first", name->s, name->s);
            }
            map_put(vm, vm->globals, name, PEEK(0));
            break;
        }
        case OP_DEFG: {
            ostr *name = STR(fr->f->k[U16()]);
            map_put(vm, vm->globals, name, PEEK(0));
            (void)POP();
            break;
        }
        case OP_INDEX: {
            oms_value i = POP(), o = POP();
            SYNC();
            PUSH(get_index(vm, o, i));
            break;
        }
        case OP_SETINDEX: {
            oms_value v = POP(), i = POP(), o = POP();
            SYNC();
            set_index(vm, o, i, v);
            PUSH(v);
            break;
        }
        case OP_ADD: {
            oms_value b = POP(), a = POP();
            SYNC();
            if (a.t == OMS_NUM && b.t == OMS_NUM)
                PUSH(oms_num(a.u.n + b.u.n));
            else if (IS(a, OBJ_STR) || IS(b, OBJ_STR))
                PUSH(concat(vm, a, b));
            else if (IS(a, OBJ_LIST) && IS(b, OBJ_LIST)) {
                oms_value l = oms_list_new(vm);
                int i;
                PUSH(l);
                for (i = 0; i < LIST(a)->n; i++) oms_list_push(vm, l, LIST(a)->v[i]);
                for (i = 0; i < LIST(b)->n; i++) oms_list_push(vm, l, LIST(b)->v[i]);
            } else
                vm_raise(vm, "can't add %s and %s", type_name(a), type_name(b));
            break;
        }
        case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            oms_value b = POP(), a = POP();
            double r;
            SYNC();
            if (op == OP_MUL && IS(a, OBJ_STR) && b.t == OMS_NUM) {   /* "-" * 10 */
                int n = (int)b.u.n, i;
                char *buf;
                if (n < 0) n = 0;
                if ((long)n * STR(a)->len > 16 << 20) vm_raise(vm, "that text would be too long");
                buf = malloc((size_t)n * (size_t)STR(a)->len + 1);
                for (i = 0; i < n; i++) memcpy(buf + (size_t)i * (size_t)STR(a)->len, STR(a)->s, (size_t)STR(a)->len);
                PUSH(oms_str(vm, buf, (size_t)n * (size_t)STR(a)->len));
                free(buf);
                break;
            }
            if (a.t != OMS_NUM || b.t != OMS_NUM)
                vm_raise(vm, "can't %s %s and %s", op == OP_SUB ? "subtract" : op == OP_MUL ? "multiply" :
                      op == OP_DIV ? "divide" : "take the remainder of", type_name(a), type_name(b));
            if (op == OP_SUB) r = a.u.n - b.u.n;
            else if (op == OP_MUL) r = a.u.n * b.u.n;
            else if (op == OP_DIV) {
                if (b.u.n == 0) vm_raise(vm, "division by zero");
                r = a.u.n / b.u.n;
            } else {
                if (b.u.n == 0) vm_raise(vm, "remainder of division by zero");
                r = fmod(a.u.n, b.u.n);
                if (r < 0 && b.u.n > 0) r += b.u.n;
            }
            PUSH(oms_num(r));
            break;
        }
        case OP_NEG: {
            oms_value a = POP();
            SYNC();
            if (a.t != OMS_NUM) vm_raise(vm, "can't make %s negative", type_name(a));
            PUSH(oms_num(-a.u.n));
            break;
        }
        case OP_NOT: { oms_value a = POP(); PUSH(oms_bool(!oms_truthy(a))); break; }
        case OP_EQ: { oms_value b = POP(), a = POP(); PUSH(oms_bool(equal(a, b))); break; }
        case OP_NE: { oms_value b = POP(), a = POP(); PUSH(oms_bool(!equal(a, b))); break; }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            oms_value b = POP(), a = POP();
            int c;
            SYNC();
            c = compare(vm, a, b);
            PUSH(oms_bool(op == OP_LT ? c < 0 : op == OP_LE ? c <= 0 : op == OP_GT ? c > 0 : c >= 0));
            break;
        }
        case OP_JMP: { int o = U16(); ip += o; break; }
        case OP_JF: { int o = U16(); oms_value c = POP(); if (!oms_truthy(c)) ip += o; break; }
        case OP_JFK: { int o = U16(); if (!oms_truthy(PEEK(0))) ip += o; break; }
        case OP_JTK: { int o = U16(); if (oms_truthy(PEEK(0))) ip += o; break; }
        case OP_LOOP: {
            int o = U16();
            ip -= o;
            if (vm->bytes > vm->next_gc) { SYNC(); gc(vm); }
            if (vm->exiting) return vm->exit_code;
            break;
        }
        case OP_CALL: {
            int argc = U8();
            SYNC();
            if (vm->sp - vm->stack < argc + 1) vm_raise(vm, "stack underflow");
            call_value(vm, PEEK(argc), argc);
            if (vm->exiting) return vm->exit_code;
            LOAD();
            if (vm->bytes > vm->next_gc) gc(vm);
            break;
        }
        case OP_RET: {
            oms_value r = POP();
            vm->sp = fr->slots;
            vm->nframes--;
            if (vm->nframes == 0)
                return 0;
            PUSH(r);
            LOAD();
            break;
        }
        case OP_LIST: {
            int n = U16(), i;
            oms_value l;
            SYNC();
            if (vm->sp - vm->stack < n) vm_raise(vm, "stack underflow");
            l = oms_list_new(vm);
            for (i = n; i > 0; i--)
                oms_list_push(vm, l, PEEK(i - 1));
            vm->sp -= n;
            PUSH(l);
            break;
        }
        case OP_MAP: {
            int n = U16(), i;
            oms_value m;
            SYNC();
            if (vm->sp - vm->stack < 2 * n) vm_raise(vm, "stack underflow");
            m = oms_map_new(vm);
            for (i = n; i > 0; i--) {
                oms_value k = PEEK(2 * i - 1);
                map_put(vm, MAP(m), IS(k, OBJ_STR) ? STR(k) : STR(to_str(vm, k)), PEEK(2 * i - 2));
            }
            vm->sp -= 2 * n;
            PUSH(m);
            break;
        }
        case OP_ITER: {
            int sl = U8(), o = U16();
            oms_value seq, *idx;
            SYNC();
            if (fr->slots + sl + 1 >= vm->sp) vm_raise(vm, "bad loop");
            seq = fr->slots[sl];
            idx = &fr->slots[sl + 1];
            if (IS(seq, OBJ_LIST)) {
                if ((int)idx->u.n < LIST(seq)->n) {
                    PUSH(LIST(seq)->v[(int)idx->u.n]);
                    idx->u.n += 1;
                } else ip += o;
            } else if (IS(seq, OBJ_STR)) {
                if ((int)idx->u.n < STR(seq)->len) {
                    PUSH(oms_str(vm, STR(seq)->s + (int)idx->u.n, 1));
                    idx->u.n += 1;
                } else ip += o;
            } else if (IS(seq, OBJ_MAP)) {               /* the keys */
                int k = (int)idx->u.n;
                omap *m = MAP(seq);
                while (k < m->cap && (!m->e[k].k || m->e[k].k == TOMB))
                    k++;
                if (k < m->cap) {
                    PUSH(objv(m->e[k].k));
                    idx->u.n = k + 1;
                } else ip += o;
            } else if (seq.t == OMS_NUM) {               /* for i in 5 */
                if (idx->u.n < seq.u.n) {
                    PUSH(oms_num(idx->u.n));
                    idx->u.n += 1;
                } else ip += o;
            } else {
                vm_raise(vm, "can't go through %s with 'for'", type_name(seq));
            }
            break;
        }
        case OP_RANGE: {
            int sl = U8(), o = U16();
            oms_value *v;
            SYNC();
            if (fr->slots + sl + 2 >= vm->sp) vm_raise(vm, "bad loop");
            v = &fr->slots[sl];
            if (v[0].t != OMS_NUM || v[1].t != OMS_NUM || v[2].t != OMS_NUM)
                vm_raise(vm, "range needs numbers");
            if (v[2].u.n == 0)
                vm_raise(vm, "range with a step of 0 never ends");
            if (v[2].u.n > 0 ? v[0].u.n < v[1].u.n : v[0].u.n > v[1].u.n) {
                PUSH(v[0]);
                v[0].u.n += v[2].u.n;
            } else ip += o;
            break;
        }
        default:
            SYNC();
            vm_raise(vm, "bad instruction %d", op);
        }
    }
#undef U8
#undef U16
#undef SYNC
#undef LOAD
}

int oms_run(struct oms_vm *vm)
{
    int r;
    if (!vm->main)
        return 1;
    vm->sp = vm->stack;
    vm->nframes = 0;
    vm->exiting = 0;
    if (setjmp(vm->jb)) {
        char msg[900];
        int i;
        snprintf(msg, sizeof(msg), "Error: %s\n", vm->err);
        say_err(vm, msg);
        for (i = vm->nframes - 1; i >= 0 && i >= vm->nframes - 8; i--) {
            struct frame *fr = &vm->frames[i];
            snprintf(msg, sizeof(msg), "  in %s, %s line %d\n",
                     fr->f == vm->main ? "the program" : fr->f->name->s, vm->file, cur_line(vm, fr));
            say_err(vm, msg);
        }
        vm->nframes = 0;
        return 1;
    }
    *vm->sp++ = objv(vm->main);
    vm->frames[0].f = vm->main;
    vm->frames[0].ip = vm->main->code;
    vm->frames[0].slots = vm->stack;
    vm->nframes = 1;
    r = execute(vm);
    return vm->exiting ? vm->exit_code : r;
}

/* ---- programs as bytes (.omx contents) --------------------------------------------- */
static void wbytes(struct buf *b, const void *p, size_t n) { bput(b, (const char *)p, n); }
static void w8(struct buf *b, int v) { unsigned char c = (unsigned char)v; wbytes(b, &c, 1); }
static void w16(struct buf *b, int v) { w8(b, v >> 8); w8(b, v); }
static void w32(struct buf *b, uint32_t v) { w8(b, (int)(v >> 24)); w8(b, (int)(v >> 16)); w8(b, (int)(v >> 8)); w8(b, (int)v); }

static void save_func(struct buf *b, ofunc *f)
{
    int i;
    w16(b, f->name->len);
    wbytes(b, f->name->s, (size_t)f->name->len);
    w8(b, f->arity);
    w32(b, (uint32_t)f->ncode);
    wbytes(b, f->code, (size_t)f->ncode);
    for (i = 0; i < f->ncode; i++)
        w16(b, f->lines[i]);
    w16(b, f->nk);
    for (i = 0; i < f->nk; i++) {
        oms_value v = f->k[i];
        if (v.t == OMS_NIL) w8(b, 0);
        else if (v.t == OMS_BOOL) w8(b, v.u.b ? 2 : 1);
        else if (v.t == OMS_NUM) { uint64_t u; memcpy(&u, &v.u.n, 8); w8(b, 3); w32(b, (uint32_t)(u >> 32)); w32(b, (uint32_t)u); }
        else if (IS(v, OBJ_STR)) { w8(b, 4); w32(b, (uint32_t)STR(v)->len); wbytes(b, STR(v)->s, (size_t)STR(v)->len); }
        else if (IS(v, OBJ_FUNC)) { w8(b, 5); save_func(b, (ofunc *)OBJ(v)); }
        else w8(b, 0);
    }
}

int oms_save(struct oms_vm *vm, unsigned char **out, size_t *n)
{
    struct buf b = { NULL, 0, 0 };
    if (!vm->main)
        return -1;
    wbytes(&b, "OMS1", 4);
    save_func(&b, vm->main);
    *out = (unsigned char *)b.s;
    *n = b.n;
    return 0;
}

struct rd { const unsigned char *p, *end; int bad; };

static uint32_t r8(struct rd *r) { if (r->p >= r->end) { r->bad = 1; return 0; } return *r->p++; }
static uint32_t r16(struct rd *r) { uint32_t a = r8(r); return a << 8 | r8(r); }
static uint32_t r32(struct rd *r) { uint32_t a = r16(r); return a << 16 | r16(r); }

static ofunc *load_func(struct oms_vm *vm, struct rd *r, int depth)
{
    ofunc *f;
    uint32_t n, i, nk;
    if (depth > 64 || r->bad)
        return NULL;
    f = new_func(vm);
    n = r16(r);
    if ((size_t)(r->end - r->p) < n) { r->bad = 1; return NULL; }
    f->name = intern(vm, (const char *)r->p, (int)n);
    r->p += n;
    f->arity = (int)r8(r);
    n = r32(r);
    if (n > (16u << 20) || (size_t)(r->end - r->p) < n * 3u) { r->bad = 1; return NULL; }
    f->code = vmem(vm, NULL, 0, n ? n : 1);
    f->lines = vmem(vm, NULL, 0, sizeof(uint16_t) * (n ? n : 1));
    f->capcode = (int)(n ? n : 1);
    f->ncode = (int)n;
    memcpy(f->code, r->p, n);
    r->p += n;
    for (i = 0; i < n; i++)
        f->lines[i] = (uint16_t)r16(r);
    nk = r16(r);
    f->k = vmem(vm, NULL, 0, sizeof(oms_value) * (nk ? nk : 1));
    f->capk = (int)(nk ? nk : 1);
    for (i = 0; i < nk && !r->bad; i++) {
        int tag = (int)r8(r);
        oms_value v = oms_nil();
        if (tag == 1 || tag == 2) v = oms_bool(tag == 2);
        else if (tag == 3) { uint64_t u = (uint64_t)r32(r) << 32; u |= r32(r); memcpy(&v.u.n, &u, 8); v.t = OMS_NUM; }
        else if (tag == 4) {
            uint32_t l = r32(r);
            if ((size_t)(r->end - r->p) < l) { r->bad = 1; break; }
            v = oms_str(vm, (const char *)r->p, l);
            r->p += l;
        } else if (tag == 5) {
            ofunc *g = load_func(vm, r, depth + 1);
            if (!g) { r->bad = 1; break; }
            v = objv(g);
        } else if (tag != 0) { r->bad = 1; break; }
        f->k[f->nk++] = v;
    }
    return r->bad ? NULL : f;
}

/* every operand in range: constants, jumps, locals (the VM checks the rest) */
static int verify(ofunc *f, int depth)
{
    int at = 0, i;
    if (depth > 64)
        return 0;
    while (at < f->ncode) {
        int op = f->code[at], len;
        if (op >= OP_COUNT)
            return 0;
        len = g_oplen[op];
        if (at + 1 + len > f->ncode)
            return 0;
        if (op == OP_CONST || op == OP_GETG || op == OP_SETG || op == OP_DEFG) {
            int k = f->code[at + 1] << 8 | f->code[at + 2];
            if (k >= f->nk)
                return 0;
            if (op != OP_CONST && !IS(f->k[k], OBJ_STR))
                return 0;
        }
        if (op == OP_JMP || op == OP_JF || op == OP_JFK || op == OP_JTK) {
            int o = f->code[at + 1] << 8 | f->code[at + 2];
            if (at + 3 + o > f->ncode)
                return 0;
        }
        if (op == OP_ITER || op == OP_RANGE) {
            int o = f->code[at + 2] << 8 | f->code[at + 3];
            if (at + 4 + o > f->ncode)
                return 0;
        }
        if (op == OP_LOOP) {
            int o = f->code[at + 1] << 8 | f->code[at + 2];
            if (at + 3 - o < 0)
                return 0;
        }
        at += 1 + len;
    }
    if (f->ncode == 0 || f->code[f->ncode - 1] != OP_RET)
        return 0;
    for (i = 0; i < f->nk; i++)
        if (IS(f->k[i], OBJ_FUNC) && !verify((ofunc *)OBJ(f->k[i]), depth + 1))
            return 0;
    return 1;
}

int oms_load(struct oms_vm *vm, const unsigned char *p, size_t n, char *err, size_t errn)
{
    struct rd r;
    ofunc *f;
    r.p = p;
    r.end = p + n;
    r.bad = 0;
    if (n < 4 || memcmp(p, "OMS1", 4) != 0) {
        snprintf(err, errn, "not OmniScript bytecode (or a newer version)");
        return -1;
    }
    r.p += 4;
    f = load_func(vm, &r, 0);
    if (!f || r.bad || !verify(f, 0)) {
        snprintf(err, errn, "the program is damaged");
        return -1;
    }
    vm->main = f;
    return 0;
}

/* ---- the core library ------------------------------------------------------------- */
static int want(struct oms_vm *vm, int argc, int lo, int hi)
{
    if (argc < lo || argc > hi) {
        if (lo == hi)
            return oms_error(vm, "takes %d value%s, got %d", lo, lo == 1 ? "" : "s", argc);
        return oms_error(vm, "takes %d to %d values, got %d", lo, hi, argc);
    }
    return 0;
}

static int need_num(struct oms_vm *vm, oms_value v, double *out)
{
    if (v.t != OMS_NUM)
        return oms_error(vm, "expected a number, got %s", type_name(v));
    *out = v.u.n;
    return 0;
}

static void out_text(struct oms_vm *vm, const char *s, size_t n)
{
    if (vm->io.out)
        vm->io.out(vm->io.ctx, s, n);
    else {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    }
}

static int n_print(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    struct buf b = { NULL, 0, 0 };
    int i;
    for (i = 0; i < argc; i++) {
        if (i) bput(&b, " ", 1);
        value_text(&b, a[i], 0, 0);
    }
    bput(&b, "\n", 1);
    out_text(vm, b.s, b.n);
    free(b.s);
    *ret = oms_nil();
    return 0;
}

static int n_input(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char *line = NULL, buf[4096];
    const char *prompt = argc > 0 && IS(a[0], OBJ_STR) ? STR(a[0])->s : "";
    if (vm->io.in) {
        line = vm->io.in(vm->io.ctx, prompt);
        if (!line) { *ret = oms_nil(); return 0; }
        *ret = oms_cstr(vm, line);
        free(line);
        return 0;
    }
    out_text(vm, prompt, strlen(prompt));
    if (!fgets(buf, sizeof(buf), stdin)) { *ret = oms_nil(); return 0; }
    buf[strcspn(buf, "\r\n")] = '\0';
    *ret = oms_cstr(vm, buf);
    return 0;
}

static int n_str(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (want(vm, argc, 1, 1)) return -1;
    *ret = to_str(vm, a[0]);
    return 0;
}

static int n_num(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char *e;
    double d;
    if (want(vm, argc, 1, 1)) return -1;
    if (a[0].t == OMS_NUM) { *ret = a[0]; return 0; }
    if (a[0].t == OMS_BOOL) { *ret = oms_num(a[0].u.b); return 0; }
    if (!IS(a[0], OBJ_STR)) { *ret = oms_nil(); return 0; }
    d = strtod(STR(a[0])->s, &e);
    while (isspace((unsigned char)*e)) e++;
    *ret = (e == STR(a[0])->s || *e) ? oms_nil() : oms_num(d);    /* nil if not a number */
    return 0;
}

static int n_int(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double d;
    if (want(vm, argc, 1, 1)) return -1;
    if (IS(a[0], OBJ_STR)) { if (n_num(vm, argc, a, ret)) return -1; if (ret->t == OMS_NUM) ret->u.n = trunc(ret->u.n); return 0; }
    if (need_num(vm, a[0], &d)) return -1;
    *ret = oms_num(trunc(d));
    return 0;
}

static int n_type(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (want(vm, argc, 1, 1)) return -1;
    *ret = oms_cstr(vm, type_name(a[0]));
    return 0;
}

static int n_len(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (want(vm, argc, 1, 1)) return -1;
    if (IS(a[0], OBJ_STR)) *ret = oms_num(STR(a[0])->len);
    else if (IS(a[0], OBJ_LIST)) *ret = oms_num(LIST(a[0])->n);
    else if (IS(a[0], OBJ_MAP)) *ret = oms_num(MAP(a[0])->n);
    else return oms_error(vm, "%s has no length", type_name(a[0]));
    return 0;
}

static int n_push(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    int i;
    if (argc < 2 || !IS(a[0], OBJ_LIST)) return oms_error(vm, "use push(list, value)");
    for (i = 1; i < argc; i++)
        oms_list_push(vm, a[0], a[i]);
    *ret = a[0];
    return 0;
}

static int n_pop(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    olist *l;
    int i;
    if (argc < 1 || !IS(a[0], OBJ_LIST)) return oms_error(vm, "use pop(list) or pop(list, position)");
    l = LIST(a[0]);
    if (l->n == 0) { *ret = oms_nil(); return 0; }
    i = argc > 1 && a[1].t == OMS_NUM ? (int)a[1].u.n : l->n - 1;
    if (i < 0) i += l->n;
    if (i < 0 || i >= l->n) return oms_error(vm, "position %d is outside the list", i);
    *ret = l->v[i];
    memmove(l->v + i, l->v + i + 1, sizeof(oms_value) * (size_t)(l->n - i - 1));
    l->n--;
    return 0;
}

static int n_insert(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    olist *l;
    int i;
    if (argc != 3 || !IS(a[0], OBJ_LIST) || a[1].t != OMS_NUM) return oms_error(vm, "use insert(list, position, value)");
    l = LIST(a[0]);
    i = (int)a[1].u.n;
    if (i < 0) i += l->n + 1;
    if (i < 0 || i > l->n) return oms_error(vm, "position %d is outside the list", i);
    oms_list_push(vm, a[0], oms_nil());
    memmove(l->v + i + 1, l->v + i, sizeof(oms_value) * (size_t)(l->n - i - 1));
    l->v[i] = a[2];
    *ret = a[0];
    return 0;
}

static int n_keys(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    oms_value l;
    int i, vals = 0;
    if (argc != 1 || !IS(a[0], OBJ_MAP)) return oms_error(vm, "expected a map");
    vals = vm->err[0] == 'v';
    l = oms_list_new(vm);
    for (i = 0; i < MAP(a[0])->cap; i++) {
        oentry *e = &MAP(a[0])->e[i];
        if (e->k && e->k != TOMB)
            oms_list_push(vm, l, vals ? e->v : objv(e->k));
    }
    *ret = l;
    return 0;
}

static int n_values(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    vm->err[0] = 'v';
    return n_keys(vm, argc, a, ret);
}

static int n_has(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    oms_value v;
    if (want(vm, argc, 2, 2)) return -1;
    if (IS(a[0], OBJ_MAP)) {
        *ret = oms_bool(map_get(MAP(a[0]), STR(to_str(vm, a[1])), &v));
    } else if (IS(a[0], OBJ_LIST)) {
        int i, f = 0;
        for (i = 0; i < LIST(a[0])->n && !f; i++)
            f = equal(LIST(a[0])->v[i], a[1]);
        *ret = oms_bool(f);
    } else if (IS(a[0], OBJ_STR) && IS(a[1], OBJ_STR)) {
        *ret = oms_bool(strstr(STR(a[0])->s, STR(a[1])->s) != NULL);
    } else
        return oms_error(vm, "expected a map, list or text");
    return 0;
}

static int n_del(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (argc != 2 || !IS(a[0], OBJ_MAP)) return oms_error(vm, "use del(map, key)");
    *ret = oms_bool(map_del(MAP(a[0]), STR(to_str(vm, a[1]))));
    return 0;
}

static int n_join(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    struct buf b = { NULL, 0, 0 };
    const char *sep = argc > 1 && IS(a[1], OBJ_STR) ? STR(a[1])->s : "";
    int i;
    if (argc < 1 || !IS(a[0], OBJ_LIST)) return oms_error(vm, "use join(list, separator)");
    bput(&b, "", 0);
    for (i = 0; i < LIST(a[0])->n; i++) {
        if (i) bput(&b, sep, strlen(sep));
        value_text(&b, LIST(a[0])->v[i], 0, 0);
    }
    *ret = oms_str(vm, b.s, b.n);
    free(b.s);
    return 0;
}

static int n_split(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    oms_value l;
    const char *s, *sep;
    size_t sl;
    if (argc < 1 || !IS(a[0], OBJ_STR)) return oms_error(vm, "use split(text, separator)");
    s = STR(a[0])->s;
    sep = argc > 1 && IS(a[1], OBJ_STR) ? STR(a[1])->s : NULL;
    l = oms_list_new(vm);
    *ret = l;
    if (!sep || !*sep) {                          /* on spaces */
        while (*s) {
            const char *e;
            while (isspace((unsigned char)*s)) s++;
            if (!*s) break;
            for (e = s; *e && !isspace((unsigned char)*e); e++) ;
            oms_list_push(vm, l, oms_str(vm, s, (size_t)(e - s)));
            s = e;
        }
        return 0;
    }
    sl = strlen(sep);
    for (;;) {
        const char *e = strstr(s, sep);
        if (!e) { oms_list_push(vm, l, oms_cstr(vm, s)); break; }
        oms_list_push(vm, l, oms_str(vm, s, (size_t)(e - s)));
        s = e + sl;
    }
    return 0;
}

static int n_case(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret, int up)
{
    char *t;
    int i;
    if (argc != 1 || !IS(a[0], OBJ_STR)) return oms_error(vm, "expected text");
    t = malloc((size_t)STR(a[0])->len + 1);
    for (i = 0; i <= STR(a[0])->len; i++)
        t[i] = (char)(up ? toupper((unsigned char)STR(a[0])->s[i]) : tolower((unsigned char)STR(a[0])->s[i]));
    *ret = oms_str(vm, t, (size_t)STR(a[0])->len);
    free(t);
    return 0;
}

static int n_upper(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return n_case(vm, argc, a, ret, 1); }
static int n_lower(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return n_case(vm, argc, a, ret, 0); }

static int n_trim(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    const char *s, *e;
    if (argc != 1 || !IS(a[0], OBJ_STR)) return oms_error(vm, "expected text");
    s = STR(a[0])->s;
    e = s + STR(a[0])->len;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *ret = oms_str(vm, s, (size_t)(e - s));
    return 0;
}

static int n_find(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (want(vm, argc, 2, 2)) return -1;
    if (IS(a[0], OBJ_STR) && IS(a[1], OBJ_STR)) {
        const char *p = strstr(STR(a[0])->s, STR(a[1])->s);
        *ret = oms_num(p ? (double)(p - STR(a[0])->s) : -1);
    } else if (IS(a[0], OBJ_LIST)) {
        int i;
        *ret = oms_num(-1);
        for (i = 0; i < LIST(a[0])->n; i++)
            if (equal(LIST(a[0])->v[i], a[1])) { *ret = oms_num(i); break; }
    } else
        return oms_error(vm, "use find(text, part) or find(list, value)");
    return 0;
}

static int n_replace(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    struct buf b = { NULL, 0, 0 };
    const char *s, *from, *to;
    size_t fl;
    if (argc != 3 || !IS(a[0], OBJ_STR) || !IS(a[1], OBJ_STR) || !IS(a[2], OBJ_STR))
        return oms_error(vm, "use replace(text, old, new)");
    s = STR(a[0])->s; from = STR(a[1])->s; to = STR(a[2])->s; fl = strlen(from);
    bput(&b, "", 0);
    if (!fl) { *ret = a[0]; free(b.s); return 0; }
    for (;;) {
        const char *e = strstr(s, from);
        if (!e) { bput(&b, s, strlen(s)); break; }
        bput(&b, s, (size_t)(e - s));
        bput(&b, to, strlen(to));
        s = e + fl;
    }
    *ret = oms_str(vm, b.s, b.n);
    free(b.s);
    return 0;
}

static int n_slice(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    int n, i0, i1, i;
    if (argc < 2 || argc > 3 || a[1].t != OMS_NUM || (argc == 3 && a[2].t != OMS_NUM))
        return oms_error(vm, "use slice(text or list, start, end)");
    if (IS(a[0], OBJ_STR)) n = STR(a[0])->len;
    else if (IS(a[0], OBJ_LIST)) n = LIST(a[0])->n;
    else return oms_error(vm, "expected text or a list");
    i0 = (int)a[1].u.n;
    i1 = argc == 3 ? (int)a[2].u.n : n;
    if (i0 < 0) i0 += n;
    if (i1 < 0) i1 += n;
    if (i0 < 0) i0 = 0;
    if (i1 > n) i1 = n;
    if (i1 < i0) i1 = i0;
    if (IS(a[0], OBJ_STR)) {
        *ret = oms_str(vm, STR(a[0])->s + i0, (size_t)(i1 - i0));
    } else {
        oms_value l = oms_list_new(vm);
        for (i = i0; i < i1; i++)
            oms_list_push(vm, l, LIST(a[0])->v[i]);
        *ret = l;
    }
    return 0;
}

static int n_chr(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double d;
    char c;
    if (want(vm, argc, 1, 1) || need_num(vm, a[0], &d)) return -1;
    c = (char)(int)d;
    *ret = oms_str(vm, &c, 1);
    return 0;
}

static int n_ord(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (argc != 1 || !IS(a[0], OBJ_STR) || STR(a[0])->len == 0) return oms_error(vm, "expected one character");
    *ret = oms_num((unsigned char)STR(a[0])->s[0]);
    return 0;
}

static int cmp_vals(const void *x, const void *y)
{
    const oms_value *a = x, *b = y;
    if (a->t == OMS_NUM && b->t == OMS_NUM)
        return a->u.n < b->u.n ? -1 : a->u.n > b->u.n;
    if (IS(*a, OBJ_STR) && IS(*b, OBJ_STR))
        return strcmp(STR(*a)->s, STR(*b)->s);
    return a->t - b->t;
}

static int n_sort(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    if (argc != 1 || !IS(a[0], OBJ_LIST)) return oms_error(vm, "expected a list");
    qsort(LIST(a[0])->v, (size_t)LIST(a[0])->n, sizeof(oms_value), cmp_vals);
    *ret = a[0];
    return 0;
}

static int n_range(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double lo = 0, hi, st = 1, x;
    oms_value l;
    if (want(vm, argc, 1, 3)) return -1;
    if (argc == 1) { if (need_num(vm, a[0], &hi)) return -1; }
    else { if (need_num(vm, a[0], &lo) || need_num(vm, a[1], &hi)) return -1; }
    if (argc == 3 && need_num(vm, a[2], &st)) return -1;
    if (st == 0) return oms_error(vm, "a step of 0 never ends");
    if (fabs((hi - lo) / st) > 1e7) return oms_error(vm, "too many numbers");
    l = oms_list_new(vm);
    for (x = lo; st > 0 ? x < hi : x > hi; x += st)
        oms_list_push(vm, l, oms_num(x));
    *ret = l;
    return 0;
}

static int n_math1(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret, double (*f)(double))
{
    double d;
    if (want(vm, argc, 1, 1) || need_num(vm, a[0], &d)) return -1;
    *ret = oms_num(f(d));
    return 0;
}

#define MATH1(name, fn) static int name(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return n_math1(vm, argc, a, ret, fn); }
MATH1(n_abs, fabs)
MATH1(n_floor, floor)
MATH1(n_ceil, ceil)
MATH1(n_round, round)
MATH1(n_sqrt, sqrt)
MATH1(n_sin, sin)
MATH1(n_cos, cos)
MATH1(n_tan, tan)

static int n_atan2(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double y, x;
    if (want(vm, argc, 2, 2) || need_num(vm, a[0], &y) || need_num(vm, a[1], &x)) return -1;
    *ret = oms_num(atan2(y, x));
    return 0;
}

static int n_pow(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x, y;
    if (want(vm, argc, 2, 2) || need_num(vm, a[0], &x) || need_num(vm, a[1], &y)) return -1;
    *ret = oms_num(pow(x, y));
    return 0;
}

static int n_minmax(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret, int mx)
{
    int i, n = argc;
    oms_value *v = a;
    if (argc == 1 && IS(a[0], OBJ_LIST)) { v = LIST(a[0])->v; n = LIST(a[0])->n; }
    if (n == 0) return oms_error(vm, "needs at least one number");
    *ret = v[0];
    for (i = 0; i < n; i++) {
        if (v[i].t != OMS_NUM) return oms_error(vm, "expected numbers");
        if (mx ? v[i].u.n > ret->u.n : v[i].u.n < ret->u.n) *ret = v[i];
    }
    return 0;
}

static int n_min(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return n_minmax(vm, argc, a, ret, 0); }
static int n_max(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return n_minmax(vm, argc, a, ret, 1); }

static int n_random(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    (void)vm; (void)argc; (void)a;
    *ret = oms_num((double)random() / ((double)RAND_MAX + 1.0));
    return 0;
}

static int n_randint(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double lo, hi;
    if (want(vm, argc, 2, 2) || need_num(vm, a[0], &lo) || need_num(vm, a[1], &hi)) return -1;
    if (hi < lo) return oms_error(vm, "the second number must not be smaller");
    *ret = oms_num(floor(lo) + (double)(random() % ((long)(floor(hi) - floor(lo)) + 1)));
    return 0;
}

static int n_time(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    struct timespec ts;
    (void)vm; (void)argc; (void)a;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ret = oms_num((double)ts.tv_sec + ts.tv_nsec / 1e9);
    return 0;
}

static int n_clock(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    struct timespec ts;
    (void)vm; (void)argc; (void)a;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *ret = oms_num((double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
    return 0;
}

static int n_sleep(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double ms;
    struct timespec ts;
    if (want(vm, argc, 1, 1) || need_num(vm, a[0], &ms)) return -1;
    if (ms < 0) ms = 0;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(fmod(ms, 1000) * 1e6);
    nanosleep(&ts, NULL);
    *ret = oms_nil();
    return 0;
}

static int n_exit(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    oms_exit(vm, argc > 0 && a[0].t == OMS_NUM ? (int)a[0].u.n : 0);
    *ret = oms_nil();
    return 0;
}

static int n_fixed(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double d, k = 2;
    char tmp[64];
    if (want(vm, argc, 1, 2) || need_num(vm, a[0], &d)) return -1;
    if (argc == 2 && need_num(vm, a[1], &k)) return -1;
    if (k < 0) k = 0;
    if (k > 12) k = 12;
    snprintf(tmp, sizeof(tmp), "%.*f", (int)k, d);
    *ret = oms_cstr(vm, tmp);
    return 0;
}

/* ---- the host API ----------------------------------------------------------------- */
void oms_define(struct oms_vm *vm, const char *name, oms_native fn)
{
    onative *n = (onative *)new_obj(vm, OBJ_NATIVE, sizeof(onative));
    n->name = intern(vm, name, (int)strlen(name));
    n->fn = fn;
    map_put(vm, vm->globals, n->name, objv(n));
}

void oms_set_global(struct oms_vm *vm, const char *name, oms_value v)
{
    map_put(vm, vm->globals, intern(vm, name, (int)strlen(name)), v);
}

void oms_set_io(struct oms_vm *vm, const struct oms_io *io) { vm->io = *io; }

void oms_set_name(struct oms_vm *vm, const char *file)
{
    const char *base = strrchr(file, '/');
    snprintf(vm->file, sizeof(vm->file), "%s", base ? base + 1 : file);
}
void *oms_user(struct oms_vm *vm) { return vm->user; }
void oms_set_user(struct oms_vm *vm, void *user) { vm->user = user; }

struct oms_vm *oms_new(void)
{
    static const struct { const char *n; oms_native f; } core[] = {
        { "print", n_print }, { "input", n_input }, { "str", n_str }, { "num", n_num },
        { "int", n_int }, { "type", n_type }, { "len", n_len }, { "push", n_push },
        { "pop", n_pop }, { "insert", n_insert }, { "keys", n_keys }, { "values", n_values },
        { "has", n_has }, { "contains", n_has }, { "del", n_del }, { "join", n_join },
        { "split", n_split }, { "upper", n_upper }, { "lower", n_lower }, { "trim", n_trim },
        { "find", n_find }, { "replace", n_replace }, { "slice", n_slice }, { "chr", n_chr },
        { "ord", n_ord }, { "sort", n_sort }, { "range", n_range }, { "abs", n_abs },
        { "floor", n_floor }, { "ceil", n_ceil }, { "round", n_round }, { "sqrt", n_sqrt },
        { "sin", n_sin }, { "cos", n_cos }, { "tan", n_tan }, { "atan2", n_atan2 },
        { "pow", n_pow }, { "min", n_min }, { "max", n_max }, { "random", n_random },
        { "randint", n_randint }, { "time", n_time }, { "clock", n_clock }, { "sleep", n_sleep },
        { "exit", n_exit }, { "fixed", n_fixed },
    };
    struct oms_vm *vm = calloc(1, sizeof(*vm));
    size_t i;
    if (!vm)
        return NULL;
    vm->sp = vm->stack;
    vm->next_gc = (size_t)1 << 20;
    vm->globals = (omap *)new_obj(vm, OBJ_MAP, sizeof(omap));
    for (i = 0; i < sizeof(core) / sizeof(core[0]); i++)
        oms_define(vm, core[i].n, core[i].f);
    oms_set_global(vm, "PI", oms_num(M_PI));
    srandom((unsigned)time(NULL) ^ (unsigned)getpid());
    return vm;
}

void oms_free(struct oms_vm *vm)
{
    struct oms_obj *o = vm->objs;
    while (o) {
        struct oms_obj *n = o->next;
        free_obj(vm, o);
        o = n;
    }
    free(vm->strs);
    free(vm->gray);
    free(vm);
}
