/* Kernel event flags: 64-bit patterns, AND/OR waits with optional clearing.
 * One lock and one condition per flag; every waiter rechecks its own condition after a
 * broadcast, so priority-ordered flags are served in wakeup order (no PS4 scheduler). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_SINGLE 0x10
#define ATTR_MULTI 0x20
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define WAIT_CLEAR_ALL 0x10
#define WAIT_CLEAR_PAT 0x20
#define FLAG_MAGIC UINT32_C(0x45464C47)
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME /* winpthreads conditions accept only CLOCK_REALTIME */
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
typedef struct EventFlag {
    uint32_t magic, attr;
    uint64_t pattern;
    unsigned waiters, generation; /* generation: bumped by cancel and delete */
    int deleted;
    int32_t wake_result;
    pthread_cond_t event;
    char name[32];
} EventFlag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static size_t created, deleted, waits, sets, timeouts;
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static EventFlag *valid(EventFlag *f) { return f && f->magic==FLAG_MAGIC && !f->deleted ? f : NULL; }
static ABI int32_t flag_create(EventFlag **out,const char *name,uint32_t attr,uint64_t initial,const void *options) {
    if (!out || !name || attr&~UINT32_C(0x33) || (attr&3)==3 || (attr&0x30)==0x30) return ERR(22);
    if (options) { fputs("STOP: event flag options are not implemented\n",stderr); exit(21); }
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_condattr_t ca;
    host_check(pthread_condattr_init(&ca));
    host_check(pthread_condattr_setclock(&ca,WAIT_CLOCK));
    host_check(pthread_cond_init(&f->event,&ca));
    host_check(pthread_condattr_destroy(&ca));
    f->magic=FLAG_MAGIC; f->attr=attr ? attr : ATTR_SINGLE; f->pattern=initial;
    snprintf(f->name,sizeof(f->name),"%s",name);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    *out=f;
    return 0;
}
static int matches(const EventFlag *f,uint64_t bits,uint32_t mode) {
    return mode&WAIT_AND ? (f->pattern&bits)==bits : (f->pattern&bits)!=0;
}
static void consume(EventFlag *f,uint64_t bits,uint32_t mode,uint64_t *result) {
    if (result) *result=f->pattern;
    if (mode&WAIT_CLEAR_ALL) f->pattern=0;
    else if (mode&WAIT_CLEAR_PAT) f->pattern&=~bits;
}
static int bad_mode(uint64_t bits,uint32_t mode) {
    uint32_t kind=mode&3, clear=mode&0x30;
    return !bits || (kind!=WAIT_AND && kind!=WAIT_OR) || clear==0x30 || mode&~UINT32_C(0x33);
}
static ABI int32_t flag_wait(EventFlag *handle,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    if (bad_mode(bits,mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    int32_t r=0;
    if (!f) r=ERR(3);
    else if (f->attr&ATTR_SINGLE && f->waiters) r=ERR(1); /* EPERM: one waiter only */
    else if (matches(f,bits,mode)) consume(f,bits,mode,result);
    else if (timeout && !*timeout) { r=ERR(60); ++timeouts; if (result) *result=f->pattern; }
    else {
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        unsigned generation=f->generation;
        ++f->waiters;
        runtime_wait_begin(timeout ? "event flag (timed)" : "event flag",f,f->name);
        for (;;) {
            int e=timeout ? pthread_cond_timedwait(&f->event,&lock,&end) : pthread_cond_wait(&f->event,&lock);
            if (f->generation!=generation) { r=f->wake_result; break; }
            if (matches(f,bits,mode)) { consume(f,bits,mode,result); break; }
            if (e==ETIMEDOUT) { r=ERR(60); ++timeouts; if (result) *result=f->pattern; break; }
            host_check(e);
        }
        runtime_wait_end();
        --f->waiters;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=r ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        if (f->deleted && !f->waiters) {
            host_check(pthread_cond_destroy(&f->event));
            f->magic=0; free(f);
        }
    }
    if (!r) ++waits;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t flag_poll(EventFlag *handle,uint64_t bits,uint32_t mode,uint64_t *result) {
    if (bad_mode(bits,mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    int32_t r=0;
    if (!f) r=ERR(3);
    else if (f->attr&ATTR_SINGLE && f->waiters) r=ERR(1);
    else if (matches(f,bits,mode)) consume(f,bits,mode,result);
    else { r=ERR(16); if (result) *result=f->pattern; } /* EBUSY */
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t flag_set(EventFlag *handle,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    int32_t r=f ? 0 : ERR(3);
    if (f) {
        f->pattern|=bits; ++sets;
        if (f->waiters) host_check(pthread_cond_broadcast(&f->event));
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t flag_clear(EventFlag *handle,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    if (f) f->pattern&=bits; /* keeps only the given bits */
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static void wake_all(EventFlag *f,int32_t result) {
    f->wake_result=result; ++f->generation;
    if (f->waiters) host_check(pthread_cond_broadcast(&f->event));
}
static ABI int32_t flag_cancel(EventFlag *handle,uint64_t pattern,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    if (f) {
        if (waiters) *waiters=(int32_t)f->waiters;
        f->pattern=pattern;
        wake_all(f,ERR(85)); /* ECANCELED */
    }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t flag_delete(EventFlag *handle) {
    pthread_mutex_lock(&lock);
    EventFlag *f=valid(handle);
    if (f) {
        f->deleted=1; ++deleted;
        wake_all(f,ERR(13)); /* EACCES: deleted while waiting */
        if (!f->waiters) { host_check(pthread_cond_destroy(&f->event)); f->magic=0; free(f); }
    }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",flag_create}, {"sceKernelDeleteEventFlag",flag_delete},
    {"sceKernelWaitEventFlag",flag_wait}, {"sceKernelPollEventFlag",flag_poll},
    {"sceKernelSetEventFlag",flag_set}, {"sceKernelClearEventFlag",flag_clear},
    {"sceKernelCancelEventFlag",flag_cancel},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, waits=%zu, sets=%zu, timeouts=%zu\n",
           created,deleted,waits,sets,timeouts);
    pthread_mutex_unlock(&lock);
}
