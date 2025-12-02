/*
 * Naming Server - Main Entry Point (Modular Version)
 * 
 * The Naming Server is the central coordinator of the distributed file system.
 * This modular version separates concerns into dedicated modules for better
 * maintainability and code organization.
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

// Common includes
#include "../common/protocol.h"
#include "../common/utils.h"

// Module includes
#include "file_manager.h"
#include "access_control.h"
#include "storage_server_manager.h"
#include "folder_manager.h"
#include "checkpoint_manager.h"
#include "search_manager.h"
#include "user_session_manager.h"
#include "persistence.h"

#define NS_PORT 8080
#define MAX_CLIENTS 100

// Forward declarations
void* handle_client(void *arg);
void shutdown_system(int sig);

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
    
    // Save file registry before shutdown
    save_file_registry("../naming_server/registry.dat");
    
    // Cleanup all modules
    cleanup_file_table();
    cleanup_folders();
    cleanup_search_cache();
    cleanup_users_and_sessions();
    
    printf("✓ Shutdown complete\n");
    exit(0);
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
                
                msg.error_code = ERR_FILE_LOCKED;
                snprintf(msg.data, sizeof(msg.data), 
                        "User '%s' is already logged in from %s since %s",
                        client_username, existing_session->client_ip, 
                        format_time(existing_session->login_time));
                send_message(client_socket, &msg);
                close(client_socket);
                return NULL;
            }
            
            // Register user and add active session
            register_user(client_username);
            
            if (!add_active_session(client_username, client_socket, client_ip)) {
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
            
            msg.error_code = RESP_SUCCESS;
            snprintf(msg.data, sizeof(msg.data), "Welcome back, %s! Your data is preserved.", client_username);
            send_message(client_socket, &msg);
            
        } else if (msg.type == MSG_REGISTER_SS) {
            // Handle storage server registration
            struct SSRegistration *reg = (struct SSRegistration*)msg.data;
            register_storage_server(reg);
            
            StorageServer *ss = find_ss_by_id(reg->ss_id);
            if (ss != NULL) {
                ss->ss_socket = client_socket;
                printf("✓ Storage server %s registered with persistent connection (socket %d)\n", 
                       ss->id, client_socket);
            }
            
            msg.error_code = RESP_SUCCESS;
            send_message(client_socket, &msg);
            
            // Keep connection alive for storage server
            while (1) {
                sleep(10);
                char test;
                int result = recv(client_socket, &test, 1, MSG_PEEK | MSG_DONTWAIT);
                if (result == 0 || (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    printf("✗ Storage server %s disconnected\n", ss->id);
                    if (ss) ss->ss_socket = -1;
                    break;
                }
            }
            close(client_socket);
            return NULL;
        }
    }
    
    // Handle subsequent client requests
    while (1) {
        memset(&msg, 0, sizeof(msg));
        
        if (recv_message(client_socket, &msg) <= 0) {
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
                
                FileEntry *existing = lookup_file(msg.filename);
                if (existing != NULL) {
                    msg.error_code = ERR_FILE_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' already exists", msg.filename);
                    send_message(client_socket, &msg);
                    printf("  ✗ File already exists\n");
                    break;
                }
                
                // Get storage server
                StorageServer *ss = NULL;
                if (strlen(msg.data) > 0) {
                    ss = find_ss_by_id(msg.data);
                    if (ss == NULL) {
                        msg.error_code = ERR_SS_UNAVAILABLE;
                        snprintf(msg.data, sizeof(msg.data), "Error: Storage server '%s' not found", msg.data);
                        send_message(client_socket, &msg);
                        break;
                    }
                } else {
                    ss = get_available_ss();
                    if (ss == NULL) {
                        msg.error_code = ERR_SS_UNAVAILABLE;
                        snprintf(msg.data, sizeof(msg.data), "Error: No storage server available");
                        send_message(client_socket, &msg);
                        break;
                    }
                }
                
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.data[0] = '\0';
                send_message(ss->ss_socket, &msg);
                
                struct Message ss_response;
                recv_message(ss->ss_socket, &ss_response);
                
                if (ss_response.error_code == RESP_SUCCESS) {
                    struct FileInfo info;
                    strncpy(info.name, msg.filename, sizeof(info.name));
                    strncpy(info.owner, client_username, sizeof(info.owner));
                    info.created_at = time(NULL);
                    info.last_modified = time(NULL);
                    info.last_accessed = time(NULL);
                    info.size = 0;
                    info.word_count = 0;
                    info.char_count = 0;
                    info.folder[0] = '\0';
                    add_file(&info, ss->id);
                    
                    invalidate_search_cache();
                    
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "File '%s' created successfully!", msg.filename);
                    
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Created file '%s' by %s on %s", msg.filename, client_username, ss->id);
                    log_message("naming_server", log_msg);
                } else {
                    msg.error_code = ss_response.error_code;
                    strncpy(msg.data, ss_response.data, sizeof(msg.data));
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_READ: {
                printf("→ READ request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to read '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                entry->info.last_accessed = time(NULL);
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss != NULL) {
                    printf("  [DEBUG] SS %s status: is_active=%d, failed=%d\n", ss->id, ss->is_active, ss->failed);
                }
                if (ss == NULL || !ss->is_active) {
                    // Primary SS down - try cache, then backup, then failover to another SS
                    printf("  → SS unavailable, trying cache/backup/failover\n");
                    char cache_path[MAX_PATH];
                    snprintf(cache_path, sizeof(cache_path), "./cache/%s", msg.filename);
                    
                    FILE *cache_file = fopen(cache_path, "r");
                    if (cache_file != NULL) {
                        size_t bytes_read = fread(msg.data, 1, sizeof(msg.data) - 1, cache_file);
                        msg.data[bytes_read] = '\0';
                        fclose(cache_file);
                        
                        msg.error_code = RESP_SUCCESS;
                        msg.data_length = bytes_read;
                        send_message(client_socket, &msg);
                        
                        char log_msg[512];
                        snprintf(log_msg, sizeof(log_msg), "READ from cache for '%s' by %s (SS down)", 
                                 msg.filename, client_username);
                        log_message("naming_server", log_msg);
                        printf("  ✓ Served from cache (SS unavailable)\n");
                        break;
                    }
                    
                    // Try backup directory
                    char backup_path[MAX_PATH];
                    snprintf(backup_path, sizeof(backup_path), "./backups/%s/%s", 
                             entry->info.storage_server_id, msg.filename);
                    
                    FILE *backup_file = fopen(backup_path, "r");
                    if (backup_file != NULL) {
                        size_t bytes_read = fread(msg.data, 1, sizeof(msg.data) - 1, backup_file);
                        msg.data[bytes_read] = '\0';
                        fclose(backup_file);
                        
                        // Create cache copy for future use
                        mkdir("./cache", 0777);
                        FILE *cache_copy = fopen(cache_path, "w");
                        if (cache_copy) {
                            fwrite(msg.data, 1, bytes_read, cache_copy);
                            fclose(cache_copy);
                        }
                        
                        msg.error_code = RESP_SUCCESS;
                        msg.data_length = bytes_read;
                        send_message(client_socket, &msg);
                        
                        char log_msg[512];
                        snprintf(log_msg, sizeof(log_msg), "READ from backup for '%s' by %s (cached)", 
                                 msg.filename, client_username);
                        log_message("naming_server", log_msg);
                        printf("  ✓ Served from backup and cached (SS unavailable)\n");
                        break;
                    }
                    
                    // Try to failover to another active SS
                    StorageServer *failover_ss = get_available_ss();
                    if (failover_ss != NULL && failover_ss != ss) {
                        printf("  → Failing over to %s\n", failover_ss->id);
                        strncpy(entry->info.storage_server_id, failover_ss->id, sizeof(entry->info.storage_server_id));
                        
                        msg.error_code = RESP_SS_INFO;
                        strncpy(msg.ss_ip, failover_ss->ip, sizeof(msg.ss_ip));
                        msg.ss_port = failover_ss->client_port;
                        snprintf(msg.data, sizeof(msg.data), "Failover to %s:%d", failover_ss->ip, failover_ss->client_port);
                        send_message(client_socket, &msg);
                        
                        char log_msg[512];
                        snprintf(log_msg, sizeof(log_msg), "READ failover for '%s' to %s", msg.filename, failover_ss->id);
                        log_message("naming_server", log_msg);
                        break;
                    }
                    
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable and no backup/cache found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d", ss->ip, ss->client_port);
                send_message(client_socket, &msg);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "READ request for '%s' by %s - forwarded to %s", msg.filename, client_username, ss->id);
                log_message("naming_server", log_msg);
                break;
            }
            
            case MSG_STREAM: {
                printf("→ STREAM request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to stream '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    // Primary SS down - try cache, then backup, then failover
                    char cache_path[MAX_PATH];
                    snprintf(cache_path, sizeof(cache_path), "./cache/%s", msg.filename);
                    
                    FILE *cache_file = fopen(cache_path, "r");
                    if (cache_file != NULL) {
                        size_t bytes_read = fread(msg.data, 1, sizeof(msg.data) - 1, cache_file);
                        msg.data[bytes_read] = '\0';
                        fclose(cache_file);
                        
                        msg.error_code = RESP_SUCCESS;
                        msg.data_length = bytes_read;
                        send_message(client_socket, &msg);
                        
                        char log_msg[512];
                        snprintf(log_msg, sizeof(log_msg), "STREAM from cache for '%s' by %s", 
                                 msg.filename, client_username);
                        log_message("naming_server", log_msg);
                        printf("  ✓ Streamed from cache\n");
                        break;
                    }
                    
                    // Try backup
                    char backup_path[MAX_PATH];
                    snprintf(backup_path, sizeof(backup_path), "./backups/%s/%s", 
                             entry->info.storage_server_id, msg.filename);
                    
                    FILE *backup_file = fopen(backup_path, "r");
                    if (backup_file != NULL) {
                        size_t bytes_read = fread(msg.data, 1, sizeof(msg.data) - 1, backup_file);
                        msg.data[bytes_read] = '\0';
                        fclose(backup_file);
                        
                        // Cache it
                        mkdir("./cache", 0777);
                        FILE *cache_copy = fopen(cache_path, "w");
                        if (cache_copy) {
                            fwrite(msg.data, 1, bytes_read, cache_copy);
                            fclose(cache_copy);
                        }
                        
                        msg.error_code = RESP_SUCCESS;
                        msg.data_length = bytes_read;
                        send_message(client_socket, &msg);
                        
                        char log_msg[512];
                        snprintf(log_msg, sizeof(log_msg), "STREAM from backup for '%s' (cached)", msg.filename);
                        log_message("naming_server", log_msg);
                        printf("  ✓ Streamed from backup and cached\n");
                        break;
                    }
                    
                    // Failover to another SS
                    StorageServer *failover_ss = get_available_ss();
                    if (failover_ss != NULL && failover_ss != ss) {
                        printf("  → Failing over to %s\n", failover_ss->id);
                        strncpy(entry->info.storage_server_id, failover_ss->id, sizeof(entry->info.storage_server_id));
                        
                        msg.error_code = RESP_SS_INFO;
                        strncpy(msg.ss_ip, failover_ss->ip, sizeof(msg.ss_ip));
                        msg.ss_port = failover_ss->client_port;
                        snprintf(msg.data, sizeof(msg.data), "Failover to %s:%d", failover_ss->ip, failover_ss->client_port);
                        send_message(client_socket, &msg);
                        break;
                    }
                    
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable and no backup/cache found");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d", ss->ip, ss->client_port);
                send_message(client_socket, &msg);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "STREAM request for '%s' by %s - forwarded to %s", msg.filename, client_username, ss->id);
                log_message("naming_server", log_msg);
                break;
            }
            
            case MSG_DELETE: {
                printf("→ DELETE request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only owner can delete file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active || ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                
                struct Message ss_response;
                recv_message(ss->ss_socket, &ss_response);
                
                if (ss_response.error_code == RESP_SUCCESS) {
                    delete_file_entry(msg.filename);
                    invalidate_search_cache();
                    
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "File '%s' deleted successfully!", msg.filename);
                    
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Deleted file '%s' by %s", msg.filename, client_username);
                    log_message("naming_server", log_msg);
                } else {
                    msg.error_code = ss_response.error_code;
                    strncpy(msg.data, ss_response.data, sizeof(msg.data));
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            // Continue with rest of message handlers...
            // (VIEW, WRITE, INFO, STREAM, UNDO, EXEC, SEARCH, folders, checkpoints, access control, etc.)
            // For brevity, I'll include key ones and indicate where others go
            
            case MSG_VIEW: {
                printf("→ VIEW request from %s (flags: %d)\n", client_username, msg.flags);
                
                int show_all = (msg.flags & 1);
                int show_details = (msg.flags & 2);
                
                char file_list[MAX_DATA] = "";
                int count = 0;
                
                // Add storage server list at the beginning
                char ss_list[512] = "Available Storage Servers: ";
                StorageServer *ss = storage_servers;
                int ss_count = 0;
                while (ss != NULL) {
                    if (ss->is_active) {
                        if (ss_count > 0) strcat(ss_list, ", ");
                        strncat(ss_list, ss->id, sizeof(ss_list) - strlen(ss_list) - 1);
                        ss_count++;
                    }
                    ss = ss->next;
                }
                if (ss_count == 0) {
                    strcat(ss_list, "None");
                }
                strcat(ss_list, "\n\n");
                strncat(file_list, ss_list, sizeof(file_list) - strlen(file_list) - 1);
                
                pthread_mutex_lock(&table_lock);
                for (int i = 0; i < HASH_TABLE_SIZE; i++) {
                    FileEntry *entry = file_table[i];
                    while (entry != NULL) {
                        int has_access = show_all ? 1 : check_permission(entry, client_username, 0);
                        
                        if (has_access) {
                            char line[1024];
                            if (show_details) {
                                char access_indicator = ' ';
                                if (strcmp(entry->info.owner, client_username) == 0) {
                                    access_indicator = 'O';
                                } else if (check_permission(entry, client_username, 1)) {
                                    access_indicator = 'W';
                                } else if (check_permission(entry, client_username, 0)) {
                                    access_indicator = 'R';
                                } else {
                                    access_indicator = '-';
                                }
                                
                                // Get real-time stats from storage server or backup
                                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                                if (ss != NULL && ss->is_active && ss->ss_socket >= 0) {
                                    struct Message ss_msg;
                                    memset(&ss_msg, 0, sizeof(ss_msg));
                                    ss_msg.type = MSG_INFO;
                                    strncpy(ss_msg.filename, entry->info.name, sizeof(ss_msg.filename));
                                    
                                    if (send_message(ss->ss_socket, &ss_msg) > 0) {
                                        struct Message ss_response;
                                        if (recv_message(ss->ss_socket, &ss_response) > 0 && 
                                            ss_response.error_code == RESP_SUCCESS) {
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
                                         access_indicator, entry->info.name, entry->info.owner,
                                         entry->info.size, entry->info.word_count, entry->info.char_count);
                            } else {
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
                    snprintf(msg.data, sizeof(msg.data), show_all ? "No files in the system" : "No files you have access to");
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
                send_message(client_socket, &msg);
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
                break;
            }
            
            case MSG_ADD_ACCESS: {
                printf("→ ADDACCESS request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the owner can grant access");
                    send_message(client_socket, &msg);
                    break;
                }
                
                char target_user[MAX_USERNAME];
                sscanf(msg.data, "%s", target_user);
                
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
                    break;
                }
                
                int can_read = (msg.flags & 1) ? 1 : 0;
                int can_write = (msg.flags & 2) ? 1 : 0;
                if (can_write) can_read = 1;
                
                pthread_mutex_lock(&table_lock);
                int result = add_access(entry, target_user, can_read, can_write);
                pthread_mutex_unlock(&table_lock);
                
                msg.error_code = RESP_SUCCESS;
                if (result == 0) {
                    snprintf(msg.data, sizeof(msg.data), "Granted %s access to '%s' for user '%s'",
                             can_write ? "write" : "read", msg.filename, target_user);
                } else {
                    snprintf(msg.data, sizeof(msg.data), "Updated access to %s for user '%s'",
                             can_write ? "write" : "read", target_user);
                }
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "Granted %s access to '%s' for user '%s' by %s",
                         can_write ? "write" : "read", msg.filename, target_user, client_username);
                log_message("naming_server", log_msg);
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_REM_ACCESS: {
                printf("→ REMACCESS request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the owner can revoke access");
                    send_message(client_socket, &msg);
                    break;
                }
                
                char target_user[MAX_USERNAME];
                sscanf(msg.data, "%s", target_user);
                
                if (strcmp(target_user, client_username) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: Owner cannot remove their own access");
                    send_message(client_socket, &msg);
                    break;
                }
                
                pthread_mutex_lock(&table_lock);
                int result = remove_access(entry, target_user);
                pthread_mutex_unlock(&table_lock);
                
                if (result == 1) {
                    msg.error_code = RESP_SUCCESS;
                    snprintf(msg.data, sizeof(msg.data), "Removed all access to '%s' for user '%s'", 
                             msg.filename, target_user);
                    
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Removed access to '%s' for user '%s' by %s",
                             msg.filename, target_user, client_username);
                    log_message("naming_server", log_msg);
                } else {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "User '%s' did not have access to '%s'", 
                             target_user, msg.filename);
                }
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_SEARCH: {
                printf("→ SEARCH request from %s: pattern='%s'\n", client_username, msg.data);
                char *search_results = search_files(msg.data, client_username);
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, search_results, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_CREATEFOLDER: {
                printf("→ CREATEFOLDER request from %s: folder='%s'\n", client_username, msg.filename);
                
                if (strlen(msg.filename) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder name cannot be empty");
                    send_message(client_socket, &msg);
                    break;
                }
                
                int result = create_folder(msg.filename, client_username);
                
                if (result == ERR_FOLDER_EXISTS) {
                    msg.error_code = ERR_FOLDER_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' already exists", msg.filename);
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
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_INFO: {
                printf("→ INFO request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to view this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                
                if (ss != NULL && ss->is_active && ss->ss_socket >= 0) {
                    struct Message ss_msg;
                    memset(&ss_msg, 0, sizeof(ss_msg));
                    ss_msg.type = MSG_INFO;
                    strncpy(ss_msg.filename, msg.filename, sizeof(ss_msg.filename));
                    strncpy(ss_msg.username, client_username, sizeof(ss_msg.username));
                    
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
                } else {
                    // SS is down, try to get info from backup file
                    char backup_path[MAX_PATH];
                    snprintf(backup_path, sizeof(backup_path), "../backups/%s/%s", 
                             entry->info.storage_server_id, msg.filename);
                    
                    FILE *backup_file = fopen(backup_path, "r");
                    if (backup_file != NULL) {
                        fseek(backup_file, 0, SEEK_END);
                        long size = ftell(backup_file);
                        fseek(backup_file, 0, SEEK_SET);
                        
                        // Count words and chars from backup
                        int word_count = 0, char_count = 0;
                        int in_word = 0;
                        int c;
                        while ((c = fgetc(backup_file)) != EOF) {
                            char_count++;
                            if (c == ' ' || c == '\n' || c == '\t') {
                                if (in_word) {
                                    word_count++;
                                    in_word = 0;
                                }
                            } else {
                                in_word = 1;
                            }
                        }
                        if (in_word) word_count++;
                        fclose(backup_file);
                        
                        entry->info.size = size;
                        entry->info.word_count = word_count;
                        entry->info.char_count = char_count;
                        
                        printf("  ✓ File info retrieved from backup (SS unavailable)\n");
                    }
                }
                
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
                break;
            }
            
            case MSG_WRITE: {
                printf("→ WRITE request for '%s' sentence %d from %s\n", 
                       msg.filename, msg.sentence_num, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have write permission");
                    send_message(client_socket, &msg);
                    break;
                }
                
                entry->info.last_modified = time(NULL);
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d for write", ss->ip, ss->client_port);
                send_message(client_socket, &msg);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "WRITE request for '%s' sentence %d by %s - forwarded to %s", 
                         msg.filename, msg.sentence_num, client_username, ss->id);
                log_message("naming_server", log_msg);
                break;
            }
            
            case MSG_UNDO: {
                printf("→ UNDO request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You need write permission to undo");
                    send_message(client_socket, &msg);
                    break;
                }
                
                entry->info.last_modified = time(NULL);
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SS_INFO;
                strncpy(msg.ss_ip, ss->ip, sizeof(msg.ss_ip));
                msg.ss_port = ss->client_port;
                snprintf(msg.data, sizeof(msg.data), "Connect to %s:%d for undo", ss->ip, ss->client_port);
                send_message(client_socket, &msg);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "UNDO request for '%s' by %s - forwarded to %s", 
                         msg.filename, client_username, ss->id);
                log_message("naming_server", log_msg);
                break;
            }
            
            case MSG_EXEC: {
                printf("→ EXEC request for '%s' from %s\n", msg.filename, client_username);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You need read permission to execute this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                entry->info.last_accessed = time(NULL);
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                int ss_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_socket < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to connect to storage server");
                    send_message(client_socket, &msg);
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
                    break;
                }
                
                struct Message read_msg;
                memset(&read_msg, 0, sizeof(read_msg));
                read_msg.type = MSG_READ;
                strncpy(read_msg.filename, msg.filename, sizeof(read_msg.filename));
                
                if (send_message(ss_socket, &read_msg) < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read file from storage");
                    send_message(client_socket, &msg);
                    close(ss_socket);
                    break;
                }
                
                memset(&read_msg, 0, sizeof(read_msg));
                if (recv_message(ss_socket, &read_msg) < 0) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read file from storage");
                    send_message(client_socket, &msg);
                    close(ss_socket);
                    break;
                }
                
                close(ss_socket);
                
                if (read_msg.error_code != RESP_SUCCESS) {
                    msg.error_code = read_msg.error_code;
                    strncpy(msg.data, read_msg.data, sizeof(msg.data));
                    send_message(client_socket, &msg);
                    break;
                }
                
                char temp_filename[256];
                snprintf(temp_filename, sizeof(temp_filename), "/tmp/exec_%s_%ld.sh", 
                         client_username, time(NULL));
                
                FILE *temp_file = fopen(temp_filename, "w");
                if (!temp_file) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to create temporary script");
                    send_message(client_socket, &msg);
                    break;
                }
                
                fprintf(temp_file, "%s", read_msg.data);
                fclose(temp_file);
                chmod(temp_filename, 0700);
                
                char exec_cmd[512];
                snprintf(exec_cmd, sizeof(exec_cmd), "/bin/bash %s 2>&1", temp_filename);
                
                FILE *pipe = popen(exec_cmd, "r");
                if (!pipe) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to execute commands");
                    send_message(client_socket, &msg);
                    unlink(temp_filename);
                    break;
                }
                
                char output[MAX_DATA];
                memset(output, 0, sizeof(output));
                size_t bytes_read = fread(output, 1, sizeof(output) - 1, pipe);
                output[bytes_read] = '\0';
                
                pclose(pipe);
                unlink(temp_filename);
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, output, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_VIEWFOLDER: {
                printf("→ VIEWFOLDER request from %s: folder='%s'\n", 
                       client_username, msg.filename);
                
                if (!folder_exists(msg.filename) && strlen(msg.filename) > 0) {
                    msg.error_code = ERR_FOLDER_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                char *file_list = list_folder_files(msg.filename);
                
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, file_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_MOVE: {
                printf("→ MOVE request from %s: file='%s' to folder='%s'\n", 
                       client_username, msg.filename, msg.folder);
                
                FileEntry *entry = lookup_file(msg.filename);
                if (entry == NULL) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File '%s' not found", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (!check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied to move '%s'", msg.filename);
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (strlen(msg.folder) > 0 && !folder_exists(msg.folder)) {
                    msg.error_code = ERR_FOLDER_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Folder '%s' not found", msg.folder);
                    send_message(client_socket, &msg);
                    break;
                }
                
                int result = move_file_to_folder(entry, msg.folder);
                
                if (result == RESP_SUCCESS) {
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
                } else {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to move file");
                }
                send_message(client_socket, &msg);
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
                
                if (strcmp(entry->info.owner, client_username) != 0 && !check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to create checkpoints");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (add_checkpoint(entry, msg.checkpoint_tag, client_username) < 0) {
                    msg.error_code = ERR_FILE_EXISTS;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint with tag '%s' already exists", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                
                if (msg.error_code == RESP_SUCCESS) {
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Created checkpoint '%s' for '%s' by %s",
                             msg.checkpoint_tag, msg.filename, client_username);
                    log_message("naming_server", log_msg);
                }
                
                send_message(client_socket, &msg);
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
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (find_checkpoint(entry, msg.checkpoint_tag) == NULL) {
                    msg.error_code = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint '%s' not found", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                send_message(client_socket, &msg);
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
                
                if (strcmp(entry->info.owner, client_username) != 0 && !check_permission(entry, client_username, 1)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: You don't have permission to revert this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                CheckpointEntry *cp = find_checkpoint(entry, msg.checkpoint_tag);
                if (cp == NULL) {
                    msg.error_code = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint '%s' not found", msg.checkpoint_tag);
                    send_message(client_socket, &msg);
                    break;
                }
                
                StorageServer *ss = find_ss_by_id(entry->info.storage_server_id);
                if (ss == NULL || !ss->is_active) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server unavailable");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (ss->ss_socket < 0) {
                    msg.error_code = ERR_SS_UNAVAILABLE;
                    snprintf(msg.data, sizeof(msg.data), "Error: Storage server not connected");
                    send_message(client_socket, &msg);
                    break;
                }
                
                send_message(ss->ss_socket, &msg);
                recv_message(ss->ss_socket, &msg);
                
                entry->info.last_modified = time(NULL);
                
                if (msg.error_code == RESP_SUCCESS) {
                    char log_msg[512];
                    snprintf(log_msg, sizeof(log_msg), "Reverted '%s' to checkpoint '%s' by %s",
                             msg.filename, msg.checkpoint_tag, client_username);
                    log_message("naming_server", log_msg);
                }
                
                send_message(client_socket, &msg);
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
                
                if (!check_permission(entry, client_username, 0)) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Permission denied");
                    send_message(client_socket, &msg);
                    break;
                }
                
                char *checkpoint_list = list_checkpoints(entry);
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, checkpoint_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
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
                
                if (strcmp(entry->info.owner, client_username) == 0) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Error: You already own this file");
                    send_message(client_socket, &msg);
                    break;
                }
                
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
                
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the file owner can view access requests");
                    send_message(client_socket, &msg);
                    break;
                }
                
                char *request_list = list_access_requests(entry);
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, request_list, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
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
                
                if (strcmp(entry->info.owner, client_username) != 0) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), "Error: Only the file owner can respond to access requests");
                    send_message(client_socket, &msg);
                    break;
                }
                
                if (respond_to_request(entry, msg.request_id, msg.flags) < 0) {
                    msg.error_code = ERR_REQUEST_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Request ID %d not found or already processed", msg.request_id);
                    send_message(client_socket, &msg);
                    break;
                }
                
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Request %s", msg.flags ? "approved" : "denied");
                send_message(client_socket, &msg);
                break;
            }
            
            default:
                printf("→ Unknown request type: %d\n", msg.type);
                msg.error_code = ERR_INVALID_REQUEST;
                snprintf(msg.data, sizeof(msg.data), "Error: Invalid request type");
                send_message(client_socket, &msg);
        }
    }
    
    close(client_socket);
    return NULL;
}

int main() {
    int server_socket, *client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    printf("=== Naming Server (Modular Version) ===\n");
    printf("Starting on port %d...\n", NS_PORT);
    
    // Register signal handlers
    signal(SIGINT, shutdown_system);
    signal(SIGTERM, shutdown_system);
    signal(SIGHUP, shutdown_system);
    
    // Initialize all modules
    init_file_table();
    init_storage_servers();
    init_folders();
    init_search_cache();
    init_users_and_sessions();
    
    // Create cache directory
    mkdir("./cache", 0777);
    printf("✓ Cache directory ready\n");
    
    // Get and display working directory
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("📂 Naming Server Working Directory: %s\n", cwd);
        printf("   Cache:   %s/cache/\n", cwd);
        printf("   Backups: %s/backups/\n", cwd);
    }
    
    // Load file registry from disk (preserves ACLs across restarts)
    load_file_registry("./naming_server/registry.dat");
    
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
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NS_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Naming Server is running and waiting for connections...\n");
    printf("Type 'SHUTDOWN' to gracefully shutdown the server\n\n");
    log_message("naming_server", "Server started successfully");
    
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    
    // Accept connections
    while (!shutdown_flag) {
        char cmd[256];
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            cmd[strcspn(cmd, "\n")] = 0;
            
            if (strcmp(cmd, "SHUTDOWN") == 0) {
                shutdown_system(0);
            }
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
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
