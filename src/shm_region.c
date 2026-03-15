#define _POSIX_C_SOURCE 200809L

#include "offset_store/shm_region.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint64_t OFFSET_STORE_REGION_MAGIC = UINT64_C(0x4f464653544f5245);

static void shm_region_reset(ShmRegion *region)
{
    if (region == NULL) {
        return;
    }

    region->fd = -1;
    region->base = NULL;
    region->size = 0;
    region->creator = false;
}

static bool shm_region_is_size_valid(size_t size)
{
    return size >= sizeof(ShmRegionHeader);
}

static bool shm_region_map_fd(ShmRegion *region, int fd, size_t size, bool creator)
{
    void *mapping;

    if (region == NULL || !shm_region_is_size_valid(size)) {
        return false;
    }

    mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }

    region->fd = fd;
    region->base = mapping;
    region->size = size;
    region->creator = creator;
    return true;
}

static bool shm_region_validate_header(const ShmRegion *region)
{
    const ShmRegionHeader *header;

    if (region == NULL || region->base == NULL) {
        return false;
    }

    header = (const ShmRegionHeader *) region->base;
    if (header->magic != OFFSET_STORE_REGION_MAGIC) {
        return false;
    }

    if (header->version != OFFSET_STORE_REGION_VERSION) {
        return false;
    }

    if (header->total_size != (uint64_t) region->size) {
        return false;
    }

    return true;
}

static bool shm_region_init_mutex(ShmRegionHeader *header)
{
    pthread_mutexattr_t attr;

    if (header == NULL) {
        errno = EINVAL;
        return false;
    }

    if (pthread_mutexattr_init(&attr) != 0) {
        errno = EINVAL;
        return false;
    }

    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&attr);
        errno = EINVAL;
        return false;
    }

    if (pthread_mutex_init(&header->mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        errno = EINVAL;
        return false;
    }

    pthread_mutexattr_destroy(&attr);
    return true;
}

bool shm_region_create(ShmRegion *out_region, const char *name, size_t size)
{
    int fd;
    ShmRegionHeader *header;

    if (out_region == NULL || name == NULL || !shm_region_is_size_valid(size)) {
        errno = EINVAL;
        return false;
    }

    shm_region_reset(out_region);

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return false;
    }

    if (ftruncate(fd, (off_t) size) != 0) {
        close(fd);
        shm_unlink(name);
        return false;
    }

    if (!shm_region_map_fd(out_region, fd, size, true)) {
        close(fd);
        shm_unlink(name);
        return false;
    }

    /*
     * The creator fully defines the initial header so later attachers can
     * validate that the mapping matches the expected format.
     */
    header = shm_region_header_mut(out_region);
    header->magic = OFFSET_STORE_REGION_MAGIC;
    header->version = OFFSET_STORE_REGION_VERSION;
    header->reserved = 0;
    header->total_size = (uint64_t) out_region->size;
    if (!shm_region_init_mutex(header)) {
        shm_region_close(out_region);
        shm_unlink(name);
        return false;
    }
    return true;
}

bool shm_region_open(ShmRegion *out_region, const char *name)
{
    int fd;
    struct stat st;

    if (out_region == NULL || name == NULL) {
        errno = EINVAL;
        return false;
    }

    shm_region_reset(out_region);

    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        return false;
    }

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    if (!shm_region_map_fd(out_region, fd, (size_t) st.st_size, false)) {
        close(fd);
        return false;
    }

    if (!shm_region_validate_header(out_region)) {
        shm_region_close(out_region);
        errno = EINVAL;
        return false;
    }

    return true;
}

bool shm_region_close(ShmRegion *region)
{
    int saved_errno;

    if (region == NULL) {
        errno = EINVAL;
        return false;
    }

    saved_errno = 0;
    if (region->base != NULL && munmap(region->base, region->size) != 0) {
        saved_errno = errno;
    }

    if (region->fd >= 0 && close(region->fd) != 0 && saved_errno == 0) {
        saved_errno = errno;
    }

    shm_region_reset(region);
    if (saved_errno != 0) {
        errno = saved_errno;
        return false;
    }

    return true;
}

bool shm_region_unlink(const char *name)
{
    if (name == NULL) {
        errno = EINVAL;
        return false;
    }

    return shm_unlink(name) == 0;
}

bool shm_region_lock(ShmRegion *region)
{
    ShmRegionHeader *header;

    if (region == NULL) {
        errno = EINVAL;
        return false;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        errno = EINVAL;
        return false;
    }

    if (pthread_mutex_lock(&header->mutex) != 0) {
        errno = EINVAL;
        return false;
    }

    return true;
}

bool shm_region_unlock(ShmRegion *region)
{
    ShmRegionHeader *header;

    if (region == NULL) {
        errno = EINVAL;
        return false;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        errno = EINVAL;
        return false;
    }

    if (pthread_mutex_unlock(&header->mutex) != 0) {
        errno = EINVAL;
        return false;
    }

    return true;
}

const ShmRegionHeader *shm_region_header(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL) {
        return NULL;
    }

    return (const ShmRegionHeader *) region->base;
}

ShmRegionHeader *shm_region_header_mut(ShmRegion *region)
{
    if (region == NULL || region->base == NULL) {
        return NULL;
    }

    return (ShmRegionHeader *) region->base;
}

void *shm_region_data(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < sizeof(ShmRegionHeader)) {
        return NULL;
    }

    /* Callers place allocator metadata and objects after the fixed header. */
    return (void *) ((unsigned char *) region->base + sizeof(ShmRegionHeader));
}

size_t shm_region_usable_size(const ShmRegion *region)
{
    if (region == NULL || region->size < sizeof(ShmRegionHeader)) {
        return 0;
    }

    return region->size - sizeof(ShmRegionHeader);
}
