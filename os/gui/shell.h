/*
 * OmniOS — os/gui/shell.h
 *
 * Public interface of the OmniOS desktop shell (os/gui/shell.c). The thin
 * entry point (desktop.c) simply calls omni_shell_run().
 */
#ifndef OMNI_OS_GUI_SHELL_H
#define OMNI_OS_GUI_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* A start-menu entry: label + absolute path to launch. */
struct omni_menu_item {
    const char *label;
    const char *path;
};

/* Run the desktop shell until power-off. Does not return normally. */
void omni_shell_run(void);

#ifdef __cplusplus
}
#endif

#endif /* OMNI_OS_GUI_SHELL_H */
