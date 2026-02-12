/*
 * Internal preload header - includes public API and internal definitions
 */

#ifndef MP_PLAYER_PRELOAD_H
#define MP_PLAYER_PRELOAD_H

// Include public API
#include "mpv/preload.h"

// Forward declaration for internal use
struct demuxer;
struct mp_cancel;

/**
 * Get demuxer for a URL (internal use).
 * Can be called in LOADING or READY state.
 * Demuxer is removed from cache after this call.
 * Caller takes ownership.
 *
 * @param url URL to get demuxer for
 * @return demuxer or NULL if not found
 */
struct demuxer *mpv_preload_get_demuxer(const char *url, struct mp_cancel *cancel);

#endif /* MP_PLAYER_PRELOAD_H */

