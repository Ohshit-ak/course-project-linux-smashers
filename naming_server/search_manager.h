#ifndef SEARCH_MANAGER_H
#define SEARCH_MANAGER_H

#include <time.h>
#include <pthread.h>
#include "../common/protocol.h"
#include "file_manager.h"
#include "access_control.h"

#define SEARCH_CACHE_SIZE 50

// Search cache entry structure for efficient repeated searches
typedef struct SearchCacheEntry {
    char query[MAX_FILENAME];
    char results[MAX_DATA];
    time_t timestamp;
    struct SearchCacheEntry *next;
} SearchCacheEntry;

// Function declarations
void init_search_cache();
char* search_files(const char *pattern, const char *username);
void cache_search_result(const char *query, const char *results);
char* get_cached_search(const char *query);
void invalidate_search_cache();
void cleanup_search_cache();

// External global variables
extern SearchCacheEntry *search_cache;
extern int search_cache_count;
extern pthread_mutex_t cache_lock;

#endif // SEARCH_MANAGER_H
