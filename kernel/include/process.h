#ifndef REBORNOS_PROCESS_H
#define REBORNOS_PROCESS_H

/* Ties the VFS, ELF loader, and scheduler together into "run this
 * program from disk as a new process" -- the one operation booting the
 * shell, SYS_EXEC, and the exec self-test all need, so it lives in one
 * place instead of three. */

/* Loads `path`, maps it into a fresh address space, and starts it as a
 * new process with argc/argv (see elf_loader.h -- pass argc=0, argv=
 * NULL for a process that takes no arguments); does not wait for it to
 * finish. Returns the new thread's id, or -1 if `path` doesn't exist. */
int process_spawn_args(const char *path, const char *thread_name, int argc, char **argv);

/* process_spawn_args() with no arguments. */
int process_spawn(const char *path, const char *thread_name);

/* Same as process_spawn_args(), but blocks the calling thread
 * (cooperatively -- see schedule()/thread_is_alive() in scheduler.h)
 * until the spawned process exits. Returns 0 once it has, or -1 if
 * `path` didn't exist (nothing was spawned, so there's nothing to
 * wait for). */
int process_spawn_args_and_wait(const char *path, const char *thread_name, int argc, char **argv);

/* process_spawn_args_and_wait() with no arguments. */
int process_spawn_and_wait(const char *path, const char *thread_name);

#endif /* REBORNOS_PROCESS_H */
