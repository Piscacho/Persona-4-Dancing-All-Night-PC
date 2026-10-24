/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler.
 * Windows: GS is the TEB and cannot be moved. The guest TCB lives in a TLS slot of the TEB
 * instead, and the loader rewrites the load to `mov rax, gs:[0x1480+8*slot]` (TlsSlots),
 * the same 9-byte instruction with another displacement. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    /* Diagnostics (P4D_HANG_DUMP): the primitive this thread is blocked on, if any. */
    const char *wait_kind;
    const void *wait_object;
    const char *wait_detail;
    uint64_t wait_since_ns;
    unsigned long host_tid;
#ifdef _WIN32
    p4d_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
uint32_t runtime_thread_tcb_offset(void) {
    if (tls_slot==TLS_OUT_OF_INDEXES) {
        tls_slot=TlsAlloc();
        if (tls_slot>=64) { fprintf(stderr,"STOP: no TEB TLS slot below 64 (got %lu)\n",tls_slot); exit(21); }
    }
    return 0x1480+8*(uint32_t)tls_slot;
}
static void set_gs(void *base) {
    runtime_thread_tcb_offset();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: TlsSetValue failed (%lu)\n",GetLastError()); exit(21); }
}
#define tls_block_alloc(size) _aligned_malloc(size,64)
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
#define tls_block_alloc(size) aligned_alloc(64,size)
#endif
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=tls_block_alloc((total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
#ifdef _WIN32
    t->host_tid=GetCurrentThreadId();
#else
    t->host_tid=(unsigned long)syscall(SYS_gettid);
#endif
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
}
#ifdef _WIN32
#define exit_setjmp(t) p4d_setjmp((t)->exit_jump)
#define exit_longjmp(t) p4d_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    set_host_name(t->name);
    if (!exit_setjmp(t)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock); t->finished=1; ++exited; pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    runtime_wait_begin("join",t,t->name);
    int e=pthread_join(t->host,&value);
    runtime_wait_end();
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#libkernel",thread_self}, {"EotR8a3ASf4#libkernel",thread_self},
        {"9BcDykPmo1I#libkernel",guest_error},
        {"nsYoNRywwNg#libkernel",attr_init}, {"62KCwEMmzcM#libkernel",attr_destroy},
        {"x1X76arYMxU#libkernel",attr_get}, {"8+s5BzZjxSg#libkernel",attr_affinity},
        {"UTXzJbWhhTE#libkernel",attr_set_stack}, {"-Wreprtu0Qs#libkernel",attr_set_detach},
        {"4+h9EzwKF4I#libkernel",attr_set_policy}, {"DzES9hQF4f4#libkernel",attr_set_param},
        {"3qxgM4ezETA#libkernel",attr_set_affinity}, {"eXbUSpEaTsA#libkernel",attr_set_inherit},
        {"JaRMy+QcpeU#libkernel",attr_get_detach}, {"-fA+7ZlGDQs#libkernel",attr_get_stack},
        {"FXPWHNk8Of0#libkernel",attr_get_param}, {"El+cQ20DynU#libkernel",attr_set_guard},
        {"6UgtwV+0zb4#libkernel",thread_create}, {"onNY9Byn-W8#libkernel",thread_join},
        {"4qGrR6eoP9Y#libkernel",thread_detach}, {"3kg7rT0NQIs#libkernel",thread_exit},
        {"T72hz6ffq08#libkernel",thread_yield}, {"1tKyG7RlMJo#libkernel",thread_get_prio},
        {"W0Hpm2X0uPE#libkernel",thread_set_prio}, {"bt3CTBKmGyI#libkernel",thread_set_affinity}, {"rcrVFJsQWRY#libkernel",thread_get_affinity},
        {"GBUY7ywdULE#libkernel",thread_rename}, {"3PtV6p3QNX4#libkernel",thread_equal},
        {"wtkt-teR1so#libkernel",posix_attr_init}, {"zHchY8ft5pk#libkernel",posix_attr_destroy},
        {"E+tyo3lp5Lw#libkernel",posix_attr_set_detach}, {"2Q0z6rnBrTE#libkernel",posix_attr_set_stack},
        {"euKRgm0Vn2M#libkernel",posix_attr_set_param}, {"OxhIB8LB-PQ#libkernel",posix_create},
        {"Jmi+9w9u0E4#libkernel",posix_create_name}, {"h9CcP3J0oVM#libkernel",posix_join},
        {"FJrT5LuUBAU#libkernel",thread_exit},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
}


