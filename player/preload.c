/*
 * Lightweight preload implementation using demux layer directly.
 * 
 * Each preload entry creates its own minimal mpv_global context,
 * opens a demuxer, and starts prefetching. The demuxer can be
 * handed off to a player even while still loading.
 */

#include <pthread.h>
#include <string.h>
#include <time.h>

#include "preload.h"

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "common/stats.h"
#include "demux/demux.h"
#include "stream/stream.h"
#include "demux/packet_pool.h"
#include "options/m_config_core.h"
#include "options/options.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "misc/dispatch.h"
#include "misc/thread_tools.h"
#include "stream/stream.h"

// Preload entry
struct preload_entry {
    char *url;
    struct mpv_global *global;      // Independent global context
    struct demuxer *demuxer;
    struct mp_cancel *cancel;
    mpv_preload_status status;
    
    mp_thread thread;
    bool thread_running;
    bool cancel_requested;
    
    // Configuration
    int64_t max_bytes;
    double readahead_secs;
    
    time_t create_time;
    
    // Persistent storage for async callback
    mpv_preload_info callback_info;
};

// Forward declarations (after struct definition)
static void *preload_thread(void *arg);
static void cleanup_entry(struct preload_entry *entry);

// Global cache
static struct {
    struct preload_entry entries[MPV_PRELOAD_MAX_ENTRIES];
    pthread_mutex_t lock;
    bool initialized;
} preload_cache;

// Global callback for completion events
static mpv_preload_callback g_preload_callback = NULL;

void mpv_preload_set_callback(mpv_preload_callback callback)
{
    g_preload_callback = callback;
}

// Helper to fill preload info from entry state
static void fill_preload_info(struct preload_entry *entry, mpv_preload_info *info)
{
    info->status = entry->status;
    info->fw_bytes = 0;
    info->total_bytes = 0;
    info->file_size = -1;
    info->buffered_secs = 0;
    info->eof_cached = false;
    
    // Fill in demuxer state if available
    if (entry->demuxer) {
        struct demux_reader_state state;
        demux_get_reader_state(entry->demuxer, &state);
        info->fw_bytes = state.fw_bytes;
        info->total_bytes = state.total_bytes;
        info->eof_cached = state.eof_cached;
        if (state.ts_info.duration >= 0)
            info->buffered_secs = state.ts_info.duration;
        // Get file size from stream
        if (entry->demuxer->stream)
            info->file_size = stream_get_size(entry->demuxer->stream);
    }
}

// Helper to invoke callback with current state
static void invoke_callback(struct preload_entry *entry)
{
    if (!g_preload_callback)
        return;
    
    // Use entry's persistent storage for callback info (avoids stack corruption with async callbacks)
    fill_preload_info(entry, &entry->callback_info);
    
    printf("[preload] Callback: status=%lld, fw=%lld, total=%lld, eof=%d\n", 
           (long long)entry->callback_info.status, (long long)entry->callback_info.fw_bytes, 
           (long long)entry->callback_info.total_bytes, entry->callback_info.eof_cached);
    g_preload_callback(entry->url, &entry->callback_info);
}

// Initialize cache once
static void ensure_initialized(void)
{
    if (!preload_cache.initialized) {
        printf("[preload] Preload API v2.0 initialized\n");
        mp_time_init();  // Initialize timer subsystem
        pthread_mutex_init(&preload_cache.lock, NULL);
        preload_cache.initialized = true;
    }
}

// Find entry by URL (must hold lock)
static struct preload_entry *find_entry_locked(const char *url)
{
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        struct preload_entry *e = &preload_cache.entries[i];
        if (e->url && strcmp(e->url, url) == 0)
            return e;
    }
    return NULL;
}

// Find free slot (must hold lock)
static struct preload_entry *find_free_slot_locked(void)
{
    // First, try to find an empty slot
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        if (!preload_cache.entries[i].url)
            return &preload_cache.entries[i];
    }
    
    // If full, evict oldest entry
    struct preload_entry *oldest = NULL;
    time_t oldest_time = 0;
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        struct preload_entry *e = &preload_cache.entries[i];
        if (!oldest || e->create_time < oldest_time) {
            oldest = e;
            oldest_time = e->create_time;
        }
    }
    
    if (oldest) {
        cleanup_entry(oldest);
    }
    
    return oldest;
}

// Create minimal mpv_global for demux operations
static struct mpv_global *create_minimal_global(int64_t max_bytes, double readahead_secs)
{
    struct mpv_global *global = talloc_zero(NULL, struct mpv_global);
    
    // Create silent log
    global->log = mp_null_log;
    
    // Create config with MPV options
    global->config = m_config_shadow_new(&mp_opt_root);
    
    // Note: NOT initializing stats - stats_ctx_create now handles NULL stats
    // This allows simple talloc_steal cleanup when demuxer is freed
    
    // Initialize packet pool (required by demuxer for packet allocation)
    demux_packet_pool_init(global);
    
