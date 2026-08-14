#include "test.h"

#include <veloco/fiber.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef long (*vl_abi_callback_fn)(void *arg);

extern long vl_test_callee_saved_registers(vl_abi_callback_fn callback,
                                           void *arg);

typedef struct yield_fixture {
    vl_fiber_sched_t *sched;
    long received;
} yield_fixture_t;

static long yield_once(void *arg)
{
    yield_fixture_t *fixture = arg;

    fixture->received = vl_fiber_yield(fixture->sched, 17);
    return fixture->received + 1;
}

VL_TEST(fiber_exchanges_values_and_propagates_return)
{
    vl_fiber_sched_t sched = {0};
    vl_fiber_t *fiber = NULL;
    yield_fixture_t fixture = {.sched = &sched, .received = 0};
    long value = 0;

    VL_REQUIRE(vl_fiber_sched_init(&sched) == VL_OK);
    VL_REQUIRE(vl_fiber_create(&sched, &fiber, 64 * 1024, yield_once,
                               &fixture) == VL_OK);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 0, &value) == VL_OK);
    VL_ASSERT(value == 17);
    VL_ASSERT(vl_fiber_get_state(fiber) == VL_FIBER_SUSPENDED);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 41, &value) == VL_OK);
    VL_ASSERT(value == 42);
    VL_ASSERT(fixture.received == 41);
    VL_ASSERT(vl_fiber_get_state(fiber) == VL_FIBER_DONE);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 0, &value) ==
              VL_ERROR_INVALID_STATE);

    vl_fiber_destroy(fiber);
    vl_fiber_sched_destroy(&sched);
}

typedef struct order_fixture {
    vl_fiber_sched_t *sched;
    vl_fiber_t *fiber_a;
    vl_fiber_t *fiber_b;
    char events[8];
    size_t event_count;
} order_fixture_t;

static void record_event(order_fixture_t *fixture, char event)
{
    fixture->events[fixture->event_count++] = event;
}

static long fiber_b(void *arg)
{
    order_fixture_t *fixture = arg;

    VL_ASSERT(vl_fiber_get_state(fixture->fiber_a) == VL_FIBER_SUSPENDED);
    record_event(fixture, 'B');
    (void)vl_fiber_yield(fixture->sched, 0);
    return 2;
}

static long fiber_a(void *arg)
{
    order_fixture_t *fixture = arg;
    long value;

    record_event(fixture, 'A');
    VL_ASSERT(vl_fiber_resume(fixture->sched, fixture->fiber_b, 0, &value) ==
              VL_OK);
    VL_ASSERT(value == 0);
    record_event(fixture, 'A');
    (void)vl_fiber_yield(fixture->sched, 0);
    VL_ASSERT(vl_fiber_resume(fixture->sched, fixture->fiber_b, 0, &value) ==
              VL_OK);
    VL_ASSERT(value == 2);
    return 1;
}

VL_TEST(nested_fibers_return_to_their_immediate_caller)
{
    vl_fiber_sched_t sched = {0};
    vl_fiber_t *fiber_a_handle = NULL;
    vl_fiber_t *fiber_b_handle = NULL;
    order_fixture_t fixture = {.sched = &sched};
    long value;

    VL_REQUIRE(vl_fiber_sched_init(&sched) == VL_OK);
    VL_REQUIRE(vl_fiber_create(&sched, &fiber_b_handle, 64 * 1024, fiber_b,
                               &fixture) == VL_OK);
    fixture.fiber_b = fiber_b_handle;
    VL_REQUIRE(vl_fiber_create(&sched, &fiber_a_handle, 64 * 1024, fiber_a,
                               &fixture) == VL_OK);
    fixture.fiber_a = fiber_a_handle;

    record_event(&fixture, 'M');
    VL_ASSERT(vl_fiber_resume(&sched, fiber_a_handle, 0, &value) == VL_OK);
    VL_ASSERT(value == 0);
    record_event(&fixture, 'M');
    VL_ASSERT(fixture.event_count == 5);
    VL_ASSERT(fixture.events[0] == 'M');
    VL_ASSERT(fixture.events[1] == 'A');
    VL_ASSERT(fixture.events[2] == 'B');
    VL_ASSERT(fixture.events[3] == 'A');
    VL_ASSERT(fixture.events[4] == 'M');

    VL_ASSERT(vl_fiber_resume(&sched, fiber_a_handle, 0, &value) == VL_OK);
    VL_ASSERT(value == 1);
    VL_ASSERT(vl_fiber_get_state(fiber_a_handle) == VL_FIBER_DONE);
    VL_ASSERT(vl_fiber_get_state(fiber_b_handle) == VL_FIBER_DONE);

    vl_fiber_destroy(fiber_a_handle);
    vl_fiber_destroy(fiber_b_handle);
    vl_fiber_sched_destroy(&sched);
}

static long consume_multiple_stack_pages(void *arg)
{
    volatile unsigned char pages[32 * 1024];
    size_t index;
    unsigned long sum = 0;

    (void)arg;
    for (index = 0; index < sizeof(pages); index += 4096) {
        pages[index] = (unsigned char)(index / 4096 + 1);
        sum += pages[index];
    }
    return (long)sum;
}

