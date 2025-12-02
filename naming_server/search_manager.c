#include "search_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

SearchCacheEntry *search_cache = NULL;
int search_cache_count = 0;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

extern FileEntry *file_table[];
extern pthread_mutex_t table_lock;

// Initialize search cache
void init_search_cache() {
    search_cache = NULL;
    search_cache_count = 0;
}

// Add or update entry in search cache
void cache_search_result(const char *query, const char *results) {
    pthread_mutex_lock(&cache_lock);
    
    // Check if query already cached
    SearchCacheEntry *current = search_cache;
    while (current != NULL) {
        if (strcmp(current->query, query) == 0) {
            // Update existing cache entry
            strncpy(current->results, results, sizeof(current->results) - 1);
            current->timestamp = time(NULL);
            pthread_mutex_unlock(&cache_lock);
            return;
        }
        current = current->next;
    }
    
    // If cache is full, remove oldest entry (simple LRU)
    if (search_cache_count >= SEARCH_CACHE_SIZE) {
        SearchCacheEntry *oldest = search_cache;
        SearchCacheEntry *prev_oldest = NULL;
        SearchCacheEntry *prev = NULL;
        current = search_cache;
        
        time_t oldest_time = oldest->timestamp;
        
        while (current != NULL) {
            if (current->timestamp < oldest_time) {
                oldest_time = current->timestamp;
                oldest = current;
                prev_oldest = prev;
            }
            prev = current;
            current = current->next;
        }
        
        // Remove oldest
        if (prev_oldest == NULL) {
            search_cache = oldest->next;
        } else {
            prev_oldest->next = oldest->next;
        }
        free(oldest);
        search_cache_count--;
    }
    
    // Add new cache entry
    SearchCacheEntry *new_entry = malloc(sizeof(SearchCacheEntry));
    strncpy(new_entry->query, query, sizeof(new_entry->query) - 1);
    strncpy(new_entry->results, results, sizeof(new_entry->results) - 1);
    new_entry->timestamp = time(NULL);
    new_entry->next = search_cache;
    search_cache = new_entry;
    search_cache_count++;
    
    pthread_mutex_unlock(&cache_lock);
}

// Check cache for search query
char* get_cached_search(const char *query) {
    pthread_mutex_lock(&cache_lock);
    
    SearchCacheEntry *current = search_cache;
    while (current != NULL) {
        if (strcmp(current->query, query) == 0) {
            // Found in cache - update timestamp (LRU)
            current->timestamp = time(NULL);
            pthread_mutex_unlock(&cache_lock);
            return current->results;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&cache_lock);
    return NULL;  // Not in cache
}

// Invalidate search cache (call when files are added/deleted)
void invalidate_search_cache() {
    pthread_mutex_lock(&cache_lock);
    
    SearchCacheEntry *current = search_cache;
    while (current != NULL) {
        SearchCacheEntry *next = current->next;
        free(current);
        current = next;
    }
    
    search_cache = NULL;
    search_cache_count = 0;
    
    pthread_mutex_unlock(&cache_lock);
}

// Perform file search with pattern matching and caching
char* search_files(const char *pattern, const char *username) {
    // Check cache first
    char *cached = get_cached_search(pattern);
    if (cached != NULL) {
        printf("  → Cache hit for search: '%s'\n", pattern);
        return cached;
    }
    
    printf("  → Cache miss, performing search for: '%s'\n", pattern);
    
    static char results[MAX_DATA];
    memset(results, 0, sizeof(results));
    int match_count = 0;
    
    pthread_mutex_lock(&table_lock);
    
    // Search through all files in hash table
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *entry = file_table[i];
        while (entry != NULL) {
            // Check if filename matches pattern
            int matches = 0;
            
            // Simple pattern matching:
            // - Exact match
            // - Contains substring (case-insensitive)
            // - Wildcard pattern (* and ?)
            
            if (strcmp(entry->info.name, pattern) == 0) {
                matches = 1;  // Exact match
            } else if (strstr(entry->info.name, pattern) != NULL) {
                matches = 1;  // Contains substring
            } else {
                // Try case-insensitive match
                char lower_name[MAX_FILENAME];
                char lower_pattern[MAX_FILENAME];
                
                for (int j = 0; entry->info.name[j] && j < MAX_FILENAME - 1; j++) {
                    lower_name[j] = tolower(entry->info.name[j]);
                }
                lower_name[strlen(entry->info.name)] = '\0';
                
                for (int j = 0; pattern[j] && j < MAX_FILENAME - 1; j++) {
                    lower_pattern[j] = tolower(pattern[j]);
                }
                lower_pattern[strlen(pattern)] = '\0';
                
                if (strstr(lower_name, lower_pattern) != NULL) {
                    matches = 1;  // Case-insensitive match
                }
            }
            
            // If matches and user has read permission, add to results
            if (matches && check_permission(entry, username, 0)) {
                if (match_count > 0) {
                    strncat(results, "\n", sizeof(results) - strlen(results) - 1);
                }
                
                char entry_line[512];
                snprintf(entry_line, sizeof(entry_line), "  %s (owner: %s, server: %s)",
                         entry->info.name, entry->info.owner, entry->info.storage_server_id);
                strncat(results, entry_line, sizeof(results) - strlen(results) - 1);
                match_count++;
            }
            
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&table_lock);
    
    // Format results
    if (match_count == 0) {
        snprintf(results, sizeof(results), "No files found matching '%s'", pattern);
    } else {
        char header[256];
        snprintf(header, sizeof(header), "Found %d file(s) matching '%s':\n", match_count, pattern);
        
        // Insert header at beginning
        char temp[MAX_DATA];
        strncpy(temp, results, sizeof(temp) - 1);
        snprintf(results, sizeof(results), "%s%s", header, temp);
    }
    
    // Cache the results
    cache_search_result(pattern, results);
    
    return results;
}

// Cleanup search cache (call on shutdown)
void cleanup_search_cache() {
    invalidate_search_cache();
}
