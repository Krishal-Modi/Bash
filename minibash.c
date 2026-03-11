/*
 * minibash_firstname_lastname_SID.c
 * COMP-8567 Assignment 3 - Winter 2026
 * University of Windsor - Master of Applied Computing
 *
 * A mini bash shell implementing:
 *   - Basic command execution (fork + exec)
 *   - killmb, killallmb
 *   - Background process (&), pstop, cont, numbg, killbp
 *   - Piping (|) up to 4 operations
 *   - Reverse piping (~) up to 4 operations
 *   - FIFO pipe (|||)
 *   - I/O Redirection (<, >, >>)
 *   - Sequential execution (;) up to 4 commands
 *   - Conditional execution (&& and ||) up to 4 operators
 *   - Text file append (++)
 *   - Word count (#)
 *   - Text file concatenation (+)
 *
 * NOTE: system() is NOT used anywhere. All commands use fork() + exec().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

/* ─── Constants ─────────────────────────────────────────────── */
#define MAX_INPUT      1024
#define MAX_ARGS       5        /* command name + up to 4 args (argc <= 4) */
#define MAX_CMDS       5        /* up to 4 pipe/semi segments + 1        */
#define MAX_BG         64       /* max background processes tracked       */
#define FIFO_PATH      "/root/Assignments/Assignment3/common_fifo"

/* ─── Background-process tracking ───────────────────────────── */
static pid_t bg_pids[MAX_BG];   /* PIDs of background processes          */
static int   bg_count = 0;      /* number of currently tracked bg procs  */
static pid_t last_bg  = -1;     /* most recently created bg PID          */
static pid_t last_stopped = -1; /* most recently stopped PID             */

/* ═══════════════════════════════════════════════════════════════
 * Utility helpers
 * ═══════════════════════════════════════════════════════════════ */

/*
 * trim_whitespace – remove leading and trailing spaces/tabs from str
 * in-place, returns pointer to the trimmed string.
 */
char *trim_whitespace(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return str;
}

/*
 * parse_args – split a single command string into argv[] tokens.
 * Returns argc (number of tokens). Enforces Rule 3: argc must be >=1 and <=4.
 * Returns -1 on rule violation.
 */
