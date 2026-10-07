// Device-side per-thread CPU sampler for the perf matrix (P14).
//
// Usage (as root, pinned to a little core):
//   taskset 01 cpusampler <package> <interval_ms> <watch_file> <out.csv> [max_s]
//
// Every interval it reads /proc/<pid>/task/<tid>/schedstat (on-CPU ns, not the 10 ms tick
// counters of stat) for every thread of every process whose cmdline starts with <package>
// (the replay process and, for spawn/tcp, the render-server process), and keeps the last
// kRingSeconds of samples in memory. When <watch_file> (benchmark.json) appears it dumps the
// ring to <out.csv> and exits; the host cuts the benchmark's tail window out of it with the
// file's mtime as the window end (make_report.py / cpu_window.py). Timestamps are
// CLOCK_REALTIME so they compare with st_mtim.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXP 8
#define MAXT 512
#define RING 4096

struct Thread {
    int pid, tid;
    char comm[24];
};
struct Sample {
    long long t;   // CLOCK_REALTIME ns
    int index;     // into threads[]
    long long cpu; // schedstat on-CPU ns
};

static struct Thread threads[MAXT];
static int threadCount;
static struct Sample* ring;
static long long ringCap, ringHead, ringSize;

static long long NowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int ReadSmall(const char* path, char* buf, int n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int r = (int)read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return r;
}

// Pseudo-threads: pid -1 carries the Adreno kgsl cumulative busy time and the GPU / CPU clocks,
// so the host can say whether a window was GPU-bound and that the pin held.
static int Pseudo(const char* name) {
    for (int i = 0; i < threadCount; ++i)
        if (threads[i].pid == -1 && strcmp(threads[i].comm, name) == 0) return i;
    if (threadCount >= MAXT) return -1;
    threads[threadCount].pid = -1;
    threads[threadCount].tid = -1;
    snprintf(threads[threadCount].comm, sizeof threads[threadCount].comm, "%s", name);
    return threadCount++;
}

static int FindThread(int pid, int tid) {
    for (int i = 0; i < threadCount; ++i)
        if (threads[i].tid == tid && threads[i].pid == pid) return i;
    if (threadCount >= MAXT) return -1;
    char path[96], buf[64];
    snprintf(path, sizeof path, "/proc/%d/task/%d/comm", pid, tid);
    threads[threadCount].pid = pid;
    threads[threadCount].tid = tid;
    threads[threadCount].comm[0] = 0;
    if (ReadSmall(path, buf, sizeof buf) > 0) {
        buf[strcspn(buf, "\n,")] = 0;
        snprintf(threads[threadCount].comm, sizeof threads[threadCount].comm, "%s", buf);
    }
    return threadCount++;
}

static int ScanPids(const char* pkg, int* pids) {
    int n = 0;
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* e;
    size_t len = strlen(pkg);
    char path[64], buf[256];
    while ((e = readdir(d)) && n < MAXP) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        if (ReadSmall(path, buf, sizeof buf) <= 0) continue;
        if (strncmp(buf, pkg, len) == 0 && (buf[len] == 0 || buf[len] == ':')) pids[n++] = atoi(e->d_name);
    }
    closedir(d);
    return n;
}

static void Push(long long t, int index, long long cpu) {
    struct Sample* s = &ring[(ringHead + ringSize) % ringCap];
    if (ringSize == ringCap) ringHead = (ringHead + 1) % ringCap;
    else ++ringSize;
    s->t = t;
    s->index = index;
    s->cpu = cpu;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: cpusampler <package> <interval_ms> <watch_file> <out.csv> [max_s]\n");
        return 2;
    }
    const char* pkg = argv[1];
    const int intervalMs = atoi(argv[2]) > 0 ? atoi(argv[2]) : 10;
    const char* watch = argv[3];
    const char* out = argv[4];
    const long long maxNs = (argc > 5 ? atoll(argv[5]) : 1800) * 1000000000LL;
    const long long ringSeconds = 30;
    ringCap = (ringSeconds * 1000 / intervalMs + 1) * 128;
    ring = calloc((size_t)ringCap, sizeof *ring);
    if (!ring) return 1;
    const long long start = NowNs();
    int pids[MAXP], pidCount = 0;
    long long iteration = 0;
    char path[96], buf[512];
    struct stat st;
    long long mtime = -1;
    for (;;) {
        const long long now = NowNs();
        if (now - start > maxNs) break;
        if (iteration % 25 == 0 || pidCount == 0) pidCount = ScanPids(pkg, pids);
        for (int p = 0; p < pidCount; ++p) {
            snprintf(path, sizeof path, "/proc/%d/task", pids[p]);
            DIR* d = opendir(path);
            if (!d) continue;
            struct dirent* e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                const int tid = atoi(e->d_name);
                snprintf(path, sizeof path, "/proc/%d/task/%d/schedstat", pids[p], tid);
                if (ReadSmall(path, buf, sizeof buf) <= 0) continue;
                const int index = FindThread(pids[p], tid);
                if (index >= 0) Push(now, index, atoll(buf));
            }
            closedir(d);
        }
        if (iteration % 10 == 0) {
            // gpu_clock_stats: cumulative busy time per power level; its sum is busy-since-boot.
            // (gpubusy is a sliding ~1 s window, not a counter.)
            if (ReadSmall("/sys/class/kgsl/kgsl-3d0/gpu_clock_stats", buf, sizeof buf) > 0) {
                long long sum = 0;
                for (char* q = buf; *q;) {
                    char* end = q;
                    const long long v = strtoll(q, &end, 10);
                    if (end == q) break;
                    sum += v;
                    q = end;
                }
                Push(now, Pseudo("gpubusy_us"), sum);
            }
            if (ReadSmall("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", buf, sizeof buf) > 0)
                Push(now, Pseudo("gpufreq"), atoll(buf));
            if (ReadSmall("/sys/devices/system/cpu/cpufreq/policy7/scaling_cur_freq", buf, sizeof buf) > 0)
                Push(now, Pseudo("cpu7freq"), atoll(buf));
            if (ReadSmall("/sys/devices/system/cpu/cpufreq/policy2/scaling_cur_freq", buf, sizeof buf) > 0)
                Push(now, Pseudo("cpu2freq"), atoll(buf));
        }
        ++iteration;
        if (stat(watch, &st) == 0 && (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec > start) {
            mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
            break;
        }
        usleep((useconds_t)intervalMs * 1000);
    }
    FILE* f = fopen(out, "w");
    if (!f) return 1;
    fprintf(f, "# watch_mtime_ns=%lld interval_ms=%d\n", mtime, intervalMs);
    fprintf(f, "t_ns,pid,tid,comm,cpu_ns\n");
    for (long long i = 0; i < ringSize; ++i) {
        const struct Sample* s = &ring[(ringHead + i) % ringCap];
        const struct Thread* th = &threads[s->index];
        fprintf(f, "%lld,%d,%d,%s,%lld\n", s->t, th->pid, th->tid, th->comm, s->cpu);
    }
    fclose(f);
    return mtime >= 0 ? 0 : 3;
}
