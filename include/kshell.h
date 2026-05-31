#ifndef KSHELL_H
#define KSHELL_H

/*
 * kshell - the in-kernel debug shell.
 *
 * Commands are data, not a hardcoded dispatch chain: each command registers a
 * { name, help, handler } record and the shell looks it up by name.  New
 * commands (from any subsystem) are added with one kshell_register() call and
 * appear in `help` automatically - the kernel stays extensible and uncluttered.
 */

/* A command handler.  `args` is the (possibly empty) argument string that
 * follows the command name; never NULL. */
typedef void (*kshell_fn)(const char *args);

/* Register a command.  Returns 0 on success, -1 if the table is full.  May be
 * called during early boot (before the shell thread runs). */
int kshell_register(const char *name, const char *help, kshell_fn handler);

/* Console output helper (VGA + COM1), usable by command handlers. */
void kshell_print(const char *s);

/* The kernel debug-shell thread entry point (used as the fallback console when
 * the userspace init cannot be started). */
void kshell_thread(void *arg);

#endif /* KSHELL_H */