    // Set demux options via config cache
    struct m_config_cache *cache = m_config_cache_from_shadow(global, global->config, &demux_conf);
    if (cache) {
        struct demux_opts *opts = cache->opts;
        if (max_bytes > 0)
            opts->max_bytes = max_bytes;
        if (readahead_secs > 0)
            opts->min_secs = readahead_secs;
        m_config_cache_write_opt(cache, &opts->max_bytes);
        m_config_cache_write_opt(cache, &opts->min_secs);
        talloc_free(cache);
    }
    
    return global;
}

// Cleanup an entry (must NOT hold lock, or call with lock and handle thread join)
static void cleanup_entry(struct preload_entry *entry)
{
    if (!entry->url)
        return;
    
    entry->cancel_requested = true;
    
    if (entry->cancel)
        mp_cancel_trigger(entry->cancel);
    
    if (entry->thread_running) {
        mp_thread_join(entry->thread);
        entry->thread_running = false;
    }
    
    if (entry->demuxer) {
        demux_cancel_and_free(entry->demuxer);
        entry->demuxer = NULL;
    }
    
    if (entry->cancel) {
        talloc_free(entry->cancel);
        entry->cancel = NULL;
    }
    
    if (entry->global) {
        talloc_free(entry->global);
        entry->global = NULL;
    }
    
    free(entry->url);
    memset(entry, 0, sizeof(*entry));
}

// Preload thread function
static void *preload_thread(void *arg)
{
    struct preload_entry *entry = arg;
    
    mp_thread_set_name("preload");
    
    // Create minimal global context
    entry->global = create_minimal_global(entry->max_bytes, entry->readahead_secs);
    if (!entry->global) {
        entry->status = MPV_PRELOAD_STATUS_ERROR;
        invoke_callback(entry);
        return NULL;
    }
    
    // Create cancel token
    entry->cancel = mp_cancel_new(NULL);
    
    // Set up demuxer params
    struct demuxer_params params = {
        .is_top_level = true,
        .stream_flags = STREAM_ORIGIN_NET,
    };
    
    // Open demuxer (this does network I/O)
    entry->demuxer = demux_open_url(entry->url, &params, entry->cancel, entry->global);
    
    if (!entry->demuxer) {
        entry->status = MPV_PRELOAD_STATUS_ERROR;
        invoke_callback(entry);
        return NULL;
    }
    
    // Select all video and audio streams for prefetching
    int num_streams = demux_get_num_stream(entry->demuxer);
    for (int i = 0; i < num_streams; i++) {
        struct sh_stream *sh = demux_get_stream(entry->demuxer, i);
        if (sh && (sh->type == STREAM_VIDEO || sh->type == STREAM_AUDIO)) {
            demuxer_select_track(entry->demuxer, sh, MP_NOPTS_VALUE, true);
        }
    }
    
    // Start demux thread for prefetching
    demux_start_thread(entry->demuxer);
    demux_start_prefetch(entry->demuxer);
    
    // Demuxer is now usable - mark as ready and invoke callback
    entry->status = MPV_PRELOAD_STATUS_READY;
    invoke_callback(entry);
    
    // Wait until user requests the demuxer or cancels
    // Check for cache target reached to trigger CACHED callback
    bool target_notified = false;
    while (!entry->cancel_requested && !mp_cancel_test(entry->cancel)) {
        // Check if cache target reached or entire file cached
        if (!target_notified && entry->demuxer) {
            struct demux_reader_state state;
            demux_get_reader_state(entry->demuxer, &state);
            // Trigger CACHED when: target bytes reached OR entire file cached
            if (state.fw_bytes >= entry->max_bytes || state.eof_cached) {
                entry->status = MPV_PRELOAD_STATUS_CACHED;
                invoke_callback(entry);
                target_notified = true;
            }
        }
        mp_cancel_wait(entry->cancel, 0.5);
    }
    
    return NULL;
}

int mpv_preload_start(const char *url, const mpv_preload_options *opts)
{
    if (!url || !url[0])
        return -1;
    
    ensure_initialized();
    
    pthread_mutex_lock(&preload_cache.lock);
    
    // Check if already preloading
    struct preload_entry *existing = find_entry_locked(url);
    if (existing) {
        pthread_mutex_unlock(&preload_cache.lock);
        return 0; // Already preloading
    }
    
    // Find a slot
    struct preload_entry *entry = find_free_slot_locked();
    if (!entry) {
        pthread_mutex_unlock(&preload_cache.lock);
        return -1;
    }
    
    // Initialize entry
    entry->url = strdup(url);
    entry->status = MPV_PRELOAD_STATUS_LOADING;
    entry->create_time = time(NULL);
    entry->cancel_requested = false;
    
    // Apply options
    entry->max_bytes = (opts && opts->max_bytes > 0) 
        ? opts->max_bytes 
        : (10 * 1024 * 1024);  // Default 10MB
    entry->readahead_secs = (opts && opts->readahead_secs > 0) 
        ? opts->readahead_secs 
        : 10.0;  // Default 10s
    
    // Start preload thread
    if (mp_thread_create(&entry->thread, preload_thread, entry) != 0) {
        free(entry->url);
        entry->url = NULL;
        entry->status = MPV_PRELOAD_STATUS_NONE;
        pthread_mutex_unlock(&preload_cache.lock);
        return -1;
    }
    
    entry->thread_running = true;
    
    pthread_mutex_unlock(&preload_cache.lock);
    return 0;
}

