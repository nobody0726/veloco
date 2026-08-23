#ifndef VELOCO_IO_INTERNAL_H
#define VELOCO_IO_INTERNAL_H

#include <veloco/io.h>

#include <pthread.h>
#include <stddef.h>

typedef struct vl_io_waiter vl_io_waiter_t;
typedef struct vl_io_completion_node vl_io_completion_node_t;

struct vl_io_waiter {
    vl_io_request_t *request;
    uint32_t events;
    vl_io_waiter_t *next;
};

struct vl_io_completion_node {
    vl_io_completion_t completion;
    vl_io_completion_node_t *next;
};

typedef struct vl_io_impl {
    int epoll_fd;
    pthread_t owner_thread;
    vl_io_waiter_t *waiters;
    vl_io_completion_node_t *completed_head;
    vl_io_completion_node_t *completed_tail;
} vl_io_impl_t;

int vl_epoll_backend_init(vl_io_impl_t *impl);
void vl_epoll_backend_destroy(vl_io_impl_t *impl);
int vl_epoll_backend_submit(vl_io_impl_t *impl, vl_io_request_t *request);
int vl_epoll_backend_cancel(vl_io_impl_t *impl, vl_io_request_t *request);
int vl_epoll_backend_poll(vl_io_impl_t *impl, int timeout_ms,
                          vl_io_completion_t *completion);
int vl_socket_is_tracked(int fd);
int vl_socket_claim(int fd, uint64_t generation);
void vl_socket_release(int fd, uint64_t generation);

#endif
