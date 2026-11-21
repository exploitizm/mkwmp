// plat.h on the PS5 (mkw-recomp's PS5 title, the plugin linked in: ps5/plugin-sources.txt). The console is FreeBSD
// underneath, so most of plat_posix.c holds; what differs is here:
//   - no /proc: memory is asked of the kernel (sceKernelQueryMemoryProtection), and a protected page is opened with
//     sceKernelMprotect keeping every other bit it had (the GPU's too: the title maps guest memory for the GPU);
//   - no dynamic libraries (the title loads none): plat_dl_* answer none, so voice and the camera stay off;
//   - no executable path: files live in the mod's folder (plat_set_home, from the plugin API's mod_dir);
//   - one process: the log needs no lock against a second copy of the game.
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5platform/kernel.h>

typedef struct { void (*fn)(void *); void *arg; } Start;

static void *thread_main(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int plat_thread_start(void (*fn)(void *arg), void *arg) {
    Start *s = malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn, s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, s)) { free(s); return 0; }
    pthread_detach(t);
    return 1;
}

struct PlatEvent {
    pthread_mutex_t mu;
    pthread_cond_t cv;   // on CLOCK_MONOTONIC
    int manual, set;
};

PlatEvent *plat_event_new(int manual_reset) {
    PlatEvent *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, &a);
    pthread_condattr_destroy(&a);
    e->manual = manual_reset;
    return e;
}

void plat_event_set(PlatEvent *e) {
    pthread_mutex_lock(&e->mu);
    e->set = 1;
    if (e->manual) pthread_cond_broadcast(&e->cv);
    else pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void plat_event_reset(PlatEvent *e) {
    pthread_mutex_lock(&e->mu);
    e->set = 0;
    pthread_mutex_unlock(&e->mu);
}

int plat_event_wait(PlatEvent *e, uint32_t ms) {
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000u;
    until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&e->mu);
    int r = 0;
    while (!e->set && r != ETIMEDOUT)
        r = ms == PLAT_FOREVER ? pthread_cond_wait(&e->cv, &e->mu) : pthread_cond_timedwait(&e->cv, &e->mu, &until);
    int got = e->set;
    if (got && !e->manual) e->set = 0;   // auto-reset: this wait consumed it
    pthread_mutex_unlock(&e->mu);
    return got;
}

unsigned long plat_thread_id(void) { return (unsigned long)pthread_getthreadid_np(); }
unsigned long plat_last_error(void) { return (unsigned long)errno; }

// ---- files
static char g_home[PATH_MAX];   // plat_set_home's folder, with a trailing '/'

void plat_set_home(const char *dir) {
    size_t n = dir ? strlen(dir) : 0;
    if (!n || n + 2 > sizeof g_home) { g_home[0] = 0; return; }
    memcpy(g_home, dir, n + 1);
    if (g_home[n - 1] != '/') g_home[n] = '/', g_home[n + 1] = 0;
}

int plat_home_path(char *out, size_t cap, const char *name) {
    if (g_home[0]) return snprintf(out, cap, "%s%s", g_home, name) < (int)cap;
    return plat_exe_path(out, cap, name);
}

// The title's package (/app0) is read-only and holds nothing of ours: without a home there is no folder.
int plat_exe_path(char *out, size_t cap, const char *name) {
    (void)out, (void)cap, (void)name;
    errno = ENOENT;
    return 0;
}

long plat_read_file(const char *path, void *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < cap) {
        ssize_t r = read(fd, (char *)buf + got, cap - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return (long)got;
}

// A PlatFile is the descriptor + 1 (so NULL stays "none"). One title runs at a time: no lock against another copy.
PlatFile *plat_log_create(const char *path, int *in_use) {
    *in_use = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    return (PlatFile *)(intptr_t)(fd + 1);
}

void plat_file_write(PlatFile *f, const void *p, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p = (const char *)p + w, n -= (size_t)w;
    }
}

// ---- environment (the title's own, which mkw-recomp fills from mkw.ini's MKW_* keys only)
int plat_getenv(const char *name, char *buf, size_t cap) {
    const char *v = getenv(name);
    if (!v) return 0;
    if (buf && cap) snprintf(buf, cap, "%s", v);
    return 1;
}

void plat_setenv(const char *name, const char *value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}

// ---- dynamic libraries: none on the console (voice.c and camlib.c then report themselves unavailable)
void *plat_dl_open(const char *path) {
    (void)path;
    errno = ENOSYS;
    return NULL;
}
void *plat_dl_sym(void *lib, const char *name) {
    (void)lib, (void)name;
    return NULL;
}
void plat_dl_close(void *lib) { (void)lib; }
void *plat_dl_loaded(const char *base) {
    (void)base;
    return NULL;
}

// ---- memory: the kernel's own record of a mapping (one call, no file to parse)
static int query(uintptr_t a, uintptr_t *lo, uintptr_t *hi, uint32_t *prot) {
    void *start = NULL, *end = NULL;
    if (a < 0x10000 || sceKernelQueryMemoryProtection((void *)a, &start, &end, prot)) return 0;
    *lo = (uintptr_t)start, *hi = (uintptr_t)end;
    return a >= *lo && a < *hi;
}

size_t plat_mem_readable(const void *p) {
    uintptr_t lo, hi;
    uint32_t prot;
    if (!query((uintptr_t)p, &lo, &hi, &prot) || !(prot & PS5_KERNEL_PROT_CPU_READ)) return 0;
    return (size_t)(hi - (uintptr_t)p);
}

// A writable page takes the store as it is; a read-only one (a vtable) is opened for it and closed again with the same
// bits, the GPU's included. A write the title's own write watch would have caught (texture memory) is not one we make:
// the probe writes vtable slots and the SDK's clock source only.
int plat_mem_write_u64(uint64_t *at, uint64_t v) {
    uintptr_t lo, hi;
    uint32_t prot;
    if (((uintptr_t)at & 7) || !query((uintptr_t)at, &lo, &hi, &prot) || (uintptr_t)(at + 1) > hi) {
        errno = EFAULT;
        return 0;
    }
    if (prot & PS5_KERNEL_PROT_CPU_WRITE) {
        __atomic_store_n(at, v, __ATOMIC_RELEASE);
        return 1;
    }
    uintptr_t page = (uintptr_t)at & ~(uintptr_t)(PS5_KERNEL_PAGE_SIZE - 1);
    if (sceKernelMprotect((void *)page, PS5_KERNEL_PAGE_SIZE, (int)(prot | PS5_KERNEL_PROT_CPU_READ |
                                                                    PS5_KERNEL_PROT_CPU_WRITE)))
        return 0;
    __atomic_store_n(at, v, __ATOMIC_RELEASE);
    sceKernelMprotect((void *)page, PS5_KERNEL_PAGE_SIZE, (int)prot);
    return 1;
}

// ---- input: the title owns the controllers; the voice UI's keyboard shortcuts do not exist here
int plat_game_focused(void) { return 0; }
int plat_key_down(int key) { (void)key; return 0; }

// ---- process
void plat_no_error_dialogs(void) {}
