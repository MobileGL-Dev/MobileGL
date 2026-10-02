// dmahash <pid> : for each dma-buf fd of <pid> at least 1 MB, print inode, size and a sampled checksum.
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%s/fd", argv[1]);
    DIR* d = opendir(dir);
    if (!d) { perror("opendir"); return 1; }
    struct dirent* e;
    while ((e = readdir(d))) {
        char p[128], l[128] = {0};
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (readlink(p, l, sizeof l - 1) <= 0 || strncmp(l, "/dmabuf", 7) != 0) continue;
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        struct stat st; fstat(fd, &st);
        if (st.st_size < (1 << 20)) { close(fd); continue; }
        const uint8_t* m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { printf("fd %s ino %lu mmap failed\n", e->d_name, (unsigned long)st.st_ino); close(fd); continue; }
        uint64_t h = 1469598103934665603ull; unsigned nz = 0;
        for (off_t i = 0; i < st.st_size; i += 64) { h = (h ^ m[i]) * 1099511628211ull; nz += m[i] != 0; }
        printf("fd %s ino %lu size %ld hash %016llx nz %u\n", e->d_name, (unsigned long)st.st_ino, (long)st.st_size,
               (unsigned long long)h, nz);
        munmap((void*)m, st.st_size); close(fd);
    }
    return 0;
}
