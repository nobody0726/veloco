#define _GNU_SOURCE

#include "io_internal.h"

#include <errno.h>
#include <liburing.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VL_URING_DEFAULT_DEPTH 256U
#define VL_URING_MINIMUM_DEPTH 8U
#define VL_URING_WAKE_DATA UINT64_MAX
#define VL_URING_CANCEL_BIT ((uintptr_t)1U)

typedef struct vl_uring_operation vl_uring_operation_t;
typedef struct vl_uring_command vl_uring_command_t;

typedef enum vl_uring_command_type {
    VL_URING_COMMAND_SUBMIT,
    VL_URING_COMMAND_CANCEL
} vl_uring_command_type_t;

struct vl_uring_operation {
    vl_io_request_t *request;
    struct __kernel_timespec timeout;
    vl_io_completion_t completion;
    vl_uring_operation_t *active_next;
    vl_uring_operation_t *completed_next;
    int resolved;
    int completion_consumed;
    int cancel_command_pending;
};

struct vl_uring_command {
    vl_uring_command_type_t type;
    vl_uring_operation_t *operation;
    vl_uring_command_t *next;
};

typedef struct vl_uring_state {
    struct io_uring ring;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_cond_t startup_condition;
    vl_uring_command_t *command_head;
    vl_uring_command_t *command_tail;
    vl_uring_operation_t *active;
    vl_uring_operation_t *completed_head;
    vl_uring_operation_t *completed_tail;
    int event_fd;
    int startup_done;
    int startup_status;
    int worker_status;
    int stop_requested;
    unsigned queue_depth;
} vl_uring_state_t;

static int vl_uring_status_from_error(int error)
{
    if (error == EPERM || error == ENOSYS || error == EOPNOTSUPP) {
        return VL_ERROR_UNSUPPORTED;
    }
    if (error == ENOMEM) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    return VL_ERROR_SYSTEM;
}

