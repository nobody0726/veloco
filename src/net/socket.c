#define _GNU_SOURCE

#include <veloco/io.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct vl_socket_slot {
    int fd;
    uint64_t generation;
    uint64_t pending_generation;
    int active;
    struct vl_socket_slot *next;
} vl_socket_slot_t;

static vl_socket_slot_t *vl_socket_slots;
static pthread_mutex_t vl_socket_slots_mutex = PTHREAD_MUTEX_INITIALIZER;

static vl_socket_slot_t *vl_socket_find_slot_locked(int fd)
{
    vl_socket_slot_t *slot;

    for (slot = vl_socket_slots; slot != NULL; slot = slot->next) {
        if (slot->fd == fd) {
            return slot;
        }
    }
    return NULL;
}

int vl_socket_set_nonblocking(int fd)
{
    int flags;

    if (fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return VL_ERROR_SYSTEM;
    }
    return VL_OK;
}

int vl_socket_create_tcp(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return VL_ERROR_SYSTEM;
    }
    if (vl_socket_set_nonblocking(fd) != VL_OK ||
        vl_socket_track(fd) != VL_OK) {
        close(fd);
        return VL_ERROR_SYSTEM;
    }
    return fd;
}

int vl_socket_track(int fd)
{
    vl_socket_slot_t *slot;

    if (fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    if (slot == NULL) {
        slot = calloc(1, sizeof(*slot));
        if (slot == NULL) {
            pthread_mutex_unlock(&vl_socket_slots_mutex);
            return VL_ERROR_OUT_OF_MEMORY;
        }
        slot->fd = fd;
        slot->next = vl_socket_slots;
        vl_socket_slots = slot;
    }
    if (slot->active || slot->generation == UINT64_MAX) {
        pthread_mutex_unlock(&vl_socket_slots_mutex);
        return VL_ERROR_INVALID_STATE;
    }
    ++slot->generation;
    if (slot->generation == 0) {
        ++slot->generation;
    }
    slot->active = 1;
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return VL_OK;
}

uint64_t vl_socket_generation(int fd)
{
    vl_socket_slot_t *slot;
    uint64_t generation;

    if (fd < 0) {
        return 0;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    generation = slot != NULL ? slot->generation : 0;
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return generation;
}

int vl_socket_is_tracked(int fd)
{
    vl_socket_slot_t *slot;
    int active;

    if (fd < 0) {
        return 0;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    active = slot != NULL && slot->active;
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return active;
}

int vl_socket_claim(int fd, uint64_t generation)
{
    vl_socket_slot_t *slot;

    if (fd < 0 || generation == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    if (slot == NULL) {
        pthread_mutex_unlock(&vl_socket_slots_mutex);
        return VL_ERROR_INVALID_STATE;
    }
    if (!slot->active || slot->generation != generation ||
        slot->pending_generation != 0) {
        pthread_mutex_unlock(&vl_socket_slots_mutex);
        return VL_ERROR_INVALID_STATE;
    }
    slot->pending_generation = generation;
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return VL_OK;
}

void vl_socket_release(int fd, uint64_t generation)
{
    vl_socket_slot_t *slot;

    if (fd < 0) {
        return;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    if (slot != NULL && slot->pending_generation == generation) {
        slot->pending_generation = 0;
    }
    pthread_mutex_unlock(&vl_socket_slots_mutex);
}

int vl_socket_request_is_stale(const vl_io_request_t *request)
{
    vl_socket_slot_t *slot;
    int stale;

    if (request == NULL || request->op == VL_IO_TIMEOUT) {
        return 0;
    }
    if (request->fd < 0) {
        return 1;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(request->fd);
    stale = slot == NULL || !slot->active ||
            slot->generation != request->generation;
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return stale;
}

int vl_socket_close(int fd)
{
    vl_socket_slot_t *slot;

    if (fd < 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&vl_socket_slots_mutex);
    slot = vl_socket_find_slot_locked(fd);
    if (slot == NULL || !slot->active) {
        pthread_mutex_unlock(&vl_socket_slots_mutex);
        return VL_ERROR_INVALID_STATE;
    }
    if (close(fd) != 0) {
        pthread_mutex_unlock(&vl_socket_slots_mutex);
        return VL_ERROR_SYSTEM;
    }
    slot->active = 0;
    if (slot->generation != UINT64_MAX) {
        ++slot->generation;
        if (slot->generation == 0) {
            ++slot->generation;
        }
    }
    pthread_mutex_unlock(&vl_socket_slots_mutex);
    return VL_OK;
}

int vl_socket_listen_loopback(uint16_t port, int backlog,
                              uint16_t *bound_port)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int fd;
    int reuse = 1;

    if (backlog <= 0 || bound_port == NULL) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return VL_ERROR_SYSTEM;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        vl_socket_set_nonblocking(fd) != VL_OK) {
        close(fd);
        return VL_ERROR_SYSTEM;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, backlog) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_length) != 0 ||
        vl_socket_track(fd) != VL_OK) {
        close(fd);
        return VL_ERROR_SYSTEM;
    }
    *bound_port = ntohs(address.sin_port);
    return fd;
}

int vl_socket_connect_loopback(uint16_t port)
{
    struct sockaddr_in address;
    int fd;
    int result;

    if (port == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || vl_socket_set_nonblocking(fd) != VL_OK ||
        vl_socket_track(fd) != VL_OK) {
        if (fd >= 0) {
            close(fd);
        }
        return VL_ERROR_SYSTEM;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
        (void)vl_socket_close(fd);
        return VL_ERROR_SYSTEM;
    }
    return fd;
}

int vl_socket_accept(int listener_fd)
{
    int fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (fd < 0) {
        return VL_ERROR_SYSTEM;
    }
    if (vl_socket_track(fd) != VL_OK) {
        close(fd);
        return VL_ERROR_SYSTEM;
    }
    return fd;
}
