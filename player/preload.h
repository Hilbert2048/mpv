/*
 * Internal preload header - includes public API and internal definitions
 */

#ifndef MP_PLAYER_PRELOAD_H
#define MP_PLAYER_PRELOAD_H

// Include public API
#include "mpv/preload.h"

// Forward declaration for internal use
struct demuxer;

/**
 * Get demuxer for a URL (internal use).
 * Can be called in LOADING or READY state.
 * Demuxer is removed from cache after this call.
 * Caller takes ownership.
 *
 * @param url URL to get demuxer for
 * @return demuxer or NULL if not found
 */
struct demuxer *mpv_preload_get_demuxer(const char *url);

#endif /* MP_PLAYER_PRELOAD_H */

