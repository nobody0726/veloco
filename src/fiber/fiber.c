#include <veloco/fiber.h>

#include "fiber_context.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VL_WITH_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define VL_WITH_TSAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define VL_WITH_ASAN 1
#endif

#if defined(__SANITIZE_THREAD__)
#define VL_WITH_TSAN 1
#endif

typedef struct vl_fiber_sched_impl vl_fiber_sched_impl_t;

struct vl_fiber {
    vl_fiber_context_t context;
    vl_fiber_sched_impl_t *owner;
    struct vl_fiber *caller;
    vl_fiber_fn fn;
    void *arg;
    void *stack_base;
    unsigned char *stack_low;
    unsigned char *mapped_low;
    unsigned char *stack_top;
    size_t mapping_size;
    vl_fiber_state_t state;
    long inbox;
    long outbound;
#if defined(VL_WITH_ASAN)
    void *asan_fake_stack;
#endif
#if defined(VL_WITH_TSAN)
    void *tsan_fiber;
#endif
};

struct vl_fiber_sched_impl {
    pthread_t owner_thread;
    vl_fiber_context_t root_context;
    vl_fiber_t *current;
    size_t page_size;
    void *signal_stack;
    size_t signal_stack_size;
    stack_t previous_signal_stack;
    _Atomic size_t live_fibers;
#if defined(VL_WITH_ASAN)
    void *root_asan_fake_stack;
#endif
#if defined(VL_WITH_TSAN)
    void *root_tsan_fiber;
#endif
};

extern void vl_context_switch(vl_fiber_context_t *from,
                              const vl_fiber_context_t *to);

#if defined(VL_WITH_ASAN)
void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                    const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old,
                                     size_t *size_old);
#endif

#if defined(VL_WITH_TSAN)
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif

static pthread_once_t vl_signal_once = PTHREAD_ONCE_INIT;
static struct sigaction vl_previous_sigsegv;
static int vl_signal_install_status = VL_ERROR_SYSTEM;
static _Thread_local vl_fiber_sched_impl_t *vl_thread_sched;

static void vl_forward_sigsegv(int signal_number, siginfo_t *info,
                               void *ucontext)
{
    if ((vl_previous_sigsegv.sa_flags & SA_SIGINFO) != 0 &&
        vl_previous_sigsegv.sa_sigaction != NULL) {
        vl_previous_sigsegv.sa_sigaction(signal_number, info, ucontext);
        return;
    }

    if (vl_previous_sigsegv.sa_handler == SIG_IGN) {
        return;
    }
    if (vl_previous_sigsegv.sa_handler != SIG_DFL &&
        vl_previous_sigsegv.sa_handler != NULL) {
        vl_previous_sigsegv.sa_handler(signal_number);
        return;
    }

    (void)sigaction(signal_number, &vl_previous_sigsegv, NULL);
    (void)raise(signal_number);
    _exit(128 + signal_number);
}

static void vl_stack_fault_handler(int signal_number, siginfo_t *info,
                                   void *ucontext)
{
    vl_fiber_sched_impl_t *impl = vl_thread_sched;
    vl_fiber_t *fiber = impl != NULL ? impl->current : NULL;
    uintptr_t fault = (uintptr_t)(info != NULL ? info->si_addr : NULL);

    if (info != NULL && info->si_code > 0 && fiber != NULL &&
        fiber->state == VL_FIBER_RUNNING &&
        fault >= (uintptr_t)fiber->stack_low &&
        fault < (uintptr_t)fiber->mapped_low) {
        uintptr_t page_mask = impl->page_size - 1;
        unsigned char *new_low =
            (unsigned char *)(fault & ~page_mask);
        size_t growth_size = (size_t)(fiber->mapped_low - new_low);

        if (mprotect(new_low, growth_size, PROT_READ | PROT_WRITE) == 0) {
            fiber->mapped_low = new_low;
            return;
        }
    }

    vl_forward_sigsegv(signal_number, info, ucontext);
}

static void vl_install_stack_fault_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = vl_stack_fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGSEGV, NULL, &vl_previous_sigsegv) == 0 &&
        sigaction(SIGSEGV, &action, NULL) == 0) {
        vl_signal_install_status = VL_OK;
    }
}

static int vl_sched_is_owner(const vl_fiber_sched_impl_t *impl)
{
    return impl != NULL && pthread_equal(impl->owner_thread, pthread_self());
}

static size_t vl_round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static vl_fiber_context_t *vl_caller_context(vl_fiber_sched_impl_t *impl,
                                             vl_fiber_t *caller)
{
    return caller != NULL ? &caller->context : &impl->root_context;
}

