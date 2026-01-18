/* Copyright (C) 2017 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Preload API - lightweight video preloading
 *
 * This API allows preloading video data before playback. It creates
 * lightweight contexts that prefetch demuxer data, which are then
 * used automatically when playing the same URL.
 */

#ifndef MPV_PRELOAD_API_H_
#define MPV_PRELOAD_API_H_

#include <stdint.h>
#include <stdbool.h>

#include "client.h"  // For MPV_EXPORT

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum number of concurrent preload entries.
 */
#define MPV_PRELOAD_MAX_ENTRIES 4

/**
 * Preload options.
 */
typedef struct mpv_preload_options {
    int64_t max_bytes;      /**< Demuxer cache size in bytes (0 = default 10MB) */
    double readahead_secs;  /**< Readahead seconds (0 = default 10s) */
} mpv_preload_options;

/**
 * Preload status.
 */
typedef enum mpv_preload_status {
    MPV_PRELOAD_STATUS_NONE = 0,     /**< No preload for this URL */
    MPV_PRELOAD_STATUS_LOADING = 1,  /**< Demuxer opened, prefetch in progress */
    MPV_PRELOAD_STATUS_READY = 2,    /**< Prefetch target reached, usable but still caching */
    MPV_PRELOAD_STATUS_ERROR = 3,    /**< Failed to open */
    MPV_PRELOAD_STATUS_CACHED = 4,   /**< Entire file cached (eof_cached = true) */
} mpv_preload_status;

/**
 * Preload info structure.
 */
typedef struct mpv_preload_info {
    int64_t status;             /**< Current status (mpv_preload_status value) */
    int64_t fw_bytes;           /**< Forward cached bytes (from current position) */
    int64_t total_bytes;        /**< Total bytes in buffer */
    int64_t file_size;          /**< Total file size (-1 if unknown) */
    double buffered_secs;       /**< Duration buffered in seconds */
    bool eof_cached;            /**< True if entire file is cached */
} mpv_preload_info;

/**
 * Start preloading a URL.
 *
 * Creates an independent context and begins prefetching data.
 * The demuxer can be used even while still loading.
 *
 * @param url URL to preload
 * @param opts Options (NULL for defaults: 10MB cache, 10s readahead)
 * @return 0 on success, -1 on error
 */
MPV_EXPORT int mpv_preload_start(const char *url, const mpv_preload_options *opts);

/**
 * Get detailed preload info for a URL.
 *
 * @param url URL to query
 * @param info Output structure for info
 * @return 0 on success, -1 if not found
 */
MPV_EXPORT int mpv_preload_get_info(const char *url, mpv_preload_info *info);

/**
 * Cancel preload for a URL.
 *
 * Stops the preload and releases resources.
 *
 * @param url URL to cancel
 * @return 0 on success, -1 if not found
 */
MPV_EXPORT int mpv_preload_cancel(const char *url);

/**
 * Clear all preloads.
 *
 * Cancels all ongoing preloads and frees resources.
 */
MPV_EXPORT void mpv_preload_clear_all(void);

/**
 * Callback type for preload status events.
 *
 * Called when preload status changes (READY, CACHED, or ERROR).
 * Note: This callback is invoked from a background thread.
 *
 * @param url URL that was preloaded
 * @param info Pointer to mpv_preload_info with current state
 */
typedef void (*mpv_preload_callback)(const char *url, const mpv_preload_info *info);

/**
 * Set global callback for preload status events.
 *
 * Only one callback can be registered at a time.
 * Pass NULL to clear the callback.
 *
 * @param callback Callback function pointer
 */
MPV_EXPORT void mpv_preload_set_callback(mpv_preload_callback callback);

#ifdef __cplusplus
}
#endif

#endif /* MPV_PRELOAD_API_H_ */
