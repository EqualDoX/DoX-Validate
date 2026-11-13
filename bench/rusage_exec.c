/* rusage_exec RESULT_FILE PROGRAM [ARG...]
 *
 * Runs PROGRAM (stdin/stdout/stderr inherited), waits for it and writes one line to RESULT_FILE:
 *   wall_ns utime_us stime_us maxrss_kb wait_status
 * from clock_gettime(CLOCK_MONOTONIC) around fork..wait4 and the wait4 rusage of the child (CPU time of the child and
 * its waited-for descendants; ru_maxrss = peak RSS of the largest process among them).
 *
 * Why a separate small process: Linux carries the RSS high-water mark of the previous memory image across fork and exec
 * into ru_maxrss, so a child spawned directly from the Python harness would report at least the harness's own RSS
 * (about 30 MiB).  This helper is a few hundred KiB; its forked child starts from that small image.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: rusage_exec RESULT_FILE PROGRAM [ARG...]\n");
        return 127;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 127;
    }
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        perror("execvp");
        _exit(127);
    }
    int status = 0;
    struct rusage ru;
    while (wait4(pid, &status, 0, &ru) < 0) {
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long wall = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    FILE *f = fopen(argv[1], "w");
    if (!f) return 127;
    fprintf(f, "%lld %lld %lld %ld %d\n", wall, (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec,
            (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec, ru.ru_maxrss, status);
    fclose(f);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}