static int vl_uring_write_wakeup(vl_uring_state_t *state)
{
    uint64_t value = 1;
    ssize_t written;

    do {
        written = write(state->event_fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)sizeof(value) ? VL_OK : VL_ERROR_SYSTEM;
}

static void vl_uring_drain_wakeup(vl_uring_state_t *state)
{
    uint64_t value;
    ssize_t result;

    do {
        result = read(state->event_fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

static void vl_uring_append_command_locked(vl_uring_state_t *state,
                                           vl_uring_command_t *command)
{
    if (state->command_tail == NULL) {
        state->command_head = command;
    } else {
        state->command_tail->next = command;
    }
    state->command_tail = command;
}

static void vl_uring_remove_active_locked(vl_uring_state_t *state,
                                          vl_uring_operation_t *operation)
{
    vl_uring_operation_t **cursor = &state->active;

    while (*cursor != NULL && *cursor != operation) {
        cursor = &(*cursor)->active_next;
    }
    if (*cursor == operation) {
        *cursor = operation->active_next;
        operation->active_next = NULL;
    }
}

static vl_uring_operation_t *vl_uring_find_active_locked(
    vl_uring_state_t *state, const vl_io_request_t *request)
{
    vl_uring_operation_t *operation;

    for (operation = state->active; operation != NULL;
         operation = operation->active_next) {
        if (operation->request == request) {
            return operation;
        }
    }
    return NULL;
}

static struct io_uring_sqe *vl_uring_get_sqe(vl_uring_state_t *state)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    int result;

    if (sqe != NULL) {
        return sqe;
    }
    result = io_uring_submit(&state->ring);
    if (result < 0) {
        return NULL;
    }
    return io_uring_get_sqe(&state->ring);
}

static int vl_uring_prepare_wakeup(vl_uring_state_t *state)
{
    struct io_uring_sqe *sqe = vl_uring_get_sqe(state);

    if (sqe == NULL) {
        return VL_ERROR_SYSTEM;
    }
    io_uring_prep_poll_add(sqe, state->event_fd, POLLIN);
    io_uring_sqe_set_data64(sqe, VL_URING_WAKE_DATA);
    return VL_OK;
}

static int vl_uring_prepare_operation(vl_uring_state_t *state,
                                      vl_uring_operation_t *operation)
{
    vl_io_request_t *request = operation->request;
    struct io_uring_sqe *sqe = vl_uring_get_sqe(state);

    if (sqe == NULL) {
        return VL_ERROR_SYSTEM;
    }
    switch (request->op) {
    case VL_IO_ACCEPT:
        io_uring_prep_accept(sqe, request->fd, NULL, NULL,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        break;
    case VL_IO_RECV:
        io_uring_prep_recv(sqe, request->fd, request->buf, request->len, 0);
        break;
    case VL_IO_SEND:
        io_uring_prep_send(sqe, request->fd, request->buf, request->len,
                           MSG_NOSIGNAL);
        break;
    case VL_IO_CONNECT:
        io_uring_prep_connect(sqe, request->fd, request->address,
                              request->address_len);
        break;
    case VL_IO_TIMEOUT:
        operation->timeout.tv_sec = request->timeout_ns / 1000000000ULL;
        operation->timeout.tv_nsec = request->timeout_ns % 1000000000ULL;
        io_uring_prep_timeout(sqe, &operation->timeout, 0, 0);
        break;
    default:
        return VL_ERROR_UNSUPPORTED;
    }
    io_uring_sqe_set_data(sqe, operation);
    return VL_OK;
}

static int vl_uring_prepare_cancel(vl_uring_state_t *state,
                                   vl_uring_operation_t *operation)
{
    struct io_uring_sqe *sqe = vl_uring_get_sqe(state);
    uintptr_t cancel_data;

    if (sqe == NULL) {
        return VL_ERROR_SYSTEM;
    }
    io_uring_prep_cancel(sqe, operation, 0);
    cancel_data = (uintptr_t)operation | VL_URING_CANCEL_BIT;
    io_uring_sqe_set_data64(sqe, (uint64_t)cancel_data);
    return VL_OK;
}

static void vl_uring_complete_operation(vl_uring_state_t *state,
                                        vl_uring_operation_t *operation,
                                        int result)
{
    vl_io_request_t *request = operation->request;

    if (request->op == VL_IO_ACCEPT && result >= 0) {
        if (vl_socket_track(result) != VL_OK) {
            close(result);
            result = -EMFILE;
        }
    }
    memset(&operation->completion, 0, sizeof(operation->completion));
    operation->completion.request = request;
    operation->completion.result = result;
    operation->completion.generation = request->generation;
    operation->completion.task = request->task;
    if (request->op == VL_IO_ACCEPT || request->op == VL_IO_RECV) {
        operation->completion.events |= VL_IO_EVENT_READABLE;
    }
    if (request->op == VL_IO_SEND || request->op == VL_IO_CONNECT) {
        operation->completion.events |= VL_IO_EVENT_WRITABLE;
    }
    if (request->op == VL_IO_RECV && result == 0) {
        operation->completion.events |= VL_IO_EVENT_EOF;
    }
    if (result < 0 && result != -ETIME && result != -ECANCELED) {
        operation->completion.events |= VL_IO_EVENT_ERROR;
    }
    if (request->op != VL_IO_TIMEOUT) {
        vl_socket_release(request->fd, request->generation);
    }

    pthread_mutex_lock(&state->mutex);
    if (!operation->resolved) {
        operation->resolved = 1;
        vl_uring_remove_active_locked(state, operation);
        if (state->completed_tail == NULL) {
            state->completed_head = operation;
        } else {
            state->completed_tail->completed_next = operation;
        }
        state->completed_tail = operation;
        pthread_cond_signal(&state->condition);
    }
    pthread_mutex_unlock(&state->mutex);
}

static int vl_uring_process_commands(vl_uring_state_t *state)
{
    vl_uring_command_t *command;
    int stopping = 0;

    pthread_mutex_lock(&state->mutex);
    command = state->command_head;
    state->command_head = NULL;
    state->command_tail = NULL;
    pthread_mutex_unlock(&state->mutex);

    while (command != NULL) {
        vl_uring_command_t *next = command->next;
        vl_uring_operation_t *operation = command->operation;
        int status = VL_OK;
        int free_operation = 0;

        if (command->type == VL_URING_COMMAND_SUBMIT) {
            status = vl_uring_prepare_operation(state, operation);
        } else if (command->type == VL_URING_COMMAND_CANCEL) {
            pthread_mutex_lock(&state->mutex);
            if (!operation->resolved) {
                status = vl_uring_prepare_cancel(state, operation);
            }
            operation->cancel_command_pending = 0;
            free_operation = operation->resolved &&
                             operation->completion_consumed;
            pthread_mutex_unlock(&state->mutex);
            if (free_operation) {
                free(operation);
            }
        }
        free(command);
        if (status != VL_OK) {
            pthread_mutex_lock(&state->mutex);
            state->worker_status = status;
            pthread_cond_broadcast(&state->condition);
            pthread_mutex_unlock(&state->mutex);
            stopping = 1;
        }
        command = next;
    }
    pthread_mutex_lock(&state->mutex);
    stopping = stopping || state->stop_requested;
    pthread_mutex_unlock(&state->mutex);
    return stopping;
}

static void vl_uring_publish_startup(vl_uring_state_t *state, int status)
{
    pthread_mutex_lock(&state->mutex);
    state->startup_status = status;
    state->startup_done = 1;
    pthread_cond_signal(&state->startup_condition);
    pthread_mutex_unlock(&state->mutex);
}

static void *vl_uring_worker_main(void *argument)
{
    vl_uring_state_t *state = argument;
    struct io_uring_cqe *cqe;
    unsigned depth = state->queue_depth;
    int result;
    int stopping = 0;

    result = io_uring_queue_init(depth, &state->ring, 0);
    if (result < 0) {
        vl_uring_publish_startup(state, vl_uring_status_from_error(-result));
        return NULL;
    }
    if (vl_uring_prepare_wakeup(state) != VL_OK ||
        io_uring_submit(&state->ring) < 0) {
        vl_uring_publish_startup(state, VL_ERROR_SYSTEM);
        io_uring_queue_exit(&state->ring);
        return NULL;
    }
    vl_uring_publish_startup(state, VL_OK);

    while (!stopping) {
        result = io_uring_wait_cqe(&state->ring, &cqe);
        if (result == -EINTR) {
            continue;
        }
        if (result < 0) {
            pthread_mutex_lock(&state->mutex);
            state->worker_status = VL_ERROR_SYSTEM;
            pthread_cond_broadcast(&state->condition);
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        if (io_uring_cqe_get_data64(cqe) == VL_URING_WAKE_DATA) {
            vl_uring_drain_wakeup(state);
            io_uring_cqe_seen(&state->ring, cqe);
            stopping = vl_uring_process_commands(state);
            if (!stopping && vl_uring_prepare_wakeup(state) != VL_OK) {
                pthread_mutex_lock(&state->mutex);
                state->worker_status = VL_ERROR_SYSTEM;
                pthread_cond_broadcast(&state->condition);
                pthread_mutex_unlock(&state->mutex);
                stopping = 1;
            }
        } else if ((io_uring_cqe_get_data64(cqe) &
                    VL_URING_CANCEL_BIT) != 0) {
            io_uring_cqe_seen(&state->ring, cqe);
        } else {
            vl_uring_operation_t *operation = io_uring_cqe_get_data(cqe);
            int completion_result = cqe->res;

            io_uring_cqe_seen(&state->ring, cqe);
            vl_uring_complete_operation(state, operation, completion_result);
        }
        if (!stopping && io_uring_submit(&state->ring) < 0) {
            pthread_mutex_lock(&state->mutex);
            state->worker_status = VL_ERROR_SYSTEM;
            pthread_cond_broadcast(&state->condition);
            pthread_mutex_unlock(&state->mutex);
            stopping = 1;
        }
    }
    io_uring_queue_exit(&state->ring);
    return NULL;
}

static void vl_uring_free_state(vl_uring_state_t *state)
{
    vl_uring_command_t *command;
    vl_uring_operation_t *operation;

    while ((command = state->command_head) != NULL) {
        state->command_head = command->next;
        free(command);
    }
    while ((operation = state->active) != NULL) {
        state->active = operation->active_next;
        if (operation->request->op != VL_IO_TIMEOUT) {
            vl_socket_release(operation->request->fd,
                              operation->request->generation);
        }
        free(operation);
    }
    while ((operation = state->completed_head) != NULL) {
        state->completed_head = operation->completed_next;
        free(operation);
    }
    if (state->event_fd >= 0) {
        close(state->event_fd);
    }
    pthread_cond_destroy(&state->startup_condition);
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->mutex);
    free(state);
}

int vl_io_worker_start(void **worker, unsigned queue_depth)
{
    vl_uring_state_t *state;
    unsigned depth = queue_depth == 0 ? VL_URING_DEFAULT_DEPTH : queue_depth;
    int status;

    if (worker == NULL || *worker != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (depth < VL_URING_MINIMUM_DEPTH) {
        depth = VL_URING_MINIMUM_DEPTH;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    state->event_fd = -1;
    state->worker_status = VL_OK;
    state->queue_depth = depth;
    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        free(state);
        return VL_ERROR_SYSTEM;
    }
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return VL_ERROR_SYSTEM;
    }
    if (pthread_cond_init(&state->startup_condition, NULL) != 0) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return VL_ERROR_SYSTEM;
    }
    state->event_fd = eventfd(0, EFD_CLOEXEC);
    if (state->event_fd < 0) {
        vl_uring_free_state(state);
        return VL_ERROR_SYSTEM;
    }
    if (pthread_create(&state->thread, NULL, vl_uring_worker_main, state) != 0) {
        vl_uring_free_state(state);
        return VL_ERROR_SYSTEM;
    }
    pthread_mutex_lock(&state->mutex);
    while (!state->startup_done) {
        pthread_cond_wait(&state->startup_condition, &state->mutex);
    }
    status = state->startup_status;
    pthread_mutex_unlock(&state->mutex);
    if (status != VL_OK) {
        pthread_join(state->thread, NULL);
        vl_uring_free_state(state);
        return status;
    }
    *worker = state;
    return VL_OK;
}

void vl_io_worker_stop(void *worker)
{
    vl_uring_state_t *state = worker;

    if (state == NULL) {
        return;
    }
    pthread_mutex_lock(&state->mutex);
    state->stop_requested = 1;
    pthread_mutex_unlock(&state->mutex);
    (void)vl_uring_write_wakeup(state);
    pthread_join(state->thread, NULL);
    vl_uring_free_state(state);
}

int vl_io_worker_submit(void *worker, vl_io_request_t *request)
{
    vl_uring_state_t *state = worker;
    vl_uring_operation_t *operation;
    vl_uring_command_t *command;

    if (state == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    operation = calloc(1, sizeof(*operation));
    command = calloc(1, sizeof(*command));
    if (operation == NULL || command == NULL) {
        free(operation);
        free(command);
        return VL_ERROR_OUT_OF_MEMORY;
    }
    operation->request = request;
    command->type = VL_URING_COMMAND_SUBMIT;
    command->operation = operation;
    pthread_mutex_lock(&state->mutex);
    if (state->worker_status != VL_OK ||
        vl_uring_find_active_locked(state, request) != NULL) {
        pthread_mutex_unlock(&state->mutex);
        free(command);
        free(operation);
        return VL_ERROR_INVALID_STATE;
    }
    operation->active_next = state->active;
    state->active = operation;
    vl_uring_append_command_locked(state, command);
    pthread_mutex_unlock(&state->mutex);
    return vl_uring_write_wakeup(state);
}

int vl_io_worker_cancel(void *worker, vl_io_request_t *request)
{
    vl_uring_state_t *state = worker;
    vl_uring_operation_t *operation;
    vl_uring_command_t *command;

    if (state == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    command = calloc(1, sizeof(*command));
    if (command == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    pthread_mutex_lock(&state->mutex);
    operation = vl_uring_find_active_locked(state, request);
    if (operation == NULL || operation->cancel_command_pending) {
        pthread_mutex_unlock(&state->mutex);
        free(command);
        return VL_ERROR_INVALID_STATE;
    }
    operation->cancel_command_pending = 1;
    command->type = VL_URING_COMMAND_CANCEL;
    command->operation = operation;
    vl_uring_append_command_locked(state, command);
    pthread_mutex_unlock(&state->mutex);
    return vl_uring_write_wakeup(state);
}

static void vl_uring_make_deadline(int timeout_ms, struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

int vl_io_worker_poll(void *worker, int timeout_ms,
                      vl_io_completion_t *completion)
{
    vl_uring_state_t *state = worker;
    vl_uring_operation_t *operation;
    struct timespec deadline;
    int wait_status = 0;
    int free_operation;

    if (state == NULL || completion == NULL || timeout_ms < -1) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (timeout_ms > 0) {
        vl_uring_make_deadline(timeout_ms, &deadline);
    }
    pthread_mutex_lock(&state->mutex);
    while (state->completed_head == NULL && state->worker_status == VL_OK) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&state->mutex);
            return VL_ERROR_WOULD_BLOCK;
        }
        if (timeout_ms < 0) {
            wait_status = pthread_cond_wait(&state->condition, &state->mutex);
        } else {
            wait_status = pthread_cond_timedwait(&state->condition,
                                                 &state->mutex, &deadline);
        }
        if (wait_status == ETIMEDOUT) {
            pthread_mutex_unlock(&state->mutex);
            return VL_ERROR_WOULD_BLOCK;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&state->mutex);
            return VL_ERROR_SYSTEM;
        }
    }
    if (state->completed_head == NULL) {
        int status = state->worker_status;

        pthread_mutex_unlock(&state->mutex);
        return status;
    }
    operation = state->completed_head;
    state->completed_head = operation->completed_next;
    if (state->completed_head == NULL) {
        state->completed_tail = NULL;
    }
    operation->completed_next = NULL;
    operation->completion_consumed = 1;
    *completion = operation->completion;
    free_operation = !operation->cancel_command_pending;
    pthread_mutex_unlock(&state->mutex);
    if (free_operation) {
        free(operation);
    }
    return VL_OK;
}
