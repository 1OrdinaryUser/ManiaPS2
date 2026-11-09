/* PS2 has no dynamic loader: the game logic is linked statically (RETRO_STANDALONE=0). */
#ifndef PS2_DLFCN_SHIM_H
#define PS2_DLFCN_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 4
#define RTLD_LOCAL  8
static inline void *dlopen(const char *f, int m) { (void)f; (void)m; return 0; }
static inline void *dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
static inline int dlclose(void *h) { (void)h; return 0; }
static inline char *dlerror(void) { return (char *)"dynamic loading unsupported on PS2"; }
#ifdef __cplusplus
}
#endif
#endif