VL_TEST(fiber_stack_grows_beyond_the_initial_page)
{
    vl_fiber_sched_t sched = {0};
    vl_fiber_t *fiber = NULL;
    long value = 0;

    VL_REQUIRE(vl_fiber_sched_init(&sched) == VL_OK);
    VL_REQUIRE(vl_fiber_create(&sched, &fiber, 256 * 1024,
                               consume_multiple_stack_pages, NULL) == VL_OK);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 0, &value) == VL_OK);
    VL_ASSERT(value == 36);
    VL_ASSERT(vl_fiber_get_state(fiber) == VL_FIBER_DONE);

    vl_fiber_destroy(fiber);
    vl_fiber_sched_destroy(&sched);
}

static long yield_during_abi_check(void *arg)
{
    vl_fiber_sched_t *sched = arg;

    (void)vl_fiber_yield(sched, 23);
    return 0;
}

static long check_callee_saved_registers(void *arg)
{
    return vl_test_callee_saved_registers(yield_during_abi_check, arg);
}

VL_TEST(fiber_preserves_abi_callee_saved_registers)
{
    vl_fiber_sched_t sched = {0};
    vl_fiber_t *fiber = NULL;
    long value = 0;

    VL_REQUIRE(vl_fiber_sched_init(&sched) == VL_OK);
    VL_REQUIRE(vl_fiber_create(&sched, &fiber, 64 * 1024,
                               check_callee_saved_registers, &sched) == VL_OK);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 0, &value) == VL_OK);
    VL_ASSERT(value == 23);
    VL_ASSERT(vl_fiber_resume(&sched, fiber, 0, &value) == VL_OK);
    VL_ASSERT(value == 1);

    vl_fiber_destroy(fiber);
    vl_fiber_sched_destroy(&sched);
}

typedef struct thread_affinity_fixture {
    vl_fiber_sched_t *sched;
    vl_fiber_t *fiber;
    int status;
} thread_affinity_fixture_t;

static long return_immediately(void *arg)
{
    (void)arg;
    return 9;
}

static void *resume_from_wrong_thread(void *arg)
{
    thread_affinity_fixture_t *fixture = arg;
    long value = 0;

    fixture->status =
        vl_fiber_resume(fixture->sched, fixture->fiber, 0, &value);
    return NULL;
}

VL_TEST(fiber_scheduler_rejects_cross_thread_resume)
{
    vl_fiber_sched_t sched = {0};
    vl_fiber_t *fiber = NULL;
    thread_affinity_fixture_t fixture = {.sched = &sched};
    pthread_t thread;

    VL_REQUIRE(vl_fiber_sched_init(&sched) == VL_OK);
    VL_REQUIRE(vl_fiber_create(&sched, &fiber, 64 * 1024,
                               return_immediately, NULL) == VL_OK);
    fixture.fiber = fiber;
    VL_REQUIRE(pthread_create(&thread, NULL, resume_from_wrong_thread,
                              &fixture) == 0);
    VL_REQUIRE(pthread_join(thread, NULL) == 0);
    VL_ASSERT(fixture.status == VL_ERROR_INVALID_STATE);

    errno = 0;
    VL_ASSERT(vl_fiber_yield(&sched, 1) == 0);
    VL_ASSERT(errno == EPERM);

    vl_fiber_destroy(fiber);
    vl_fiber_sched_destroy(&sched);
}

static long write_to_unrelated_guard_page(void *arg)
{
    volatile int *guard = arg;

    *guard = 1;
    return 0;
}

VL_TEST(unrelated_fault_is_not_consumed_by_stack_growth_handler)
{
    pid_t child = fork();
    int status = 0;

    VL_ASSERT(child >= 0);
    if (child == 0) {
        vl_fiber_sched_t sched = {0};
        vl_fiber_t *fiber = NULL;
        void *guard;
        long value = 0;

        guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (guard == MAP_FAILED) {
            _exit(2);
        }
        if (vl_fiber_sched_init(&sched) != VL_OK) {
            _exit(3);
        }
        if (vl_fiber_create(&sched, &fiber, 64 * 1024,
                            write_to_unrelated_guard_page, guard) != VL_OK) {
            _exit(4);
        }
        (void)vl_fiber_resume(&sched, fiber, 0, &value);
        _exit(0);
    }

    if (child > 0) {
        VL_ASSERT(waitpid(child, &status, 0) == child);
        VL_ASSERT((WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) ||
                  (WIFEXITED(status) && WEXITSTATUS(status) != 0));
    }
}

void vl_register_fiber_tests(void)
{
    vl_test_add("fiber_exchanges_values_and_propagates_return",
                fiber_exchanges_values_and_propagates_return);
    vl_test_add("nested_fibers_return_to_their_immediate_caller",
                nested_fibers_return_to_their_immediate_caller);
    vl_test_add("fiber_stack_grows_beyond_the_initial_page",
                fiber_stack_grows_beyond_the_initial_page);
    vl_test_add("fiber_preserves_abi_callee_saved_registers",
                fiber_preserves_abi_callee_saved_registers);
    vl_test_add("fiber_scheduler_rejects_cross_thread_resume",
                fiber_scheduler_rejects_cross_thread_resume);
    vl_test_add("unrelated_fault_is_not_consumed_by_stack_growth_handler",
                unrelated_fault_is_not_consumed_by_stack_growth_handler);
}
