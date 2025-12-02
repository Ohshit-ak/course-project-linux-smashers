#include "storage_server_manager.h"
#include "file_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

StorageServer *storage_servers = NULL;
volatile int shutdown_flag = 0;

// Initialize storage servers
void init_storage_servers() {
    storage_servers = NULL;
}

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
    // Check if SS already exists (reconnecting)
    StorageServer *existing_ss = find_ss_by_id(reg->ss_id);
    
    if (existing_ss != NULL) {
        // Storage server reconnecting - preserve existing data
        printf("✓ Storage Server %s reconnecting - preserving existing data\n", reg->ss_id);
        strncpy(existing_ss->ip, reg->ip, sizeof(existing_ss->ip));
        existing_ss->nm_port = reg->nm_port;
        existing_ss->client_port = reg->client_port;
        existing_ss->is_active = 1;
        existing_ss->failed = 0;
        existing_ss->last_heartbeat = time(NULL);
        
        char msg[256];
        snprintf(msg, sizeof(msg), "Storage Server reconnected: %s at %s:%d (ACLs preserved)", 
                 existing_ss->id, existing_ss->ip, existing_ss->client_port);
        log_message("naming_server", msg);
        
        // Clean up cache for this SS's files
        printf("  \u2192 Cleaning cache for reconnected SS...\n");
        for (int i = 0; i < reg->file_count; i++) {
            char cache_path[MAX_PATH];
            snprintf(cache_path, sizeof(cache_path), "../cache/%s", reg->files[i]);
            if (remove(cache_path) == 0) {
                printf("  \u2713 Removed cached: %s\n", reg->files[i]);
            }
        }
        
        // Only add NEW files from SS that aren't in registry
        for (int i = 0; i < reg->file_count; i++) {
            FileEntry *existing_file = lookup_file(reg->files[i]);
            if (existing_file == NULL) {
                // This is a new file - add it
                struct FileInfo info;
                strncpy(info.name, reg->files[i], sizeof(info.name));
                strncpy(info.owner, "system", sizeof(info.owner));
                info.created_at = time(NULL);
                info.last_modified = time(NULL);
                info.last_accessed = time(NULL);
                info.size = 0;
                info.word_count = 0;
                info.char_count = 0;
                add_file(&info, existing_ss->id);
                printf("  + Added new file: %s\n", reg->files[i]);
            } else {
                // File already exists - preserve ACLs and metadata
                // Update SS assignment in case it changed
                strncpy(existing_file->info.storage_server_id, existing_ss->id, 
                        sizeof(existing_file->info.storage_server_id));
                printf("  \u2713 File exists with ACLs preserved: %s\n", reg->files[i]);
            }
        }
        return;
    }
    
    // New storage server - create new entry
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
    snprintf(msg, sizeof(msg), "Registered NEW Storage Server: %s at %s:%d", 
             ss->id, ss->ip, ss->client_port);
    log_message("naming_server", msg);
    printf("✓ Storage Server registered: %s (client port: %d)\n", ss->id, ss->client_port);
    
    // Register all files from this new SS
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
        sleep(10);  // Check every 10 seconds (was 5)
        
        StorageServer *ss = storage_servers;
        time_t now = time(NULL);
        
        while (ss != NULL) {
            if (ss->is_active) {
                // Check if SS has missed heartbeat (timeout: 60 seconds)
                if (now - ss->last_heartbeat > 60) {
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
