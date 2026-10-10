/* Runs a program and writes what it used to a file: its peak resident set (KB, ru_maxrss) and its
 * user and system CPU seconds. Exits as the program did.
 *
 *     rusage OUT PROGRAM [ARGS...]
 *
 * The benchmarks run programs through this on Linux, where a process's peak resident set starts
 * at its parent's: Linux carries the forking process's peak through exec, so a program started
 * straight from Python could never measure less than Python itself (about 15–20 MB). This
 * launcher is a few hundred KB, and the program it forks inherits that instead. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: rusage OUT PROGRAM [ARGS...]\n");
        return 2;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("rusage: fork");
        return 2;
    }
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        perror(argv[2]);
        _exit(127);
    }
    int status;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) {
        perror("rusage: wait4");
        return 2;
    }
    FILE *out = fopen(argv[1], "w");
    if (out) {
        fprintf(out, "%ld %.6f %.6f\n", ru.ru_maxrss, ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
        fclose(out);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
