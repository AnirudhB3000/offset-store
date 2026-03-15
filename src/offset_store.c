#include "offset_store/offset_store.h"

const char *offset_store_status_string(OffsetStoreStatus status)
{
    switch (status) {
    case OFFSET_STORE_STATUS_OK:
        return "ok";
    case OFFSET_STORE_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case OFFSET_STORE_STATUS_INVALID_STATE:
        return "invalid state";
    case OFFSET_STORE_STATUS_NOT_FOUND:
        return "not found";
    case OFFSET_STORE_STATUS_ALREADY_EXISTS:
        return "already exists";
    case OFFSET_STORE_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case OFFSET_STORE_STATUS_SYSTEM_ERROR:
        return "system error";
    default:
        return "unknown status";
    }
}
