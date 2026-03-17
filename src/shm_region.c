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
 * @brief Private fixed-size root table entry stored in the shared region header.
 *
 * Root names are stored inline so discovery remains deterministic and does not
 * require allocator recursion during early attach flows.
 */
typedef struct {
    /** Whether this entry currently stores a valid root binding. */
    uint8_t in_use;
    /** Reserved padding for future entry flags. */
    uint8_t reserved[7];
    /** Null-terminated root name stored inline in the shared header. */
    char name[OFFSET_STORE_ROOT_NAME_LENGTH];
    /** Offset handle associated with the root name. */
    OffsetPtr object;
} ShmRegionRootEntry;

/**
 * @brief Private fixed-size index entry stored in the shared region header.
 *
 * Index keys are also stored inline so the directory remains deterministic and
 * independent of allocator-managed dynamic storage.
 */
typedef struct {
    /** Whether this entry currently stores a valid index binding. */
    uint8_t in_use;
    /** Reserved padding for future entry flags. */
    uint8_t reserved[7];
    /** Null-terminated index key stored inline in the shared header. */
    char key[OFFSET_STORE_INDEX_KEY_LENGTH];
    /** Offset handle associated with the index key. */
    OffsetPtr object;
} ShmRegionIndexEntry;

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
    /** Fixed-capacity root table for well-known shared objects. */
    ShmRegionRootEntry roots[OFFSET_STORE_ROOT_CAPACITY];
    /** Fixed-capacity index table for general shared object discovery. */
    ShmRegionIndexEntry index[OFFSET_STORE_INDEX_CAPACITY];
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
 * @brief Validates the private shared region header for an attached mapping.
 *
 * @param region Region descriptor to validate.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_validate(const ShmRegion *region)
{
    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    return shm_region_validate_header(region)
        ? OFFSET_STORE_STATUS_OK
        : OFFSET_STORE_STATUS_INVALID_STATE;
}

/**
 * @brief Returns whether a candidate root name fits in the fixed root table.
 *
 * @param name Root name to validate.
 * @return true if the name is non-empty and fits in the fixed inline buffer.
 * @return false otherwise.
 */
static bool shm_region_is_root_name_valid(const char *name)
{
    size_t length;

    if (name == NULL) {
        return false;
    }

    length = strnlen(name, OFFSET_STORE_ROOT_NAME_LENGTH);
    if (length == 0 || length >= OFFSET_STORE_ROOT_NAME_LENGTH) {
        return false;
    }

    return true;
}

/**
 * @brief Returns whether a candidate index key fits in the fixed index table.
 *
 * @param key Index key to validate.
 * @return true if the key is non-empty and fits in the fixed inline buffer.
 * @return false otherwise.
 */
