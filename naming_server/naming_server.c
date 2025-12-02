/*
 * Naming Server - Main Entry Point
 * 
 * The Naming Server is the central coordinator of the distributed file system.
 * It maintains:
 * - File metadata and locations
 * - Storage server registry
 * - User access control lists
 * - Efficient file lookup using hash tables
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "../common/protocol.h"
#include "../common/utils.h"

#define NS_PORT 8080
#define MAX_CLIENTS 100
#define HASH_TABLE_SIZE 1024
#define SEARCH_CACHE_SIZE 50

// Search cache entry structure for efficient repeated searches
typedef struct SearchCacheEntry {
    char query[MAX_FILENAME];
    char results[MAX_DATA];  // Cached search results
    time_t timestamp;
    struct SearchCacheEntry *next;
} SearchCacheEntry;

// File metadata structure
typedef struct FileEntry {
    struct FileInfo info;
    struct AccessControl *acl;
    struct CheckpointEntry *checkpoints;  // Linked list of checkpoints
    struct AccessRequestNode *access_requests;  // Linked list of access requests
    struct FileEntry *next;  // For hash table chaining
} FileEntry;

// Access control entry
typedef struct AccessControl {
    char username[MAX_USERNAME];
    int can_read;
    int can_write;
    struct AccessControl *next;
} AccessControl;

// Storage server structure
typedef struct StorageServer {
    char id[64];
    char ip[16];
    int nm_port;
    int client_port;
    int ss_socket;  // Persistent connection socket
    int is_active;
    time_t last_heartbeat;
    int failed;  // 1 if detected as failed
    struct StorageServer *next;
} StorageServer;

// User registry structure
typedef struct UserEntry {
    char username[MAX_USERNAME];
    time_t registered_at;
    struct UserEntry *next;
} UserEntry;

// Active session structure
typedef struct ActiveSession {
    char username[MAX_USERNAME];
    int client_socket;
    char client_ip[16];
    time_t login_time;
    struct ActiveSession *next;
} ActiveSession;

// Folder structure
typedef struct FolderEntry {
    char name[MAX_FILENAME];        // Folder path (e.g., "docs" or "docs/photos")
    char owner[MAX_USERNAME];       // Creator of the folder
    time_t created_at;
    struct FolderEntry *next;
} FolderEntry;

// Checkpoint structure
typedef struct CheckpointEntry {
    char tag[MAX_FILENAME];
    char creator[MAX_USERNAME];
    time_t created_at;
    long size;
    struct CheckpointEntry *next;
} CheckpointEntry;

// Access request structure (internal linked list node)
typedef struct AccessRequestNode {
    int request_id;
    char requester[MAX_USERNAME];
    int access_type;  // 1=read, 2=write, 3=both
    time_t requested_at;
    int status;  // 0=pending, 1=approved, 2=denied
    struct AccessRequestNode *next;
} AccessRequestNode;

// Global state
FileEntry *file_table[HASH_TABLE_SIZE];
StorageServer *storage_servers = NULL;
UserEntry *registered_users = NULL;
SearchCacheEntry *search_cache = NULL;
FolderEntry *folders = NULL;        // List of all folders
ActiveSession *active_sessions = NULL;  // List of active user sessions
int search_cache_count = 0;
int next_request_id = 1;
volatile int shutdown_flag = 0;  // Set to 1 when NS needs to shutdown
pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t user_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t folder_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

// Hash function for file lookup
unsigned int hash_function(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_TABLE_SIZE;
}

// Add file to hash table
void add_file(struct FileInfo *info, const char *ss_id) {
    unsigned int index = hash_function(info->name);
    
    pthread_mutex_lock(&table_lock);
    
    FileEntry *entry = malloc(sizeof(FileEntry));
    memcpy(&entry->info, info, sizeof(struct FileInfo));
    strncpy(entry->info.storage_server_id, ss_id, sizeof(entry->info.storage_server_id));
    entry->acl = NULL;
    entry->checkpoints = NULL;
    entry->access_requests = NULL;
    entry->next = file_table[index];
    file_table[index] = entry;
    
    pthread_mutex_unlock(&table_lock);
    
    log_message("naming_server", "Added file to registry");
}

// Lookup file in hash table
FileEntry* lookup_file(const char *filename) {
    unsigned int index = hash_function(filename);
    
    pthread_mutex_lock(&table_lock);
    
    FileEntry *current = file_table[index];
    while (current != NULL) {
        if (strcmp(current->info.name, filename) == 0) {
            pthread_mutex_unlock(&table_lock);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    return NULL;
}

// Check if user has permission
int check_permission(FileEntry *entry, const char *username, int need_write) {
    // Owner always has full access
    if (strcmp(entry->info.owner, username) == 0)
        return 1;
    
    // Check ACL
    AccessControl *acl = entry->acl;
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            if (need_write)
                return acl->can_write;
            else
                return acl->can_read;
        }
        acl = acl->next;
    }
    
    return 0;  // No access
}

// Add access control entry to a file
int add_access(FileEntry *entry, const char *username, int can_read, int can_write) {
    // Check if user already has access
    AccessControl *acl = entry->acl;
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            // Update existing access
            acl->can_read = can_read;
            acl->can_write = can_write;
            return 1;  // Updated
        }
        acl = acl->next;
    }
    
    // Add new ACL entry
    AccessControl *new_acl = malloc(sizeof(AccessControl));
    strncpy(new_acl->username, username, sizeof(new_acl->username));
    new_acl->can_read = can_read;
    new_acl->can_write = can_write;
    new_acl->next = entry->acl;
    entry->acl = new_acl;
    
    return 0;  // Added new
}

// Remove access control entry from a file
int remove_access(FileEntry *entry, const char *username) {
    AccessControl *acl = entry->acl;
    AccessControl *prev = NULL;
    
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            // Found the entry to remove
            if (prev == NULL) {
                entry->acl = acl->next;
            } else {
                prev->next = acl->next;
            }
            free(acl);
            return 1;  // Removed
        }
        prev = acl;
        acl = acl->next;
    }
    
    return 0;  // Not found
}

// ==================== SESSION MANAGEMENT FUNCTIONS ====================

// Check if user already has active session
ActiveSession* find_active_session(const char *username) {
    pthread_mutex_lock(&session_lock);
    
    ActiveSession *current = active_sessions;
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&session_lock);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&session_lock);
    return NULL;
}

// Add active session
int add_active_session(const char *username, int client_socket, const char *client_ip) {
    pthread_mutex_lock(&session_lock);
    
    // Check if user already has active session
    ActiveSession *existing = active_sessions;
    while (existing != NULL) {
        if (strcmp(existing->username, username) == 0) {
            pthread_mutex_unlock(&session_lock);
            return 0;  // Session already exists
        }
        existing = existing->next;
    }
    
    // Create new session
    ActiveSession *new_session = malloc(sizeof(ActiveSession));
    strncpy(new_session->username, username, sizeof(new_session->username));
    new_session->client_socket = client_socket;
    strncpy(new_session->client_ip, client_ip, sizeof(new_session->client_ip));
    new_session->login_time = time(NULL);
    new_session->next = active_sessions;
    active_sessions = new_session;
    
    pthread_mutex_unlock(&session_lock);
    return 1;  // Session added successfully
}

// Remove active session
void remove_active_session(const char *username) {
    pthread_mutex_lock(&session_lock);
    
    ActiveSession *current = active_sessions;
    ActiveSession *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            if (prev == NULL) {
                active_sessions = current->next;
            } else {
                prev->next = current->next;
            }
            
            printf("✓ Session ended for user: %s\n", username);
            free(current);
            pthread_mutex_unlock(&session_lock);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&session_lock);
}

// Register user if not already registered
void register_user(const char *username) {
    pthread_mutex_lock(&user_lock);
    
    // Check if user already exists
    UserEntry *current = registered_users;
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&user_lock);
            return;  // User already registered
        }
        current = current->next;
    }
    
    // Add new user
    UserEntry *new_user = malloc(sizeof(UserEntry));
    strncpy(new_user->username, username, sizeof(new_user->username));
    new_user->registered_at = time(NULL);
    new_user->next = registered_users;
    registered_users = new_user;
    
    pthread_mutex_unlock(&user_lock);
}

// Get all registered users
char* get_all_users() {
    pthread_mutex_lock(&user_lock);
    
    static char user_list[MAX_DATA * 2];
    memset(user_list, 0, sizeof(user_list));
    
    int count = 0;
    UserEntry *current = registered_users;
    
    while (current != NULL) {
        if (count > 0) {
            strncat(user_list, "\n", sizeof(user_list) - strlen(user_list) - 1);
        }
        strncat(user_list, current->username, sizeof(user_list) - strlen(user_list) - 1);
        count++;
        current = current->next;
    }
    
    pthread_mutex_unlock(&user_lock);
    
    if (count == 0) {
        strncpy(user_list, "(no users registered)", sizeof(user_list));
    }
    
    return user_list;
}

// ==================== FOLDER MANAGEMENT FUNCTIONS ====================

// Check if folder exists
int folder_exists(const char *folder_path) {
    pthread_mutex_lock(&folder_lock);
    FolderEntry *current = folders;
    while (current != NULL) {
        if (strcmp(current->name, folder_path) == 0) {
            pthread_mutex_unlock(&folder_lock);
            return 1;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&folder_lock);
    return 0;
}

// Create a new folder
int create_folder(const char *folder_path, const char *owner) {
    // Check if folder already exists
    if (folder_exists(folder_path)) {
        return ERR_FOLDER_EXISTS;
    }
    
    // Create parent folders if needed (e.g., "docs/photos" creates "docs" first)
    char parent_path[MAX_FILENAME] = "";
    char *last_slash = strrchr(folder_path, '/');
    if (last_slash != NULL) {
        size_t parent_len = last_slash - folder_path;
        strncpy(parent_path, folder_path, parent_len);
        parent_path[parent_len] = '\0';
        
        // Recursively create parent if it doesn't exist
        if (!folder_exists(parent_path) && strlen(parent_path) > 0) {
            int result = create_folder(parent_path, owner);
            if (result != RESP_SUCCESS) {
                return result;
            }
        }
    }
    
    pthread_mutex_lock(&folder_lock);
    
    FolderEntry *new_folder = malloc(sizeof(FolderEntry));
    strncpy(new_folder->name, folder_path, sizeof(new_folder->name) - 1);
    strncpy(new_folder->owner, owner, sizeof(new_folder->owner) - 1);
    new_folder->created_at = time(NULL);
    new_folder->next = folders;
    folders = new_folder;
    
    pthread_mutex_unlock(&folder_lock);
    
    printf("📁 Folder created: %s by %s\n", folder_path, owner);
    return RESP_SUCCESS;
}

// List all files in a folder
char* list_folder_files(const char *folder_path) {
    static char file_list[MAX_DATA];
    memset(file_list, 0, sizeof(file_list));
    
    pthread_mutex_lock(&table_lock);
    
    int count = 0;
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *current = file_table[i];
        while (current != NULL) {
            // Check if file is in this folder
            if (strcmp(current->info.folder, folder_path) == 0) {
                if (count > 0) {
                    strncat(file_list, "\n", sizeof(file_list) - strlen(file_list) - 1);
                }
                strncat(file_list, current->info.name, sizeof(file_list) - strlen(file_list) - 1);
                count++;
            }
            current = current->next;
        }
    }
    
    pthread_mutex_unlock(&table_lock);
    
    if (count == 0) {
        strncpy(file_list, "(empty folder)", sizeof(file_list));
    }
    
    return file_list;
}

// Move file to folder (caller must provide valid FileEntry)
int move_file_to_folder(FileEntry *entry, const char *folder_path) {
    if (entry == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    pthread_mutex_lock(&table_lock);
    
    // Update file's folder
    strncpy(entry->info.folder, folder_path, sizeof(entry->info.folder) - 1);
    entry->info.folder[sizeof(entry->info.folder) - 1] = '\0';
    
    pthread_mutex_unlock(&table_lock);
    return RESP_SUCCESS;
}

// ==================== SEARCH CACHE FUNCTIONS ====================
// These implement caching for efficient repeated searches

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

// ==================== STORAGE SERVER FUNCTIONS ====================

// Get first active storage server (simple selection)
StorageServer* get_available_ss() {
    StorageServer *ss = storage_servers;
    while (ss != NULL) {
        if (ss->is_active) {
            return ss;
        }
        ss = ss->next;
    }
    return NULL;
}

// Find storage server by ID
StorageServer* find_ss_by_id(const char *ss_id) {
    StorageServer *ss = storage_servers;
    while (ss != NULL) {
        if (strcmp(ss->id, ss_id) == 0) {
            return ss;
        }
        ss = ss->next;
    }
    return NULL;
}

// Register storage server
void register_storage_server(struct SSRegistration *reg) {
    StorageServer *ss = malloc(sizeof(StorageServer));
    strncpy(ss->id, reg->ss_id, sizeof(ss->id));
    strncpy(ss->ip, reg->ip, sizeof(ss->ip));
    ss->nm_port = reg->nm_port;
    ss->client_port = reg->client_port;
    ss->ss_socket = -1;  // Will be set after registration completes
    ss->is_active = 1;
    ss->last_heartbeat = time(NULL);
    ss->failed = 0;
    ss->next = storage_servers;
    storage_servers = ss;
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Registered Storage Server: %s at %s:%d", 
             ss->id, ss->ip, ss->client_port);
    log_message("naming_server", msg);
    printf("✓ Storage Server registered: %s (client port: %d)\n", ss->id, ss->client_port);
    
    // Register all files from this SS
    for (int i = 0; i < reg->file_count; i++) {
        struct FileInfo info;
        strncpy(info.name, reg->files[i], sizeof(info.name));
        strncpy(info.owner, "system", sizeof(info.owner));
        info.created_at = time(NULL);
        info.last_modified = time(NULL);
        info.last_accessed = time(NULL);
        info.size = 0;
        info.word_count = 0;
        info.char_count = 0;
        add_file(&info, ss->id);
    }
}

// ==================== FILE SEARCH FUNCTION ====================
// Performs efficient file search with pattern matching and caching

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

// ==================== CHECKPOINT MANAGEMENT FUNCTIONS ====================

// Add checkpoint to file
int add_checkpoint(FileEntry *entry, const char *tag, const char *creator) {
    pthread_mutex_lock(&table_lock);
    
    // Check if checkpoint with this tag already exists
    CheckpointEntry *cp = entry->checkpoints;
    while (cp != NULL) {
        if (strcmp(cp->tag, tag) == 0) {
            pthread_mutex_unlock(&table_lock);
            return -1;  // Checkpoint already exists
        }
        cp = cp->next;
    }
    
    // Create new checkpoint entry
    CheckpointEntry *new_cp = malloc(sizeof(CheckpointEntry));
    strncpy(new_cp->tag, tag, sizeof(new_cp->tag));
    strncpy(new_cp->creator, creator, sizeof(new_cp->creator));
    new_cp->created_at = time(NULL);
    new_cp->size = entry->info.size;
    new_cp->next = entry->checkpoints;
    entry->checkpoints = new_cp;
    
    pthread_mutex_unlock(&table_lock);
    return 0;
}

// Find checkpoint by tag
CheckpointEntry* find_checkpoint(FileEntry *entry, const char *tag) {
    pthread_mutex_lock(&table_lock);
    
    CheckpointEntry *cp = entry->checkpoints;
    while (cp != NULL) {
        if (strcmp(cp->tag, tag) == 0) {
            pthread_mutex_unlock(&table_lock);
            return cp;
        }
        cp = cp->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    return NULL;
}

// List all checkpoints for a file
char* list_checkpoints(FileEntry *entry) {
    static char results[MAX_DATA];
    memset(results, 0, sizeof(results));
    
    pthread_mutex_lock(&table_lock);
    
    CheckpointEntry *cp = entry->checkpoints;
    int count = 0;
    
    while (cp != NULL) {
        if (count > 0) {
            strncat(results, "\n", sizeof(results) - strlen(results) - 1);
        }
        
        char line[512];
        snprintf(line, sizeof(line), "  [%s] Created by %s at %s (size: %ld bytes)",
                 cp->tag, cp->creator, format_time(cp->created_at), cp->size);
        strncat(results, line, sizeof(results) - strlen(results) - 1);
        count++;
        cp = cp->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    
    if (count == 0) {
        snprintf(results, sizeof(results), "No checkpoints found for this file");
    } else {
        char header[256];
        snprintf(header, sizeof(header), "Checkpoints for '%s' (%d total):\n", entry->info.name, count);
        char temp[MAX_DATA];
        strncpy(temp, results, sizeof(temp) - 1);
        snprintf(results, sizeof(results), "%s%s", header, temp);
    }
    
    return results;
}

// ==================== ACCESS REQUEST MANAGEMENT FUNCTIONS ====================

// Add access request
int add_access_request(FileEntry *entry, const char *requester, int access_type) {
    pthread_mutex_lock(&request_lock);
    
    // Check if request already exists and is pending
    AccessRequestNode *req = entry->access_requests;
    while (req != NULL) {
        if (strcmp(req->requester, requester) == 0 && req->status == 0) {
            pthread_mutex_unlock(&request_lock);
            return -1;  // Request already pending
        }
        req = req->next;
    }
    
    // Create new request
    AccessRequestNode *new_req = malloc(sizeof(AccessRequestNode));
    new_req->request_id = next_request_id++;
    strncpy(new_req->requester, requester, sizeof(new_req->requester));
    new_req->access_type = access_type;
    new_req->requested_at = time(NULL);
    new_req->status = 0;  // Pending
    new_req->next = entry->access_requests;
    entry->access_requests = new_req;
    
    pthread_mutex_unlock(&request_lock);
    return new_req->request_id;
}

// List pending access requests for a file
char* list_access_requests(FileEntry *entry) {
    static char results[MAX_DATA];
    memset(results, 0, sizeof(results));
    
    pthread_mutex_lock(&request_lock);
    
    AccessRequestNode *req = entry->access_requests;
    int count = 0;
    
    while (req != NULL) {
        if (req->status == 0) {  // Only show pending requests
            if (count > 0) {
                strncat(results, "\n", sizeof(results) - strlen(results) - 1);
            }
            
            char line[512];
            const char *access_str = (req->access_type == 1) ? "Read" : 
                                    (req->access_type == 2) ? "Write" : "Read+Write";
            snprintf(line, sizeof(line), "  [ID:%d] %s requests %s access at %s",
                     req->request_id, req->requester, access_str, format_time(req->requested_at));
            strncat(results, line, sizeof(results) - strlen(results) - 1);
            count++;
        }
        req = req->next;
    }
    
    pthread_mutex_unlock(&request_lock);
    
    if (count == 0) {
        snprintf(results, sizeof(results), "No pending access requests");
    } else {
        char header[256];
        snprintf(header, sizeof(header), "Pending access requests for '%s' (%d total):\n", entry->info.name, count);
        char temp[MAX_DATA];
        strncpy(temp, results, sizeof(temp) - 1);
        snprintf(results, sizeof(results), "%s%s", header, temp);
    }
    
    return results;
}

// Find and respond to access request
int respond_to_request(FileEntry *entry, int request_id, int approve) {
    pthread_mutex_lock(&request_lock);
    
    AccessRequestNode *req = entry->access_requests;
    while (req != NULL) {
        if (req->request_id == request_id && req->status == 0) {
            req->status = approve ? 1 : 2;  // 1=approved, 2=denied
            
            // If approved, add to ACL
            if (approve) {
                int can_read = (req->access_type == 1 || req->access_type == 3);
                int can_write = (req->access_type == 2 || req->access_type == 3);
                add_access(entry, req->requester, can_read, can_write);
            }
            
            pthread_mutex_unlock(&request_lock);
            return 0;
        }
        req = req->next;
    }
    
    pthread_mutex_unlock(&request_lock);
    return -1;  // Request not found
}

// Handle client request
void* handle_client(void *arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    struct Message msg;
    char client_username[MAX_USERNAME] = "unknown";
    
    // Get client IP address
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    char client_ip[16];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    
    // First message should be registration
    if (recv_message(client_socket, &msg) > 0) {
        if (msg.type == MSG_REGISTER_CLIENT) {
            strncpy(client_username, msg.username, sizeof(client_username));
            
            // Check if user already has active session
            ActiveSession *existing_session = find_active_session(client_username);
            if (existing_session != NULL) {
                printf("✗ Login blocked: %s already logged in from %s\n", 
                       client_username, existing_session->client_ip);
                
                // Send error response
                msg.error_code = ERR_FILE_LOCKED;  // Reuse locked error for "already logged in"
                snprintf(msg.data, sizeof(msg.data), 
                        "User '%s' is already logged in from %s since %s",
                        client_username, existing_session->client_ip, 
                        format_time(existing_session->login_time));
                send_message(client_socket, &msg);
                close(client_socket);
                return NULL;
            }
            
            // Register user in the user registry (persists across sessions)
            register_user(client_username);
            
            // Add active session
            if (!add_active_session(client_username, client_socket, client_ip)) {
                // Unlikely race condition - another thread added session
                msg.error_code = ERR_FILE_LOCKED;
                snprintf(msg.data, sizeof(msg.data), "Login conflict detected");
                send_message(client_socket, &msg);
                close(client_socket);
                return NULL;
            }
            
            printf("✓ Client logged in: %s from %s\n", client_username, client_ip);
            
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "Client logged in: %s from %s", 
                    client_username, client_ip);
            log_message("naming_server", log_msg);
            
            // Send acknowledgment
            msg.error_code = RESP_SUCCESS;
            snprintf(msg.data, sizeof(msg.data), "Welcome back, %s! Your data is preserved.", client_username);
            send_message(client_socket, &msg);
        } else if (msg.type == MSG_REGISTER_SS) {
            // Handle storage server registration
            struct SSRegistration *reg = (struct SSRegistration*)msg.data;
            register_storage_server(reg);
            
            // Store the persistent socket in the StorageServer structure
            StorageServer *ss = find_ss_by_id(reg->ss_id);
            if (ss != NULL) {
                ss->ss_socket = client_socket;
                printf("✓ Storage server %s registered with persistent connection (socket %d)\n", ss->id, client_socket);
            }
            
            msg.error_code = RESP_SUCCESS;
            send_message(client_socket, &msg);
            
            // Storage servers don't send further messages in this initial handshake
            // But keep the connection open - don't enter the main client loop
            // Just wait here to keep the socket alive
            while (1) {
                // Keep connection alive, but don't expect messages from SS
                sleep(10);
                // Check if socket is still alive
                char test;
                int result = recv(client_socket, &test, 1, MSG_PEEK | MSG_DONTWAIT);
                if (result == 0 || (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    printf("✗ Storage server %s disconnected\n", ss->id);
                    if (ss) ss->ss_socket = -1;
                    break;
                }
            }
            close(client_socket);
            // Note: arg was already freed at the beginning of handle_client()
            return NULL;
        }
    }
    
    // Handle subsequent requests
    while (1) {
        struct Message msg;
        memset(&msg, 0, sizeof(msg));  // Clear message buffer before each receive
        
        if (recv_message(client_socket, &msg) <= 0) {
            // Client disconnected - clean up session
            printf("✓ Client disconnected: %s\n", client_username);
            remove_active_session(client_username);
            break;
        }
        
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Request from %s: type=%d, file=%s", 
                 client_username, msg.type, msg.filename);
        log_message("naming_server", log_msg);
        
        // Handle different message types
        switch (msg.type) {
            case MSG_CREATE: {
                printf("→ CREATE request for '%s' from %s\n", msg.filename, client_username);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "CREATE request for '%s' from %s", 
                         msg.filename, client_username);
                log_message("naming_server", log_msg);
                
                // Check if file already exists
                FileEntry *existing = lookup_file(msg.filename);
                if (existing != NULL) {
                    msg.error_code = ERR_FILE_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' already exists", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File already exists\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "CREATE failed for '%s' - file already exists", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Get storage server - either specified or most recent
                StorageServer *ss = NULL;
                if (strlen(msg.data) > 0) {
                    // Client specified a storage server ID
                    ss = find_ss_by_id(msg.data);
                    if (ss == NULL) {
                        msg.error_code = ERR_SS_UNAVAILABLE;
                        snprintf(msg.data, sizeof(msg.data), "Error: Storage server '%s' not found", msg.data);
                        send_message(client_socket, &msg);
                        printf("  ✗ Specified SS '%s' not found\n", msg.data);
                        break;
                    }
                    printf("  Using specified SS: %s\n", ss->id);
                } else {
                    // Use most recent (last registered) storage server
                    ss = get_available_ss();
                    if (ss == NULL) {
                        msg.error_code = ERR_SS_UNAVAILABLE;
                        snprintf(msg.data, sizeof(msg.data), "Error: No storage server available");
                        send_message(client_socket, &msg);
                        printf("  ✗ No storage server available\n");
                        break;
                    }
                    printf("  Using most recent SS: %s\n", ss->id);
                }
                
                // Use persistent socket to send command to SS
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    printf("  ✗ SS not connected\n");
                    break;
                }
                
                // Clear data field before forwarding to SS (it contained SS selection info)
                msg.data[0] = '\0';
                
                // Forward CREATE to SS
                send_message(ss->ss_socket, &msg);
                
                // Wait for SS response on the persistent socket
                struct Message ss_response;
                recv_message(ss->ss_socket, &ss_response);
                
                if (ss_response.error_code == RESP_SUCCESS) {
                    // Add file to registry
                    struct FileInfo info;
                    strncpy(info.name, msg.filename, sizeof(info.name));
                    strncpy(info.owner, client_username, sizeof(info.owner));
                    info.created_at = time(NULL);
                    info.last_modified = time(NULL);
                    info.last_accessed = time(NULL);
                    info.size = 0;
                    info.word_count = 0;
                    info.char_count = 0;
                    info.folder[0] = '\0'; // Initialize to root folder
                    add_file(&info, ss->id);
                    
                    // Invalidate search cache since file list changed
                    invalidate_search_cache();
                    
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "File '%s' created successfully!", msg.filename);
                    printf("  ✓ File created successfully\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "CREATE succeeded for '%s' on SS %s", 
                             msg.filename, ss->id);
                    log_message("naming_server", log_msg);
                } else {
                    msg.error_code = ss_response.error_code;
                    strncpy(msg.data, ss_response.data, sizeof(msg.data));
                    printf("  ✗ SS returned error: %d\n", ss_response.error_code);
                    
                    snprintf(log_msg, sizeof(log_msg), "CREATE failed for '%s' - SS error %d", 
                             msg.filename, ss_response.error_code);
                    log_message("naming_server", log_msg);
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_READ: {
                printf("→ READ request for '%s' from %s (msg type: %d)\n", 
                       msg.filename, client_username, msg.type);
                fflush(stdout);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "READ request for '%s' from %s", 
                         msg.filename, client_username);
                log_message("naming_server", log_msg);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "READ failed for '%s' - file not found", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Check read permission
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to read '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied for user %s\n", client_username);
                    
                    snprintf(log_msg, sizeof(log_msg), "READ denied for '%s' - no permission for %s", 
                             msg.filename, client_username);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Update last accessed time
                entry->info.last_accessed = time(NULL);
                
                // Find the storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "READ failed for '%s' - storage server unavailable", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Send SS info to client
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d", ss->ip, ss->client_port);
                
                printf("  ✓ Sending SS info: %s:%d (code: %d, data: '%s')\n", 
                       ss->ip, ss->client_port, msg.error_code, msg.data);
                fflush(stdout);
                
                send_message(client_socket, &msg);
                printf("  ✓ Message sent successfully\n");
                fflush(stdout);
                
                snprintf(log_msg, sizeof(log_msg), "READ approved for '%s' - directed to SS %s:%d", 
                         msg.filename, ss->ip, ss->client_port);
                log_message("naming_server", log_msg);
                break;
            }

            case MSG_STREAM: {
                printf("→ STREAM request for '%s' from %s\n", msg.filename, client_username);

                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "STREAM request for '%s' from %s", 
                         msg.filename, client_username);
                log_message("naming_server", log_msg);

                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM failed for '%s' - file not found", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }

                // Check read permission
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to stream '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied for user %s\n", client_username);
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM denied for '%s' - no permission for %s", 
                             msg.filename, client_username);
                    log_message("naming_server", log_msg);
                    break;
                }

                // Find storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM failed for '%s' - storage server unavailable", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }

                // Send SS info back to client
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d", ss->ip, ss->client_port);
                send_message(client_socket, &msg);
                printf("  ✓ Sent SS info for streaming: %s:%d\n", ss->ip, ss->client_port);
                
                snprintf(log_msg, sizeof(log_msg), "STREAM approved for '%s' - directed to SS %s:%d", 
                         msg.filename, ss->ip, ss->client_port);
                log_message("naming_server", log_msg);
                break;
            }
            
            case MSG_DELETE: {
                printf("→ DELETE request for '%s' from %s\n", msg.filename, client_username);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "DELETE request for '%s' from %s", 
                         msg.filename, client_username);
                log_message("naming_server", log_msg);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "DELETE failed for '%s' - file not found", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Check if user is owner
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only owner can delete file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "DELETE denied for '%s' - user %s is not owner", 
                             msg.filename, client_username);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Find the storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    break;
                }
                
                // Use persistent socket connection
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server not connected\n");
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                
                struct Message ss_response;
                recv_message(ss->ss_socket, &ss_response);
                
                if (ss_response.error_code == RESP_SUCCESS) {
                    // Remove from registry
                    unsigned int index = hash_function(msg.filename);
                    pthread_mutex_lock(&table_lock);
                    
                    FileEntry *prev = NULL;
                    FileEntry *current = file_table[index];
                    while (current != NULL) {
                        if (strcmp(current->info.name, msg.filename) == 0) {
                            if (prev == NULL) {
                                file_table[index] = current->next;
                            } else {
                                prev->next = current->next;
                            }
                            free(current);
                            break;
                        }
                        prev = current;
                        current = current->next;
                    }
                    
                    pthread_mutex_unlock(&table_lock);
                    
                    // Invalidate search cache since file list changed
                    invalidate_search_cache();
                    
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "File '%s' deleted successfully!", msg.filename);
                    printf("  ✓ File deleted successfully\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "DELETE succeeded for '%s'", msg.filename);
                    log_message("naming_server", log_msg);
                } else {
                    msg.error_code = ss_response.error_code;
                    strncpy(msg.data, ss_response.data, sizeof(msg.data));
                    
                    snprintf(log_msg, sizeof(log_msg), "DELETE failed for '%s' - SS error %d", 
                             msg.filename, ss_response.error_code);
                    log_message("naming_server", log_msg);
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_VIEW: {
                printf("→ VIEW request from %s (flags: %d)\n", client_username, msg.flags);
                fflush(stdout);
                
                int show_all = (msg.flags & 1);  // -a flag
                int show_details = (msg.flags & 2);  // -l flag
                
                char file_list[MAX_DATA] = "";
                int count = 0;
                
                pthread_mutex_lock(&table_lock);
                for (int i = 0; i < HASH_TABLE_SIZE; i++) {
                    FileEntry *entry = file_table[i];
                    while (entry != NULL) {
                        // Check access permissions - only show files user can access
                        int has_access = 0;
                        if (show_all) {
                            // With -a flag, show all files (but indicate access)
                            has_access = 1;
                        } else {
                            // Without -a, only show files user has access to
                            has_access = check_permission(entry, client_username, 0);
                        }
                        
                        if (has_access) {
                            char line[1024];
                            if (show_details) {
                                // Show detailed info
                                char access_indicator = ' ';
                                if (strcmp(entry->info.owner, client_username) == 0) {
                                    access_indicator = 'O';  // Owner
                                } else if (check_permission(entry, client_username, 1)) {
                                    access_indicator = 'W';  // Write access
                                } else if (check_permission(entry, client_username, 0)) {
                                    access_indicator = 'R';  // Read-only access
                                } else {
                                    access_indicator = '-';  // No access
                                }
                                
                                // Get real-time stats from storage server
                                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                                if (ss != NULL && ss->is_active && ss->ss_socket >= 0) {
                                    struct Message ss_msg;
                                    memset(&ss_msg, 0, sizeof(ss_msg));
                                    ss_msg.type = MSG_INFO;
                                    strncpy(ss_msg.filename, entry->info.name, sizeof(ss_msg.filename));
                                    
                                    if (send_message(ss->ss_socket, &ss_msg) > 0) {
                                        struct Message ss_response;
                                        if (recv_message(ss->ss_socket, &ss_response) > 0 && ss_response.error_code == RESP_SUCCESS) {
                                            long size = 0;
                                            int word_count = 0;
                                            int char_count = 0;
                                            if (sscanf(ss_response.data, "%ld:%d:%d", &size, &word_count, &char_count) == 3) {
                                                entry->info.size = size;
                                                entry->info.word_count = word_count;
                                                entry->info.char_count = char_count;
                                            }
                                        }
                                    }
                                }
                                
                                snprintf(line, sizeof(line), "[%c] %-30s  Owner: %-15s  %6ld bytes  %5d words  %5d chars\n", 
                                         access_indicator,
                                         entry->info.name, 
                                         entry->info.owner,
                                         entry->info.size,
                                         entry->info.word_count,
                                         entry->info.char_count);
                            } else {
                                // Simple list
                                if (show_all && !check_permission(entry, client_username, 0)) {
                                    snprintf(line, sizeof(line), "[-] %s (no access)\n", entry->info.name);
                                } else {
                                    snprintf(line, sizeof(line), "--> %s\n", entry->info.name);
                                }
                            }
                            strncat(file_list, line, sizeof(file_list) - strlen(file_list) - 1);
                            count++;
                        }
                        entry = entry->next;
                    }
                }
                pthread_mutex_unlock(&table_lock);
                
                if (count == 0) {
                    if (show_all) {
                        snprintf(msg.data, sizeof(msg.data), "No files in the system");
                    } else {
                        snprintf(msg.data, sizeof(msg.data), "No files you have access to");
                    }
                } else {
                    if (show_details) {
                        char header[256];
                        snprintf(header, sizeof(header), 
                                 "Access Legend: [O]=Owner [W]=Write [R]=Read [-]=No Access\n"
                                 "────────────────────────────────────────────────────────────\n");
                        snprintf(msg.data, sizeof(msg.data), "%s%s", header, file_list);
                    } else {
                        snprintf(msg.data, sizeof(msg.data), "%s", file_list);
                    }
                }
                
                msg.error_code = RESP_SUCCESS;
                
                printf("  ✓ Sending list of %d accessible files\n", count);
                fflush(stdout);
                
                send_message(client_socket, &msg);
                printf("  ✓ VIEW response sent\n");
                fflush(stdout);
                break;
            }
            
            case MSG_LIST_SS: {
                printf("→ LISTSS request from %s\n", client_username);
                
                char ss_list[MAX_DATA] = "";
                StorageServer *ss = storage_servers;
                int ss_count = 0;
                
                while (ss != NULL) {
                    char ss_info[256];
                    snprintf(ss_info, sizeof(ss_info), "%s\t%s:%d\t%s\n", 
                             ss->id, ss->ip, ss->client_port, 
                             ss->is_active ? "Active" : "Inactive");
                    strncat(ss_list, ss_info, sizeof(ss_list) - strlen(ss_list) - 1);
                    ss_count++;
                    ss = ss->next;
                }
                
                if (ss_count == 0) {
                    strcpy(ss_list, "No storage servers registered\n");
                }
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, ss_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_LIST_USERS: {
                printf("→ LIST request from %s\n", client_username);
                
                char *user_list = get_all_users();
                
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "%s", user_list);
                send_message(client_socket, &msg);
                
                printf("  ✓ Sent list of registered users\n");
                break;
            }
            
            case MSG_ADD_ACCESS: {
                printf("→ ADDACCESS request for '%s' from %s\n", msg.filename, client_username);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check if requester is the owner
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the owner can grant access");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied: %s is not the owner\n", client_username);
                    break;
                }
                
                // Extract target username from data field
                // Format: "target_username" (first word in data)
                char target_user[MAX_USERNAME];
                sscanf(msg.data, "%s", target_user);
                
                // Check if target user exists (registered)
                int user_exists = 0;
                pthread_mutex_lock(&user_lock);
                UserEntry *current = registered_users;
                while (current != NULL) {
                    if (strcmp(current->username, target_user) == 0) {
                        user_exists = 1;
                        break;
                    }
                    current = current->next;
                }
                pthread_mutex_unlock(&user_lock);
                
                if (!user_exists) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: User '%s' not found", target_user);
                    send_message(client_socket, &msg);
                    printf("  ✗ User '%s' does not exist\n", target_user);
                    break;
                }
                
                // msg.flags: 1 = read access, 2 = write access
                int can_read = (msg.flags & 1) ? 1 : 0;
                int can_write = (msg.flags & 2) ? 1 : 0;
                
                // Write access implies read access
                if (can_write) {
                    can_read = 1;
                }
                
                // Add access
                pthread_mutex_lock(&table_lock);
                int result = add_access(entry, target_user, can_read, can_write);
                pthread_mutex_unlock(&table_lock);
                
                msg.error_code = RESP_SUCCESS;
                if (result == 0) {
                    snprintf(msg.data, sizeof(msg.data), 
                             "Granted %s access to '%s' for user '%s'",
                             can_write ? "write" : "read", msg.filename, target_user);
                    printf("  ✓ Added %s access for %s\n", can_write ? "write" : "read", target_user);
                } else {
                    snprintf(msg.data, sizeof(msg.data), 
                             "Updated access to %s for user '%s'",
                             can_write ? "write" : "read", target_user);
                    printf("  ✓ Updated access for %s\n", target_user);
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_REM_ACCESS: {
                printf("→ REMACCESS request for '%s' from %s\n", msg.filename, client_username);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check if requester is the owner
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the owner can revoke access");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied: %s is not the owner\n", client_username);
                    break;
                }
                
                // Extract target username from data field
                char target_user[MAX_USERNAME];
                sscanf(msg.data, "%s", target_user);
                
                // Prevent owner from removing their own access
                if (strcmp(target_user, client_username) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: Owner cannot remove their own access");
                    send_message(client_socket, &msg);
                    printf("  ✗ Cannot remove owner's access\n");
                    break;
                }
                
                // Remove access
                pthread_mutex_lock(&table_lock);
                int result = remove_access(entry, target_user);
                pthread_mutex_unlock(&table_lock);
                
                if (result == 1) {
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), 
                             "Removed all access to '%s' for user '%s'", msg.filename, target_user);
                    send_message(client_socket, &msg);
                    printf("  ✓ Removed access for %s\n", target_user);
                } else {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), 
                             "User '%s' did not have access to '%s'", target_user, msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ User '%s' had no access to remove\n", target_user);
                }
                
                break;
            }
            
            case MSG_INFO:
            {
                printf("→ INFO request for '%s' from %s\n", msg.filename, client_username);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check if user has permission to view file info
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to view this file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    break;
                }
                
                // Get storage server info
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                
                // Request updated file stats from storage server
                if (ss != NULL && ss->is_active && ss->ss_socket >= 0) {
                    struct Message ss_msg;
                    memset(&ss_msg, 0, sizeof(ss_msg));
                    ss_msg.type = MSG_INFO;
                    strncpy(ss_msg.filename, msg.filename, sizeof(ss_msg.filename));
                    strncpy(ss_msg.username, client_username, sizeof(ss_msg.username));
                    
                    // Send INFO request to storage server
                    if (send_message(ss->ss_socket, &ss_msg) > 0) {
                        struct Message ss_response;
                        if (recv_message(ss->ss_socket, &ss_response) > 0 && ss_response.error_code == RESP_SUCCESS) {
                            // Update cached file stats from storage server response
                            // Format: "size:word_count:char_count"
                            long size = 0;
                            int word_count = 0;
                            int char_count = 0;
                            if (sscanf(ss_response.data, "%ld:%d:%d", &size, &word_count, &char_count) == 3) {
                                entry->info.size = size;
                                entry->info.word_count = word_count;
                                entry->info.char_count = char_count;
                                printf("  ✓ Updated file stats: %ld bytes, %d words, %d chars\n", 
                                       size, word_count, char_count);
                            }
                        }
                    }
                }
                
                // Build access rights string
                char access_rights[512] = "";
                if (strcmp(entry->info.owner, client_username) == 0) {
                    strcat(access_rights, "Owner (Full Access)\n");
                } else {
                    AccessControl *acl = entry->acl;
                    int found = 0;
                    while (acl != NULL) {
                        if (strcmp(acl->username, client_username) == 0) {
                            found = 1;
                            if (acl->can_write) {
                                strcat(access_rights, "Read & Write Access\n");
                            } else if (acl->can_read) {
                                strcat(access_rights, "Read-Only Access\n");
                            }
                            break;
                        }
                        acl = acl->next;
                    }
                    if (!found) {
                        strcat(access_rights, "Limited Access\n");
                    }
                }
                
                // Add ACL list if user is owner
                if (strcmp(entry->info.owner, client_username) == 0) {
                    strcat(access_rights, "  Shared with:\n");
                    AccessControl *acl = entry->acl;
                    if (acl == NULL) {
                        strcat(access_rights, "    (No other users)\n");
                    } else {
                        while (acl != NULL) {
                            char acl_entry[128];
                            snprintf(acl_entry, sizeof(acl_entry), "    - %s: %s%s\n", 
                                     acl->username,
                                     acl->can_read ? "Read" : "",
                                     acl->can_write ? " & Write" : "");
                            strcat(access_rights, acl_entry);
                            acl = acl->next;
                        }
                    }
                }
                
                // Prepare comprehensive file info
                char info[MAX_DATA];
                snprintf(info, sizeof(info),
                         "╔════════════════════════════════════════════════════════════╗\n"
                         "║              FILE INFORMATION                              ║\n"
                         "╚════════════════════════════════════════════════════════════╝\n\n"
                         "📄 Filename:        %s\n"
                         "👤 Owner:           %s\n"
                         "📊 Size:            %ld bytes (%ld KB)\n"
                         "📝 Word Count:      %d words\n"
                         "🔤 Character Count: %d characters\n\n"
                         "🔒 Your Access Rights:\n"
                         "%s\n"
                         "📅 Timestamps:\n"
                         "  Created:        %s"
                         "  Last Modified:  %s"
                         "  Last Accessed:  %s\n"
                         "💾 Storage Info:\n"
                         "  Server ID:      %s\n"
                         "  Server IP:      %s\n"
                         "  Server Port:    %d\n",
                         entry->info.name,
                         entry->info.owner,
                         entry->info.size,
                         entry->info.size / 1024,
                         entry->info.word_count,
                         entry->info.char_count,
                         access_rights,
                         ctime(&entry->info.created_at),
                         ctime(&entry->info.last_modified),
                         ctime(&entry->info.last_accessed),
                         entry->info.storage_server_id,
                         ss ? ss->ip : "N/A",
                         ss ? ss->client_port : 0);
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, info, sizeof(msg.data) - 1);
                msg.data[sizeof(msg.data) - 1] = '\0';
                
                send_message(client_socket, &msg);
                printf("  ✓ Sent comprehensive file info\n");
                break;
            }
            
            case MSG_WRITE: {
                printf("→ WRITE request for '%s' sentence %d from %s\n", 
                       msg.filename, msg.sentence_num, client_username);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "WRITE request for '%s' sentence %d from %s", 
                         msg.filename, msg.sentence_num, client_username);
                log_message("naming_server", log_msg);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "WRITE failed for '%s' - file not found", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Check write permission
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have write permission");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "WRITE denied for '%s' - no write permission for %s", 
                             msg.filename, client_username);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Update last modified time
                entry->info.last_modified = time(NULL);
                
                // Find the storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    break;
                }
                
                // Send SS info to client
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d for write", ss->ip, ss->client_port);
                
                printf("  ✓ Sending SS info: %s:%d\n", ss->ip, ss->client_port);
                
                snprintf(log_msg, sizeof(log_msg), "WRITE approved for '%s' - directed to SS %s:%d", 
                         msg.filename, ss->ip, ss->client_port);
                log_message("naming_server", log_msg);
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_UNDO: {
                printf("→ UNDO request for '%s' from %s\n", msg.filename, client_username);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check write permission (need write access to undo)
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You need write permission to undo");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    break;
                }
                
                // Update last modified time
                entry->info.last_modified = time(NULL);
                
                // Find the storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    break;
                }
                
                // Send SS info to client
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d for undo", ss->ip, ss->client_port);
                
                printf("  ✓ Sending SS info: %s:%d\n", ss->ip, ss->client_port);
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_EXEC: {
                printf("→ EXEC request for '%s' from %s\n", msg.filename, client_username);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "EXEC request for '%s' from %s", 
                         msg.filename, client_username);
                log_message("naming_server", log_msg);
                
                // Lookup file
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "EXEC failed for '%s' - file not found", msg.filename);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Check read permission (need at least read access to execute)
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You need read permission to execute this file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "EXEC denied for '%s' - no read permission for %s", 
                             msg.filename, client_username);
                    log_message("naming_server", log_msg);
                    break;
                }
                
                // Update last accessed time
                entry->info.last_accessed = time(NULL);
                
                // Find the storage server to read file content
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    printf("  ✗ Storage server unavailable\n");
                    break;
                }
                
                // Connect to storage server to get file content
                int ss_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_socket < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to connect to storage server");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to create socket\n");
                    break;
                }
                
                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(ss->client_port);
                inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr);
                
                if (connect(ss_socket, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to connect to storage server");
                    send_message(client_socket, &msg);
                    close(ss_socket);
                    printf("  ✗ Failed to connect to SS\n");
                    break;
                }
                
                // Request file content from storage server
                struct Message read_msg;
                memset(&read_msg, 0, sizeof(read_msg));
                read_msg.type = MSG_READ;
                strncpy(read_msg.filename, msg.filename, sizeof(read_msg.filename));
                
                if (send_message(ss_socket, &read_msg) < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read file from storage");
                    send_message(client_socket, &msg);
                    close(ss_socket);
                    printf("  ✗ Failed to send read request\n");
                    break;
                }
                
                // Receive file content
                memset(&read_msg, 0, sizeof(read_msg));
                if (recv_message(ss_socket, &read_msg) < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read file from storage");
                    send_message(client_socket, &msg);
                    close(ss_socket);
                    printf("  ✗ Failed to receive file content\n");
                    break;
                }
                
                close(ss_socket);
                
                if (read_msg.error_code != RESP_SUCCESS) {
                    msg.error_code = read_msg.error_code;
                    strncpy(msg.data, read_msg.data, sizeof(msg.data));
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to read file: %s\n", read_msg.data);
                    break;
                }
                
                // Execute file content as shell commands
                printf("  → Executing commands from file...\n");
                
                // Create a temporary file with the commands
                char temp_filename[256];
                snprintf(temp_filename, sizeof(temp_filename), "/tmp/exec_%s_%ld.sh", 
                         client_username, time(NULL));
                
                FILE *temp_file = fopen(temp_filename, "w");
                if (!temp_file) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to create temporary script");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to create temp file\n");
                    break;
                }
                
                fprintf(temp_file, "%s", read_msg.data);
                fclose(temp_file);
                
                // Make it executable
                chmod(temp_filename, 0700);
                
                // Execute and capture output
                char exec_cmd[512];
                snprintf(exec_cmd, sizeof(exec_cmd), "/bin/bash %s 2>&1", temp_filename);
                
                FILE *pipe = popen(exec_cmd, "r");
                if (!pipe) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to execute commands");
                    send_message(client_socket, &msg);
                    unlink(temp_filename);
                    printf("  ✗ Failed to execute\n");
                    break;
                }
                
                // Read output
                char output[MAX_DATA];
                memset(output, 0, sizeof(output));
                size_t bytes_read = fread(output, 1, sizeof(output) - 1, pipe);
                output[bytes_read] = '\0';
                
                int exit_status = pclose(pipe);
                unlink(temp_filename);  // Clean up temp file
                
                // Send output back to client
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, output, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                printf("  ✓ Executed successfully (exit: %d, %zu bytes output)\n", 
                       WEXITSTATUS(exit_status), bytes_read);
                
                snprintf(log_msg, sizeof(log_msg), "EXEC completed for '%s' - exit code %d, %zu bytes output", 
                         msg.filename, WEXITSTATUS(exit_status), bytes_read);
                log_message("naming_server", log_msg);
                
                break;
            }
            
            case MSG_SEARCH: {
                printf("→ SEARCH request from %s: pattern='%s'\n", 
                       client_username, msg.data);
                
                // Perform search using efficient hash table + caching
                char *search_results = search_files(msg.data, client_username);
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, search_results, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                printf("  ✓ Search completed\n");
                
                break;
            }
            
            case MSG_CREATEFOLDER: {
                printf("→ CREATEFOLDER request from %s: folder='%s'\n", 
                       client_username, msg.filename);
                
                // Validate folder name
                if (strlen(msg.filename) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder name cannot be empty");
                    send_message(client_socket, &msg);
                    printf("  ✗ Empty folder name\n");
                    break;
                }
                
                // Create folder in naming server metadata
                int result = create_folder(msg.filename, client_username);
                
                if (result == ERR_FOLDER_EXISTS) {
                    msg.error_code = ERR_FOLDER_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' already exists", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ Folder already exists\n");
                } else {
                    // Forward folder creation to selected or available storage server only
                    StorageServer *ss = NULL;
                    if (strlen(msg.data) > 0) {
                        ss = find_ss_by_id(msg.data);
                    } else {
                        ss = get_available_ss();
                    }
                    
                    if (ss != NULL && ss->is_active && ss->ss_socket >= 0) {
                        struct Message folder_msg;
                        memset(&folder_msg, 0, sizeof(folder_msg));
                        folder_msg.type = MSG_CREATEFOLDER;
                        strncpy(folder_msg.filename, msg.filename, sizeof(folder_msg.filename));
                        send_message(ss->ss_socket, &folder_msg);
                        printf("  ✓ Folder creation sent to %s\n", ss->id);
                    }
                    
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "Folder '%s' created successfully", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✓ Folder created in naming server\n");
                }
                
                break;
            }
            
            case MSG_VIEWFOLDER: {
                printf("→ VIEWFOLDER request from %s: folder='%s'\n", 
                       client_username, msg.filename);
                
                // Check if folder exists
                if (!folder_exists(msg.filename) && strlen(msg.filename) > 0) {
                    msg.error_code = ERR_FOLDER_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ Folder not found\n");
                    break;
                }
                
                // List files in folder
                char *file_list = list_folder_files(msg.filename);
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, file_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                printf("  ✓ Listed folder contents\n");
                
                break;
            }
            
            case MSG_MOVE: {
                printf("→ MOVE request from %s: file='%s' to folder='%s'\n", 
                       client_username, msg.filename, msg.folder);
                
                // Check if file exists
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check permission (only owner or those with write access can move)
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied to move '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ Permission denied\n");
                    break;
                }
                
                // Check if target folder exists (empty string = root, which always exists)
                if (strlen(msg.folder) > 0 && !folder_exists(msg.folder)) {
                    msg.error_code = ERR_FOLDER_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' not found", msg.folder);
                    send_message(client_socket, &msg);
                    printf("  ✗ Target folder not found\n");
                    break;
                }
                
                // Move file (pass the FileEntry to avoid deadlock)
                int result = move_file_to_folder(entry, msg.folder);
                
                if (result == RESP_SUCCESS) {
                    // Forward move command to storage server to physically move the file
                    StorageServer *ss = storage_servers;
                    while (ss != NULL) {
                        if (strcmp(ss->id, entry->info.storage_server_id) == 0) {
                            if (ss->is_active && ss->ss_socket >= 0) {
                                struct Message move_msg;
                                memset(&move_msg, 0, sizeof(move_msg));
                                move_msg.type = MSG_MOVE;
                                strncpy(move_msg.filename, msg.filename, sizeof(move_msg.filename));
                                strncpy(move_msg.folder, msg.folder, sizeof(move_msg.folder));
                                send_message(ss->ss_socket, &move_msg);
                                // Don't wait for response
                            }
                            break;
                        }
                        ss = ss->next;
                    }
                    
                    msg.error_code = RESP_SUCCESS;
                    if (strlen(msg.folder) == 0) {
                        snprintf(msg.data, sizeof(msg.data), "File '%s' moved to root", msg.filename);
                    } else {
                        snprintf(msg.data, sizeof(msg.data), "File '%s' moved to folder '%s'", msg.filename, msg.folder);
                    }
                    send_message(client_socket, &msg);
                    printf("  ✓ File moved (metadata and physical)\n");
                } else {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to move file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Move failed\n");
                }
                
                break;
            }

            case MSG_CHECKPOINT: {
                printf("→ CHECKPOINT request from %s: file='%s', tag='%s'\n",
                       client_username, msg.filename, msg.checkpoint_tag);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check if user owns the file or has write access
                if (strcmp(entry->info.owner, client_username) != 0 && !check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to create checkpoints");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Add checkpoint to metadata
                if (add_checkpoint(entry, msg.checkpoint_tag, client_username) < 0) {
                    msg.error_code = ERR_FILE_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint with tag '%s' already exists", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Forward checkpoint request to storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Use persistent socket connection
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                
                send_message(client_socket, &msg);
                printf("  ✓ Checkpoint '%s' created\n", msg.checkpoint_tag);
                
                break;
            }

            case MSG_VIEWCHECKPOINT: {
                printf("→ VIEWCHECKPOINT request from %s: file='%s', tag='%s'\n",
                       client_username, msg.filename, msg.checkpoint_tag);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check permission
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check if checkpoint exists
                if (find_checkpoint(entry, msg.checkpoint_tag) == NULL) {
                    msg.error_code = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint '%s' not found", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Forward to storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Use persistent socket connection
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                send_message(client_socket, &msg);
                printf("  ✓ Checkpoint viewed\n");
                
                break;
            }

            case MSG_REVERT: {
                printf("→ REVERT request from %s: file='%s', tag='%s'\n",
                       client_username, msg.filename, msg.checkpoint_tag);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check if user owns file or has write permission
                if (strcmp(entry->info.owner, client_username) != 0 && !check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to revert this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check if checkpoint exists
                CheckpointEntry *cp = find_checkpoint(entry, msg.checkpoint_tag);
                if (cp == NULL) {
                    msg.error_code = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint '%s' not found", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Forward to storage server
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Use persistent socket connection
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                
                // Update last modified time
                entry->info.last_modified = time(NULL);
                
                send_message(client_socket, &msg);
                printf("  ✓ File reverted to checkpoint '%s'\n", msg.checkpoint_tag);
                
                break;
            }

            case MSG_LISTCHECKPOINTS: {
                printf("→ LISTCHECKPOINTS request from %s: file='%s'\n",
                       client_username, msg.filename);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Check permission
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // List checkpoints
                char *checkpoint_list = list_checkpoints(entry);
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, checkpoint_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                printf("  ✓ Listed checkpoints\n");
                break;
            }

            case MSG_REQUESTACCESS: {
                printf("→ REQUESTACCESS from %s: file='%s', type=%d\n",
                       client_username, msg.filename, msg.flags);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Can't request access to your own file
                if (strcmp(entry->info.owner, client_username) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: You already own this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Add access request
                int request_id = add_access_request(entry, client_username, msg.flags);
                if (request_id < 0) {
                    msg.error_code = ERR_FILE_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: You already have a pending request for this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Access request submitted (ID: %d). Owner will be notified.", request_id);
                send_message(client_socket, &msg);
                
                printf("  ✓ Access request created (ID: %d)\n", request_id);
                break;
            }

            case MSG_VIEWREQUESTS: {
                printf("→ VIEWREQUESTS from %s: file='%s'\n",
                       client_username, msg.filename);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Only owner can view requests
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the file owner can view access requests");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // List requests
                char *request_list = list_access_requests(entry);
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, request_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                printf("  ✓ Listed access requests\n");
                break;
            }

            case MSG_RESPONDREQUEST: {
                printf("→ RESPONDREQUEST from %s: file='%s', request_id=%d, approve=%d\n",
                       client_username, msg.filename, msg.request_id, msg.flags);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Only owner can respond to requests
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the file owner can respond to access requests");
                    send_message(client_socket, &msg);
                    break;
                }
                
                // Respond to request
                if (respond_to_request(entry, msg.request_id, msg.flags) < 0) {
                    msg.error_code = ERR_REQUEST_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Request ID %d not found or already processed", msg.request_id);
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Request %s", msg.flags ? "approved" : "denied");
                send_message(client_socket, &msg);
                
                printf("  ✓ Request %s\n", msg.flags ? "approved" : "denied");
                break;
            }
        

            default:
                printf("→ Unknown request type: %d\n", msg.type);
                msg.error_code = ERR_INVALID_REQUEST;
                snprintf(msg.data, sizeof(msg.data), "Error: Invalid request type");
                send_message(client_socket, &msg);
        }
    }
    
    printf("✗ Client %s disconnected\n", client_username);
    close(client_socket);
    return NULL;
}

// ==================== FAULT TOLERANCE FUNCTIONS ====================

// Replicate operation to all storage servers (async)
void replicate_to_all_ss(struct Message *msg) {
    StorageServer *ss = storage_servers;
    
    while (ss != NULL) {
        if (ss->is_active && !ss->failed && ss->ss_socket >= 0) {
            // Use persistent socket for replication
            msg->type = MSG_REPLICATE;
            send_message(ss->ss_socket, msg);
            // Don't wait for response (async replication)
        }
        ss = ss->next;
    }
}

// Heartbeat thread - checks storage servers
void* heartbeat_monitor(void *arg) {
    printf("✓ Heartbeat monitor started\n");
    
    while (!shutdown_flag) {
        sleep(5);  // Check every 5 seconds
        
        StorageServer *ss = storage_servers;
        time_t now = time(NULL);
        
        while (ss != NULL) {
            if (ss->is_active) {
                // Check if SS has missed heartbeat (timeout: 15 seconds)
                if (now - ss->last_heartbeat > 15) {
                    if (!ss->failed) {
                        printf("⚠ Storage server %s failed (no heartbeat)\n", ss->id);
                        ss->failed = 1;
                        ss->is_active = 0;
                    }
                } else {
                    // Try to ping the storage server using persistent socket
                    if (ss->ss_socket >= 0) {
                        struct Message ping;
                        memset(&ping, 0, sizeof(ping));
                        ping.type = MSG_HEARTBEAT;
                        
                        if (send_message(ss->ss_socket, &ping) > 0 && recv_message(ss->ss_socket, &ping) > 0) {
                            ss->last_heartbeat = now;
                            
                            // If it was previously failed, mark as recovered
                            if (ss->failed) {
                                printf("✓ Storage server %s recovered\n", ss->id);
                                ss->failed = 0;
                                ss->is_active = 1;
                                // TODO: Trigger data synchronization
                            }
                        } else {
                            if (!ss->failed) {
                                printf("⚠ Storage server %s unreachable (socket error)\n", ss->id);
                                ss->failed = 1;
                                ss->is_active = 0;
                                // Close bad socket
                                if (ss->ss_socket >= 0) {
                                    close(ss->ss_socket);
                                    ss->ss_socket = -1;
                                }
                            }
                        }
                    } else {
                        if (!ss->failed) {
                            printf("⚠ Storage server %s not connected\n", ss->id);
                            ss->failed = 1;
                            ss->is_active = 0;
                        }
                    }
                }
            }
            ss = ss->next;
        }
    }
    
    return NULL;
}

// Shutdown handler - send shutdown to all SS and clients
void shutdown_system(int sig) {
    printf("\n⚠ Naming Server shutting down (signal %d)...\n", sig);
    shutdown_flag = 1;
    
    // Send shutdown message to all storage servers
    StorageServer *ss = storage_servers;
    while (ss != NULL) {
        if (ss->is_active && ss->ss_socket >= 0) {
            struct Message msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_SHUTDOWN;
            snprintf(msg.data, sizeof(msg.data), "Naming server is shutting down");
            send_message(ss->ss_socket, &msg);
            printf("  → Sent shutdown to storage server %s\n", ss->id);
            close(ss->ss_socket);
            ss->ss_socket = -1;
        }
        ss = ss->next;
    }
    
    printf("✓ Shutdown complete\n");
    exit(0);
}

int main() {
    int server_socket, *client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    printf("=== Naming Server ===\n");
    printf("Starting on port %d...\n", NS_PORT);
    
    // Register signal handlers for graceful shutdown
    signal(SIGINT, shutdown_system);   // Ctrl+C
    signal(SIGTERM, shutdown_system);  // kill command
    signal(SIGHUP, shutdown_system);   // Terminal hangup
    
    // Initialize hash table
    memset(file_table, 0, sizeof(file_table));
    
    // Start heartbeat monitor thread
    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_monitor, NULL) != 0) {
        perror("Failed to create heartbeat thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(heartbeat_thread);
    
    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind socket
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NS_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Naming Server is running and waiting for connections...\n");
    printf("Type 'SHUTDOWN' to gracefully shutdown the server\n\n");
    log_message("naming_server", "Server started successfully");
    
    // Make stdin non-blocking so we can check for commands while accepting connections
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    
    // Accept connections
    while (!shutdown_flag) {
        // Check for SHUTDOWN command from terminal
        char cmd[256];
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            // Remove newline
            cmd[strcspn(cmd, "\n")] = 0;
            
            if (strcmp(cmd, "SHUTDOWN") == 0) {
                printf("\n⚠️  Initiating Naming Server shutdown...\n");
                
                // Notify all storage servers
                StorageServer *ss = storage_servers;
                while (ss != NULL) {
                    if (ss->is_active && ss->ss_socket >= 0) {
                        printf("  Notifying storage server %s to disconnect...\n", ss->id);
                        
                        struct Message shutdown_msg;
                        memset(&shutdown_msg, 0, sizeof(shutdown_msg));
                        shutdown_msg.type = MSG_SHUTDOWN;
                        send_message(ss->ss_socket, &shutdown_msg);
                        close(ss->ss_socket);
                        ss->ss_socket = -1;
                    }
                    ss = ss->next;
                }
                
                printf("✓ All storage servers notified\n");
                printf("✓ Naming Server shutting down\n");
                close(server_socket);
                exit(0);
            }
        }
        
        // Set timeout for accept to check shutdown flag periodically
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
        int activity = select(server_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (activity > 0 && FD_ISSET(server_socket, &readfds)) {
            client_socket = malloc(sizeof(int));
            *client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
            
            if (*client_socket < 0) {
                perror("Accept failed");
                free(client_socket);
                continue;
            }
            
            printf("New connection from %s:%d\n", 
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            // Create thread to handle client
            pthread_t thread;
            if (pthread_create(&thread, NULL, handle_client, client_socket) != 0) {
                perror("Thread creation failed");
                free(client_socket);
                continue;
            }
            
            pthread_detach(thread);
        }
    }
    
    close(server_socket);
    return 0;
}