static void vl_transfer(vl_fiber_sched_impl_t *impl,
                        vl_fiber_context_t *from, vl_fiber_context_t *to,
                        vl_fiber_t *from_fiber, vl_fiber_t *to_fiber,
                        int from_will_return)
{
    (void)impl;
#if defined(VL_WITH_ASAN)
    void **fake_stack_save = NULL;
    const void *target_bottom = NULL;
    size_t target_size = 0;

    if (from_will_return) {
        fake_stack_save = from_fiber != NULL
                              ? &from_fiber->asan_fake_stack
                              : &impl->root_asan_fake_stack;
    }
    if (to_fiber != NULL) {
        target_bottom = to_fiber->stack_low;
        target_size = (size_t)(to_fiber->stack_top - to_fiber->stack_low);
    }
    __sanitizer_start_switch_fiber(fake_stack_save, target_bottom,
                                   target_size);
#else
    (void)from_fiber;
    (void)from_will_return;
#endif

#if defined(VL_WITH_TSAN)
    __tsan_switch_to_fiber(to_fiber != NULL ? to_fiber->tsan_fiber
                                             : impl->root_tsan_fiber,
                           0);
#else
    (void)to_fiber;
#endif

    vl_context_switch(from, to);

#if defined(VL_WITH_ASAN)
    {
        void *fake_stack = from_fiber != NULL
                               ? from_fiber->asan_fake_stack
                               : impl->root_asan_fake_stack;
        __sanitizer_finish_switch_fiber(fake_stack, NULL, NULL);
    }
#endif
}

static _Noreturn void vl_fiber_unreachable(void)
{
    abort();
}

static _Noreturn void vl_fiber_trampoline(void)
{
    vl_fiber_sched_impl_t *impl = vl_thread_sched;
    vl_fiber_t *fiber = impl != NULL ? impl->current : NULL;
    vl_fiber_t *caller;

#if defined(VL_WITH_ASAN)
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);
#endif

    if (fiber == NULL || fiber->state != VL_FIBER_RUNNING) {
        vl_fiber_unreachable();
    }

    fiber->outbound = fiber->fn(fiber->arg);
    fiber->state = VL_FIBER_DONE;
    caller = fiber->caller;
    impl->current = caller;
    if (caller != NULL) {
        caller->state = VL_FIBER_RUNNING;
    }
    vl_transfer(impl, &fiber->context, vl_caller_context(impl, caller), fiber,
                caller, 0);
    vl_fiber_unreachable();
}

