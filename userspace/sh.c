/* lsh - the Lariat shell.
 *
 * Supports built-ins, external commands (searched in /bin), pipelines (cmd |
 * cmd), redirection (>, >>, <), command sequencing (;), background jobs (&),
 * and basic job control (jobs/fg/bg + Ctrl-Z stop) built on process groups. */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/signal.h"
#include "libc/sys/ioctl.h"

#define MAX_ARGS  64
#define MAX_STAGES 16
#define LINE_MAX  512
#define MAX_JOBS  32

#define WEXITSTATUS(st) (((st) >> 8) & 0xff)
#define WIFSTOPPED(st)  (((st) & 0xff) == 0x7f)
#define WIFEXITED(st)   (((st) & 0x7f) == 0)

static volatile int g_interrupted = 0;
static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }

struct stage {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;
    char *outfile;
    int   append;
};

struct job {
    int  used;
    int  pgid;
    int  stopped;
    int  pids[MAX_STAGES];
    int  npids;
    char cmd[128];
};
static struct job jobs[MAX_JOBS];
static int shell_pgid;

static int add_job(int pgid, int *pids, int npids, const char *cmd, int stopped) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            jobs[i].used = 1;
            jobs[i].pgid = pgid;
            jobs[i].stopped = stopped;
            jobs[i].npids = npids;
            for (int k = 0; k < npids && k < MAX_STAGES; k++) jobs[i].pids[k] = pids[k];
            strncpy(jobs[i].cmd, cmd, sizeof(jobs[i].cmd) - 1);
            jobs[i].cmd[sizeof(jobs[i].cmd) - 1] = '\0';
            return i + 1;   /* job numbers are 1-based */
        }
    }
    return -1;
}

static void reap_done_jobs(void) {
    /* Reap finished background children without blocking, retiring jobs whose
     * processes have all exited. */
    int st, r;
    while ((r = waitpid(-1, &st, 1 /*WNOHANG*/)) > 0) {
        if (WIFSTOPPED(st)) continue;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (!jobs[i].used || jobs[i].stopped) continue;
            int live = 0;
            for (int k = 0; k < jobs[i].npids; k++) {
                if (jobs[i].pids[k] == r) jobs[i].pids[k] = -1;
                if (jobs[i].pids[k] > 0) live++;
            }
            if (live == 0) {
                printf("[%d] Done  %s\n", i + 1, jobs[i].cmd);
                jobs[i].used = 0;
            }
        }
    }
}

/* --- line parsing --- */
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *save = 0;
    char *tok = strtok_r(line, " \t", &save);
    while (tok && argc < max - 1) {
        argv[argc++] = tok;
        tok = strtok_r(0, " \t", &save);
    }
    argv[argc] = 0;
    return argc;
}

/* Parse a single stage's tokens, pulling out redirections. */
static void parse_stage(char **toks, int n, struct stage *s) {
    s->argc = 0;
    s->infile = s->outfile = 0;
    s->append = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], ">") == 0 && i + 1 < n) {
            s->outfile = toks[++i]; s->append = 0;
        } else if (strcmp(toks[i], ">>") == 0 && i + 1 < n) {
            s->outfile = toks[++i]; s->append = 1;
        } else if (strcmp(toks[i], "<") == 0 && i + 1 < n) {
            s->infile = toks[++i];
        } else if (s->argc < MAX_ARGS - 1) {
            s->argv[s->argc++] = toks[i];
        }
    }
    s->argv[s->argc] = 0;
}

static void builtin_help(void) {
    puts("lsh built-ins: cd pwd help exit env export unset jobs fg bg wait");
    puts("features: pipelines (a | b), redirection (> >> <), sequencing (;),");
    puts("          background (&), job control (Ctrl-Z, fg, bg)");
    puts("coreutils in /bin: ls cat echo hello true false clear sleep mkdir");
    puts("  rmdir rm cp mv wc grep head tail ps kill httpget echosrv echocli");
}

static int is_builtin(const char *cmd) {
    static const char *names[] = { "cd","pwd","help","exit","env","export",
                                   "unset","jobs","fg","bg","wait", 0 };
    for (int i = 0; names[i]; i++)
        if (strcmp(cmd, names[i]) == 0) return 1;
    return 0;
}

static void continue_job(int idx, int foreground);

