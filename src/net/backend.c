#include "io_internal.h"

#include "../runtime/runtime_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int vl_io_is_owner(const vl_io_impl_t *impl)
{
    return impl != NULL &&
           pthread_equal(impl->owner_thread, pthread_self());
}

int vl_io_init(vl_io_t *io)
{
    vl_io_config_t config;
    int status;

    config.backend = VL_IO_BACKEND_EPOLL;
    config.queue_depth = 0;
#if defined(VELOCO_HAS_URING)
    config.backend = VL_IO_BACKEND_URING;
#endif
    status = vl_io_init_with_config(io, &config);
    if (status == VL_OK) {
        return VL_OK;
    }
    if (status != VL_ERROR_UNSUPPORTED ||
        config.backend == VL_IO_BACKEND_EPOLL) {
        return status;
    }
    config.backend = VL_IO_BACKEND_EPOLL;
    return vl_io_init_with_config(io, &config);
}

int vl_io_init_with_config(vl_io_t *io, const vl_io_config_t *config)
{
    vl_io_impl_t *impl;
    int status;

    if (io == NULL || io->impl != NULL || config == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (config->backend != VL_IO_BACKEND_EPOLL &&
        config->backend != VL_IO_BACKEND_URING) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    impl->epoll_fd = -1;
    impl->backend = config->backend;
    impl->owner_thread = pthread_self();
    if (config->backend == VL_IO_BACKEND_URING) {
#if defined(VELOCO_HAS_URING)
        status = vl_uring_backend_init(impl, config->queue_depth);
#else
        status = VL_ERROR_UNSUPPORTED;
#endif
    } else {
        status = vl_epoll_backend_init(impl);
    }
    if (status != VL_OK) {
        free(impl);
        return status;
    }
    io->impl = impl;
    return VL_OK;
}

vl_io_backend_t vl_io_backend(const vl_io_t *io)
{
    return io != NULL && io->impl != NULL
               ? ((const vl_io_impl_t *)io->impl)->backend
               : VL_IO_BACKEND_EPOLL;
}

void vl_io_destroy(vl_io_t *io)
{
    vl_io_impl_t *impl;

    if (io == NULL || (impl = io->impl) == NULL || !vl_io_is_owner(impl)) {
        return;
    }
#if defined(VELOCO_HAS_URING)
    if (impl->backend == VL_IO_BACKEND_URING) {
        vl_uring_backend_destroy(impl);
    } else
#endif
    {
        vl_epoll_backend_destroy(impl);
    }
    free(impl);
    io->impl = NULL;
}

int vl_io_submit(vl_io_t *io, vl_io_request_t *request)
{
    vl_io_impl_t *impl;
    int status;

    if (io == NULL || io->impl == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = io->impl;
    if (!vl_io_is_owner(impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    request->completed = 0;
    request->result = 0;
    request->events = VL_IO_EVENT_NONE;
    if (request->task != NULL &&
        vl_task_can_park_for_io(request->task, io) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    if (impl->backend == VL_IO_BACKEND_URING) {
#if defined(VELOCO_HAS_URING)
        status = vl_uring_backend_submit(impl, request);
#else
        status = VL_ERROR_UNSUPPORTED;
#endif
    } else {
        status = vl_epoll_backend_submit(impl, request);
    }
    if (status != VL_OK || request->task == NULL) {
        return status;
    }
    status = vl_task_park_for_io(request->task, io);
    if (status != VL_OK) {
        return status;
    }
    return request->completed ? VL_OK : VL_ERROR_INVALID_STATE;
}

int vl_io_cancel(vl_io_t *io, vl_io_request_t *request)
{
    vl_io_impl_t *impl;

    if (io == NULL || io->impl == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = io->impl;
    if (!vl_io_is_owner(impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    if (impl->backend == VL_IO_BACKEND_URING) {
#if defined(VELOCO_HAS_URING)
        return vl_uring_backend_cancel(impl, request);
#else
        return VL_ERROR_UNSUPPORTED;
#endif
    }
    return vl_epoll_backend_cancel(impl, request);
}

int vl_io_poll(vl_io_t *io, int timeout_ms, vl_io_completion_t *completion)
{
    vl_io_impl_t *impl;
    int status;

    if (io == NULL || io->impl == NULL || completion == NULL ||
        timeout_ms < -1) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = io->impl;
    if (!vl_io_is_owner(impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    memset(completion, 0, sizeof(*completion));
    if (impl->backend == VL_IO_BACKEND_URING) {
#if defined(VELOCO_HAS_URING)
        status = vl_uring_backend_poll(impl, timeout_ms, completion);
#else
        status = VL_ERROR_UNSUPPORTED;
#endif
    } else {
        status = vl_epoll_backend_poll(impl, timeout_ms, completion);
    }
    if (status != VL_OK) {
        return status;
    }
    if (vl_socket_request_is_stale(completion->request)) {
        if (completion->request->op == VL_IO_ACCEPT &&
            completion->result >= 0) {
            (void)vl_socket_close((int)completion->result);
        }
        completion->result = -ESTALE;
        completion->events |= VL_IO_EVENT_ERROR;
    }
    completion->request->result = completion->result;
    completion->request->events = completion->events;
    completion->request->completed = 1;
    if (completion->task != NULL) {
        status = vl_task_complete_io(completion->task, completion);
        if (status != VL_OK) {
            return status;
        }
    }
    return VL_OK;
}
