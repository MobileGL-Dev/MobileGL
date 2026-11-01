/*
 * Starts the vendor Mali counter producer so perfetto can record the
 * "gpu.counters" data source. Run as root, with SELinux permissive.
 *
 * Build: aarch64-linux-android30-clang -O2 -o gpuprod gpuprod.c
 */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    void* lib = dlopen("/vendor/lib64/libgpudataproducer.so", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    void (*start)(void) = (void (*)(void))dlsym(lib, "start");
    if (!start) {
        fprintf(stderr, "libgpudataproducer.so exports no start()\n");
        return 1;
    }
    fprintf(stderr, "starting producer\n");
    start();
    for (;;)
        pause();
}