static bool shm_region_is_index_key_valid(const char *key)
{
    size_t length;

    if (key == NULL) {
        return false;
    }

    length = strnlen(key, OFFSET_STORE_INDEX_KEY_LENGTH);
    if (length == 0 || length >= OFFSET_STORE_INDEX_KEY_LENGTH) {
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
 * @brief Returns the fixed root entry array stored in the private header.
 *
 * @param header Shared region header to inspect.
 * @return Pointer to the first root entry, or `NULL` on failure.
 */
static ShmRegionRootEntry *shm_region_roots(ShmRegionHeader *header)
{
    if (header == NULL) {
        return NULL;
    }

    return header->roots;
}

/**
 * @brief Returns the fixed index entry array stored in the private header.
 *
 * @param header Shared region header to inspect.
 * @return Pointer to the first index entry, or `NULL` on failure.
 */
static ShmRegionIndexEntry *shm_region_index(ShmRegionHeader *header)
{
    if (header == NULL) {
        return NULL;
    }

    return header->index;
}

/**
 * @brief Finds an existing root entry by name.
 *
 * @param header Shared region header to inspect.
 * @param name Root name to search for.
 * @return Matching root entry, or `NULL` if the name is absent.
 */
static ShmRegionRootEntry *shm_region_find_root(ShmRegionHeader *header, const char *name)
{
    size_t index;
    ShmRegionRootEntry *roots;

    if (header == NULL || name == NULL) {
        return NULL;
    }

    roots = shm_region_roots(header);
    for (index = 0; index < OFFSET_STORE_ROOT_CAPACITY; ++index) {
        if (roots[index].in_use != 0 && strcmp(roots[index].name, name) == 0) {
            return &roots[index];
        }
    }

    return NULL;
}

/**
 * @brief Finds the first unused root-table slot.
 *
 * @param header Shared region header to inspect.
 * @return Unused root entry, or `NULL` if the table is full.
 */
static ShmRegionRootEntry *shm_region_find_free_root(ShmRegionHeader *header)
{
    size_t index;
    ShmRegionRootEntry *roots;

    if (header == NULL) {
        return NULL;
    }

    roots = shm_region_roots(header);
    for (index = 0; index < OFFSET_STORE_ROOT_CAPACITY; ++index) {
        if (roots[index].in_use == 0) {
            return &roots[index];
        }
    }

    return NULL;
}

/**
 * @brief Finds an existing index entry by key.
 *
 * @param header Shared region header to inspect.
 * @param key Index key to search for.
 * @return Matching index entry, or `NULL` if the key is absent.
 */
static ShmRegionIndexEntry *shm_region_find_index_entry(ShmRegionHeader *header, const char *key)
{
    size_t index;
    ShmRegionIndexEntry *entries;

    if (header == NULL || key == NULL) {
        return NULL;
    }

    entries = shm_region_index(header);
    for (index = 0; index < OFFSET_STORE_INDEX_CAPACITY; ++index) {
        if (entries[index].in_use != 0 && strcmp(entries[index].key, key) == 0) {
            return &entries[index];
        }
    }

    return NULL;
}

/**
 * @brief Finds the first unused index-table slot.
 *
 * @param header Shared region header to inspect.
 * @return Unused index entry, or `NULL` if the table is full.
 */
static ShmRegionIndexEntry *shm_region_find_free_index_entry(ShmRegionHeader *header)
{
    size_t index;
    ShmRegionIndexEntry *entries;

    if (header == NULL) {
        return NULL;
    }

    entries = shm_region_index(header);
    for (index = 0; index < OFFSET_STORE_INDEX_CAPACITY; ++index) {
        if (entries[index].in_use == 0) {
            return &entries[index];
        }
    }

    return NULL;
}

/**
 * @brief Clears the fixed root table stored in a new shared region.
 *
 * @param header Shared region header to initialize.
 */
static void shm_region_init_roots(ShmRegionHeader *header)
{
    size_t index;
    ShmRegionRootEntry *roots;

    if (header == NULL) {
        return;
    }

    roots = shm_region_roots(header);
    for (index = 0; index < OFFSET_STORE_ROOT_CAPACITY; ++index) {
        roots[index].in_use = 0;
        memset(roots[index].reserved, 0, sizeof(roots[index].reserved));
        memset(roots[index].name, 0, sizeof(roots[index].name));
        roots[index].object = offset_ptr_null();
    }
}

/**
 * @brief Clears the fixed index table stored in a new shared region.
 *
 * @param header Shared region header to initialize.
 */
static void shm_region_init_index(ShmRegionHeader *header)
{
    size_t index;
    ShmRegionIndexEntry *entries;

    if (header == NULL) {
        return;
    }

    entries = shm_region_index(header);
    for (index = 0; index < OFFSET_STORE_INDEX_CAPACITY; ++index) {
        entries[index].in_use = 0;
        memset(entries[index].reserved, 0, sizeof(entries[index].reserved));
        memset(entries[index].key, 0, sizeof(entries[index].key));
        entries[index].object = offset_ptr_null();
    }
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
    shm_region_init_roots(header);
    shm_region_init_index(header);
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
    return (void *) shm_region_data_const(region);
}

/**
 * @brief Returns the first usable byte after the private region header for read-only callers.
 *
 * @param region Region descriptor to inspect.
 * @return Const pointer to usable region data, or `NULL` on failure.
 */
const void *shm_region_data_const(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < sizeof(ShmRegionHeader)) {
        return NULL;
    }

    /* Callers place allocator metadata and objects after the fixed header. */
    return (const void *) ((const unsigned char *) region->base + sizeof(ShmRegionHeader));
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

/**
 * @brief Stores or replaces a named root in the shared region header.
 *
 * @param region Region descriptor whose root table should be updated.
 * @param name Root name to create or replace.
 * @param object Object handle stored for the root.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_set_root(ShmRegion *region, const char *name, OffsetPtr object)
{
    OffsetStoreStatus status;
    ShmRegionHeader *header;
    ShmRegionRootEntry *entry;

    if (region == NULL || !shm_region_is_root_name_valid(name) || offset_ptr_is_null(object)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_root(header, name);
    if (entry == NULL) {
        entry = shm_region_find_free_root(header);
    }

    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
    }

    entry->in_use = 1;
    memset(entry->reserved, 0, sizeof(entry->reserved));
    memset(entry->name, 0, sizeof(entry->name));
    memcpy(entry->name, name, strlen(name));
    entry->object = object;

    return shm_region_unlock(region);
}

/**
 * @brief Resolves a named root from the shared region header.
 *
 * @param region Region descriptor whose root table should be queried.
 * @param name Root name to resolve.
 * @param[out] out_object Stored root object handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_root(ShmRegion *region, const char *name, OffsetPtr *out_object)
{
    OffsetStoreStatus status;
    const ShmRegionHeader *header;
    const ShmRegionRootEntry *entry;

    if (region == NULL || !shm_region_is_root_name_valid(name) || out_object == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_root((ShmRegionHeader *) header, name);
    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    *out_object = entry->object;
    return shm_region_unlock(region);
}

/**
 * @brief Removes a named root from the shared region header.
 *
 * @param region Region descriptor whose root table should be updated.
 * @param name Root name to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_remove_root(ShmRegion *region, const char *name)
{
    OffsetStoreStatus status;
    ShmRegionHeader *header;
    ShmRegionRootEntry *entry;

    if (region == NULL || !shm_region_is_root_name_valid(name)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_root(header, name);
    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    entry->in_use = 0;
    memset(entry->reserved, 0, sizeof(entry->reserved));
    memset(entry->name, 0, sizeof(entry->name));
    entry->object = offset_ptr_null();

    return shm_region_unlock(region);
}

/**
 * @brief Stores or replaces an indexed entry in the shared region header.
 *
 * @param region Region descriptor whose index should be updated.
 * @param key Index key to create or replace.
 * @param object Object handle stored for the key.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_put(ShmRegion *region, const char *key, OffsetPtr object)
{
    OffsetStoreStatus status;
    ShmRegionHeader *header;
    ShmRegionIndexEntry *entry;

    if (region == NULL || !shm_region_is_index_key_valid(key) || offset_ptr_is_null(object)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_index_entry(header, key);
    if (entry == NULL) {
        entry = shm_region_find_free_index_entry(header);
    }

    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
    }

    entry->in_use = 1;
    memset(entry->reserved, 0, sizeof(entry->reserved));
    memset(entry->key, 0, sizeof(entry->key));
    memcpy(entry->key, key, strlen(key));
    entry->object = object;

    return shm_region_unlock(region);
}

/**
 * @brief Resolves an indexed entry from the shared region header.
 *
 * @param region Region descriptor whose index should be queried.
 * @param key Index key to resolve.
 * @param[out] out_object Stored handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_get(ShmRegion *region, const char *key, OffsetPtr *out_object)
{
    OffsetStoreStatus status;
    const ShmRegionHeader *header;
    const ShmRegionIndexEntry *entry;

    if (region == NULL || !shm_region_is_index_key_valid(key) || out_object == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_index_entry((ShmRegionHeader *) header, key);
    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    *out_object = entry->object;
    return shm_region_unlock(region);
}

/**
 * @brief Returns whether an indexed entry exists in the shared region header.
 *
 * @param region Region descriptor whose index should be queried.
 * @param key Index key to test.
 * @param[out] out_contains `true` if the key is present.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_contains(ShmRegion *region, const char *key, bool *out_contains)
{
    OffsetStoreStatus status;
    const ShmRegionHeader *header;

    if (region == NULL || !shm_region_is_index_key_valid(key) || out_contains == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    *out_contains = shm_region_find_index_entry((ShmRegionHeader *) header, key) != NULL;
    return shm_region_unlock(region);
}

/**
 * @brief Removes an indexed entry from the shared region header.
 *
 * @param region Region descriptor whose index should be updated.
 * @param key Index key to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_remove(ShmRegion *region, const char *key)
{
    OffsetStoreStatus status;
    ShmRegionHeader *header;
    ShmRegionIndexEntry *entry;

    if (region == NULL || !shm_region_is_index_key_valid(key)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = shm_region_header_mut(region);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = shm_region_lock(region);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    entry = shm_region_find_index_entry(header, key);
    if (entry == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    entry->in_use = 0;
    memset(entry->reserved, 0, sizeof(entry->reserved));
    memset(entry->key, 0, sizeof(entry->key));
    entry->object = offset_ptr_null();

    return shm_region_unlock(region);
}
