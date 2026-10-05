/* pthread/tsd_private.h, Barm's own: the part of Darwin's private header JavaScriptCore needs.
 *
 * With it, WTF (HAVE_FAST_TLS) and libpas (PAS_HAVE_PTHREAD_MACHDEP_H) keep their thread-local
 * data in the slots Darwin reserves for JavaScriptCore and read them straight from the thread's
 * TSD area, as Apple's own builds do, instead of calling pthread_getspecific each time (the
 * allocator's thread cache and the engine's current thread are read on nearly every call).
 *
 * The slots are the system JavaScriptCore's: a process that also loaded JavaScriptCore.framework
 * would share them, which a Barm program, linking its own engine statically, doesn't do. */

#ifndef BARM_PTHREAD_TSD_PRIVATE_H
#define BARM_PTHREAD_TSD_PRIVATE_H

#include <pthread.h>
#include <stdint.h>

#if !defined(__APPLE__) || !(defined(__arm64__) || defined(__x86_64__))
#error "Barm's pthread/tsd_private.h is for Darwin on arm64 or x86_64"
#endif

/* the keys Darwin reserves for JavaScriptCore */
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY0 90
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY1 91
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY2 92
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY3 93
#define __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY4 94

#ifdef __cplusplus
extern "C" {
#endif

/* (libsystem_pthread's, exported: gives a reserved key its destructor) */
extern int pthread_key_init_np(int, void (*)(void *));

#ifdef __cplusplus
}
#endif

#if defined(__arm64__)
/* the thread's TSD area (TPIDRRO_EL0 holds its address) */
__attribute__((always_inline, const)) static inline void **_barm_tsd_base(void)
{
    uint64_t tsd;
    __asm__("mrs %0, TPIDRRO_EL0" : "=r"(tsd));
    return (void **)(uintptr_t)tsd;
}

__attribute__((always_inline)) static inline void *_pthread_getspecific_direct(unsigned long slot)
{
    return _barm_tsd_base()[slot];
}

__attribute__((always_inline)) static inline int _pthread_setspecific_direct(unsigned long slot, void *value)
{
    _barm_tsd_base()[slot] = value;
    return 0;
}
#else
/* (x86_64: the TSD area is %gs-relative) */
__attribute__((always_inline)) static inline void *_pthread_getspecific_direct(unsigned long slot)
{
    void *value;
    __asm__("mov %%gs:%1, %0" : "=r"(value) : "m"(*(void **)(slot * sizeof(void *))));
    return value;
}

__attribute__((always_inline)) static inline int _pthread_setspecific_direct(unsigned long slot, void *value)
{
    __asm__("movq %1, %%gs:%0" : "=m"(*(void **)(slot * sizeof(void *))) : "rn"(value));
    return 0;
}
#endif

#endif /* BARM_PTHREAD_TSD_PRIVATE_H */
