/*
 * OmniOS — os/apps/oms.h
 *
 * OmniScript: the programming language of OmniOS programs. A small,
 * friendly, dynamically typed language (numbers, strings, lists, maps,
 * functions) compiled to bytecode for its own virtual machine. Programs
 * can be run straight from source (.oms) or built into OmniOS executables
 * (.omx, see omx.c), which carry the bytecode.
 *
 *   let name = "World"
 *   fn greet(who) { return "Hello, " + who + "!" }
 *   print(greet(name))
 *   for i in range(3) { print(i) }
 *
 * This is the engine: compiler, bytecode, VM and core library. omx.c adds
 * windows, files, the network and permissions.
 */
#ifndef OMNI_OS_APPS_OMS_H
#define OMNI_OS_APPS_OMS_H

#include <stddef.h>
#include <stdint.h>

enum { OMS_NIL, OMS_BOOL, OMS_NUM, OMS_OBJ };
enum { OBJ_STR, OBJ_LIST, OBJ_MAP, OBJ_FUNC, OBJ_NATIVE };

struct oms_vm;
struct oms_obj;

typedef struct {
    unsigned char t;
    union { double n; int b; struct oms_obj *o; } u;
} oms_value;

/* natives: return 0 (result in *ret) or oms_error(...) */
typedef int (*oms_native)(struct oms_vm *vm, int argc, oms_value *argv, oms_value *ret);

struct oms_vm *oms_new(void);
void oms_free(struct oms_vm *vm);

/* compile source (file is for messages); 0, or -1 with "file:line: why" */
int  oms_compile(struct oms_vm *vm, const char *src, const char *file, char *err, size_t errn);
/* the compiled program as bytes (malloc'd) / load bytes made by oms_save */
int  oms_save(struct oms_vm *vm, unsigned char **out, size_t *n);
int  oms_load(struct oms_vm *vm, const unsigned char *p, size_t n, char *err, size_t errn);
/* run it: 0 when it ends, or the exit() code; a runtime error is printed
 * (through vm->write_err) and gives 1 */
int  oms_run(struct oms_vm *vm);

/* host side */
void oms_define(struct oms_vm *vm, const char *name, oms_native fn);
void oms_set_global(struct oms_vm *vm, const char *name, oms_value v);
int  oms_error(struct oms_vm *vm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void oms_exit(struct oms_vm *vm, int code);      /* from a native: stop the program */

oms_value oms_nil(void);
oms_value oms_bool(int b);
oms_value oms_num(double n);
oms_value oms_str(struct oms_vm *vm, const char *s, size_t n);
oms_value oms_cstr(struct oms_vm *vm, const char *s);
oms_value oms_list_new(struct oms_vm *vm);
void      oms_list_push(struct oms_vm *vm, oms_value list, oms_value v);
oms_value oms_map_new(struct oms_vm *vm);
void      oms_map_set(struct oms_vm *vm, oms_value map, const char *key, oms_value v);

int  oms_is_str(oms_value v);
int  oms_is_list(oms_value v);
int  oms_is_map(oms_value v);
const char *oms_str_of(oms_value v, size_t *n);   /* NULL if not a string */
int  oms_list_len(oms_value v);
oms_value oms_list_get(oms_value v, int i);
int  oms_truthy(oms_value v);
/* v as text (malloc'd): what print() shows */
char *oms_to_text(oms_value v);

/* output: print() and errors go here (default: stdout / stderr) */
struct oms_io {
    void (*out)(void *ctx, const char *s, size_t n);
    void (*err)(void *ctx, const char *s, size_t n);
    /* a line of input for input(); NULL at the end. Default: stdin */
    char *(*in)(void *ctx, const char *prompt);
    void *ctx;
};
void oms_set_io(struct oms_vm *vm, const struct oms_io *io);
/* the file name errors mention (oms_compile sets it from its argument) */
void oms_set_name(struct oms_vm *vm, const char *file);
void *oms_user(struct oms_vm *vm);
void  oms_set_user(struct oms_vm *vm, void *user);

#endif /* OMNI_OS_APPS_OMS_H */
