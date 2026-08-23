#define _GNU_SOURCE

#include "io_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static vl_io_waiter_t *vl_find_waiter(vl_io_impl_t *impl, int fd)
{
    vl_io_waiter_t *waiter;

    for (waiter = impl->waiters; waiter != NULL; waiter = waiter->next) {
        if (waiter->request->fd == fd) {
            return waiter;
        }
    }
    return NULL;
}

static vl_io_waiter_t *vl_find_request(vl_io_impl_t *impl,
                                       const vl_io_request_t *request)
{
    vl_io_waiter_t *waiter;

    for (waiter = impl->waiters; waiter != NULL; waiter = waiter->next) {
        if (waiter->request == request) {
            return waiter;
        }
    }
    return NULL;
}

static void vl_remove_waiter(vl_io_impl_t *impl, vl_io_waiter_t *target)
{
    vl_io_waiter_t **cursor = &impl->waiters;

    while (*cursor != NULL && *cursor != target) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == target) {
        *cursor = target->next;
        target->next = NULL;
    }
}

static void vl_delete_waiter(vl_io_impl_t *impl, vl_io_waiter_t *waiter)
{
    (void)epoll_ctl(impl->epoll_fd, EPOLL_CTL_DEL, waiter->request->fd, NULL);
    vl_remove_waiter(impl, waiter);
    vl_socket_release(waiter->request->fd, waiter->request->generation);
    free(waiter);
}