int parse_args(char *cmd, char *argv[]) {
    int argc = 0;
    char *token = strtok(cmd, " \t");
    while (token != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    /* Rule 3: argc must be >= 1 and <= 4 */
    if (argc < 1) {
        fprintf(stderr, "minibash: empty command\n");
        return -1;
    }
    if (argc > 4) {
        fprintf(stderr, "minibash: Rule 3 violated – argc must be <= 4\n");
        return -1;
    }
    return argc;
}

/*
 * add_bg_pid – register a new background PID in our tracking array.
 */
void add_bg_pid(pid_t pid) {
    if (bg_count < MAX_BG) {
        bg_pids[bg_count++] = pid;
        last_bg = pid;
    } else {
        fprintf(stderr, "minibash: too many background processes\n");
    }
}

/*
 * remove_bg_pid – remove a PID from the tracking array (called when
 * a bg process terminates or is killed).
 */
void remove_bg_pid(pid_t pid) {
    for (int i = 0; i < bg_count; i++) {
        if (bg_pids[i] == pid) {
            bg_pids[i] = bg_pids[--bg_count];
            if (last_bg == pid) {
                last_bg = (bg_count > 0) ? bg_pids[bg_count - 1] : -1;
            }
            return;
        }
    }
}

/*
 * reap_zombies – non-blocking wait for any terminated background children.
 * Cleans up the tracking array automatically.
 */
void reap_zombies(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        remove_bg_pid(pid);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Core command executor
 * ═══════════════════════════════════════════════════════════════ */

/*
 * exec_cmd – fork + exec a single command given as argv[].
 *   in_fd  : if != -1, dup2 to STDIN_FILENO  before exec
 *   out_fd : if != -1, dup2 to STDOUT_FILENO before exec
 *   bg     : if non-zero, do not wait for child (background)
 * Returns child PID (caller waits if needed).
 */
pid_t exec_cmd(char *argv[], int in_fd, int out_fd, int bg) {
    if (argv == NULL || argv[0] == NULL) {
        fprintf(stderr, "minibash: no command specified\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("minibash: fork");
        return -1;
    }

    if (pid == 0) {
        /* ── Child process ── */

        /* Redirect stdin if needed */
        if (in_fd != -1) {
            if (dup2(in_fd, STDIN_FILENO) < 0) {
                perror("minibash: dup2 stdin");
                exit(EXIT_FAILURE);
            }
            close(in_fd);
        }

        /* Redirect stdout if needed */
        if (out_fd != -1) {
            if (dup2(out_fd, STDOUT_FILENO) < 0) {
                perror("minibash: dup2 stdout");
                exit(EXIT_FAILURE);
            }
            close(out_fd);
        }

        /* Execute the command */
        execvp(argv[0], argv);
        /* execvp returns only on failure */
        fprintf(stderr, "minibash: command not found: %s\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* ── Parent process ── */
    if (bg) {
        /* Background: register PID and return immediately */
        add_bg_pid(pid);
        printf("[bg] process started with PID %d\n", pid);
    }
    /* Caller is responsible for waiting on foreground children */
    return pid;
}

/* ═══════════════════════════════════════════════════════════════
 * Rule 1 & 2 – kill commands
 * ═══════════════════════════════════════════════════════════════ */

/*
 * cmd_killmb – kill the current minibash instance (Rule 1).
 * Sends SIGTERM to the current process.
 */
void cmd_killmb(void) {
    printf("minibash: terminating current session (PID %d)\n", getpid());
    kill(getpid(), SIGTERM);
}

/*
 * cmd_killallmb – kill ALL minibash processes running on the system (Rule 2).
 * Uses 'pkill' via fork+exec so system() is not used.
 */
void cmd_killallmb(void) {
    char *argv[] = {"pkill", "-f", "minibash", NULL};
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("minibash: pkill");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Background-process management commands
 * ═══════════════════════════════════════════════════════════════ */

/*
 * cmd_pstop – stop (SIGSTOP) the most recently created background process.
 */
void cmd_pstop(void) {
    reap_zombies(); /* clean up finished processes first */
    if (last_bg == -1 || bg_count == 0) {
        fprintf(stderr, "minibash: pstop – no background process to stop\n");
        return;
    }
    if (kill(last_bg, SIGSTOP) < 0) {
        perror("minibash: pstop");
    } else {
        printf("minibash: stopped PID %d\n", last_bg);
        last_stopped = last_bg;
    }
}

/*
 * cmd_cont – continue (SIGCONT) the most recently stopped process and
 * bring it to the foreground (wait for it).
 */
void cmd_cont(void) {
    if (last_stopped == -1) {
        fprintf(stderr, "minibash: cont – no stopped process to continue\n");
        return;
    }
    if (kill(last_stopped, SIGCONT) < 0) {
        perror("minibash: cont");
        return;
    }
    printf("minibash: continuing PID %d in foreground\n", last_stopped);
    /* Remove from bg tracking since it is now foreground */
    remove_bg_pid(last_stopped);
    waitpid(last_stopped, NULL, 0);
    last_stopped = -1;
}

/*
 * cmd_numbg – print the number of active background processes.
 */
void cmd_numbg(void) {
    reap_zombies();
    printf("minibash: number of background processes: %d\n", bg_count);
}

/*
 * cmd_killbp – kill all background (child) processes of the current minibash,
 * but leave minibash itself alive.
 */
void cmd_killbp(void) {
    reap_zombies();
    if (bg_count == 0) {
        printf("minibash: no background processes to kill\n");
        return;
    }
    for (int i = 0; i < bg_count; i++) {
        if (kill(bg_pids[i], SIGKILL) == 0) {
            printf("minibash: killed PID %d\n", bg_pids[i]);
        } else {
            perror("minibash: killbp");
        }
    }
    bg_count = 0;
    last_bg  = -1;
}

/* ═══════════════════════════════════════════════════════════════
 * Piping (|) – up to 4 pipe operations (5 commands)
 * ═══════════════════════════════════════════════════════════════ */

/*
 * handle_pipe – execute a chain of commands connected by '|'.
 * segments[] : array of command strings already split on '|'
 * n          : number of segments (2 to 5)
 */
void handle_pipe(char *segments[], int n) {
    /* Validate all segments satisfy Rule 3 before doing anything */
    char *args_copy[MAX_CMDS][MAX_ARGS];
    char  buf[MAX_CMDS][MAX_INPUT];

    for (int i = 0; i < n; i++) {
        strncpy(buf[i], segments[i], MAX_INPUT - 1);
        buf[i][MAX_INPUT - 1] = '\0';
        char *argv[MAX_ARGS];
        if (parse_args(buf[i], argv) < 0) return; /* rule violation */
        for (int j = 0; argv[j] != NULL; j++) args_copy[i][j] = argv[j];
        /* mark end */
    }

    /* Create (n-1) pipes */
    int pipes[4][2]; /* at most 4 pipes for 5 commands */
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("minibash: pipe");
            return;
        }
    }

    pid_t pids[MAX_CMDS];

    for (int i = 0; i < n; i++) {
        /* Re-parse segment i into fresh argv */
        char tmp[MAX_INPUT];
        strncpy(tmp, segments[i], MAX_INPUT - 1);
        tmp[MAX_INPUT - 1] = '\0';
        char *argv[MAX_ARGS];
        if (parse_args(tmp, argv) < 0) {
            /* clean up pipes on error */
            for (int k = 0; k < n - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            return;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("minibash: fork"); return; }

        if (pid == 0) {
            /* If not first command, read from previous pipe */
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            /* If not last command, write to current pipe */
            if (i < n - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            /* Close all pipe fds in child */
            for (int k = 0; k < n - 1; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            execvp(argv[0], argv);
            fprintf(stderr, "minibash: command not found: %s\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        pids[i] = pid;
    }

    /* Parent closes all pipe ends */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    /* Wait for all children */
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], NULL, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Reverse Piping (~) – execute segments in reverse order
 * e.g., "wc -w ~ wc ~ ls -1" → ls -1 | wc | wc -w → stdout
 * ═══════════════════════════════════════════════════════════════ */

void handle_reverse_pipe(char *segments[], int n) {
    /* Reverse the array and call handle_pipe */
    char *rev[MAX_CMDS];
    for (int i = 0; i < n; i++) {
        rev[i] = segments[n - 1 - i];
    }
    handle_pipe(rev, n);
}

/* ═══════════════════════════════════════════════════════════════
 * FIFO pipe (|||)
 * ═══════════════════════════════════════════════════════════════ */

/*
 * ensure_fifo – create the common FIFO if it does not already exist.
 */
void ensure_fifo(void) {
    /* Create directory path if needed */
    char dir[] = "/root/Assignments/Assignment3";
    /* Use mkdir -p via fork+exec */
    char *argv[] = {"mkdir", "-p", dir, NULL};
    pid_t pid = fork();
    if (pid == 0) { execvp("mkdir", argv); exit(EXIT_FAILURE); }
    if (pid > 0)  waitpid(pid, NULL, 0);

    /* Create FIFO only if it doesn't exist */
    struct stat st;
    if (stat(FIFO_PATH, &st) != 0) {
        if (mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
            perror("minibash: mkfifo");
        }
    }
}

/*
 * handle_fifo_write – "cmd|||" → run cmd with stdout → FIFO
 */
void handle_fifo_write(char *cmd_str) {
    ensure_fifo();
    char tmp[MAX_INPUT];
    strncpy(tmp, cmd_str, MAX_INPUT - 1);
    tmp[MAX_INPUT - 1] = '\0';

    char *argv[MAX_ARGS];
    if (parse_args(tmp, argv) < 0) return;

    /* Open FIFO for writing (non-blocking so we don't hang without a reader) */
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        /* No reader yet – inform user */
        fprintf(stderr, "minibash: FIFO write – no reader attached yet (%s)\n", strerror(errno));
        /* Try blocking open in a forked child so shell stays responsive */
        pid_t pid = fork();
        if (pid == 0) {
            int bfd = open(FIFO_PATH, O_WRONLY); /* blocks until reader */
            if (bfd < 0) { perror("minibash: FIFO open"); exit(EXIT_FAILURE); }
            dup2(bfd, STDOUT_FILENO); close(bfd);
            execvp(argv[0], argv);
            fprintf(stderr, "minibash: command not found: %s\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        if (pid > 0) {
            add_bg_pid(pid); /* write will complete when reader connects */
            printf("minibash: writing to FIFO in background (PID %d)\n", pid);
        }
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO); close(fd);
        execvp(argv[0], argv);
        fprintf(stderr, "minibash: command not found: %s\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    close(fd);
    if (pid > 0) waitpid(pid, NULL, 0);
}

/*
 * handle_fifo_read – "|||cmd" → run cmd with stdin ← FIFO
 */
void handle_fifo_read(char *cmd_str) {
    ensure_fifo();
    char tmp[MAX_INPUT];
    strncpy(tmp, cmd_str, MAX_INPUT - 1);
    tmp[MAX_INPUT - 1] = '\0';

    char *argv[MAX_ARGS];
    if (parse_args(tmp, argv) < 0) return;

    /* Open FIFO for reading (blocks until writer sends data) */
    int fd = open(FIFO_PATH, O_RDONLY);
    if (fd < 0) {
        perror("minibash: FIFO read");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, STDIN_FILENO); close(fd);
        execvp(argv[0], argv);
        fprintf(stderr, "minibash: command not found: %s\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    close(fd);
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════
 * I/O Redirection (<, >, >>)
 * ═══════════════════════════════════════════════════════════════ */

/*
 * handle_redirection – parse input for redirection operators and execute.
 * Supports: < (input), > (output overwrite), >> (output append).
 * cmd_str is modified in place.
 */
void handle_redirection(char *cmd_str) {
    char *in_file  = NULL;
    char *out_file = NULL;
    int   append   = 0;

    /* Detect >> before > */
    char *pos;

    /* Check for append >> */
    if ((pos = strstr(cmd_str, ">>")) != NULL) {
        *pos = '\0';
        out_file = trim_whitespace(pos + 2);
        append = 1;
    }
    /* Check for output redirect > */
    else if ((pos = strchr(cmd_str, '>')) != NULL) {
        *pos = '\0';
        out_file = trim_whitespace(pos + 1);
        append = 0;
    }

    /* Check for input redirect < */
    char *in_pos;
    if ((in_pos = strchr(cmd_str, '<')) != NULL) {
        *in_pos = '\0';
        in_file = trim_whitespace(in_pos + 1);
        /* If out_file was found after '<' symbol, in_file might include it – handle edge case */
    }

    char tmp[MAX_INPUT];
    strncpy(tmp, cmd_str, MAX_INPUT - 1);
    tmp[MAX_INPUT - 1] = '\0';

    char *argv[MAX_ARGS];
    if (parse_args(trim_whitespace(tmp), argv) < 0) return;

    int in_fd  = -1;
    int out_fd = -1;

    /* Open input file if specified */
    if (in_file != NULL && strlen(in_file) > 0) {
        in_fd = open(in_file, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "minibash: cannot open '%s': %s\n", in_file, strerror(errno));
            return;
        }
    }

    /* Open output file if specified */
    if (out_file != NULL && strlen(out_file) > 0) {
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        out_fd = open(out_file, flags, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "minibash: cannot open '%s': %s\n", out_file, strerror(errno));
            if (in_fd != -1) close(in_fd);
            return;
        }
    }

    pid_t pid = exec_cmd(argv, in_fd, out_fd, 0 /* foreground */);
    if (in_fd  != -1) close(in_fd);
    if (out_fd != -1) close(out_fd);
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════
 * Sequential execution (;) – up to 4 commands
 * ═══════════════════════════════════════════════════════════════ */

void handle_sequential(char *input) {
    char *cmds[MAX_CMDS];
    int   n = 0;
    char *token = strtok(input, ";");
    while (token != NULL && n < MAX_CMDS) {
        cmds[n++] = trim_whitespace(token);
        token = strtok(NULL, ";");
    }
    if (n > 4) {
        fprintf(stderr, "minibash: sequential – max 4 commands supported\n");
        return;
    }

    for (int i = 0; i < n; i++) {
        if (strlen(cmds[i]) == 0) continue;
        char tmp[MAX_INPUT];
        strncpy(tmp, cmds[i], MAX_INPUT - 1);
        tmp[MAX_INPUT - 1] = '\0';
        char *argv[MAX_ARGS];
        if (parse_args(tmp, argv) < 0) return;
        pid_t pid = exec_cmd(argv, -1, -1, 0);
        if (pid > 0) waitpid(pid, NULL, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Conditional execution (&& / ||) – up to 4 operators
 * ═══════════════════════════════════════════════════════════════ */

/*
 * run_single – run a single command string, return exit status (0 = success).
 */
int run_single(char *cmd_str) {
    char tmp[MAX_INPUT];
    strncpy(tmp, cmd_str, MAX_INPUT - 1);
    tmp[MAX_INPUT - 1] = '\0';
    char *argv[MAX_ARGS];
    if (parse_args(trim_whitespace(tmp), argv) < 0) return 1;
    pid_t pid = exec_cmd(argv, -1, -1, 0);
    if (pid < 0) return 1;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/*
 * handle_conditional – parse && / || chains and execute conditionally.
 * Tokens alternate between command strings and operators ("&&" or "||").
 */
void handle_conditional(char *input) {
    /* Tokenise by && and || while remembering which operator came between */
    char cmds[MAX_CMDS][MAX_INPUT];
    char ops[MAX_CMDS][3]; /* "&&" or "||" */
    int  ncmds = 0;
    int  nops  = 0;

    char buf[MAX_INPUT];
    strncpy(buf, input, MAX_INPUT - 1);
    buf[MAX_INPUT - 1] = '\0';

    char *p = buf;
    char *seg_start = p;

    while (*p != '\0') {
        if (p[0] == '&' && p[1] == '&') {
            /* End of current command segment */
            size_t len = (size_t)(p - seg_start);
            if (len >= MAX_INPUT) len = MAX_INPUT - 1;
            strncpy(cmds[ncmds], seg_start, len);
            cmds[ncmds][len] = '\0';
            ncmds++;
            strcpy(ops[nops++], "&&");
            p += 2;
            seg_start = p;
        } else if (p[0] == '|' && p[1] == '|') {
            size_t len = (size_t)(p - seg_start);
            if (len >= MAX_INPUT) len = MAX_INPUT - 1;
            strncpy(cmds[ncmds], seg_start, len);
            cmds[ncmds][len] = '\0';
            ncmds++;
            strcpy(ops[nops++], "||");
            p += 2;
            seg_start = p;
        } else {
            p++;
        }
    }
    /* Last segment */
    strncpy(cmds[ncmds++], seg_start, MAX_INPUT - 1);
    cmds[ncmds - 1][MAX_INPUT - 1] = '\0';

    if (ncmds > 5) {
        fprintf(stderr, "minibash: conditional – max 4 operators supported\n");
        return;
    }

    /* Execute with short-circuit evaluation */
    int last_status = run_single(trim_whitespace(cmds[0]));
    for (int i = 0; i < nops; i++) {
        if (strcmp(ops[i], "&&") == 0) {
            if (last_status == 0) {
                last_status = run_single(trim_whitespace(cmds[i + 1]));
            }
            /* else skip */
        } else { /* || */
            if (last_status != 0) {
                last_status = run_single(trim_whitespace(cmds[i + 1]));
            }
            /* else skip */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ++ – Append text files to each other (binary operation)
 * minibash$ sample1.txt ++ sample2.txt
 * Appends sample2 → sample1 AND sample1_original → sample2
 * ═══════════════════════════════════════════════════════════════ */

/*
 * append_file_to_file – read all of src and append to dst.
 */
int append_file_to_file(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "minibash: cannot open '%s': %s\n", src, strerror(errno));
        return -1;
    }
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (dst_fd < 0) {
        fprintf(stderr, "minibash: cannot open '%s': %s\n", dst, strerror(errno));
        close(src_fd);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, (size_t)n) != n) {
            perror("minibash: write");
            close(src_fd); close(dst_fd);
            return -1;
        }
    }
    close(src_fd);
    close(dst_fd);
    return 0;
}

void handle_append_files(char *file1, char *file2) {
    file1 = trim_whitespace(file1);
    file2 = trim_whitespace(file2);

    /* Save original content of file1 before modification */
    /* Read file1 into a temp buffer so we can append it to file2 afterwards */
    int fd1 = open(file1, O_RDONLY);
    if (fd1 < 0) {
        fprintf(stderr, "minibash: cannot open '%s': %s\n", file1, strerror(errno));
        return;
    }
    /* Determine size of file1 */
    off_t size1 = lseek(fd1, 0, SEEK_END);
    lseek(fd1, 0, SEEK_SET);
    char *buf1 = malloc((size_t)size1 + 1);
    if (!buf1) { perror("malloc"); close(fd1); return; }
    read(fd1, buf1, (size_t)size1);
    buf1[size1] = '\0';
    close(fd1);

    /* Append file2 → file1 */
    if (append_file_to_file(file2, file1) < 0) { free(buf1); return; }

    /* Append original file1 content → file2 */
    int fd2 = open(file2, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd2 < 0) {
        fprintf(stderr, "minibash: cannot open '%s': %s\n", file2, strerror(errno));
        free(buf1); return;
    }
    write(fd2, buf1, (size_t)size1);
    close(fd2);
    free(buf1);

    printf("minibash: appended '%s' ↔ '%s' successfully\n", file1, file2);
}

/* ═══════════════════════════════════════════════════════════════
 * # – Count words in a .txt file
 * ═══════════════════════════════════════════════════════════════ */

void handle_word_count(char *filename) {
    filename = trim_whitespace(filename);
    if (strlen(filename) == 0) {
        fprintf(stderr, "minibash: # requires a filename\n");
        return;
    }
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "minibash: cannot open '%s': %s\n", filename, strerror(errno));
        return;
    }
    char buf[4096];
    ssize_t n;
    int word_count  = 0;
    int in_word     = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (isspace((unsigned char)buf[i])) {
                in_word = 0;
            } else {
                if (!in_word) { word_count++; in_word = 1; }
            }
        }
    }
    close(fd);
    printf("%d\n", word_count);
}

/* ═══════════════════════════════════════════════════════════════
 * + – Concatenate .txt files → stdout (up to 4 files)
 * ═══════════════════════════════════════════════════════════════ */

void handle_concat_files(char *input) {
    /* Split on '+' */
    char *files[MAX_CMDS];
    int   n = 0;
    char *token = strtok(input, "+");
    while (token != NULL && n < MAX_CMDS) {
        files[n++] = trim_whitespace(token);
        token = strtok(NULL, "+");
    }
    if (n < 2) {
        fprintf(stderr, "minibash: + requires at least two files\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        int fd = open(files[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "minibash: cannot open '%s': %s\n", files[i], strerror(errno));
            return;
        }
        char buf[4096];
        ssize_t bytes;
        while ((bytes = read(fd, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)bytes);
        }
        close(fd);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Background (&) – run a single command in background
 * ═══════════════════════════════════════════════════════════════ */

void handle_background(char *cmd_str) {
    char tmp[MAX_INPUT];
    strncpy(tmp, cmd_str, MAX_INPUT - 1);
    tmp[MAX_INPUT - 1] = '\0';
    char *argv[MAX_ARGS];
    if (parse_args(trim_whitespace(tmp), argv) < 0) return;
    exec_cmd(argv, -1, -1, 1 /* background */);
}

/* ═══════════════════════════════════════════════════════════════
 * Input dispatcher – detect operator and route accordingly
 * ═══════════════════════════════════════════════════════════════ */

/*
 * contains_op – search for a multi-character operator in str.
 * Returns pointer to first occurrence or NULL.
 * Used before single-character searches to avoid false positives.
 */
char *find_op(const char *str, const char *op) {
    return strstr(str, op);
}

/*
 * dispatch – main routing function. Receives a raw input line and
 * decides which handler to call.
 */
void dispatch(char *input) {
    input = trim_whitespace(input);

    if (strlen(input) == 0) return;

    /* ── Special commands (no fork needed) ── */
    if (strcmp(input, "killmb") == 0) {
        cmd_killmb();
        return;
    }
    if (strcmp(input, "killallmb") == 0) {
        cmd_killallmb();
        return;
    }
    if (strcmp(input, "pstop") == 0) {
        cmd_pstop();
        return;
    }
    if (strcmp(input, "cont") == 0) {
        cmd_cont();
        return;
    }
    if (strcmp(input, "numbg") == 0) {
        cmd_numbg();
        return;
    }
    if (strcmp(input, "killbp") == 0) {
        cmd_killbp();
        return;
    }

    /* ── Word count: # filename ── */
    if (input[0] == '#') {
        handle_word_count(input + 1);
        return;
    }

    /* ── FIFO: ||| (must be checked BEFORE normal | check) ── */
    if (find_op(input, "|||") != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        /* Determine direction */
        char *pos = strstr(buf, "|||");
        if (pos == buf) {
            /* |||cmd – read from FIFO */
            handle_fifo_read(trim_whitespace(pos + 3));
        } else {
            /* cmd||| – write to FIFO */
            *pos = '\0';
            handle_fifo_write(trim_whitespace(buf));
        }
        return;
    }

    /* ── Conditional &&/|| (must be checked BEFORE single |) ── */
    if (find_op(input, "&&") != NULL || 
        (find_op(input, "||") != NULL && find_op(input, "|||") == NULL)) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        handle_conditional(buf);
        return;
    }

    /* ── Reverse piping ~ ── */
    if (strchr(input, '~') != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        char *segs[MAX_CMDS];
        int   n = 0;
        char *tok = strtok(buf, "~");
        while (tok != NULL && n < MAX_CMDS) {
            segs[n++] = trim_whitespace(tok);
            tok = strtok(NULL, "~");
        }
        if (n > 5) {
            fprintf(stderr, "minibash: ~ supports at most 4 operations\n");
            return;
        }
        handle_reverse_pipe(segs, n);
        return;
    }

    /* ── Normal piping | ── */
    if (strchr(input, '|') != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        char *segs[MAX_CMDS];
        int   n = 0;
        char *tok = strtok(buf, "|");
        while (tok != NULL && n < MAX_CMDS) {
            segs[n++] = trim_whitespace(tok);
            tok = strtok(NULL, "|");
        }
        if (n > 5) {
            fprintf(stderr, "minibash: | supports at most 4 pipe operations\n");
            return;
        }
        handle_pipe(segs, n);
        return;
    }

    /* ── Sequential execution ; ── */
    if (strchr(input, ';') != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        handle_sequential(buf);
        return;
    }

    /* ── I/O Redirection (<, >, >>) ── */
    if (strstr(input, ">>") != NULL ||
        strchr(input, '>') != NULL  ||
        strchr(input, '<') != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        handle_redirection(buf);
        return;
    }

    /* ── File append ++ ── */
    if (find_op(input, "++") != NULL) {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        char *pos = strstr(buf, "++");
        *pos = '\0';
        handle_append_files(buf, pos + 2);
        return;
    }

    /* ── File concatenation + (at least one .txt token) ── */
    /* Check: input contains '+' not preceded/followed by another '+',
       and at least one operand looks like a .txt file.
       We do a simple check: if '+' is present and not '++', it's concat. */
    {
        /* Count single '+' occurrences (not '++') */
        char tmp2[MAX_INPUT];
        strncpy(tmp2, input, MAX_INPUT - 1);
        tmp2[MAX_INPUT - 1] = '\0';
        int has_single_plus = 0;
        for (int i = 0; tmp2[i] != '\0'; i++) {
            if (tmp2[i] == '+' && tmp2[i+1] != '+' && (i == 0 || tmp2[i-1] != '+')) {
                has_single_plus = 1;
                break;
            }
        }
        if (has_single_plus) {
            char buf[MAX_INPUT];
            strncpy(buf, input, MAX_INPUT - 1);
            buf[MAX_INPUT - 1] = '\0';
            handle_concat_files(buf);
            return;
        }
    }

    /* ── Background & ── */
    if (input[strlen(input) - 1] == '&') {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        buf[strlen(buf) - 1] = '\0'; /* remove trailing '&' */
        handle_background(buf);
        return;
    }

    /* ── Default: plain command execution ── */
    {
        char buf[MAX_INPUT];
        strncpy(buf, input, MAX_INPUT - 1);
        buf[MAX_INPUT - 1] = '\0';
        char *argv[MAX_ARGS];
        if (parse_args(buf, argv) < 0) return;
        pid_t pid = exec_cmd(argv, -1, -1, 0);
        if (pid > 0) waitpid(pid, NULL, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SIGCHLD handler – reap background zombies automatically
 * ═══════════════════════════════════════════════════════════════ */
void sigchld_handler(int sig) {
    (void)sig;
    reap_zombies();
}

/* ═══════════════════════════════════════════════════════════════
 * main – REPL loop
 * ═══════════════════════════════════════════════════════════════ */
int main(void) {
    char input[MAX_INPUT];

    /* Set up SIGCHLD handler to auto-reap background children */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    printf("Welcome to minibash! Type 'killmb' to exit.\n");

    /* ── Infinite REPL loop ── */
    while (1) {
        printf("minibash$ ");
        fflush(stdout);

        /* Read a line of input */
        if (fgets(input, MAX_INPUT, stdin) == NULL) {
            /* EOF (Ctrl-D) – exit gracefully */
            printf("\nminibash: EOF received, exiting.\n");
            break;
        }

        /* Strip the trailing newline */
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') {
            input[len - 1] = '\0';
        }

        /* Dispatch to the appropriate handler */
        dispatch(input);

        /* Opportunistically reap any finished background processes */
        reap_zombies();
    }

    return 0;
}
