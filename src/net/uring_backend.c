#include "io_internal.h"

#include <stdint.h>

static int vl_uring_validate_request(const vl_io_request_t *request)
{
    if (request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    switch (request->op) {
    case VL_IO_ACCEPT:
        return request->fd >= 0 ? VL_OK : VL_ERROR_INVALID_ARGUMENT;
    case VL_IO_RECV:
    case VL_IO_SEND:
        if (request->fd < 0 || (request->buf == NULL && request->len != 0)) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        return VL_OK;
    case VL_IO_CONNECT:
        if (request->fd < 0 || request->address == NULL ||
            request->address_len == 0) {
            return VL_ERROR_INVALID_ARGUMENT;
        }
        return VL_OK;
    case VL_IO_TIMEOUT:
        return request->timeout_ns != 0 ? VL_OK : VL_ERROR_INVALID_ARGUMENT;
    default:
        return VL_ERROR_UNSUPPORTED;
    }
}

int vl_uring_backend_init(vl_io_impl_t *impl, unsigned queue_depth)
{
    int status;

    if (impl == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    status = vl_io_worker_start(&impl->uring, queue_depth);
    if (status == VL_OK) {
        impl->backend = VL_IO_BACKEND_URING;
    }
    return status;
}

void vl_uring_backend_destroy(vl_io_impl_t *impl)
{
    if (impl == NULL) {
        return;
    }
    vl_io_worker_stop(impl->uring);
    impl->uring = NULL;
}

int vl_uring_backend_submit(vl_io_impl_t *impl, vl_io_request_t *request)
{
    uint64_t generation;
    int status;

    if (impl == NULL || impl->uring == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    status = vl_uring_validate_request(request);
    if (status != VL_OK) {
        return status;
    }
    if (request->op == VL_IO_TIMEOUT) {
        request->generation = 0;
        return vl_io_worker_submit(impl->uring, request);
    }
    generation = vl_socket_generation(request->fd);
    if (generation == 0 ||
        (request->generation != 0 && request->generation != generation)) {
        return VL_ERROR_INVALID_STATE;
    }
    if (request->generation == 0) {
        request->generation = generation;
    }
    if (vl_socket_claim(request->fd, request->generation) != VL_OK) {
        return VL_ERROR_INVALID_STATE;
    }
    status = vl_io_worker_submit(impl->uring, request);
    if (status != VL_OK) {
        vl_socket_release(request->fd, request->generation);
    }
    return status;
}

int vl_uring_backend_cancel(vl_io_impl_t *impl, vl_io_request_t *request)
{
    if (impl == NULL || impl->uring == NULL || request == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    return vl_io_worker_cancel(impl->uring, request);
}

int vl_uring_backend_poll(vl_io_impl_t *impl, int timeout_ms,
                          vl_io_completion_t *completion)
{
    if (impl == NULL || impl->uring == NULL || completion == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    return vl_io_worker_poll(impl->uring, timeout_ms, completion);
}
