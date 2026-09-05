#ifndef VELOCO_IO_H
#define VELOCO_IO_H

#include <veloco/common.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct vl_task vl_task_t;

typedef struct vl_io {
    void *impl;
} vl_io_t;

typedef enum vl_io_backend {
    VL_IO_BACKEND_EPOLL = 0,
    VL_IO_BACKEND_URING = 1
} vl_io_backend_t;

typedef struct vl_io_config {
    vl_io_backend_t backend;
    unsigned queue_depth;
} vl_io_config_t;

typedef enum vl_io_op {
    VL_IO_ACCEPT = 0,
    VL_IO_RECV = 1,
    VL_IO_SEND = 2,
    VL_IO_CONNECT = 3,
    VL_IO_TIMEOUT = 4,
    VL_IO_CANCEL = 5
} vl_io_op_t;

typedef enum vl_io_event {
    VL_IO_EVENT_NONE = 0,
    VL_IO_EVENT_READABLE = 1u << 0,
    VL_IO_EVENT_WRITABLE = 1u << 1,
    VL_IO_EVENT_EOF = 1u << 2,
    VL_IO_EVENT_ERROR = 1u << 3
} vl_io_event_t;

typedef struct vl_io_request {
    vl_io_op_t op;
    int fd;
    void *buf;
    size_t len;
    const struct sockaddr *address;
    socklen_t address_len;
    uint64_t timeout_ns;
    uint64_t generation;
    vl_task_t *task;
    ssize_t result;
    uint32_t events;
    int completed;
} vl_io_request_t;

typedef struct vl_io_completion {
    vl_io_request_t *request;
    ssize_t result;
    uint32_t events;
    uint64_t generation;
    vl_task_t *task;
} vl_io_completion_t;

typedef struct vl_io_stats {
    size_t submissions;
    size_t completions;
    size_t cancellations;
} vl_io_stats_t;

/*
 * A submitted request and its buffer remain caller-owned and must stay valid
 * until vl_io_poll returns their completion or the owning I/O handle is
 * destroyed. One request may be pending per fd process-wide. An I/O handle
 * is bound to its initializing thread. Socket helpers track fd generations;
 * external sockets must first call vl_socket_track and must be closed with
 * vl_socket_close.
 *
 * vl_io_init selects io_uring when it was compiled in and the kernel permits
 * ring creation, otherwise it falls back to epoll. vl_io_init_with_config
 * never falls back from an explicitly selected backend. queue_depth is used
 * by io_uring; zero selects the library default.
 *
 * When request->task is NULL, submit returns immediately and the owner calls
 * vl_io_poll. When it is vl_task_current(), submit parks that Task; its
 * Runtime drives the I/O handle and submit returns after completion fields
 * have been written to the request.
 */
VL_API int vl_io_init(vl_io_t *io);
VL_API int vl_io_init_with_config(vl_io_t *io,
                                  const vl_io_config_t *config);
VL_API vl_io_backend_t vl_io_backend(const vl_io_t *io);
VL_API void vl_io_destroy(vl_io_t *io);
VL_API int vl_io_submit(vl_io_t *io, vl_io_request_t *request);
VL_API int vl_io_cancel(vl_io_t *io, vl_io_request_t *request);
VL_API int vl_io_poll(vl_io_t *io, int timeout_ms,
                      vl_io_completion_t *completion);
VL_API void vl_io_get_stats(const vl_io_t *io, vl_io_stats_t *out);

VL_API int vl_socket_set_nonblocking(int fd);
VL_API int vl_socket_create_tcp(void);
VL_API int vl_socket_listen_loopback(uint16_t port, int backlog,
                                    uint16_t *bound_port);
VL_API int vl_socket_connect_loopback(uint16_t port);
VL_API int vl_socket_accept(int listener_fd);
VL_API int vl_socket_track(int fd);
VL_API uint64_t vl_socket_generation(int fd);
VL_API int vl_socket_close(int fd);

#endif
