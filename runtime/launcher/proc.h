/* Child processes for the launcher's compiler runs: output to a log file, poll, kill. */
#ifndef LAUNCHER_PROC_H
#define LAUNCHER_PROC_H
#include <stdbool.h>
typedef struct Proc Proc;
/* argv[0] is looked up on PATH when it has no directory part. NULL on failure (err filled). */
Proc *proc_start(char *const argv[], const char *log_path, char *err, int errn);
/* true once finished; *code = exit status (or -1 for a signal / unknown) */
bool proc_poll(Proc *p, int *code);
int proc_wait(Proc *p);   /* blocks, frees */
void proc_kill(Proc *p);  /* kills, waits, frees */
void proc_free(Proc *p);  /* after proc_poll returned true */
/* Runs argv to completion and returns its output (malloc'd, NUL-terminated; NULL if it could
 * not start). *code receives the exit status. */
char *proc_capture(char *const argv[], const char *tmp_path, int *code);
#endif
