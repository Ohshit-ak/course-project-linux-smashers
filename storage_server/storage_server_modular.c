/*
 * Storage Server - Main Entry Point (Modular Version)
 * 
 * This modular version separates file operations, sentence parsing,
 * lock management, and undo management into dedicated modules for
 * better maintainability and code organization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>

// Common includes
#include "../common/protocol.h"
#include "../common/utils.h"

// Module includes
#include "file_operations.h"
#include "sentence_parser.h"
#include "lock_manager.h"
#include "undo_manager.h"

// Global state
char ns_ip[16];
int ns_port;
int nm_port;  // Port for NS communication
int client_port;  // Port for client communication
char ss_id[64];
char storage_dir[MAX_PATH];  // Dynamic: ../storage/SS1/
char backup_dir[MAX_PATH];   // Dynamic: ../backups/SS1/

// Forward declarations
void* handle_client(void *arg);
void* handle_ns_commands(void *arg);
void* accept_ns_connections(void *arg);

// Register with naming server
int register_with_ns() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_error("storage_server", "Failed to create socket for NS registration");
        return -1;
    }
    
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    inet_pton(AF_INET, ns_ip, &ns_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&ns_addr, sizeof(ns_addr)) < 0) {
        log_error("storage_server", "Failed to connect to Naming Server");
        close(sock);
        return -1;
    }
    
    // Prepare registration message
    struct SSRegistration reg;
    strncpy(reg.ss_id, ss_id, sizeof(reg.ss_id));

    // Determine a routable local IP address to advertise to the Naming Server.
    char local_ip[64] = "127.0.0.1";
    {
        int udpsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpsock >= 0) {
            struct sockaddr_in remote;
            memset(&remote, 0, sizeof(remote));
            remote.sin_family = AF_INET;
            remote.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
            if (connect(udpsock, (struct sockaddr*)&remote, sizeof(remote)) == 0) {
                struct sockaddr_in name;
                socklen_t namelen = sizeof(name);
                if (getsockname(udpsock, (struct sockaddr*)&name, &namelen) == 0) {
                    inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip));
                }
            }
            close(udpsock);
        }
    }

    strncpy(reg.ip, local_ip, sizeof(reg.ip));
    reg.nm_port = nm_port;
    reg.client_port = client_port;
    reg.file_count = list_files(reg.files);
    
    struct Message msg;
    msg.type = MSG_REGISTER_SS;
    msg.data_length = sizeof(reg);
    size_t copy_size = sizeof(reg) < sizeof(msg.data) ? sizeof(reg) : sizeof(msg.data);
    memcpy(msg.data, &reg, copy_size);
    
    if (send_message(sock, &msg) < 0) {
        log_error("storage_server", "Failed to send registration");
        close(sock);
        return -1;
    }
    
    // Wait for acknowledgment
    memset(&msg, 0, sizeof(msg));
    if (recv_message(sock, &msg) <= 0 || msg.error_code != RESP_SUCCESS) {
        log_error("storage_server", "Failed to receive registration acknowledgment");
        close(sock);
        return -1;
    }
    
    log_message("storage_server", "Successfully registered with Naming Server");
    printf("Registered with NS. Advertised %d files.\n", reg.file_count);
    printf("✓ Persistent connection to NS established\n");
    
    return sock;
}

// Accept NS connections (runs in separate thread)
void* accept_ns_connections(void *arg) {
    int nm_listener = *(int*)arg;
    
    while (1) {
        struct sockaddr_in ns_addr;
        socklen_t ns_len = sizeof(ns_addr);
        int *ns_sock = malloc(sizeof(int));
        *ns_sock = accept(nm_listener, (struct sockaddr*)&ns_addr, &ns_len);
        
        if (*ns_sock < 0) {
            free(ns_sock);
            continue;
        }
        
        printf("✓ Naming Server connected for commands\n");
        
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_ns_commands, ns_sock) != 0) {
            free(ns_sock);
            continue;
        }
        
        pthread_detach(thread);
    }
    
    return NULL;
}

// Handle NS commands
void* handle_ns_commands(void *arg) {
    int nm_socket = *(int*)arg;
    free(arg);
    
    struct Message msg;
    
    printf("✓ NS connection handler started\n");
    
    while (1) {
        if (recv_message(nm_socket, &msg) <= 0) {
            printf("✗ Lost connection to Naming Server\n");
            break;
        }
        
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Received command from NS: type=%d, file=%s", 
                 msg.type, msg.filename);
        log_message("storage_server", log_msg);
        
        int result = RESP_SUCCESS;
        
        switch (msg.type) {
            case MSG_CREATE:
                printf("→ CREATE command for '%s'\n", msg.filename);
                result = create_file(msg.filename);
                if (result == RESP_SUCCESS) {
                    snprintf(msg.data, sizeof(msg.data), "File created on storage server");
                    printf("  ✓ File created\n");
                    snprintf(log_msg, sizeof(log_msg), "Created file: %s", msg.filename);
                    log_message("storage_server", log_msg);
                } else if (result == ERR_FILE_EXISTS) {
                    snprintf(msg.data, sizeof(msg.data), "File already exists");
                    printf("  ✗ File already exists\n");
                    snprintf(log_msg, sizeof(log_msg), "File already exists: %s", msg.filename);
                    log_message("storage_server", log_msg);
                } else {
                    snprintf(msg.data, sizeof(msg.data), "Failed to create file");
                    printf("  ✗ Failed to create file\n");
                    log_error("storage_server", "Failed to create file");
                }
                break;
                
            case MSG_DELETE:
                printf("→ DELETE command for '%s'\n", msg.filename);
                result = delete_file(msg.filename);
                if (result == RESP_SUCCESS) {
                    snprintf(msg.data, sizeof(msg.data), "File deleted from storage server");
                    printf("  ✓ File deleted\n");
                    snprintf(log_msg, sizeof(log_msg), "Deleted file: %s", msg.filename);
                    log_message("storage_server", log_msg);
                } else if (result == ERR_FILE_NOT_FOUND) {
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    printf("  ✗ File not found\n");
                } else {
                    snprintf(msg.data, sizeof(msg.data), "Failed to delete file");
                    printf("  ✗ Failed to delete file\n");
                    log_error("storage_server", "Failed to delete file");
                }
                break;
                
            case MSG_CREATEFOLDER: {
                printf("→ CREATEFOLDER command for '%s'\n", msg.filename);
                
                char dirpath[MAX_PATH];
                snprintf(dirpath, sizeof(dirpath), "%s%s", storage_dir, msg.filename);
                
                char *path_copy = strdup(dirpath);
                char *p = path_copy + strlen(storage_dir);
                
                for (; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(path_copy, 0777);
                        *p = '/';
                    }
                }
                mkdir(path_copy, 0777);
                
                free(path_copy);
                
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Folder created on storage server");
                printf("  ✓ Directory created: %s\n", dirpath);
                snprintf(log_msg, sizeof(log_msg), "Created folder: %s", msg.filename);
                log_message("storage_server", log_msg);
                break;
            }
            
            case MSG_MOVE: {
                printf("→ MOVE command: '%s' to folder '%s'\n", msg.filename, msg.folder);
                
                char old_path[MAX_PATH], new_path[MAX_PATH];
                snprintf(old_path, sizeof(old_path), "%s%s", storage_dir, msg.filename);
                
                if (strlen(msg.folder) == 0) {
                    snprintf(new_path, sizeof(new_path), "%s%s", storage_dir, msg.filename);
                } else {
                    char folder_path[MAX_PATH];
                    snprintf(folder_path, sizeof(folder_path), "%s%s", storage_dir, msg.folder);
                    mkdir(folder_path, 0777);
                    
                    snprintf(new_path, sizeof(new_path), "%s%s/%s", storage_dir, msg.folder, msg.filename);
                }
                
                if (rename(old_path, new_path) == 0) {
                    result = RESP_SUCCESS;
                    printf("  ✓ File moved from %s to %s\n", old_path, new_path);
                    snprintf(log_msg, sizeof(log_msg), "Moved file '%s' to folder '%s'", 
                             msg.filename, msg.folder);
                    log_message("storage_server", log_msg);
                } else {
                    result = ERR_SERVER_ERROR;
                    printf("  ✗ Failed to move file\n");
                    log_error("storage_server", "Failed to move file");
                }
                
                snprintf(msg.data, sizeof(msg.data), "File moved on storage server");
                break;
            }

            case MSG_CHECKPOINT: {
                printf("→ CHECKPOINT command: '%s' with tag '%s'\n", msg.filename, msg.checkpoint_tag);
                
                char checkpoints_dir[MAX_PATH];
                snprintf(checkpoints_dir, sizeof(checkpoints_dir), "%scheckpoints/", storage_dir);
                mkdir(checkpoints_dir, 0777);
                
                char src_path[MAX_PATH], checkpoint_path[MAX_PATH];
                snprintf(src_path, sizeof(src_path), "%s%s", storage_dir, msg.filename);
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s", 
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                int src_fd = open(src_path, O_RDONLY);
                if (src_fd < 0) {
                    result = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Source file not found");
                    printf("  ✗ Source file not found\n");
                    break;
                }
                
                int dest_fd = open(checkpoint_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (dest_fd < 0) {
                    close(src_fd);
                    result = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to create checkpoint file");
                    printf("  ✗ Failed to create checkpoint file\n");
                    break;
                }
                
                char buffer[4096];
                ssize_t bytes;
                while ((bytes = read(src_fd, buffer, sizeof(buffer))) > 0) {
                    write(dest_fd, buffer, bytes);
                }
                
                close(src_fd);
                close(dest_fd);
                
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Checkpoint '%s' created successfully", msg.checkpoint_tag);
                printf("  ✓ Checkpoint created: %s\n", checkpoint_path);
                snprintf(log_msg, sizeof(log_msg), "Created checkpoint '%s' for '%s'", 
                         msg.checkpoint_tag, msg.filename);
                log_message("storage_server", log_msg);
                break;
            }

            case MSG_VIEWCHECKPOINT: {
                printf("→ VIEWCHECKPOINT command: '%s' with tag '%s'\n", msg.filename, msg.checkpoint_tag);
                
                char checkpoint_path[MAX_PATH];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s",
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                int fd = open(checkpoint_path, O_RDONLY);
                if (fd < 0) {
                    result = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint not found");
                    printf("  ✗ Checkpoint not found\n");
                    break;
                }
                
                ssize_t bytes = read(fd, msg.data, sizeof(msg.data) - 1);
                close(fd);
                
                if (bytes < 0) {
                    result = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read checkpoint");
                } else {
                    msg.data[bytes] = '\0';
                    result = RESP_SUCCESS;
                    printf("  ✓ Checkpoint read (%zd bytes)\n", bytes);
                }
                break;
            }

            case MSG_REVERT: {
                printf("→ REVERT command: '%s' to tag '%s'\n", msg.filename, msg.checkpoint_tag);
                
                char file_path[MAX_PATH], checkpoint_path[MAX_PATH];
                snprintf(file_path, sizeof(file_path), "%s%s", storage_dir, msg.filename);
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s",
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                int cp_fd = open(checkpoint_path, O_RDONLY);
                if (cp_fd < 0) {
                    result = ERR_CHECKPOINT_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: Checkpoint not found");
                    printf("  ✗ Checkpoint not found\n");
                    break;
                }
                
                char buffer[4096];
                ssize_t bytes = read(cp_fd, buffer, sizeof(buffer));
                close(cp_fd);
                
                if (bytes < 0) {
                    result = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Error: Failed to read checkpoint");
                    break;
                }
                
                int file_fd = open(file_path, O_WRONLY | O_TRUNC);
                if (file_fd < 0) {
                    result = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "Error: File not found");
                    printf("  ✗ File not found\n");
                    break;
                }
                
                write(file_fd, buffer, bytes);
                close(file_fd);
                
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "File reverted to checkpoint '%s'", msg.checkpoint_tag);
                printf("  ✓ File reverted to checkpoint '%s'\n", msg.checkpoint_tag);
                snprintf(log_msg, sizeof(log_msg), "Reverted '%s' to checkpoint '%s'", 
                         msg.filename, msg.checkpoint_tag);
                log_message("storage_server", log_msg);
                break;
            }

            case MSG_HEARTBEAT: {
                // Respond to heartbeat immediately
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "alive");
                // Log heartbeat (optional - can be commented out to reduce spam)
                // printf("[DEBUG] Heartbeat response sent\n");
                break;
            }

            case MSG_SHUTDOWN: {
                printf("→ SHUTDOWN command received from naming server\n");
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Shutting down");
                send_message(nm_socket, &msg);
                printf("✓ Storage server %s shutting down\n", ss_id);
                exit(0);
            }

            case MSG_REPLICATE: {
                printf("→ REPLICATE command: replicating data\n");
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Replication received");
                break;
            }

            case MSG_INFO: {
                printf("→ INFO request for '%s' from naming server\n", msg.filename);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                if (!file_exists(filepath)) {
                    result = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    printf("  ✗ File not found\n");
                    break;
                }
                
                FILE *fp = fopen(filepath, "r");
                if (!fp) {
                    result = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to open file");
                    printf("  ✗ Failed to open file\n");
                    break;
                }
                
                long size = 0;
                int word_count = 0;
                int char_count = 0;
                int in_word = 0;
                int c;
                
                while ((c = fgetc(fp)) != EOF) {
                    size++;
                    
                    if (c != '\n' && c != '\r') {
                        char_count++;
                    }
                    
                    if (isspace(c)) {
                        in_word = 0;
                    } else {
                        if (!in_word) {
                            word_count++;
                            in_word = 1;
                        }
                    }
                }
                fclose(fp);
                
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "%ld:%d:%d", size, word_count, char_count);
                printf("  ✓ File stats: %ld bytes, %d words, %d chars\n", size, word_count, char_count);
                
                snprintf(log_msg, sizeof(log_msg), "INFO completed for '%s' - %ld bytes, %d words, %d chars", 
                         msg.filename, size, word_count, char_count);
                log_message("storage_server", log_msg);
                break;
            }
                
            default:
                result = ERR_INVALID_REQUEST;
                snprintf(msg.data, sizeof(msg.data), "Invalid command");
                printf("→ Invalid command type: %d\n", msg.type);
        }
        
        msg.error_code = result;
        send_message(nm_socket, &msg);
    }
    
    close(nm_socket);
    return NULL;
}

// Handle client connection (this function is quite large - contains READ, WRITE, STREAM, UNDO logic)
void* handle_client(void *arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    struct Message msg;
    
    while (1) {
        if (recv_message(client_socket, &msg) <= 0) {
            break;
        }
        
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Client request: type=%d, file=%s", 
                 msg.type, msg.filename);
        log_message("storage_server", log_msg);
        
        switch (msg.type) {
            case MSG_READ: {
                printf("→ READ request for '%s'\n", msg.filename);
                
                char buffer[MAX_DATA];
                int result = read_file(msg.filename, buffer, sizeof(buffer));
                
                if (result == RESP_SUCCESS) {
                    msg.error_code = RESP_SUCCESS;
                    strncpy(msg.data, buffer, sizeof(msg.data) - 1);
                    msg.data[sizeof(msg.data) - 1] = '\0';
                    printf("  ✓ File read successfully (%ld bytes)\n", strlen(buffer));
                    snprintf(log_msg, sizeof(log_msg), "READ completed for '%s' - %ld bytes", 
                             msg.filename, strlen(buffer));
                    log_message("storage_server", log_msg);
                } else {
                    msg.error_code = result;
                    if (result == ERR_FILE_NOT_FOUND) {
                        snprintf(msg.data, sizeof(msg.data), "File not found");
                        printf("  ✗ File not found\n");
                    } else {
                        snprintf(msg.data, sizeof(msg.data), "Failed to read file");
                        printf("  ✗ Failed to read file\n");
                    }
                }
                
                send_message(client_socket, &msg);
                break;
            }
            
            case MSG_STREAM: {
                printf("→ STREAM request for '%s'\n", msg.filename);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                char buffer[MAX_DATA];
                int result = read_file(msg.filename, buffer, sizeof(buffer));
                
                if (result != RESP_SUCCESS) {
                    msg.error_code = result;
                    snprintf(msg.data, sizeof(msg.data), "Failed to read file");
                    send_message(client_socket, &msg);
                    break;
                }
                
                int word_count = 0;
                char **words = parse_words(buffer, &word_count);
                
                for (int i = 0; i < word_count; i++) {
                    msg.error_code = RESP_DATA;
                    strncpy(msg.data, words[i], sizeof(msg.data) - 1);
                    send_message(client_socket, &msg);
                    free(words[i]);
                    usleep(100000); // 0.1s delay
                }
                
                if (words) free(words);
                
                msg.error_code = RESP_SUCCESS;
                strcpy(msg.data, "STREAM_END");
                send_message(client_socket, &msg);
                printf("  ✓ Stream completed (%d words)\n", word_count);
                snprintf(log_msg, sizeof(log_msg), "STREAM completed for '%s' - %d words", 
                         msg.filename, word_count);
                log_message("storage_server", log_msg);
                break;
            }
            
            case MSG_UNDO: {
                printf("→ UNDO request for '%s' by %s\n", msg.filename, msg.username);
                
                UndoState *undo = get_undo_state(msg.filename);
                
                if (undo && undo->undo_performed) {
                    msg.error_code = ERR_INVALID_REQUEST;
                    snprintf(msg.data, sizeof(msg.data), "Cannot undo twice consecutively");
                    send_message(client_socket, &msg);
                    printf("  ✗ Cannot undo twice consecutively\n");
                    break;
                }
                
                char filepath[MAX_PATH], backup_path[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                snprintf(backup_path, sizeof(backup_path), "%s%s.backup", backup_dir, msg.filename);
                
                if (!file_exists(backup_path)) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "No backup available to undo");
                    send_message(client_socket, &msg);
                    printf("  ✗ No backup available\n");
                    break;
                }
                
                // Copy backup back to original
                FILE *src = fopen(backup_path, "r");
                FILE *dest = fopen(filepath, "w");
                
                if (!src || !dest) {
                    if (src) fclose(src);
                    if (dest) fclose(dest);
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to restore backup");
                    send_message(client_socket, &msg);
                    break;
                }
                
                char buffer[4096];
                size_t bytes;
                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                    fwrite(buffer, 1, bytes, dest);
                }
                
                fclose(src);
                fclose(dest);
                
                set_undo_state(msg.filename, 1);
                
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Undo successful");
                send_message(client_socket, &msg);
                printf("  ✓ Undo completed\n");
                snprintf(log_msg, sizeof(log_msg), "UNDO completed for '%s' by %s", 
                         msg.filename, msg.username);
                log_message("storage_server", log_msg);
                break;
            }
            
            case MSG_WRITE: {
                printf("→ WRITE request for '%s' sentence %d from %s\n", 
                       msg.filename, msg.sentence_num, msg.username);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                if (!file_exists(filepath)) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Read file content
                FILE *fp = fopen(filepath, "r");
                if (!fp) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to open file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to open file\n");
                    break;
                }
                
                char content[MAX_DATA * 4] = "";
                char line[MAX_DATA];
                while (fgets(line, sizeof(line), fp)) {
                    size_t len = strlen(line);
                    if (len > 0 && line[len - 1] == '\n') {
                        line[len - 1] = '\0';
                    }
                    if (len > 0 && line[len - 1] == '\r') {
                        line[len - 1] = '\0';
                    }
                    strncat(content, line, sizeof(content) - strlen(content) - 1);
                    if (strlen(content) < sizeof(content) - 1) {
                        strncat(content, " ", sizeof(content) - strlen(content) - 1);
                    }
                }
                fclose(fp);
                
                // Parse into sentences using module function
                int sentence_count = 0;
                char **sentences = parse_sentences(content, &sentence_count);
                
                if (strlen(content) == 0 || sentence_count == 0) {
                    sentences = NULL;
                    sentence_count = 0;
                }
                
                // Validate sentence access (same logic as original)
                if (sentence_count == 0) {
                    if (msg.sentence_num != 0) {
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = 0;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "File is empty. Only sentence 0 is accessible.");
                        send_message(client_socket, &msg);
                        printf("  ✗ File empty, only sentence 0 allowed\n");
                        break;
                    }
                    sentence_count = 1;
                    sentences = malloc(sizeof(char*));
                    sentences[0] = strdup("");
                } else {
                    if (msg.sentence_num < 0) {
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = sentence_count - 1;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "Invalid sentence number. Must be non-negative.");
                        send_message(client_socket, &msg);
                        if (sentences) {
                            for (int i = 0; i < sentence_count; i++) {
                                free(sentences[i]);
                            }
                            free(sentences);
                        }
                        break;
                    }
                    
                    if (msg.sentence_num == sentence_count) {
                        if (sentence_count > 0 && !sentence_has_delimiter(sentences[sentence_count - 1])) {
                            msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                            msg.word_index = sentence_count - 1;
                            snprintf(msg.data, sizeof(msg.data), 
                                     "Cannot access sentence %d. Previous sentence must end with delimiter.",
                                     msg.sentence_num);
                            send_message(client_socket, &msg);
                            if (sentences) {
                                for (int i = 0; i < sentence_count; i++) {
                                    free(sentences[i]);
                                }
                                free(sentences);
                            }
                            break;
                        }
                        
                        sentence_count++;
                        sentences = realloc(sentences, sizeof(char*) * sentence_count);
                        sentences[msg.sentence_num] = strdup("");
                    } else if (msg.sentence_num > sentence_count) {
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = sentence_count;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "Cannot skip sentences. Can access 0 to %d.", sentence_count);
                        send_message(client_socket, &msg);
                        if (sentences) {
                            for (int i = 0; i < sentence_count; i++) {
                                free(sentences[i]);
                            }
                            free(sentences);
                        }
                        break;
                    }
                }
                
                // Try to lock sentence using module function
                if (!add_sentence_lock(msg.filename, msg.sentence_num, msg.username)) {
                    SentenceLock *lock = find_sentence_lock(msg.filename, msg.sentence_num);
                    msg.error_code = ERR_FILE_LOCKED;
                    snprintf(msg.data, sizeof(msg.data), "%s", lock->username);
                    send_message(client_socket, &msg);
                    printf("  ✗ Sentence locked by %s\n", lock->username);
                    
                    for (int i = 0; i < sentence_count; i++) {
                        free(sentences[i]);
                    }
                    free(sentences);
                    break;
                }
                
                printf("  ✓ Sentence locked for %s\n", msg.username);
                
                // Send current sentence
                char *current_sentence = sentences[msg.sentence_num];
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, current_sentence, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                // Parse sentence into words using module function
                int word_count = 0;
                char **words = parse_words(current_sentence, &word_count);
                
                if (word_count == 0 || words == NULL) {
                    printf("  → Sentence is empty, no words yet\n");
                    word_count = 0;
                    words = NULL;
                }
                
                printf("  → Sentence has %d word(s)\n", word_count);
                
                // Enter edit loop
                int editing = 1;
                while (editing) {
                    struct Message update_msg;
                    memset(&update_msg, 0, sizeof(update_msg));
                    
                    if (recv_message(client_socket, &update_msg) <= 0) {
                        printf("  ✗ Connection lost during edit\n");
                        editing = 0;
                        break;
                    }
                    
                    if (strcmp(update_msg.data, "ETIRW") == 0) {
                        printf("  ✓ ETIRW received - finalizing changes\n");
                        
                        // Rebuild sentence using module function
                        char *new_sentence = rebuild_sentence(words, word_count);
                        free(sentences[msg.sentence_num]);
                        sentences[msg.sentence_num] = new_sentence;
                        
                        // Create backup
                        char backup_path[MAX_PATH];
                        snprintf(backup_path, sizeof(backup_path), "%s%s.backup", backup_dir, msg.filename);
                        
                        FILE *orig = fopen(filepath, "r");
                        FILE *backup = fopen(backup_path, "w");
                        
                        if (orig && backup) {
                            char buf[4096];
                            size_t bytes;
                            while ((bytes = fread(buf, 1, sizeof(buf), orig)) > 0) {
                                fwrite(buf, 1, bytes, backup);
                            }
                        }
                        if (orig) fclose(orig);
                        if (backup) fclose(backup);
                        
                        // Write updated content
                        FILE *out = fopen(filepath, "w");
                        if (out) {
                            for (int i = 0; i < sentence_count; i++) {
                                fputs(sentences[i], out);
                                if (i < sentence_count - 1) {
                                    fputs(" ", out);
                                }
                            }
                            fclose(out);
                        }
                        
                        // Release lock using module function
                        remove_sentence_lock(msg.filename, msg.sentence_num, msg.username);
                        
                        // Update undo state using module function
                        set_undo_state(msg.filename, 0);
                        
                        update_msg.error_code = RESP_SUCCESS;
                        strcpy(update_msg.data, "Changes saved");
                        send_message(client_socket, &update_msg);
                        
                        // Update backup file with new content (non-.backup format)
                        char final_backup_path[MAX_PATH];
                        snprintf(final_backup_path, sizeof(final_backup_path), "%s%s", backup_dir, msg.filename);
                        
                        FILE *backup_src = fopen(filepath, "r");
                        FILE *backup_dst = fopen(final_backup_path, "w");
                        if (backup_src && backup_dst) {
                            char buffer[MAX_DATA * 10];
                            size_t bytes = fread(buffer, 1, sizeof(buffer), backup_src);
                            fwrite(buffer, 1, bytes, backup_dst);
                            fclose(backup_src);
                            fclose(backup_dst);
                            printf("  ✓ Backup updated for '%s'\n", msg.filename);
                        }
                        
                        printf("  ✓ Changes saved and lock released\n");
                        snprintf(log_msg, sizeof(log_msg), "WRITE completed for '%s' sentence %d by %s", 
                                 msg.filename, msg.sentence_num, msg.username);
                        log_message("storage_server", log_msg);
                        editing = 0;
                    } else {
                        // Handle word insert/replace/delete
                        int word_idx = update_msg.word_index;
                        
                        if (word_idx < 0 || word_idx > word_count) {
                            update_msg.error_code = ERR_WORD_OUT_OF_RANGE;
                            update_msg.word_index = word_count;
                            snprintf(update_msg.data, sizeof(update_msg.data), 
                                     "Invalid word index. Can insert at 0 to %d", word_count);
                            send_message(client_socket, &update_msg);
                            continue;
                        }
                        
                        if (strlen(update_msg.data) == 0) {
                            // Delete word
                            if (word_idx < word_count) {
                                free(words[word_idx]);
                                for (int i = word_idx; i < word_count - 1; i++) {
                                    words[i] = words[i + 1];
                                }
                                word_count--;
                                words = realloc(words, sizeof(char*) * word_count);
                                printf("  → Deleted word at index %d\n", word_idx);
                            }
                        } else {
                            // ALWAYS INSERT (not replace)
                            // Insert at word_idx means: shift everything from word_idx onwards to the right
                            word_count++;
                            words = realloc(words, sizeof(char*) * word_count);
                            
                            // Shift words to make space
                            for (int i = word_count - 1; i > word_idx; i--) {
                                words[i] = words[i - 1];
                            }
                            
                            // Insert new word
                            words[word_idx] = strdup(update_msg.data);
                            printf("  → Inserted word '%s' at index %d\n", update_msg.data, word_idx);
                        }
                        
                        update_msg.error_code = RESP_SUCCESS;
                        update_msg.word_index = word_count;
                        send_message(client_socket, &update_msg);
                    }
                }
                
                // Cleanup
                for (int i = 0; i < sentence_count; i++) {
                    free(sentences[i]);
                }
                free(sentences);
                
                if (words) {
                    for (int i = 0; i < word_count; i++) {
                        free(words[i]);
                    }
                    free(words);
                }
                break;
            }
            
            default:
                printf("→ Invalid command type: %d\n", msg.type);
        }
    }
    
    close(client_socket);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s <ss_id> <ns_ip> <ns_port> <client_port>\n", argv[0]);
        printf("Example: %s SS1 127.0.0.1 8080 8081\n", argv[0]);
        return 1;
    }
    
    strncpy(ss_id, argv[1], sizeof(ss_id));
    strncpy(ns_ip, argv[2], sizeof(ns_ip));
    ns_port = atoi(argv[3]);
    client_port = atoi(argv[4]);
    nm_port = client_port + 1000;
    
    printf("=== Storage Server %s (Modular Version) ===\n", ss_id);
    printf("NS: %s:%d\n", ns_ip, ns_port);
    printf("Client Port: %d\n", client_port);
    printf("NM Port: %d\n", nm_port);
    
    // Initialize storage using module function
    init_storage();
    
    // Register with naming server
    int ns_socket = register_with_ns();
    if (ns_socket < 0) {
        fprintf(stderr, "Failed to register with Naming Server\n");
        return 1;
    }
    
    // Start NS command handler thread
    int *ns_sock_ptr = malloc(sizeof(int));
    *ns_sock_ptr = ns_socket;
    pthread_t ns_thread;
    if (pthread_create(&ns_thread, NULL, handle_ns_commands, ns_sock_ptr) != 0) {
        fprintf(stderr, "Failed to create NS command handler thread\n");
        close(ns_socket);
        return 1;
    }
    pthread_detach(ns_thread);
    
    // Create client listener socket
    int client_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (client_listener < 0) {
        perror("Client socket creation failed");
        return 1;
    }
    
    int opt = 1;
    setsockopt(client_listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = INADDR_ANY;
    client_addr.sin_port = htons(client_port);
    
    if (bind(client_listener, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0) {
        perror("Client bind failed");
        return 1;
    }
    
    if (listen(client_listener, 10) < 0) {
        perror("Client listen failed");
        return 1;
    }
    
    printf("Storage Server is running and ready for client connections...\n");
    printf("Type 'DISCONNECT' to shutdown\n\n");
    log_message("storage_server", "Server started successfully");
    
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    
    // Accept client connections
    while (1) {
        char cmd[256];
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            cmd[strcspn(cmd, "\n")] = 0;
            
            if (strcmp(cmd, "DISCONNECT") == 0) {
                printf("\n⚠️  Shutting down...\n");
                printf("✓ Storage server %s shutdown complete\n", ss_id);
                close(client_listener);
                if (ns_socket >= 0) close(ns_socket);
                exit(0);
            }
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_listener, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        int activity = select(client_listener + 1, &readfds, NULL, NULL, &tv);
        
        if (activity > 0 && FD_ISSET(client_listener, &readfds)) {
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            int *client_sock = malloc(sizeof(int));
            *client_sock = accept(client_listener, (struct sockaddr*)&client, &client_len);
            
            if (*client_sock < 0) {
                free(client_sock);
                continue;
            }
            
            printf("Client connected from %s:%d\n", 
                   inet_ntoa(client.sin_addr), ntohs(client.sin_port));
            
            pthread_t thread;
            if (pthread_create(&thread, NULL, handle_client, client_sock) != 0) {
                free(client_sock);
                continue;
            }
            
            pthread_detach(thread);
        }
    }
    
    close(client_listener);
    return 0;
}