static int run_builtin(int argc, char **argv, int *should_exit, int *exit_code) {
    if (strcmp(argv[0], "exit") == 0) {
        *should_exit = 1;
        *exit_code = (argc > 1) ? atoi(argv[1]) : 0;
        return 1;
    }
    if (strcmp(argv[0], "help") == 0) { builtin_help(); return 1; }
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = (argc > 1) ? argv[1] : "/";
        if (chdir(dir) < 0)
            fprintf(STDERR_FILENO, "cd: %s: error %d\n", dir, errno);
        return 1;
    }
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof(buf))) puts(buf);
        return 1;
    }
    if (strcmp(argv[0], "env") == 0) {
        if (environ) for (char **e = environ; *e; e++) puts(*e);
        return 1;
    }
    if (strcmp(argv[0], "export") == 0) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = '\0'; setenv(argv[i], eq + 1, 1); *eq = '='; }
            else if (!getenv(argv[i])) setenv(argv[i], "", 1);
        }
        return 1;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; i < argc; i++) unsetenv(argv[i]);
        return 1;
    }
    if (strcmp(argv[0], "jobs") == 0) {
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used)
                printf("[%d] %s  %s\n", i + 1,
                       jobs[i].stopped ? "Stopped" : "Running", jobs[i].cmd);
        return 1;
    }
    if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
        int idx = -1;
        if (argc > 1) {
            int n = atoi(argv[1][0] == '%' ? argv[1] + 1 : argv[1]);
            if (n >= 1 && n <= MAX_JOBS && jobs[n - 1].used) idx = n - 1;
        } else {
            for (int i = MAX_JOBS - 1; i >= 0; i--)
                if (jobs[i].used) { idx = i; break; }
        }
        if (idx < 0) { fprintf(STDERR_FILENO, "%s: no such job\n", argv[0]); return 1; }
        continue_job(idx, argv[0][0] == 'f');
        return 1;
    }
    if (strcmp(argv[0], "wait") == 0) {
        int st;
        while (waitpid(-1, &st, 0) > 0) { }
        for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].stopped) jobs[i].used = 0;
        return 1;
    }
    return 0;
}

/* Wait for the foreground process group's known pids to exit, or for the group
 * to stop (Ctrl-Z).  Records a job if it stops.  Reaps stray background exits
 * silently. */
static void wait_group(int pgid, int *pids, int npids, const char *cmd) {
    int alive[MAX_STAGES];
    int remaining = npids;
    for (int i = 0; i < npids; i++) alive[i] = 1;
    int stopped = 0;

    while (remaining > 0 && !stopped) {
        int st;
        int r = waitpid(-1, &st, 2 /*WUNTRACED*/);
        if (r <= 0) break;
        for (int i = 0; i < npids; i++) {
            if (pids[i] == r && alive[i]) {
                if (WIFSTOPPED(st)) {
                    stopped = 1;
                } else {
                    alive[i] = 0;
                    remaining--;
                }
                break;
            }
        }
        /* r not in our group => a background child; already reaped, ignore. */
    }

    tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (stopped) {
        int jn = add_job(pgid, pids, npids, cmd, 1);
        printf("\n[%d] Stopped  %s\n", jn, cmd);
    }
}

static void continue_job(int idx, int foreground) {
    struct job j = jobs[idx];
    jobs[idx].used = 0;     /* detach; re-added if it stops again */
    if (foreground) {
        tcsetpgrp(STDIN_FILENO, j.pgid);
        kill(-j.pgid, SIGCONT);
        wait_group(j.pgid, j.pids, j.npids, j.cmd);
    } else {
        kill(-j.pgid, SIGCONT);
        jobs[idx].used = 1;
        jobs[idx].stopped = 0;
        printf("[%d] %s &\n", idx + 1, j.cmd);
    }
}

/* Execute a pipeline of `nstages` stages.  Returns nothing; sets up pipes,
 * process group, redirection and foreground/background handling. */
