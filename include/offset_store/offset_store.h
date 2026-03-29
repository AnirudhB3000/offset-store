#ifndef OFFSET_STORE_OFFSET_STORE_H
#define OFFSET_STORE_OFFSET_STORE_H

/**
 * @file offset_store.h
 * @brief Common public status codes shared across the library API surface.
 */

/**
 * @brief Library-owned status codes used by the public API surface.
 *
 * Public-facing APIs use explicit library-owned status codes so callers do not
 * need to infer failure causes from a boolean result plus ambient errno state.
 */
typedef enum {
    /** Operation completed successfully. */
    OFFSET_STORE_STATUS_OK = 0,
    /** Caller supplied invalid input. */
    OFFSET_STORE_STATUS_INVALID_ARGUMENT,
    /** Shared-memory metadata or object state is inconsistent. */
    OFFSET_STORE_STATUS_INVALID_STATE,
    /** Requested shared resource or allocation was not found. */
    OFFSET_STORE_STATUS_NOT_FOUND,
    /** Requested create or initialization path already exists. */
    OFFSET_STORE_STATUS_ALREADY_EXISTS,
    /** Requested operation could not be satisfied due to insufficient memory. */
    OFFSET_STORE_STATUS_OUT_OF_MEMORY,
    /** An underlying system call or library call failed. */
    OFFSET_STORE_STATUS_SYSTEM_ERROR
} OffsetStoreStatus;

/**
 * @name Status Helpers
 * @{
 */

/**
 * @brief Returns a human-readable string for a public status code.
 *
 * @param status Status code to stringify.
 * @return Static string describing the status.
 */
const char *offset_store_status_string(OffsetStoreStatus status);

/** @} */

#endif
