#include "io_internal.h"

#include "../runtime/runtime_internal.h"

#include <stdlib.h>
#include <string.h>

static int vl_io_is_owner(const vl_io_impl_t *impl)
{
    return impl != NULL &&
           pthread_equal(impl->owner_thread, pthread_self());
}

int vl_io_init(vl_io_t *io)
{
    vl_io_impl_t *impl;

    if (io == NULL || io->impl != NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    if (vl_epoll_backend_init(impl) != VL_OK) {
        free(impl);
        return VL_ERROR_SYSTEM;
    }
    impl->owner_thread = pthread_self();
    io->impl = impl;
    return VL_OK;
}

void vl_io_destroy(vl_io_t *io)
{
    vl_io_impl_t *impl;

    if (io == NULL || (impl = io->impl) == NULL || !vl_io_is_owner(impl)) {
        return;
    }
    vl_epoll_backend_destroy(impl);
    free(impl);
    io->impl = NULL;
}

int vl_io_submit(vl_io_t *io, vl_io_request_t *request)
{
    int status;

    if (io == NULL || io->impl == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_io_is_owner(io->impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    request->completed = 0;
    request->result = 0;
    request->events = VL_IO_EVENT_NONE;
    if (request->task != NULL &&
        vl_task_can_park_for_io(request->task, io) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    status = vl_epoll_backend_submit(io->impl, request);
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
    if (io == NULL || io->impl == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_io_is_owner(io->impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    return vl_epoll_backend_cancel(io->impl, request);
}

int vl_io_poll(vl_io_t *io, int timeout_ms, vl_io_completion_t *completion)
{
    int status;

    if (io == NULL || io->impl == NULL || completion == NULL ||
        timeout_ms < -1) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    if (!vl_io_is_owner(io->impl)) {
        return VL_ERROR_INVALID_STATE;
    }
    memset(completion, 0, sizeof(*completion));
    status = vl_epoll_backend_poll(io->impl, timeout_ms, completion);
    if (status != VL_OK) {
        return status;
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
