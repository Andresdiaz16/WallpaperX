// Test-only preload for the ThreadSanitizer tier on the NVIDIA driver; never linked into anything.
//
// The driver (libnvidia-eglcore) looks up pthread_create and friends with dlvsym on libc's own
// handle, which goes around ThreadSanitizer's interceptors. The sanitizer then never learns of the
// thread the driver starts in vkCreateDevice, and that thread's first intercepted call (a calloc)
// crashes the sanitizer runtime. Here a pthread_* lookup through dlvsym is answered from the global
// scope instead, where the sanitizer's interceptor is found first. dlsym is deliberately left alone:
// overriding it changes what dlsym(RTLD_NEXT, ...) returns to the tests' own interposers.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

// libc's own definitions, bound by version so the dlvsym defined below never calls itself.
__asm__(".symver libc_dlsym, dlsym@GLIBC_2.34");
__asm__(".symver libc_dlvsym, dlvsym@GLIBC_2.34");
extern void* libc_dlsym(void* handle, const char* name);
extern void* libc_dlvsym(void* handle, const char* name, const char* version);

// Exported as dlvsym@@GLIBC_2.2.5 by tsan_dlvsym_shim.map: the version the driver binds to.
void* dlvsym(void* handle, const char* name, const char* version) {
    void* found = libc_dlvsym(handle, name, version);
    if (name != NULL && strncmp(name, "pthread_", 8) == 0) {
        void* global = libc_dlsym(RTLD_DEFAULT, name);
        if (global != NULL) return global;
    }
    return found;
}
