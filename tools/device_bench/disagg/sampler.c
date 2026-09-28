// Device-side /proc sampler: per-tid syscall occupancy + CPU + switch counters.
// strace attach is SELinux-blocked on this device; /proc/<tid>/syscall is
// readable same-uid via run-as, which gives the blocked-syscall histogram.
// Usage: sampler <pid> <duration_s> <interval_ms> <out.csv>
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void onsig(int s) { (void)s; g_stop = 1; }

#define MAXT 128
#define MAXSY 400

struct T {
    int tid;
    char name[32];
    long long cpu0;        // utime+stime at start (ticks)
    long long vcs0;        // nr_voluntary_switches at start
    long long run;         // samples with syscall "running"
    long long sys[MAXSY];  // samples while in syscall nr
    long long openerr;
};

static long long readfile(const char *p, char *buf, int n) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    long long r = read(fd, buf, n - 1);
    close(fd);
    if (r > 0) buf[r] = 0;
    return r;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: sampler pid dur_s interval_ms out\n"); return 2; }
    int pid = atoi(argv[1]);
    int durMs = atoi(argv[2]) * 1000;
    int intMs = argc > 4 ? atoi(argv[3]) : 2;
    const char *out = argc > 4 ? argv[4] : "sampler.csv";

    struct T ts[MAXT];
    int n = 0;
    char path[256], buf[8192];

    // initial scan
    for (int pass = 0; pass < 2; pass++) {
        snprintf(path, sizeof path, "/proc/%d/task", pid);
        DIR *d = opendir(path);
        if (!d) { perror("opendir task"); return 1; }
        struct dirent *e;
        n = 0;
        while ((e = readdir(d)) && n < MAXT) {
            if (e->d_name[0] == '.') continue;
            int tid = atoi(e->d_name);
            if (tid <= 0) continue;
            struct T *t = &ts[n++];
            memset(t, 0, sizeof *t);
            t->tid = tid;
            snprintf(path, sizeof path, "/proc/%d/task/%d/comm", pid, tid);
            if (readfile(path, buf, sizeof buf) > 0) {
                strncpy(t->name, buf, sizeof t->name - 1);
                char *nl = strchr(t->name, '\n'); if (nl) *nl = 0;
            }
            snprintf(path, sizeof path, "/proc/%d/task/%d/stat", pid, tid);
            long long ut = 0, st = 0;
            if (readfile(path, buf, sizeof buf) > 0) {
                // fields: 1 pid, 2 comm, 3 state ... 14 utime, 15 stime.
                // tokens after ')' start at field 3, so utime is f==11, stime f==12.
                char *r = strrchr(buf, ')');
                if (r) {
                    int f = 0; char *sp = r + 1;
                    while (*sp) {
                        if (f == 11) ut = atoll(sp);
                        if (f == 12) { st = atoll(sp); break; }
                        while (*sp && *sp != ' ') sp++;
                        while (*sp == ' ') sp++;
                        f++;
                    }
                }
            }
            t->cpu0 = ut + st;
            snprintf(path, sizeof path, "/proc/%d/task/%d/sched", pid, tid);
            t->vcs0 = -1;
            if (readfile(path, buf, sizeof buf) > 0) {
                char *v = strstr(buf, "nr_voluntary_switches");
                if (v) {
                    char *col = strchr(v, ':');
                    if (col) t->vcs0 = atoll(col + 1);
                }
            }
        }
        closedir(d);
        if (n > 1) break;  // first pass may catch a dying thread set
        usleep(50000);
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long long elapsed = 0;
    signal(SIGTERM, onsig);
    signal(SIGINT, onsig);
    struct timespec slp = {.tv_sec = 0, .tv_nsec = (long)intMs * 1000000L};
    while (elapsed < durMs && !g_stop) {
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", pid, ts[i].tid);
            long long r = readfile(path, buf, sizeof buf);
            if (r <= 0) { ts[i].openerr++; continue; }
            if (!strncmp(buf, "running", 7)) { ts[i].run++; continue; }
            long nr = -2;
            if (buf[0] != '-') nr = strtol(buf, NULL, 10);
            if (nr < 0) { ts[i].run++; continue; }  // -1 means running
            if (nr >= MAXSY) nr = MAXSY - 1;
            ts[i].sys[nr]++;
        }
        nanosleep(&slp, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (now.tv_sec - t0.tv_sec) * 1000LL + (now.tv_nsec - t0.tv_nsec) / 1000000LL;
    }

    FILE *f = fopen(out, "w");
    if (!f) { perror("fopen out"); return 1; }
    fprintf(f, "# pid=%d dur_ms=%lld interval_ms=%d samples_per_thread~=%lld\n",
            pid, elapsed, intMs, elapsed / intMs);
    fprintf(f, "tid,name,cpu_ticks_delta,vcs_delta,run_samples,open_err,sys_hist\n");
    for (int i = 0; i < n; i++) {
        struct T *t = &ts[i];
        long long ut = 0, st = 0;
        snprintf(path, sizeof path, "/proc/%d/task/%d/stat", pid, t->tid);
        if (readfile(path, buf, sizeof buf) > 0) {
            char *r = strrchr(buf, ')');
            if (r) {
                int f2 = 0; char *sp = r + 1;
                while (*sp) {
                    if (f2 == 11) ut = atoll(sp);
                    if (f2 == 12) { st = atoll(sp); break; }
                    while (*sp && *sp != ' ') sp++;
                    while (*sp == ' ') sp++;
                    f2++;
                }
            }
        }
        long long vcs = -1;
        snprintf(path, sizeof path, "/proc/%d/task/%d/sched", pid, t->tid);
        if (readfile(path, buf, sizeof buf) > 0) {
            char *v = strstr(buf, "nr_voluntary_switches");
            if (v) {
                char *col = strchr(v, ':');
                if (col) vcs = atoll(col + 1);
            }
        }
        fprintf(f, "%d,%s,%lld,%lld,%lld,%lld,\"", t->tid, t->name,
                (ut + st) - t->cpu0, (vcs >= 0 && t->vcs0 >= 0) ? vcs - t->vcs0 : -1,
                t->run, t->openerr);
        for (int s = 0; s < MAXSY; s++)
            if (t->sys[s]) fprintf(f, "%d:%lld;", s, t->sys[s]);
        fprintf(f, "\"\n");
    }
    fclose(f);
    return 0;
}
