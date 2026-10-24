/* libSceFiber: user-mode fibers on guest-provided context memory. A fiber runs on the
 * host thread that called sceFiberRun; switching saves the System V callee-saved registers,
 * MXCSR and the x87 control word on the old stack and resumes the other one (fiber_switch).
 * The guest's 256-byte SceFiber object holds this runtime's Fiber record.
 * Windows: the TEB stack range follows the running fiber, as Windows fibers do, so stack
 * probes and exception dispatch accept the guest context memory. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define FIBER_ERR(n) ((int32_t)(UINT32_C(0x80590000)|(n)))
#define ERR_NULL FIBER_ERR(1)
#define ERR_ALIGNMENT FIBER_ERR(2)
#define ERR_RANGE FIBER_ERR(3)
#define ERR_INVALID FIBER_ERR(4)
#define ERR_PERMISSION FIBER_ERR(5)
#define ERR_STATE FIBER_ERR(6)
#define MAGIC_START UINT32_C(0xDEF1649C)
#define MAGIC_END UINT32_C(0xB37592A0)
#define CONTEXT_MINIMUM 512
enum { STATE_IDLE, STATE_RUNNING };

typedef void (ABI *FiberEntry)(uint64_t on_initialize, uint64_t on_run);
typedef struct {
    uint32_t magic_start, state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    unsigned char *context;
    uint64_t context_size;
    char name[32];
    void *sp;      /* saved stack pointer while not running; NULL before the first run */
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)<=256,"SceFiber is 256 bytes");

/* Per host thread: the fiber running on it and the thread's own stack while a fiber runs. */
static _Thread_local Fiber *current;
static _Thread_local void *thread_sp;
static _Thread_local uint64_t transfer; /* argument handed across the last switch */
#ifdef _WIN32
static _Thread_local void *thread_stack_base, *thread_stack_limit;
#endif
static atomic_size_t initialized, runs, switches;

/* fiber_switch(save, load): suspend here (*save) and resume the stack at load. */
ABI void fiber_switch(void **save, void *load);
ABI void fiber_trampoline(void);
__asm__(".text\n"
#ifdef _WIN32
        ".def fiber_switch; .scl 2; .type 32; .endef\n"
#endif
        ".globl fiber_switch\nfiber_switch:\n"
        " push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " sub $8,%rsp\n stmxcsr (%rsp)\n fnstcw 4(%rsp)\n"
        " mov %rsp,(%rdi)\n mov %rsi,%rsp\n"
        " ldmxcsr (%rsp)\n fldcw 4(%rsp)\n add $8,%rsp\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n"
#ifdef _WIN32
        ".def fiber_trampoline; .scl 2; .type 32; .endef\n"
#endif
        ".globl fiber_trampoline\nfiber_trampoline:\n"
        " sub $8,%rsp\n call fiber_main\n ud2\n");

static void set_stack_range(const Fiber *f) {
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    if (!thread_stack_base) { thread_stack_base=tib->StackBase; thread_stack_limit=tib->StackLimit; }
    tib->StackBase=f ? f->context+f->context_size : thread_stack_base;
    tib->StackLimit=f ? f->context : thread_stack_limit;
#else
    (void)f;
#endif
}
ABI __attribute__((noreturn, used)) void fiber_main(void) {
    Fiber *f=current;
    f->entry(f->arg_on_initialize,transfer);
    fprintf(stderr,"STOP: fiber '%s' returned from its entry function\n",f->name);
    runtime_report();
    exit(23);
}
static Fiber *valid(Fiber *f) {
    return f && f->magic_start==MAGIC_START && f->magic_end==MAGIC_END ? f : NULL;
}
/* First run: a frame that fiber_switch "returns" into fiber_trampoline with default FP state. */
static void prepare(Fiber *f) {
    uint64_t *top=(uint64_t *)(((uintptr_t)(f->context+f->context_size))&~(uintptr_t)15);
    uint64_t *sp=top-9; /* the trampoline starts with rsp=top-8, as after a call */
    memset(sp,0,9*sizeof(*sp));
    sp[0]=UINT64_C(0x1f80)|(UINT64_C(0x037f)<<32); /* MXCSR, x87 control word */
    sp[7]=(uint64_t)(uintptr_t)fiber_trampoline;  /* after six registers */
    f->sp=sp;
}
static ABI int32_t fiber_initialize(Fiber *f,const char *name,FiberEntry entry,uint64_t arg,
                                    void *context,uint64_t size,const void *options,uint32_t build) {
    (void)build;
    if (!f || !name || !entry) return ERR_NULL;
    if ((uintptr_t)f&7 || (uintptr_t)context&15) return ERR_ALIGNMENT;
    if (!context || size<CONTEXT_MINIMUM || size&15) {
        if (!context && !size) { fputs("STOP: fibers on the caller's stack (no context) are not implemented\n",stderr); exit(21); }
        return ERR_RANGE;
    }
    if (options) { fputs("STOP: fiber options are not implemented\n",stderr); exit(21); }
    memset(f,0,sizeof(*f));
    f->magic_start=MAGIC_START; f->magic_end=MAGIC_END; f->state=STATE_IDLE;
    f->entry=entry; f->arg_on_initialize=arg; f->context=context; f->context_size=size;
    snprintf(f->name,sizeof(f->name),"%s",name);
    atomic_fetch_add(&initialized,1);
    return 0;
}
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    if (f->state!=STATE_IDLE) return ERR_STATE;
    f->magic_start=0; f->magic_end=0;
    return 0;
}
/* Makes f the running fiber of this thread and switches to it from *save. */
static void enter(Fiber *f,uint64_t argument,void **save) {
    if (!f->sp) prepare(f);
    f->state=STATE_RUNNING; current=f; transfer=argument;
    set_stack_range(f);
    void *load=f->sp; f->sp=NULL;
    fiber_switch(save,load);
}
static ABI int32_t fiber_run(Fiber *f,uint64_t argument,uint64_t *on_return) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    if (current) return ERR_PERMISSION; /* only a thread runs a fiber */
    if (f->state!=STATE_IDLE) return ERR_STATE;
    atomic_fetch_add(&runs,1);
    enter(f,argument,&thread_sp);
    /* Back on the thread: the last fiber returned to it. */
    if (on_return) *on_return=transfer;
    return 0;
}
static ABI int32_t fiber_switch_to(Fiber *f,uint64_t argument,uint64_t *on_run) {
    if (!f) return ERR_NULL;
    if ((uintptr_t)f&7) return ERR_ALIGNMENT;
    if (!valid(f)) return ERR_INVALID;
    Fiber *self=current;
    if (!self) return ERR_PERMISSION;
    if (f->state!=STATE_IDLE) return ERR_STATE;
    atomic_fetch_add(&switches,1);
    self->state=STATE_IDLE;
    enter(f,argument,&self->sp);
    if (on_run) *on_run=transfer; /* resumed by a run or a switch */
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t argument,uint64_t *on_run) {
    Fiber *self=current;
    if (!self) return ERR_PERMISSION;
    self->state=STATE_IDLE; current=NULL; transfer=argument;
    set_stack_range(NULL);
    fiber_switch(&self->sp,thread_sp);
    if (on_run) *on_run=transfer;
    return 0;
}
static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return ERR_NULL;
    if (!current) return ERR_PERMISSION;
    *out=current;
    return 0;
}
static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch_to},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberGetSelf",fiber_get_self},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_fiber_report(void) {
    printf("Runtime: fibers initialized=%zu, runs=%zu, switches=%zu\n",
           (size_t)initialized,(size_t)runs,(size_t)switches);
}
