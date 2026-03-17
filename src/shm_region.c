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

/**
 * @brief Private shared-memory header stored at offset zero in every region.
 *
 * Keeping this typedef private prevents callers from depending on binary layout
 * details that should remain an implementation concern of the region module.
 */
typedef struct {
    /** Magic value identifying an offset-store region. */
    uint64_t magic;
    /** Region layout version expected by this binary. */
    uint32_t version;
    /** Reserved field for future expansion. */
    uint32_t reserved;
    /** Total mapped size in bytes. */
    uint64_t total_size;
    /** Process-shared mutex protecting coarse-grained shared mutation. */
    pthread_mutex_t mutex;
} ShmRegionHeader;

/**
 * @brief Resets a process-local region descriptor to its empty state.
 *
 * @param region Region descriptor to reset.
 */
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

/**
 * @brief Returns whether a region is large enough to hold the private header.
 *
 * @param size Candidate region size in bytes.
 * @return true if the size is valid.
 * @return false otherwise.
 */
static bool shm_region_is_size_valid(size_t size)
{
    return size >= sizeof(ShmRegionHeader);
}

/**
 * @brief Maps an already-open file descriptor as shared memory.
 *
 * @param region Region descriptor to populate.
 * @param fd Open shared-memory file descriptor.
 * @param size Mapping size in bytes.
 * @param creator Whether the caller created the region.
 * @return true if mapping succeeds.
 * @return false otherwise.
 */
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

/**
 * @brief Validates the private shared header of an attached region.
 *
 * @param region Region descriptor to inspect.
 * @return true if the header is valid.
 * @return false otherwise.
 */
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

/**
 * @brief Returns a const view of the private shared region header.
 *
 * @param region Region descriptor to inspect.
 * @return Const shared header pointer, or `NULL` on failure.
 */
static const ShmRegionHeader *shm_region_header(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL) {
        return NULL;
    }

    return (const ShmRegionHeader *) region->base;
}

/**
 * @brief Returns a mutable view of the private shared region header.
 *
 * @param region Region descriptor to inspect.
 * @return Mutable shared header pointer, or `NULL` on failure.
 */
static ShmRegionHeader *shm_region_header_mut(ShmRegion *region)
{
    if (region == NULL || region->base == NULL) {
        return NULL;
    }

    return (ShmRegionHeader *) region->base;
}

/**
 * @brief Initializes the process-shared mutex stored in the region header.
 *
 * @param header Shared region header to initialize.
 * @return Status code describing success or failure.
 */
static OffsetStoreStatus shm_region_init_mutex(ShmRegionHeader *header)
{
    pthread_mutexattr_t attr;

    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (pthread_mutexattr_init(&attr) != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&attr);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (pthread_mutex_init(&header->mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    pthread_mutexattr_destroy(&attr);
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Creates and initializes a new shared-memory region.
 *
 * @param out_region Region descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @param size Requested mapping size in bytes.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_create(ShmRegion *out_region, const char *name, size_t size)
{
    int fd;
    ShmRegionHeader *header;
    OffsetStoreStatus status;

    if (out_region == NULL || name == NULL || !shm_region_is_size_valid(size)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    shm_region_reset(out_region);

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == EEXIST) {
            return OFFSET_STORE_STATUS_ALREADY_EXISTS;
        }

        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (ftruncate(fd, (off_t) size) != 0) {
        close(fd);
        shm_unlink(name);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!shm_region_map_fd(out_region, fd, size, true)) {
        close(fd);
        shm_unlink(name);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
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
    status = shm_region_init_mutex(header);
    if (status != OFFSET_STORE_STATUS_OK) {
        shm_region_close(out_region);
        shm_unlink(name);
        return status;
    }
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Opens and validates an existing shared-memory region.
 *
 * @param out_region Region descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_open(ShmRegion *out_region, const char *name)
{
    int fd;
    struct stat st;

    if (out_region == NULL || name == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    shm_region_reset(out_region);

    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            return OFFSET_STORE_STATUS_NOT_FOUND;
        }

        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!shm_region_map_fd(out_region, fd, (size_t) st.st_size, false)) {
        close(fd);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!shm_region_validate_header(out_region)) {
        shm_region_close(out_region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Unmaps and closes a process-local shared-memory mapping.
 *
 * @param region Region descriptor to close.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_close(ShmRegion *region)
{
    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (region->base != NULL && munmap(region->base, region->size) != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (region->fd >= 0 && close(region->fd) != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    shm_region_reset(region);
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Removes a shared-memory object from the system namespace.
 *
 * @param name POSIX shared-memory object name.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_unlink(const char *name)
{
    if (name == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (shm_unlink(name) != 0) {
        if (errno == ENOENT) {
            return OFFSET_STORE_STATUS_NOT_FOUND;
        }

        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Acquires the region's shared mutex.
 *
 * @param region Region descriptor whose mutex should be locked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_lock(ShmRegion *region)
{
    ShmRegionHeader *header;

    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if (pthread_mutex_lock(&header->mutex) != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Releases the region's shared mutex.
 *
 * @param region Region descriptor whose mutex should be unlocked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_unlock(ShmRegion *region)
{
    ShmRegionHeader *header;

    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if (pthread_mutex_unlock(&header->mutex) != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the size of the private region header.
 *
 * @return Header size in bytes.
 */
size_t shm_region_header_size(void)
{
    return sizeof(ShmRegionHeader);
}

/**
 * @brief Returns the total size recorded in the shared region header.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_total_size Total size in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_total_size(const ShmRegion *region, uint64_t *out_total_size)
{
    const ShmRegionHeader *header;

    if (region == NULL || out_total_size == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_total_size = header->total_size;
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the region layout version recorded in shared metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_version Version value on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_version(const ShmRegion *region, uint32_t *out_version)
{
    const ShmRegionHeader *header;

    if (region == NULL || out_version == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_version = header->version;
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the first usable byte after the private region header.
 *
 * @param region Region descriptor to inspect.
 * @return Pointer to usable region data, or `NULL` on failure.
 */
void *shm_region_data(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < sizeof(ShmRegionHeader)) {
        return NULL;
    }

    /* Callers place allocator metadata and objects after the fixed header. */
    return (void *) ((unsigned char *) region->base + sizeof(ShmRegionHeader));
}

/**
 * @brief Returns the number of usable bytes after the private region header.
 *
 * @param region Region descriptor to inspect.
 * @return Usable byte count, or zero on failure.
 */
size_t shm_region_usable_size(const ShmRegion *region)
{
    if (region == NULL || region->size < sizeof(ShmRegionHeader)) {
        return 0;
    }

    return region->size - sizeof(ShmRegionHeader);
}