/* ---- Hang diagnostics (P4D_HANG_DUMP=seconds) ----
 * Blocking primitives mark the calling guest thread with what it waits on; a host thread
 * prints every guest thread's wait, instruction pointer and the return addresses found on
 * its stack (guest offsets, or p4d-probe.exe offsets for llvm-symbolizer). */
static uint64_t monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
void runtime_wait_begin(const char *kind,const void *object,const char *detail) {
    GuestThread *t=current;
    if (!t) return;
    t->wait_object=object; t->wait_detail=detail; t->wait_since_ns=monotonic_ns();
    __atomic_store_n(&t->wait_kind,kind,__ATOMIC_RELEASE);
}
void runtime_wait_end(void) {
    if (current) __atomic_store_n(&current->wait_kind,NULL,__ATOMIC_RELEASE);
}
void *runtime_thread_self(void) { return current; }
const char *runtime_thread_label(const void *handle) {
    const GuestThread *t=handle;
    return t ? t->name : "-";
}
static const unsigned char *dump_image;
static size_t dump_image_size;
#ifdef _WIN32
static uintptr_t exe_start, exe_end;
static int readable(uintptr_t address,size_t size) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)address,&info,sizeof(info)) || info.State!=MEM_COMMIT) return 0;
    if (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) return 0;
    return address+size<=(uintptr_t)info.BaseAddress+info.RegionSize;
}
/* A return address follows a call: e8 rel32, ff 15 rip32, ff /2 with a short modrm. */
static int after_call(uintptr_t v) {
    if (!readable(v-7,7)) return 0;
    const unsigned char *p=(const unsigned char *)v;
    if (p[-5]==0xe8) return 1;
    if (p[-6]==0xff && p[-5]==0x15) return 1;
    if (p[-2]==0xff && (p[-1]&0xf8)==0xd0) return 1;   /* call reg */
    if (p[-3]==0xff && (p[-2]&0x38)==0x10) return 1;   /* call [reg+d8] */
    if (p[-3]==0x41 && p[-2]==0xff && (p[-1]&0xf8)==0xd0) return 1;
    return 0;
}
static void describe(char *out,size_t size,uintptr_t v) {
    if (dump_image && v-(uintptr_t)dump_image<dump_image_size) snprintf(out,size,"guest+0x%llx",(unsigned long long)(v-(uintptr_t)dump_image));
    else if (v>=exe_start && v<exe_end) snprintf(out,size,"exe+0x%llx",(unsigned long long)(v-exe_start));
    else snprintf(out,size,"0x%llx",(unsigned long long)v);
}
static void dump_stack(GuestThread *t) {
    HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,t->host_tid);
    if (!h) { printf("    (cannot open thread %lu)\n",t->host_tid); return; }
    if (SuspendThread(h)==(DWORD)-1) { CloseHandle(h); return; }
    CONTEXT c; memset(&c,0,sizeof(c)); c.ContextFlags=CONTEXT_FULL;
    if (GetThreadContext(h,&c)) {
        char where[64]; describe(where,sizeof(where),c.Rip);
        printf("    rip=%s rsp=%llx rdi=%llx rsi=%llx\n",where,(unsigned long long)c.Rsp,(unsigned long long)c.Rdi,(unsigned long long)c.Rsi);
        printf("    frames:");
        int shown=0;
        for (uintptr_t at=(uintptr_t)c.Rsp&~(uintptr_t)7,end=at+256*1024;at<end && shown<20;at+=8) {
            if ((at&4095)==0 || at==((uintptr_t)c.Rsp&~(uintptr_t)7)) if (!readable(at,8)) break;
            uintptr_t v=*(const uintptr_t *)at;
            int guest=dump_image && v-(uintptr_t)dump_image<dump_image_size;
            if ((!guest && !(v>=exe_start && v<exe_end)) || !after_call(v)) continue;
            describe(where,sizeof(where),v); printf(" %s",where); ++shown;
        }
        putchar('\n');
    }
    ResumeThread(h); CloseHandle(h);
}
void runtime_print_frames(uintptr_t rsp) {
    char where[64];
    int shown=0;
    for (uintptr_t at=rsp&~(uintptr_t)7,end=at+256*1024;at<end && shown<24;at+=8) {
        if (((at&4095)==0 || at==(rsp&~(uintptr_t)7)) && !readable(at,8)) break;
        uintptr_t v=*(const uintptr_t *)at;
        int guest=dump_image && v-(uintptr_t)dump_image<dump_image_size;
        if ((!guest && !(v>=exe_start && v<exe_end)) || !after_call(v)) continue;
        describe(where,sizeof(where),v); printf(" %s",where); ++shown;
    }
    putchar('\n');
}
/* Return addresses on the calling thread's stack (GPU library diagnostics). */
static void print_backtrace(const char *why) {
    uintptr_t rsp=(uintptr_t)__builtin_frame_address(0);
    printf("Backtrace (%s) on '%s':",why,current ? current->name : "host thread");
    runtime_print_frames(rsp);
    fflush(stdout);
}
#else
static void dump_stack(GuestThread *t) { (void)t; }
static void print_backtrace(const char *why) { (void)why; }
void runtime_print_frames(uintptr_t rsp) { (void)rsp; }
#endif
void runtime_thread_dump(void) {
    uint64_t now=monotonic_ns();
    pthread_mutex_lock(&lock);
    printf("==== thread dump: %zu created, %zu exited ====\n",created,exited);
    for (GuestThread *t=threads;t;t=t->next) {
        if (t->finished) continue;
        const char *kind=__atomic_load_n(&t->wait_kind,__ATOMIC_ACQUIRE);
        if (kind) printf("  [%lu] '%s' waits on %s %p%s%s for %.1f s\n",t->host_tid,t->name,kind,t->wait_object,
                         t->wait_detail ? " " : "",t->wait_detail ? t->wait_detail : "",(double)(now>t->wait_since_ns ? now-t->wait_since_ns : 0)/1e9);
        else printf("  [%lu] '%s' running (not in a tracked wait)\n",t->host_tid,t->name);
        if (t->host_tid && t!=current) dump_stack(t);
    }
    printf("==== end of thread dump ====\n");
    pthread_mutex_unlock(&lock);
    runtime_report();
    fflush(stdout);
}
static void *hang_dump_loop(void *p) {
    unsigned seconds=(unsigned)(uintptr_t)p;
    for (;;) {
#ifdef _WIN32
        compat_sleep_ns((uint64_t)seconds*1000000000);
#else
        sleep(seconds);
#endif
        runtime_thread_dump();
    }
    return NULL;
}
/* Defined by the GPU library (shadPS4 equeue waits). */
extern void (*p4d_wait_begin_hook)(const char *kind,const void *object,const char *detail);
extern void (*p4d_wait_end_hook)(void);
extern void (*p4d_backtrace_hook)(const char *why);
void runtime_hang_dump_start(const void *image,size_t size) {
    const char *value=getenv("P4D_HANG_DUMP");
    unsigned seconds=value ? (unsigned)strtoul(value,NULL,10) : 0;
    if (!seconds) return;
    p4d_wait_begin_hook=runtime_wait_begin; p4d_wait_end_hook=runtime_wait_end;
    p4d_backtrace_hook=print_backtrace;
    dump_image=image; dump_image_size=size;
#ifdef _WIN32
    HMODULE exe=GetModuleHandleA(NULL);
    MODULEINFO info;
    exe_start=(uintptr_t)exe;
    exe_end=K32GetModuleInformation(GetCurrentProcess(),exe,&info,sizeof(info)) ? exe_start+info.SizeOfImage : exe_start+0x10000000;
#endif
    pthread_t dumper;
    if (pthread_create(&dumper,NULL,hang_dump_loop,(void *)(uintptr_t)seconds)) return;
    pthread_detach(dumper);
    printf("Diagnostics: thread dump every %u s (P4D_HANG_DUMP)\n",seconds);
}