static void run_pipeline(struct stage *stages, int nstages, int background,
                         const char *cmdtext) {
    int prev_read = -1;
    int pgid = 0;
    int pids[MAX_STAGES];

    for (int i = 0; i < nstages; i++) {
        int pipefd[2] = { -1, -1 };
        if (i < nstages - 1) {
            if (pipe(pipefd) < 0) { fprintf(STDERR_FILENO, "pipe failed\n"); return; }
        }

        int pid = fork();
        if (pid < 0) { fprintf(STDERR_FILENO, "fork failed\n"); return; }
        if (pid == 0) {
            /* Child: join the pipeline's process group. */
            setpgid(0, pgid ? pgid : 0);
            if (!background) tcsetpgrp(STDIN_FILENO, getpgrp());
            /* Default signal dispositions (execve also resets, but builtins
             * in a child path should behave normally). */
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            if (prev_read >= 0) { dup2(prev_read, STDIN_FILENO); close(prev_read); }
            if (pipefd[1] >= 0) { dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]); }
            if (pipefd[0] >= 0) close(pipefd[0]);

            struct stage *s = &stages[i];
            if (s->infile) {
                int fd = open(s->infile, O_RDONLY);
                if (fd < 0) { fprintf(STDERR_FILENO, "%s: error %d\n", s->infile, errno); _exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (s->outfile) {
                int fl = O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC);
                int fd = open(s->outfile, fl);
                if (fd < 0) { fprintf(STDERR_FILENO, "%s: error %d\n", s->outfile, errno); _exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            }

            char path[256];
            const char *prog = s->argv[0];
            if (!strchr(prog, '/')) {
                snprintf(path, sizeof(path), "/bin/%s", prog);
                prog = path;
            }
            execve(prog, s->argv, environ);
            fprintf(STDERR_FILENO, "%s: command not found\n", s->argv[0]);
            _exit(127);
        }

        /* Parent: assign the child to the pipeline's group. */
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;

        if (prev_read >= 0) close(prev_read);
        if (pipefd[1] >= 0) close(pipefd[1]);
        prev_read = pipefd[0];
    }
    if (prev_read >= 0) close(prev_read);

    if (background) {
        int jn = add_job(pgid, pids, nstages, cmdtext, 0);
        printf("[%d] %d\n", jn, pids[nstages - 1]);
    } else {
        tcsetpgrp(STDIN_FILENO, pgid);
        wait_group(pgid, pids, nstages, cmdtext);
    }
}

/* Run one command (a pipeline, possibly backgrounded). */
static void run_command(char *cmd, int *should_exit, int *exit_code) {
    /* Trailing '&' => background. */
    int background = 0;
    int len = (int)strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t')) cmd[--len] = '\0';
    if (len > 0 && cmd[len - 1] == '&') { background = 1; cmd[--len] = '\0'; }

    char cmdtext[128];
    strncpy(cmdtext, cmd, sizeof(cmdtext) - 1);
    cmdtext[sizeof(cmdtext) - 1] = '\0';

    /* Split into pipeline stages on '|'. */
    struct stage stages[MAX_STAGES];
    int nstages = 0;
    char *psave = 0;
    char *seg = strtok_r(cmd, "|", &psave);
    while (seg && nstages < MAX_STAGES) {
        char *toks[MAX_ARGS];
        int n = tokenize(seg, toks, MAX_ARGS);
        if (n > 0) parse_stage(toks, n, &stages[nstages++]);
        seg = strtok_r(0, "|", &psave);
    }
    if (nstages == 0) return;

    /* A single, non-background builtin runs in the shell itself. */
    if (nstages == 1 && !background && is_builtin(stages[0].argv[0])) {
        run_builtin(stages[0].argc, stages[0].argv, should_exit, exit_code);
        return;
    }
    run_pipeline(stages, nstages, background, cmdtext);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[LINE_MAX];
    char cwd[256];

    /* Become a process-group leader and claim the controlling terminal. */
    setpgid(0, 0);
    shell_pgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    signal(SIGINT, on_sigint);
    signal(SIGTSTP, SIG_IGN);   /* the shell itself ignores Ctrl-Z */

    puts("");
    puts("Lariat shell (lsh). Type 'help' for built-ins, 'exit' to quit.");

    for (;;) {
        reap_done_jobs();
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf("lariat:%s$ ", cwd);

        g_interrupted = 0;
        int n = read_line(STDIN_FILENO, line, sizeof(line));
        if (n < 0) {
            if (g_interrupted || errno == EINTR) { putchar('\n'); continue; }
            puts("");
            break;
        }
        if (n == 0) continue;

        /* Sequence commands separated by ';'. */
        int should_exit = 0, exit_code = 0;
        char *ssave = 0;
        char *one = strtok_r(line, ";", &ssave);
        while (one) {
            run_command(one, &should_exit, &exit_code);
            if (should_exit) return exit_code;
            one = strtok_r(0, ";", &ssave);
        }
    }
    return 0;
}