int mpv_preload_get_info(const char *url, mpv_preload_info *info)
{
    if (!url || !info || !preload_cache.initialized)
        return -1;
    
    memset(info, 0, sizeof(*info));
    
    pthread_mutex_lock(&preload_cache.lock);
    
    struct preload_entry *entry = find_entry_locked(url);
    if (!entry) {
        pthread_mutex_unlock(&preload_cache.lock);
        info->status = MPV_PRELOAD_STATUS_NONE;
        return -1;
    }
    
    fill_preload_info(entry, info);
    
    pthread_mutex_unlock(&preload_cache.lock);
    return 0;
}


struct demuxer *mpv_preload_get_demuxer(const char *url)
{
    if (!url || !preload_cache.initialized)
        return NULL;
    
    pthread_mutex_lock(&preload_cache.lock);
    
    struct preload_entry *entry = find_entry_locked(url);
    if (!entry || entry->status == MPV_PRELOAD_STATUS_NONE || 
        entry->status == MPV_PRELOAD_STATUS_ERROR || !entry->demuxer) {
        pthread_mutex_unlock(&preload_cache.lock);
        return NULL;
    }
    
    // Request preload thread to stop
    // DON'T trigger cancel - that would propagate to demuxer's child cancel
    // and stop network reads. The preload thread will exit on its own
    // within 0.5s (mp_cancel_wait timeout).
    entry->cancel_requested = true;
    
    // Wait for thread to finish (max 0.5s wait due to mp_cancel_wait timeout)
    if (entry->thread_running) {
        pthread_mutex_unlock(&preload_cache.lock);
        mp_thread_join(entry->thread);
        pthread_mutex_lock(&preload_cache.lock);
        entry->thread_running = false;
    }
    
    // Take ownership of demuxer
    struct demuxer *demux = entry->demuxer;
    entry->demuxer = NULL;  // Detach from entry
    
    // Transfer ownership of global and cancel to demuxer using talloc_steal.
    // Since we don't initialize stats in preload global (stats_ctx_create returns NULL),
    // there's no stats_ctx registered, so talloc_steal is safe now.
    // When demux_free() is called, demuxer's children are freed first, then
    // the demuxer destructor runs, which frees global and cancel.
    if (entry->global)
        talloc_steal(demux, entry->global);
    if (entry->cancel)
        talloc_steal(demux, entry->cancel);
    
    // Clear entry
    free(entry->url);
    entry->url = NULL;
    entry->status = MPV_PRELOAD_STATUS_NONE;
    entry->global = NULL;
    entry->cancel = NULL;
    
    pthread_mutex_unlock(&preload_cache.lock);
    return demux;
}

int mpv_preload_cancel(const char *url)
{
    if (!url || !preload_cache.initialized)
        return -1;
    
    pthread_mutex_lock(&preload_cache.lock);
    
    struct preload_entry *entry = find_entry_locked(url);
    if (!entry) {
        pthread_mutex_unlock(&preload_cache.lock);
        return -1;
    }
    
    entry->cancel_requested = true;
    if (entry->cancel)
        mp_cancel_trigger(entry->cancel);
    
    pthread_mutex_unlock(&preload_cache.lock);
    
    // Join thread outside lock
    if (entry->thread_running) {
        mp_thread_join(entry->thread);
        entry->thread_running = false;
    }
    
    pthread_mutex_lock(&preload_cache.lock);
    cleanup_entry(entry);
    pthread_mutex_unlock(&preload_cache.lock);
    
    return 0;
}

void mpv_preload_clear_all(void)
{
    if (!preload_cache.initialized)
        return;
    
    // First, request all to cancel
    pthread_mutex_lock(&preload_cache.lock);
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        struct preload_entry *entry = &preload_cache.entries[i];
        if (entry->url) {
            entry->cancel_requested = true;
            if (entry->cancel)
                mp_cancel_trigger(entry->cancel);
        }
    }
    pthread_mutex_unlock(&preload_cache.lock);
    
    // Join all threads
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        struct preload_entry *entry = &preload_cache.entries[i];
        if (entry->thread_running) {
            mp_thread_join(entry->thread);
            entry->thread_running = false;
        }
    }
    
    // Cleanup all
    pthread_mutex_lock(&preload_cache.lock);
    for (int i = 0; i < MPV_PRELOAD_MAX_ENTRIES; i++) {
        cleanup_entry(&preload_cache.entries[i]);
    }
    pthread_mutex_unlock(&preload_cache.lock);
}
