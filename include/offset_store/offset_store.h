#ifndef OFFSET_STORE_OFFSET_STORE_H
#define OFFSET_STORE_OFFSET_STORE_H

/*
 * Public-facing APIs use explicit library-owned status codes so callers do not
 * need to infer failure causes from a boolean result plus ambient errno state.
 */
typedef enum {
    OFFSET_STORE_STATUS_OK = 0,
    OFFSET_STORE_STATUS_INVALID_ARGUMENT,
    OFFSET_STORE_STATUS_INVALID_STATE,
    OFFSET_STORE_STATUS_NOT_FOUND,
    OFFSET_STORE_STATUS_ALREADY_EXISTS,
    OFFSET_STORE_STATUS_OUT_OF_MEMORY,
    OFFSET_STORE_STATUS_SYSTEM_ERROR
} OffsetStoreStatus;

const char *offset_store_status_string(OffsetStoreStatus status);

#endif