int vl_fiber_sched_init(vl_fiber_sched_t *sched)
{
    vl_fiber_sched_impl_t *impl;
    stack_t signal_stack;
    long page_size;

    if (sched == NULL || sched->impl != NULL || vl_thread_sched != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((size_t)page_size & ((size_t)page_size - 1)) != 0) {
        return VL_ERROR_SYSTEM;
    }

    if (pthread_once(&vl_signal_once, vl_install_stack_fault_handler) != 0 ||
        vl_signal_install_status != VL_OK) {
        return VL_ERROR_SYSTEM;
    }

    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    impl->signal_stack_size = vl_round_up(64 * 1024, (size_t)page_size);
    impl->signal_stack =
        mmap(NULL, impl->signal_stack_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (impl->signal_stack == MAP_FAILED) {
        free(impl);
        return VL_ERROR_SYSTEM;
    }

    memset(&signal_stack, 0, sizeof(signal_stack));
    signal_stack.ss_sp = impl->signal_stack;
    signal_stack.ss_size = impl->signal_stack_size;
    if (sigaltstack(NULL, &impl->previous_signal_stack) != 0 ||
        sigaltstack(&signal_stack, NULL) != 0) {
        munmap(impl->signal_stack, impl->signal_stack_size);
        free(impl);
        return VL_ERROR_SYSTEM;
    }

    impl->owner_thread = pthread_self();
    impl->page_size = (size_t)page_size;
#if defined(VL_WITH_TSAN)
    impl->root_tsan_fiber = __tsan_get_current_fiber();
#endif
    sched->impl = impl;
    vl_thread_sched = impl;
    return VL_OK;
}

void vl_fiber_sched_destroy(vl_fiber_sched_t *sched)
{
    vl_fiber_sched_impl_t *impl;

    if (sched == NULL) {
        return;
    }
    impl = sched->impl;
    if (!vl_sched_is_owner(impl) || impl->current != NULL ||
        atomic_load_explicit(&impl->live_fibers, memory_order_acquire) != 0) {
        return;
    }

    (void)sigaltstack(&impl->previous_signal_stack, NULL);
    (void)munmap(impl->signal_stack, impl->signal_stack_size);
    vl_thread_sched = NULL;
    sched->impl = NULL;
    free(impl);
}

int vl_fiber_create(vl_fiber_sched_t *sched, vl_fiber_t **out,
                    size_t stack_size, vl_fiber_fn fn, void *arg)
{
    vl_fiber_sched_impl_t *impl;
    vl_fiber_t *fiber;
    size_t usable_size;
    uintptr_t initial_sp;

    if (sched == NULL || out == NULL || *out != NULL || fn == NULL ||
        stack_size == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = sched->impl;
    if (!vl_sched_is_owner(impl)) {
        return VL_ERROR_INVALID_STATE;
    }

    if (stack_size > SIZE_MAX - (impl->page_size - 1)) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    usable_size = vl_round_up(stack_size, impl->page_size);
    if (usable_size < impl->page_size ||
        usable_size > SIZE_MAX - impl->page_size) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    fiber = calloc(1, sizeof(*fiber));
    if (fiber == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    fiber->mapping_size = usable_size + impl->page_size;
    fiber->stack_base = mmap(NULL, fiber->mapping_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (fiber->stack_base == MAP_FAILED) {
        free(fiber);
        return VL_ERROR_SYSTEM;
    }

    fiber->stack_low = (unsigned char *)fiber->stack_base + impl->page_size;
    fiber->stack_top = (unsigned char *)fiber->stack_base + fiber->mapping_size;
    fiber->mapped_low = fiber->stack_top - impl->page_size;
    if (mprotect(fiber->mapped_low, impl->page_size,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(fiber->stack_base, fiber->mapping_size);
        free(fiber);
        return VL_ERROR_SYSTEM;
    }

    fiber->owner = impl;
    fiber->fn = fn;
    fiber->arg = arg;
    fiber->state = VL_FIBER_READY;
    initial_sp = (uintptr_t)fiber->stack_top & ~(uintptr_t)0xf;

#if defined(__x86_64__)
    initial_sp -= sizeof(uintptr_t);
    *(uintptr_t *)initial_sp = (uintptr_t)vl_fiber_unreachable;
    fiber->context.rsp = initial_sp;
    fiber->context.rip = (uintptr_t)vl_fiber_trampoline;
#elif defined(__aarch64__)
    fiber->context.sp = initial_sp;
    fiber->context.x30 = (uintptr_t)vl_fiber_trampoline;
#endif

#if defined(VL_WITH_TSAN)
    fiber->tsan_fiber = __tsan_create_fiber(0);
    if (fiber->tsan_fiber == NULL) {
        munmap(fiber->stack_base, fiber->mapping_size);
        free(fiber);
        return VL_ERROR_OUT_OF_MEMORY;
    }
#endif

    atomic_fetch_add_explicit(&impl->live_fibers, 1, memory_order_relaxed);
    *out = fiber;
    return VL_OK;
}

int vl_fiber_resume(vl_fiber_sched_t *sched, vl_fiber_t *fiber,
                    long send_value, long *return_value)
{
    vl_fiber_sched_impl_t *impl;
    vl_fiber_t *caller;

    if (sched == NULL || fiber == NULL || return_value == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = sched->impl;
    if (!vl_sched_is_owner(impl) || fiber->owner != impl ||
        fiber->state == VL_FIBER_RUNNING || fiber->state == VL_FIBER_DONE) {
        return VL_ERROR_INVALID_STATE;
    }

    caller = impl->current;
    if (caller != NULL) {
        caller->state = VL_FIBER_SUSPENDED;
    }
    fiber->caller = caller;
    fiber->inbox = send_value;
    fiber->state = VL_FIBER_RUNNING;
    impl->current = fiber;
    vl_transfer(impl, vl_caller_context(impl, caller), &fiber->context, caller,
                fiber, 1);

    impl->current = caller;
    if (caller != NULL) {
        caller->state = VL_FIBER_RUNNING;
    }
    fiber->caller = NULL;
    *return_value = fiber->outbound;
    return VL_OK;
}

long vl_fiber_yield(vl_fiber_sched_t *sched, long send_value)
{
    vl_fiber_sched_impl_t *impl;
    vl_fiber_t *fiber;
    vl_fiber_t *caller;

    if (sched == NULL || !vl_sched_is_owner(sched->impl)) {
        errno = EINVAL;
        return 0;
    }
    impl = sched->impl;
    fiber = impl->current;
    if (fiber == NULL || fiber->state != VL_FIBER_RUNNING) {
        errno = EPERM;
        return 0;
    }

    caller = fiber->caller;
    fiber->outbound = send_value;
    fiber->state = VL_FIBER_SUSPENDED;
    impl->current = caller;
    if (caller != NULL) {
        caller->state = VL_FIBER_RUNNING;
    }
    vl_transfer(impl, &fiber->context, vl_caller_context(impl, caller), fiber,
                caller, 1);
    return fiber->inbox;
}

vl_fiber_state_t vl_fiber_get_state(const vl_fiber_t *fiber)
{
    return fiber != NULL ? fiber->state : VL_FIBER_DONE;
}

void vl_fiber_destroy(vl_fiber_t *fiber)
{
    vl_fiber_sched_impl_t *impl;

    if (fiber == NULL || fiber->state == VL_FIBER_RUNNING) {
        return;
    }
    impl = fiber->owner;
    if (!vl_sched_is_owner(impl)) {
        return;
    }

#if defined(VL_WITH_TSAN)
    __tsan_destroy_fiber(fiber->tsan_fiber);
#endif
    (void)munmap(fiber->stack_base, fiber->mapping_size);
    atomic_fetch_sub_explicit(&impl->live_fibers, 1, memory_order_acq_rel);
    free(fiber);
}
