#include "offset_store/store.h"

static void offset_store_reset(OffsetStore *store)
{
    if (store == NULL) {
        return;
    }

    store->region.fd = -1;
    store->region.base = NULL;
    store->region.size = 0;
    store->region.creator = false;
}

OffsetStoreStatus offset_store_bootstrap(OffsetStore *store, const char *name, size_t size)
{
    OffsetStoreStatus status;

    if (store == NULL || name == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    offset_store_reset(store);

    status = shm_region_create(&store->region, name, size);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    status = allocator_init(&store->region);
    if (status != OFFSET_STORE_STATUS_OK) {
        shm_region_close(&store->region);
        shm_region_unlink(name);
        return status;
    }

    return OFFSET_STORE_STATUS_OK;
}

OffsetStoreStatus offset_store_open_existing(OffsetStore *store, const char *name)
{
    OffsetStoreStatus status;

    if (store == NULL || name == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    offset_store_reset(store);

    status = shm_region_open(&store->region, name);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    status = allocator_validate(&store->region);
    if (status != OFFSET_STORE_STATUS_OK) {
        shm_region_close(&store->region);
        return status;
    }

    return OFFSET_STORE_STATUS_OK;
}

OffsetStoreStatus offset_store_close(OffsetStore *store)
{
    if (store == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    return shm_region_close(&store->region);
}