static int vl_queue_completion(vl_io_impl_t *impl,
                               const vl_io_completion_t *completion)
{
    vl_io_completion_node_t *node = malloc(sizeof(*node));

    if (node == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    node->completion = *completion;
    node->next = NULL;
    if (impl->completed_tail == NULL) {
        impl->completed_head = node;
    } else {
        impl->completed_tail->next = node;
    }
    impl->completed_tail = node;
    return VL_OK;
}

static int vl_pop_completion(vl_io_impl_t *impl,
                             vl_io_completion_t *completion)
{
    vl_io_completion_node_t *node;

    if (impl->completed_head == NULL) {
        return VL_ERROR_WOULD_BLOCK;
    }
    node = impl->completed_head;
    impl->completed_head = node->next;
    if (impl->completed_head == NULL) {
        impl->completed_tail = NULL;
    }
    *completion = node->completion;
    free(node);
    return VL_OK;
}

static int vl_request_events(vl_io_op_t op, uint32_t *events)
{
    switch (op) {
    case VL_IO_ACCEPT:
    case VL_IO_RECV:
        *events = EPOLLIN;
        return VL_OK;
    case VL_IO_SEND:
    case VL_IO_CONNECT:
        *events = EPOLLOUT;
        return VL_OK;
    default:
        return VL_ERROR_UNSUPPORTED;
    }
}

int vl_epoll_backend_init(vl_io_impl_t *impl)
{
    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    return impl->epoll_fd >= 0 ? VL_OK : VL_ERROR_SYSTEM;
}

void vl_epoll_backend_destroy(vl_io_impl_t *impl)
{
    vl_io_waiter_t *waiter;
    vl_io_waiter_t *next_waiter;
    vl_io_completion_node_t *node;
    vl_io_completion_node_t *next_node;

    if (impl == NULL) {
        return;
    }
    for (waiter = impl->waiters; waiter != NULL; waiter = next_waiter) {
        next_waiter = waiter->next;
        vl_socket_release(waiter->request->fd,
                          waiter->request->generation);
        free(waiter);
    }
    for (node = impl->completed_head; node != NULL; node = next_node) {
        next_node = node->next;
        free(node);
    }
    if (impl->epoll_fd >= 0) {
        close(impl->epoll_fd);
    }
    impl->epoll_fd = -1;
    impl->waiters = NULL;
    impl->completed_head = NULL;
    impl->completed_tail = NULL;
}

int vl_epoll_backend_submit(vl_io_impl_t *impl, vl_io_request_t *request)
{
    vl_io_waiter_t *waiter;
    struct epoll_event event;
    uint32_t events;
    uint64_t generation;

    if (impl == NULL || request == NULL || request->fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_request_events(request->op, &events) != VL_OK) {
        return VL_ERROR_UNSUPPORTED;
    }
    if ((request->op == VL_IO_RECV || request->op == VL_IO_SEND) &&
        request->buf == NULL && request->len != 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (vl_find_request(impl, request) != NULL ||
        vl_find_waiter(impl, request->fd) != NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    generation = vl_socket_generation(request->fd);
    if (!vl_socket_is_tracked(request->fd) || generation == 0) {
        return VL_ERROR_INVALID_STATE;
    }
    if (request->generation != 0 && generation != request->generation) {
        return VL_ERROR_INVALID_STATE;
    }
    if (request->generation == 0) {
        request->generation = generation;
    }
    if (vl_socket_claim(request->fd, request->generation) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    waiter = calloc(1, sizeof(*waiter));
    if (waiter == NULL) {
        vl_socket_release(request->fd, request->generation);
        return VL_ERROR_OUT_OF_MEMORY;
    }
    waiter->request = request;
    waiter->events = events;
    memset(&event, 0, sizeof(event));
    event.events = events | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    event.data.ptr = waiter;
    if (epoll_ctl(impl->epoll_fd, EPOLL_CTL_ADD, request->fd, &event) != 0) {
        vl_socket_release(request->fd, request->generation);
        free(waiter);
        return errno == EBADF ? VL_ERROR_INVALID_STATE : VL_ERROR_SYSTEM;
    }
    waiter->next = impl->waiters;
    impl->waiters = waiter;
    return VL_OK;
}

int vl_epoll_backend_cancel(vl_io_impl_t *impl, vl_io_request_t *request)
{
    vl_io_waiter_t *waiter;
    vl_io_completion_t completion;

    if (impl == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    waiter = vl_find_request(impl, request);
    if (waiter == NULL) {
        return VL_ERROR_INVALID_STATE;
    }
    memset(&completion, 0, sizeof(completion));
    completion.request = request;
    completion.result = -ECANCELED;
    completion.generation = request->generation;
    completion.task = request->task;
    if (vl_queue_completion(impl, &completion) != VL_OK) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    vl_delete_waiter(impl, waiter);
    return VL_OK;
}

static int vl_remaining_timeout(const struct timespec *deadline,
                                int timeout_ms)
{
    struct timespec now;
    int64_t milliseconds;

    if (timeout_ms < 0) {
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return timeout_ms;
    }
    milliseconds = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000;
    milliseconds += (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (milliseconds <= 0) {
        return 0;
    }
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static int vl_make_deadline(int timeout_ms, struct timespec *deadline)
{
    if (timeout_ms < 0) {
        return VL_OK;
    }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
        return VL_ERROR_SYSTEM;
    }
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return VL_OK;
}

static int vl_complete_stale_waiter(vl_io_impl_t *impl,
                                    vl_io_waiter_t *waiter,
                                    vl_io_completion_t *completion)
{
    if (!vl_socket_request_is_stale(waiter->request)) {
        return 0;
    }
    completion->request = waiter->request;
    completion->result = -ESTALE;
    completion->generation = waiter->request->generation;
    completion->task = waiter->request->task;
    vl_delete_waiter(impl, waiter);
    return 1;
}

static int vl_scan_stale_waiters(vl_io_impl_t *impl,
                                 vl_io_completion_t *completion)
{
    vl_io_waiter_t *waiter;

    for (waiter = impl->waiters; waiter != NULL; waiter = waiter->next) {
        if (vl_complete_stale_waiter(impl, waiter, completion)) {
            return VL_OK;
        }
    }
    return VL_ERROR_WOULD_BLOCK;
}

static int vl_execute_ready(vl_io_impl_t *impl, vl_io_waiter_t *waiter,
                            uint32_t event_flags,
                            vl_io_completion_t *completion)
{
    vl_io_request_t *request = waiter->request;
    ssize_t result;
    int error;
    int accepted_fd;
    socklen_t error_length = sizeof(error);

    if (vl_socket_request_is_stale(request)) {
        completion->result = -ESTALE;
    } else {
        switch (request->op) {
        case VL_IO_ACCEPT:
            accepted_fd = accept4(request->fd, NULL, NULL,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (accepted_fd >= 0) {
                if (vl_socket_track(accepted_fd) == VL_OK) {
                    completion->result = accepted_fd;
                } else {
                    close(accepted_fd);
                    completion->result = -EMFILE;
                }
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return VL_ERROR_WOULD_BLOCK;
            } else {
                completion->result = -errno;
            }
            break;
        case VL_IO_RECV:
            result = recv(request->fd, request->buf, request->len, 0);
            if (result >= 0) {
                completion->result = result;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return VL_ERROR_WOULD_BLOCK;
            } else {
                completion->result = -errno;
            }
            break;
        case VL_IO_SEND:
            result = send(request->fd, request->buf, request->len,
                          MSG_NOSIGNAL);
            if (result >= 0) {
                completion->result = result;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return VL_ERROR_WOULD_BLOCK;
            } else {
                completion->result = -errno;
            }
            break;
        case VL_IO_CONNECT:
            if (getsockopt(request->fd, SOL_SOCKET, SO_ERROR, &error,
                           &error_length) != 0) {
                completion->result = -errno;
            } else {
                completion->result = error == 0 ? 0 : -error;
            }
            break;
        default:
            return VL_ERROR_UNSUPPORTED;
        }
    }
    completion->request = request;
    completion->events = VL_IO_EVENT_NONE;
    if ((event_flags & EPOLLIN) != 0) {
        completion->events |= VL_IO_EVENT_READABLE;
    }
    if ((event_flags & EPOLLOUT) != 0) {
        completion->events |= VL_IO_EVENT_WRITABLE;
    }
    if ((event_flags & (EPOLLHUP | EPOLLRDHUP)) != 0 ||
        (request->op == VL_IO_RECV && completion->result == 0)) {
        completion->events |= VL_IO_EVENT_EOF;
    }
    if ((event_flags & EPOLLERR) != 0 || completion->result < 0) {
        completion->events |= VL_IO_EVENT_ERROR;
    }
    completion->generation = request->generation;
    completion->task = request->task;
    vl_delete_waiter(impl, waiter);
    return VL_OK;
}

int vl_epoll_backend_poll(vl_io_impl_t *impl, int timeout_ms,
                          vl_io_completion_t *completion)
{
    struct epoll_event event;
    struct timespec deadline;
    int wait_timeout;
    int event_count;
    int status;

    if (impl == NULL || completion == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    status = vl_pop_completion(impl, completion);
    if (status == VL_OK) {
        return VL_OK;
    }
    if (vl_make_deadline(timeout_ms, &deadline) != VL_OK) {
        return VL_ERROR_SYSTEM;
    }
    for (;;) {
        status = vl_scan_stale_waiters(impl, completion);
        if (status == VL_OK) {
            return VL_OK;
        }
        wait_timeout = vl_remaining_timeout(&deadline, timeout_ms);
        event_count = epoll_wait(impl->epoll_fd, &event, 1, wait_timeout);
        if (event_count == 0) {
            return VL_ERROR_WOULD_BLOCK;
        }
        if (event_count < 0) {
            if (errno == EINTR) {
                if (wait_timeout == 0) {
                    return VL_ERROR_WOULD_BLOCK;
                }
                continue;
            }
            return VL_ERROR_SYSTEM;
        }
        status = vl_execute_ready(impl, event.data.ptr, event.events,
                                  completion);
        if (status == VL_OK || status == VL_ERROR_UNSUPPORTED) {
            return status;
        }
        if (timeout_ms == 0) {
            return VL_ERROR_WOULD_BLOCK;
        }
    }
}
