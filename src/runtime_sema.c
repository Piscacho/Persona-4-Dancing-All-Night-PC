/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    pthread_mutex_lock(&lock);
    if (next_id==UINT32_MAX) { pthread_mutex_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("P4D_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        runtime_wait_begin(timeout ? "kernel semaphore (timed)" : "kernel semaphore",(const void *)(uintptr_t)id,NULL);
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_wait_end();
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    pthread_mutex_unlock(&lock);
    return n;
}
/* POSIX unnamed semaphores: the guest sem_t is one pointer, filled by sem_init. Errors set
 * the guest errno (FreeBSD numbering) and return -1. */
#define POSIX_SEM_MAGIC UINT32_C(0x50534d31)
#define POSIX_SEM_MAX 0x7fffffff
typedef struct { uint32_t magic; int32_t value; unsigned waiters; pthread_cond_t event; } PosixSem;
static int32_t posix_fail(int32_t e) { *runtime_errno()=e; return -1; }
static PosixSem *posix_sem(PosixSem **sem) { return sem && *sem && (*sem)->magic==POSIX_SEM_MAGIC ? *sem : NULL; }
static ABI int32_t posix_sem_init(PosixSem **sem,int32_t shared,uint32_t value) {
    (void)shared;
    if (!sem || value>POSIX_SEM_MAX) return posix_fail(22);
    PosixSem *s=calloc(1,sizeof(*s));
    if (!s) return posix_fail(28); /* ENOSPC */
    pthread_condattr_t attr;
    host_check(pthread_condattr_init(&attr));
    host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
    host_check(pthread_cond_init(&s->event,&attr));
    host_check(pthread_condattr_destroy(&attr));
    s->magic=POSIX_SEM_MAGIC; s->value=(int32_t)value;
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    *sem=s;
    return 0;
}
static ABI int32_t posix_sem_destroy(PosixSem **sem) {
    pthread_mutex_lock(&lock);
    PosixSem *s=posix_sem(sem);
    if (!s || s->waiters) { pthread_mutex_unlock(&lock); return posix_fail(s ? 16 : 22); } /* EBUSY/EINVAL */
    s->magic=0; ++deleted; *sem=NULL;
    pthread_mutex_unlock(&lock);
    host_check(pthread_cond_destroy(&s->event));
    free(s);
    return 0;
}
static int32_t posix_sem_take(PosixSem **sem,const struct timespec *end,int block) {
    pthread_mutex_lock(&lock);
    PosixSem *s=posix_sem(sem);
    int32_t e=0;
    if (!s) e=22;
    else {
        ++s->waiters;
        if (!s->value && block) runtime_wait_begin(end ? "posix semaphore (timed)" : "posix semaphore",s,NULL);
        while (!s->value && block) {
            int r=end ? pthread_cond_timedwait(&s->event,&lock,end) : pthread_cond_wait(&s->event,&lock);
            if (r==ETIMEDOUT) { ++timed_out; break; }
            host_check(r);
        }
        runtime_wait_end();
        --s->waiters;
        if (s->value) { --s->value; ++acquired; }
        else e=block ? 60 : 35; /* ETIMEDOUT / EAGAIN */
    }
    pthread_mutex_unlock(&lock);
    return e ? posix_fail(e) : 0;
}
static ABI int32_t posix_sem_wait(PosixSem **sem) { return posix_sem_take(sem,NULL,1); }
static ABI int32_t posix_sem_trywait(PosixSem **sem) { return posix_sem_take(sem,NULL,0); }
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t posix_sem_timedwait(PosixSem **sem,const GuestTimespec *abstime) {
    /* abstime is CLOCK_REALTIME; convert to the wait clock. */
    if (!abstime || abstime->nsec<0 || abstime->nsec>=1000000000) return posix_fail(22);
    struct timespec real;
    clock_gettime(CLOCK_REALTIME,&real);
    int64_t left=(abstime->sec-real.tv_sec)*1000000000+(abstime->nsec-real.tv_nsec);
    uint64_t deadline=now_ns()+(uint64_t)(left>0 ? left : 0);
    struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    return posix_sem_take(sem,&end,1);
}
static ABI int32_t posix_sem_post(PosixSem **sem) {
    pthread_mutex_lock(&lock);
    PosixSem *s=posix_sem(sem);
    int32_t e=!s ? 22 : s->value==POSIX_SEM_MAX ? 84 : 0; /* EOVERFLOW */
    if (!e) {
        ++s->value; ++signaled;
        if (s->waiters) host_check(pthread_cond_signal(&s->event));
    }
    pthread_mutex_unlock(&lock);
    return e ? posix_fail(e) : 0;
}
static ABI int32_t posix_sem_getvalue(PosixSem **sem,int32_t *value) {
    pthread_mutex_lock(&lock);
    PosixSem *s=posix_sem(sem);
    if (s && value) *value=s->value;
    pthread_mutex_unlock(&lock);
    return s && value ? 0 : posix_fail(22);
}
static const RuntimeExport posix_exports[]={
    {"sem_init",posix_sem_init}, {"sem_destroy",posix_sem_destroy}, {"sem_wait",posix_sem_wait},
    {"sem_trywait",posix_sem_trywait}, {"sem_timedwait",posix_sem_timedwait}, {"sem_post",posix_sem_post},
    {"sem_getvalue",posix_sem_getvalue},
};
uintptr_t runtime_sema_resolve(const char *name) {
    uintptr_t posix=RUNTIME_LOOKUP(posix_exports,name);
    if (posix) return posix;
    if (!strcmp(name,"188x57JYp0g#libkernel")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#libkernel")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#libkernel")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#libkernel")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#libkernel")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#libkernel")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    pthread_mutex_unlock(&lock);
}

