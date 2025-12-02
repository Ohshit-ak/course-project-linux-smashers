/*
 * Storage Server - Main Entry Point
 * 
 * The Storage Server handles actual file storage and operations.
 * It:
 * - Registers with the Naming Server
 * - Stores files on disk
 * - Handles direct client connections for READ/WRITE/STREAM
 * - Manages sentence locks during WRITE operations
 * - Maintains undo history
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
#include "../common/protocol.h"
#include "../common/utils.h"

#define BASE_STORAGE_DIR "../storage/"
#define BASE_BACKUP_DIR "../backups/"

// Sentence lock structure
typedef struct SentenceLock {
    char filename[MAX_FILENAME];
    int sentence_num;
    char locked_by[MAX_USERNAME];
    time_t lock_time;
    struct SentenceLock *next;
} SentenceLock;

// Undo state tracking - prevents consecutive undos
typedef struct UndoState {
    char filename[MAX_FILENAME];
    int last_undo_performed;  // 1 if last operation was undo, 0 otherwise
    struct UndoState *next;
} UndoState;

// Global state
char ns_ip[16];
int ns_port;
int nm_port;  // Port for NS communication
int client_port;  // Port for client communication
char ss_id[64];
char storage_dir[MAX_PATH];  // Dynamic: ../storage/SS1/
char backup_dir[MAX_PATH];   // Dynamic: ../backups/SS1/
SentenceLock *locks = NULL;
UndoState *undo_states = NULL;
pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t undo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations
void* handle_client(void *arg);
void* handle_ns_commands(void *arg);
void* accept_ns_connections(void *arg);

// Initialize storage directories
void init_storage() {
    // Create base directories if they don't exist
    mkdir(BASE_STORAGE_DIR, 0777);
    mkdir(BASE_BACKUP_DIR, 0777);
    
    // Create SS-specific subdirectories: ../storage/SS1/ and ../backups/SS1/
    snprintf(storage_dir, sizeof(storage_dir), "%s%s/", BASE_STORAGE_DIR, ss_id);
    snprintf(backup_dir, sizeof(backup_dir), "%s%s/", BASE_BACKUP_DIR, ss_id);
    
    mkdir(storage_dir, 0777);
    mkdir(backup_dir, 0777);
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Storage directories initialized: %s and %s", storage_dir, backup_dir);
    log_message("storage_server", log_msg);
    printf("Storage: %s\n", storage_dir);
    printf("Backups: %s\n", backup_dir);
}

// List all files in storage directory
int list_files(char files[][MAX_FILENAME]) {
    DIR *dir = opendir(storage_dir);
    if (!dir) {
        log_error("storage_server", "Could not open storage directory");
        return 0;
    }
    
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Regular file
            strncpy(files[count], entry->d_name, MAX_FILENAME);
            count++;
            if (count >= MAX_FILES) break;
        }
    }
    
    closedir(dir);
    return count;
}

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
    // We create a UDP socket and connect to a public address (no packets sent)
    // then read the socket's local address. This works behind NAT on the LAN.
    char local_ip[64] = "127.0.0.1";
    {
        int udpsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpsock >= 0) {
            struct sockaddr_in remote;
            memset(&remote, 0, sizeof(remote));
            remote.sin_family = AF_INET;
            remote.sin_port = htons(53); // DNS port
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
    // Copy only what fits in data buffer
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
    
    // Return the socket to keep connection alive (don't close it!)
    return sock;
}

// Create a file
int create_file(const char *filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (file_exists(filepath)) {
        return ERR_FILE_EXISTS;
    }
    
    // Ensure parent directory exists
    char *last_slash = strrchr(filepath, '/');
    if (last_slash != NULL && last_slash != filepath) {
        char dirpath[MAX_PATH];
        size_t dir_len = last_slash - filepath;
        strncpy(dirpath, filepath, dir_len);
        dirpath[dir_len] = '\0';
        
        // Create directory recursively
        char *p = dirpath + strlen(storage_dir);
        for (; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(dirpath, 0777);
                *p = '/';
            }
        }
        mkdir(dirpath, 0777);
    }
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        log_error("storage_server", "Failed to create file");
        return ERR_SERVER_ERROR;
    }
    
    fclose(fp);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Created file: %s", filename);
    log_message("storage_server", msg);
    
    return RESP_SUCCESS;
}

// Read file content
int read_file(const char *filename, char *buffer, int buffer_size) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        return ERR_SERVER_ERROR;
    }
    
    int bytes_read = fread(buffer, 1, buffer_size - 1, fp);
    buffer[bytes_read] = '\0';
    fclose(fp);
    
    return RESP_SUCCESS;
}

// Delete a file
int delete_file(const char *filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    if (remove(filepath) != 0) {
        log_error("storage_server", "Failed to delete file");
        return ERR_SERVER_ERROR;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Deleted file: %s", filename);
    log_message("storage_server", msg);
    
    return RESP_SUCCESS;
}
int file_info(const char* filename)
{
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return ERR_SERVER_ERROR;
    }
    
    printf("File: %s\n", filename);
    printf("Size: %ld bytes\n", st.st_size);
    printf("Created: %s", ctime(&st.st_ctime));
    printf("Last Modified: %s", ctime(&st.st_mtime));
    printf("Last Accessed: %s", ctime(&st.st_atime));
    
    return RESP_SUCCESS;
}

// Check if a sentence ends with a delimiter
int sentence_has_delimiter(const char *sentence) {
    if (!sentence || strlen(sentence) == 0) return 0;
    
    // Trim trailing whitespace
    int len = strlen(sentence);
    while (len > 0 && isspace(sentence[len - 1])) {
        len--;
    }
    
    if (len == 0) return 0;
    
    char last = sentence[len - 1];
    
    // Check if last character is a delimiter
    if (last != '.' && last != '!' && last != '?') {
        return 0;  // Not a delimiter
    }
    
    // Check if it's a SINGLE delimiter (not multiple like ... or !!!)
    // Look at the character before the last one
    if (len >= 2) {
        char second_last = sentence[len - 2];
        // If second-to-last is also a delimiter, this is NOT a sentence ending
        // (e.g., "..." or "!!!" should be treated as a word, not sentence ending)
        if (second_last == '.' || second_last == '!' || second_last == '?') {
            return 0;  // Multiple delimiters - treat as word, not sentence end
        }
    }
    
    // Single delimiter at the end - valid sentence ending
    return 1;
}

// Parse file into sentences (sentences end with SINGLE . ! ?)
// Multiple delimiters like ... or !!! are treated as words, not sentence endings
char** parse_sentences(const char *content, int *sentence_count) {
    if (!content || strlen(content) == 0) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Check if content is only whitespace
    const char *check = content;
    int has_non_whitespace = 0;
    while (*check) {
        if (!isspace(*check)) {
            has_non_whitespace = 1;
            break;
        }
        check++;
    }
    
    if (!has_non_whitespace) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Count sentences first
    // EVERY delimiter (., !, ?) creates a separate sentence
    // Example: "..." = 3 sentences: ".", ".", "."
    *sentence_count = 0;
    const char *p = content;
    int in_sentence = 0;
    
    while (*p) {
        if (*p == '.' || *p == '!' || *p == '?') {
            // EVERY delimiter creates a sentence boundary
            (*sentence_count)++;
            in_sentence = 0;
        } else if (!in_sentence && !isspace(*p)) {
            in_sentence = 1;
        }
        p++;
    }
    
    // If last sentence doesn't end with delimiter, count it
    if (in_sentence) {
        (*sentence_count)++;
    }
    
    if (*sentence_count == 0) {
        *sentence_count = 0;
        return NULL;
    }
    
    // Allocate array
    char **sentences = malloc(sizeof(char*) * (*sentence_count));
    
    // Extract sentences
    int idx = 0;
    const char *start = content;
    p = content;
    
    while (*p && idx < *sentence_count) {
        if (*p == '.' || *p == '!' || *p == '?') {
            // EVERY delimiter marks sentence boundary
            int len = p - start + 1;  // Include the delimiter
            sentences[idx] = malloc(len + 1);
            strncpy(sentences[idx], start, len);
            sentences[idx][len] = '\0';
            idx++;
            
            // Skip whitespace after delimiter
            p++;
            while (*p && isspace(*p)) p++;
            start = p;
        } else {
            p++;
        }
    }
    
    // Handle last sentence if it doesn't end with delimiter
    if (idx < *sentence_count && *start) {
        int len = strlen(start);
        sentences[idx] = malloc(len + 1);
        strcpy(sentences[idx], start);
    }
    
    return sentences;
}

// Parse sentence into words
char** parse_words(const char *sentence, int *word_count) {
    if (!sentence || strlen(sentence) == 0) {
        *word_count = 0;
        return NULL;
    }
    
    char *copy = strdup(sentence);
    *word_count = 0;
    
    // Count words
    char *token = strtok(copy, " \t\n");
    while (token != NULL) {
        (*word_count)++;
        token = strtok(NULL, " \t\n");
    }
    
    if (*word_count == 0) {
        free(copy);
        return NULL;
    }
    
    // Allocate array
    char **words = malloc(sizeof(char*) * (*word_count));
    
    // Extract words
    strcpy(copy, sentence);
    int idx = 0;
    token = strtok(copy, " \t\n");
    while (token != NULL && idx < *word_count) {
        words[idx] = strdup(token);
        idx++;
        token = strtok(NULL, " \t\n");
    }
    
    free(copy);
    return words;
}

// Rebuild sentence from words
char* rebuild_sentence(char **words, int word_count) {
    if (word_count == 0) return strdup("");
    
    int total_len = 0;
    for (int i = 0; i < word_count; i++) {
        total_len += strlen(words[i]) + 1;  // +1 for space
    }
    
    char *result = malloc(total_len + 1);
    result[0] = '\0';
    
    for (int i = 0; i < word_count; i++) {
        if (i > 0) strcat(result, " ");
        strcat(result, words[i]);
    }
    
    return result;
}

// Check if sentence is locked
SentenceLock* find_sentence_lock(const char *filename, int sentence_num) {
    pthread_mutex_lock(&lock_mutex);
    
    SentenceLock *current = locks;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num) {
            pthread_mutex_unlock(&lock_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&lock_mutex);
    return NULL;
}

// Add sentence lock
int add_sentence_lock(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    
    // Check if already locked
    SentenceLock *current = locks;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num) {
            pthread_mutex_unlock(&lock_mutex);
            return 0;  // Already locked
        }
        current = current->next;
    }
    
    // Add new lock
    SentenceLock *new_lock = malloc(sizeof(SentenceLock));
    strncpy(new_lock->filename, filename, sizeof(new_lock->filename));
    new_lock->sentence_num = sentence_num;
    strncpy(new_lock->locked_by, username, sizeof(new_lock->locked_by));
    new_lock->lock_time = time(NULL);
    new_lock->next = locks;
    locks = new_lock;
    
    pthread_mutex_unlock(&lock_mutex);
    return 1;  // Lock acquired
}

// Remove sentence lock
void remove_sentence_lock(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    
    SentenceLock *current = locks;
    SentenceLock *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num &&
            strcmp(current->locked_by, username) == 0) {
            
            if (prev == NULL) {
                locks = current->next;
            } else {
                prev->next = current->next;
            }
            
            free(current);
            pthread_mutex_unlock(&lock_mutex);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&lock_mutex);
}

// Get undo state for a file
UndoState* get_undo_state(const char *filename) {
    pthread_mutex_lock(&undo_mutex);
    UndoState *current = undo_states;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            pthread_mutex_unlock(&undo_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&undo_mutex);
    return NULL;
}

// Set undo state for a file (1 = undo performed, 0 = file modified)
void set_undo_state(const char *filename, int undo_performed) {
    pthread_mutex_lock(&undo_mutex);
    
    UndoState *current = undo_states;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            current->last_undo_performed = undo_performed;
            pthread_mutex_unlock(&undo_mutex);
            return;
        }
        current = current->next;
    }
    
    // Create new undo state entry
    UndoState *new_state = malloc(sizeof(UndoState));
    strncpy(new_state->filename, filename, sizeof(new_state->filename) - 1);
    new_state->filename[sizeof(new_state->filename) - 1] = '\0';
    new_state->last_undo_performed = undo_performed;
    new_state->next = undo_states;
    undo_states = new_state;
    
    pthread_mutex_unlock(&undo_mutex);
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

// Handle client connection
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
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "READ request for '%s'", msg.filename);
                log_message("storage_server", log_msg);
                
                char buffer[MAX_DATA];
                int result = read_file(msg.filename, buffer, sizeof(buffer));
                
                if (result == RESP_SUCCESS) {
                    msg.error_code = RESP_SUCCESS;
                    strncpy(msg.data, buffer, sizeof(msg.data) - 1);
                    msg.data[sizeof(msg.data) - 1] = '\0';
                    printf("  ✓ File read successfully (%ld bytes)\n", strlen(buffer));
                    
                    snprintf(log_msg, sizeof(log_msg), "READ completed for '%s' - sent %ld bytes", 
                             msg.filename, strlen(buffer));
                    log_message("storage_server", log_msg);
                } else {
                    msg.error_code = result;
                    if (result == ERR_FILE_NOT_FOUND) {
                        snprintf(msg.data, sizeof(msg.data), "File not found");
                        printf("  ✗ File not found\n");
                        
                        snprintf(log_msg, sizeof(log_msg), "READ failed for '%s' - file not found", msg.filename);
                        log_message("storage_server", log_msg);
                    } else {
                        snprintf(msg.data, sizeof(msg.data), "Failed to read file");
                        printf("  ✗ Failed to read file\n");
                        
                        snprintf(log_msg, sizeof(log_msg), "READ failed for '%s' - server error", msg.filename);
                        log_message("storage_server", log_msg);
                    }
                }
                
                send_message(client_socket, &msg);
                
                // Log stop packet (response sent)
                snprintf(log_msg, sizeof(log_msg), "READ stop packet sent for '%s'", msg.filename);
                log_message("storage_server", log_msg);
                break;
            }
            
            case MSG_WRITE: {
                printf("→ WRITE request for '%s' sentence %d from %s\n", 
                       msg.filename, msg.sentence_num, msg.username);
                
                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "WRITE request for '%s' sentence %d from %s", 
                         msg.filename, msg.sentence_num, msg.username);
                log_message("storage_server", log_msg);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                // Check if file exists
                if (!file_exists(filepath)) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Read file content as a single line and parse into sentences
                FILE *fp = fopen(filepath, "r");
                if (!fp) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to open file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to open file\n");
                    break;
                }
                
                // Read entire file content (should be on one line)
                char content[MAX_DATA * 4] = "";
                char line[MAX_DATA];
                while (fgets(line, sizeof(line), fp)) {
                    // Remove trailing newline
                    size_t len = strlen(line);
                    if (len > 0 && line[len - 1] == '\n') {
                        line[len - 1] = '\0';
                    }
                    if (len > 0 && line[len - 1] == '\r') {
                        line[len - 1] = '\0';
                    }
                    // Concatenate all lines (though there should typically be just one)
                    strncat(content, line, sizeof(content) - strlen(content) - 1);
                    if (strlen(content) < sizeof(content) - 1) {
                        strncat(content, " ", sizeof(content) - strlen(content) - 1);
                    }
                }
                fclose(fp);
                
                // Parse content into sentences using existing parse_sentences function
                int sentence_count = 0;
                char **sentences = parse_sentences(content, &sentence_count);
                
                // Handle empty file - treat as having 0 sentences initially
                if (strlen(content) == 0 || sentence_count == 0) {
                    sentences = NULL;
                    sentence_count = 0;
                }
                
                // CRITICAL: Check if we can access the requested sentence
                // Rules:
                // 1. Can only access sentence 0 to sentence_count (0-indexed)
                // 2. Can always access next sentence (sentence_count)
                // 3. Cannot skip sentences
                
                // For empty file, only sentence 0 is allowed
                if (sentence_count == 0) {
                    if (msg.sentence_num != 0) {
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = 0;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "File is empty. Only sentence 0 is accessible.");
                        send_message(client_socket, &msg);
                        printf("  ✗ File empty, only sentence 0 allowed (requested: %d)\n", msg.sentence_num);
                        break;
                    }
                    // Create first empty sentence
                    sentence_count = 1;
                    sentences = malloc(sizeof(char*));
                    sentences[0] = strdup("");
                } else {
                                        // Validate sentence number
                    if (msg.sentence_num < 0) {
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = sentence_count - 1;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "Invalid sentence number. Must be non-negative.");
                        send_message(client_socket, &msg);
                        printf("  ✗ Invalid sentence number %d\n", msg.sentence_num);
                        if (sentences) {
                            for (int i = 0; i < sentence_count; i++) {
                                free(sentences[i]);
                            }
                            free(sentences);
                        }
                        break;
                    }
                    
                    // Check if accessing new sentence (sentence_num == sentence_count)
                    if (msg.sentence_num == sentence_count) {
                        // Can only access new sentence if previous sentence has a delimiter
                        if (sentence_count > 0 && !sentence_has_delimiter(sentences[sentence_count - 1])) {
                            msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                            msg.word_index = sentence_count - 1;
                            snprintf(msg.data, sizeof(msg.data), 
                                     "Cannot access sentence %d. Previous sentence %d must end with a single delimiter (., !, ?).",
                                     msg.sentence_num, sentence_count - 1);
                            send_message(client_socket, &msg);
                            printf("  ✗ Sentence %d needs delimiter before accessing sentence %d\n", 
                                   sentence_count - 1, msg.sentence_num);
                            if (sentences) {
                                for (int i = 0; i < sentence_count; i++) {
                                    free(sentences[i]);
                                }
                                free(sentences);
                            }
                            break;
                        }
                        
                        // Allowed - create new sentence
                        printf("  → Adding new sentence at position %d\n", msg.sentence_num);
                        sentence_count++;
                        sentences = realloc(sentences, sizeof(char*) * sentence_count);
                        sentences[msg.sentence_num] = strdup("");  // Start with empty sentence
                    } else if (msg.sentence_num > sentence_count) {
                        // Cannot skip sentences
                        msg.error_code = ERR_SENTENCE_OUT_OF_RANGE;
                        msg.word_index = sentence_count;
                        snprintf(msg.data, sizeof(msg.data), 
                                 "Cannot skip sentences. File has %d sentence(s). Can access 0 to %d.",
                                 sentence_count, sentence_count);
                        send_message(client_socket, &msg);
                        printf("  ✗ Cannot skip to sentence %d (max accessible: %d)\n", 
                               msg.sentence_num, sentence_count);
                        if (sentences) {
                            for (int i = 0; i < sentence_count; i++) {
                                free(sentences[i]);
                            }
                            free(sentences);
                        }
                        break;
                    }
                }
                
                // Try to lock the sentence
                if (!add_sentence_lock(msg.filename, msg.sentence_num, msg.username)) {
                    SentenceLock *lock = find_sentence_lock(msg.filename, msg.sentence_num);
                    msg.error_code = ERR_FILE_LOCKED;
                    snprintf(msg.data, sizeof(msg.data), "%s", lock->locked_by);
                    send_message(client_socket, &msg);
                    printf("  ✗ Sentence locked by %s\n", lock->locked_by);
                    
                    // Free sentences
                    for (int i = 0; i < sentence_count; i++) {
                        free(sentences[i]);
                    }
                    free(sentences);
                    break;
                }
                
                printf("  ✓ Sentence locked for %s\n", msg.username);
                
                // Send current sentence to client (sentence_num is 0-indexed)
                char *current_sentence = sentences[msg.sentence_num];
                msg.error_code = RESP_SUCCESS;
                strncpy(msg.data, current_sentence, sizeof(msg.data) - 1);
                send_message(client_socket, &msg);
                
                // Parse sentence into words for editing
                int word_count = 0;
                char **words = parse_words(current_sentence, &word_count);
                
                // Handle empty sentence - start with 0 words
                // User can only insert at index 0 initially
                if (word_count == 0 || words == NULL) {
                    printf("  → Sentence is empty, no words yet (can insert at index 0)\n");
                    word_count = 0;
                    words = NULL;
                }
                
                printf("  → Sentence has %d word(s): %s\n", word_count, 
                       strlen(current_sentence) > 0 ? current_sentence : "(empty)");
                
                // Enter edit loop - receive word updates
                int editing = 1;
                while (editing) {
                    struct Message update_msg;
                    memset(&update_msg, 0, sizeof(update_msg));
                    
                    if (recv_message(client_socket, &update_msg) <= 0) {
                        printf("  ✗ Connection lost during edit\n");
                        editing = 0;
                        break;
                    }
                    
                    // Check for ETIRW (end editing)
                    if (strcmp(update_msg.data, "ETIRW") == 0) {
                        printf("  ✓ ETIRW received - finalizing changes\n");
                        
                        // Rebuild sentence from words
                        char *new_sentence = rebuild_sentence(words, word_count);
                        
                        // Update sentence in array (sentence_num is 0-indexed)
                        free(sentences[msg.sentence_num]);
                        sentences[msg.sentence_num] = new_sentence;
                        
                        // Create backup - backup the original file line-by-line
                        char backup_path[MAX_PATH];
                        snprintf(backup_path, sizeof(backup_path), "%s%s.backup", backup_dir, msg.filename);
                        
                        // Read original file and backup
                        FILE *orig_fp = fopen(filepath, "r");
                        if (orig_fp) {
                            FILE *backup_fp = fopen(backup_path, "w");
                            if (backup_fp) {
                                char backup_line[MAX_DATA];
                                while (fgets(backup_line, sizeof(backup_line), orig_fp)) {
                                    fputs(backup_line, backup_fp);
                                }
                                fclose(backup_fp);
                            }
                            fclose(orig_fp);
                        }
                        
                        // Write to temp file first
                        char temp_path[MAX_PATH];
                        snprintf(temp_path, sizeof(temp_path), "%s%s.tmp", storage_dir, msg.filename);
                        FILE *temp_fp = fopen(temp_path, "w");
                        if (!temp_fp) {
                            update_msg.error_code = ERR_SERVER_ERROR;
                            snprintf(update_msg.data, sizeof(update_msg.data), "Failed to write changes");
                            send_message(client_socket, &update_msg);
                            editing = 0;
                            break;
                        }
                        
                        // Write all sentences on a single line separated by spaces
                        // Keep delimiters - they are part of the content now
                        for (int i = 0; i < sentence_count; i++) {
                            char *sent = sentences[i];
                            size_t len = strlen(sent);
                            
                            // Write sentence as-is (with delimiters)
                            if (len > 0) {
                                fprintf(temp_fp, "%s", sent);
                            }
                            
                            // Add space between sentences (but not after the last one)
                            if (i < sentence_count - 1 && len > 0) {
                                fprintf(temp_fp, " ");
                            }
                        }
                        
                        // End with a single newline
                        fprintf(temp_fp, "\n");
                        
                        fclose(temp_fp);
                        
                        // Atomically replace original file
                        if (rename(temp_path, filepath) != 0) {
                            update_msg.error_code = ERR_SERVER_ERROR;
                            snprintf(update_msg.data, sizeof(update_msg.data), "Failed to save changes");
                            send_message(client_socket, &update_msg);
                            remove(temp_path);
                            editing = 0;
                            break;
                        }
                        
                        // Success - send ALL sentences concatenated (with delimiters) to show full content
                        update_msg.error_code = RESP_SUCCESS;
                        
                        // Build complete content with all sentences
                        char full_content[MAX_DATA] = "";
                        for (int i = 0; i < sentence_count; i++) {
                            strncat(full_content, sentences[i], sizeof(full_content) - strlen(full_content) - 1);
                            if (i < sentence_count - 1 && strlen(sentences[i]) > 0) {
                                strncat(full_content, " ", sizeof(full_content) - strlen(full_content) - 1);
                            }
                        }
                        
                        strncpy(update_msg.data, full_content, sizeof(update_msg.data) - 1);
                        send_message(client_socket, &update_msg);
                        
                        // Reset undo state - file has been modified
                        set_undo_state(msg.filename, 0);
                        
                        printf("  ✓ Changes saved. Full content: %s\n", full_content);
                        
                        // Update backup file with new content
                        char final_backup_path[MAX_PATH];
                        snprintf(final_backup_path, sizeof(final_backup_path), "%s%s", backup_dir, msg.filename);
                        
                        FILE *backup_src = fopen(filepath, "r");
                        FILE *backup_dst = fopen(final_backup_path, "w");
                        if (backup_src && backup_dst) {
                            char buffer[4096 * 10];
                            size_t bytes = fread(buffer, 1, sizeof(buffer), backup_src);
                            fwrite(buffer, 1, bytes, backup_dst);
                            fclose(backup_src);
                            fclose(backup_dst);
                            printf("  ✓ Backup updated for '%s'\n", msg.filename);
                        }
                        
                        snprintf(log_msg, sizeof(log_msg), "WRITE completed for '%s' sentence %d - changes saved", 
                                 msg.filename, msg.sentence_num);
                        log_message("storage_server", log_msg);
                        
                        editing = 0;
                    } else {
                        // Word update - INSERT at specified position (not replace)
                        int word_idx = update_msg.word_index;
                        char *new_content_str = update_msg.data;
                        
                        // CRITICAL: Validate word index
                        // Rules:
                        // 1. For empty sentence (word_count=0), only index 0 is allowed
                        // 2. For non-empty, can insert at 0 to word_count (inclusive)
                        // 3. Index i means: insert BEFORE word i, or AFTER all words if i==word_count
                        // 4. Cannot skip indices
                        
                        if (word_idx < 0 || word_idx > word_count) {
                            update_msg.error_code = ERR_WORD_OUT_OF_RANGE;
                            update_msg.word_index = word_count;  // Send current max
                            snprintf(update_msg.data, sizeof(update_msg.data), 
                                     "Word index must be between 0 and %d. Current word count: %d", 
                                     word_count, word_count);
                            send_message(client_socket, &update_msg);
                            printf("  ✗ Word index %d out of range (valid: 0-%d)\n", word_idx, word_count);
                            continue;
                        }
                        
                        // Position is 0-indexed - use directly
                        int insert_pos = word_idx;
                        
                        // Tokenize input by spaces to get individual words to insert
                        char temp_content[MAX_DATA];
                        strncpy(temp_content, new_content_str, sizeof(temp_content) - 1);
                        temp_content[sizeof(temp_content) - 1] = '\0';
                        
                        // Count and collect tokens (words)
                        char *token = strtok(temp_content, " \t\n");
                        int tokens_to_insert = 0;
                        char *tokens[256];
                        
                        while (token != NULL && tokens_to_insert < 256) {
                            tokens[tokens_to_insert++] = strdup(token);
                            token = strtok(NULL, " \t\n");
                        }
                        
                        if (tokens_to_insert == 0) {
                            // Empty content, skip
                            update_msg.error_code = RESP_SUCCESS;
                            char *unchanged = rebuild_sentence(words, word_count);
                            strncpy(update_msg.data, unchanged, sizeof(update_msg.data) - 1);
                            free(unchanged);
                            send_message(client_socket, &update_msg);
                            continue;
                        }
                        
                        // Expand array to accommodate new words
                        words = realloc(words, sizeof(char*) * (word_count + tokens_to_insert));
                        
                        // Shift existing words to the right to make room
                        for (int i = word_count - 1; i >= insert_pos; i--) {
                            words[i + tokens_to_insert] = words[i];
                        }
                        
                        // Insert new tokens
                        for (int i = 0; i < tokens_to_insert; i++) {
                            words[insert_pos + i] = tokens[i];
                        }
                        
                        word_count += tokens_to_insert;
                        
                        printf("  → Inserted %d word(s) at index %d\n", tokens_to_insert, insert_pos);
                        
                        // Rebuild sentence to check for delimiters
                        char *updated_sentence = rebuild_sentence(words, word_count);
                        
                        // Check if the updated sentence contains delimiters
                        // If yes, split into multiple sentences dynamically
                        int split_count = 0;
                        char **split_sentences = parse_sentences(updated_sentence, &split_count);
                        
                        if (split_count > 1) {
                            // Sentence was split! Update the sentence array dynamically
                            printf("  ⚡ Delimiter detected - splitting into %d sentences\n", split_count);
                            
                            // Free old words array
                            for (int i = 0; i < word_count; i++) {
                                free(words[i]);
                            }
                            free(words);
                            
                            // Update the current sentence (msg.sentence_num) with the first split
                            free(sentences[msg.sentence_num]);
                            sentences[msg.sentence_num] = strdup(split_sentences[0]);
                            
                            // Insert remaining splits as new sentences after current one
                            // Need to expand sentences array
                            int old_sentence_count = sentence_count;
                            sentence_count += (split_count - 1);
                            sentences = realloc(sentences, sizeof(char*) * sentence_count);
                            
                            // Shift existing sentences to make room
                            for (int i = old_sentence_count - 1; i > msg.sentence_num; i--) {
                                sentences[i + (split_count - 1)] = sentences[i];
                            }
                            
                            // Insert new sentences
                            for (int i = 1; i < split_count; i++) {
                                sentences[msg.sentence_num + i] = strdup(split_sentences[i]);
                            }
                            
                            printf("  → Sentence %d split: \"%s\"\n", msg.sentence_num, split_sentences[0]);
                            for (int i = 1; i < split_count; i++) {
                                printf("  → New sentence %d created: \"%s\"\n", msg.sentence_num + i, split_sentences[i]);
                            }
                            
                            // Free split sentences (we've copied them)
                            for (int i = 0; i < split_count; i++) {
                                free(split_sentences[i]);
                            }
                            free(split_sentences);
                            
                            // Re-parse the current sentence (which is now the first split)
                            words = parse_words(sentences[msg.sentence_num], &word_count);
                            
                            // Send updated current sentence to client
                            update_msg.error_code = RESP_SUCCESS;
                            update_msg.word_index = word_count;
                            strncpy(update_msg.data, sentences[msg.sentence_num], sizeof(update_msg.data) - 1);
                            send_message(client_socket, &update_msg);
                            
                            printf("  → Continuing edit of sentence %d (now has %d words)\n", msg.sentence_num, word_count);
                        } else {
                            // No split - normal update
                            update_msg.error_code = RESP_SUCCESS;
                            update_msg.word_index = word_count;
                            strncpy(update_msg.data, updated_sentence, sizeof(update_msg.data) - 1);
                            send_message(client_socket, &update_msg);
                            
                            printf("  → Updated sentence: %s\n", updated_sentence);
                            
                            // Free split_sentences if allocated
                            if (split_sentences) {
                                for (int i = 0; i < split_count; i++) {
                                    free(split_sentences[i]);
                                }
                                free(split_sentences);
                            }
                        }
                        
                        free(updated_sentence);
                    }
                }
                
                // Release lock
                remove_sentence_lock(msg.filename, msg.sentence_num, msg.username);
                printf("  ✓ Lock released\n");
                
                // Free memory
                for (int i = 0; i < word_count; i++) {
                    free(words[i]);
                }
                free(words);
                
                for (int i = 0; i < sentence_count; i++) {
                    free(sentences[i]);
                }
                free(sentences);
                
                break;
            }
            
            case MSG_STREAM: {
                printf("→ STREAM request for '%s'\n", msg.filename);

                char log_msg[512];
                snprintf(log_msg, sizeof(log_msg), "STREAM request for '%s'", msg.filename);
                log_message("storage_server", log_msg);

                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);

                // Check if file exists
                if (!file_exists(filepath)) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM failed for '%s' - file not found", msg.filename);
                    log_message("storage_server", log_msg);
                    break;
                }

                // Read file content
                FILE *fp = fopen(filepath, "r");
                if (!fp) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to open file");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to open file for streaming\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM failed for '%s' - could not open file", msg.filename);
                    log_message("storage_server", log_msg);
                    break;
                }

                char *content = NULL;
                size_t content_size = 0;
                char buffer[4096];
                while (!feof(fp)) {
                    size_t r = fread(buffer, 1, sizeof(buffer), fp);
                    if (r > 0) {
                        char *newc = realloc(content, content_size + r + 1);
                        if (!newc) break;
                        content = newc;
                        memcpy(content + content_size, buffer, r);
                        content_size += r;
                    }
                }
                if (content) content[content_size] = '\0';
                fclose(fp);

                // If empty, send immediate success
                if (!content || content_size == 0) {
                    struct Message out;
                    memset(&out, 0, sizeof(out));
                    out.type = MSG_STREAM;
                    out.error_code = RESP_SUCCESS;
                    snprintf(out.data, sizeof(out.data), "");
                    send_message(client_socket, &out);
                    if (content) free(content);
                    printf("  ✓ Streamed (empty file)\n");
                    
                    snprintf(log_msg, sizeof(log_msg), "STREAM completed for '%s' - empty file, stop packet sent", msg.filename);
                    log_message("storage_server", log_msg);
                    break;
                }

                // Split into lines, then stream each word with line info
                char *copy = strdup(content);
                char *line = strtok(copy, "\n");
                int sent_words = 0;
                
                snprintf(log_msg, sizeof(log_msg), "STREAM started for '%s' - streaming content", msg.filename);
                log_message("storage_server", log_msg);
                
                while (line != NULL) {
                    // Tokenize this line into words
                    char *line_copy = strdup(line);
                    char *word = strtok(line_copy, " \t\r");
                    
                    while (word != NULL) {
                        struct Message out;
                        memset(&out, 0, sizeof(out));
                        out.type = MSG_STREAM;
                        out.error_code = RESP_DATA;
                        strncpy(out.data, word, sizeof(out.data) - 1);
                        send_message(client_socket, &out);
                        
                        sent_words++;
                        // Delay ~0.1s per word
                        usleep(100000);
                        
                        word = strtok(NULL, " \t\r");
                    }
                    
                    // Send newline marker after each line
                    struct Message newline_msg;
                    memset(&newline_msg, 0, sizeof(newline_msg));
                    newline_msg.type = MSG_STREAM;
                    newline_msg.error_code = RESP_DATA;
                    strncpy(newline_msg.data, "\n", sizeof(newline_msg.data) - 1);
                    send_message(client_socket, &newline_msg);
                    
                    free(line_copy);
                    line = strtok(NULL, "\n");
                }

                // Send final success (STOP packet)
                struct Message fin;
                memset(&fin, 0, sizeof(fin));
                fin.type = MSG_STREAM;
                fin.error_code = RESP_SUCCESS;
                snprintf(fin.data, sizeof(fin.data), "");
                send_message(client_socket, &fin);

                if (copy) free(copy);
                if (content) free(content);

                printf("  ✓ Streamed %d words\n", sent_words);
                
                snprintf(log_msg, sizeof(log_msg), "STREAM completed for '%s' - sent %d words, stop packet sent", 
                         msg.filename, sent_words);
                log_message("storage_server", log_msg);
                break;
            }
            
            case MSG_UNDO: {
                printf("→ UNDO request for '%s' from %s\n", msg.filename, msg.username);
                
                char undo_log_msg[512];
                snprintf(undo_log_msg, sizeof(undo_log_msg), "UNDO request for '%s' from %s", msg.filename, msg.username);
                log_message("storage_server", undo_log_msg);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                char backup_path[MAX_PATH];
                snprintf(backup_path, sizeof(backup_path), "%s%s.backup", backup_dir, msg.filename);
                
                // Check if file exists
                if (!file_exists(filepath)) {
                    msg.error_code = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    send_message(client_socket, &msg);
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Check if backup exists
                if (!file_exists(backup_path)) {
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "No backup available. File has not been modified yet.");
                    send_message(client_socket, &msg);
                    printf("  ✗ No backup found\n");
                    break;
                }
                
                // Check if consecutive undo is being attempted
                UndoState *undo_state = get_undo_state(msg.filename);
                if (undo_state != NULL && undo_state->last_undo_performed == 1) {
                    msg.error_code = ERR_PERMISSION_DENIED;
                    snprintf(msg.data, sizeof(msg.data), 
                             "Cannot perform consecutive UNDO. Please modify the file first before undoing again.");
                    send_message(client_socket, &msg);
                    printf("  ✗ Consecutive UNDO not allowed\n");
                    break;
                }
                
                // Create a backup of current file (in case undo needs to be undone)
                char temp_backup[MAX_PATH];
                snprintf(temp_backup, sizeof(temp_backup), "%s%s.tmp_backup", backup_dir, msg.filename);
                
                FILE *src = fopen(filepath, "r");
                FILE *dst = fopen(temp_backup, "w");
                if (src && dst) {
                    char buffer[4096];
                    size_t bytes;
                    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        fwrite(buffer, 1, bytes, dst);
                    }
                    fclose(src);
                    fclose(dst);
                }
                
                // Copy backup to main file
                src = fopen(backup_path, "r");
                dst = fopen(filepath, "w");
                
                if (!src || !dst) {
                    if (src) fclose(src);
                    if (dst) fclose(dst);
                    msg.error_code = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to restore from backup");
                    send_message(client_socket, &msg);
                    printf("  ✗ Failed to restore\n");
                    break;
                }
                
                char buffer[4096];
                size_t bytes;
                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                    fwrite(buffer, 1, bytes, dst);
                }
                
                fclose(src);
                fclose(dst);
                
                // Move temp backup to become new backup
                rename(temp_backup, backup_path);
                
                // Mark that undo was performed
                set_undo_state(msg.filename, 1);
                
                // Success
                msg.error_code = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "File reverted to previous version");
                send_message(client_socket, &msg);
                
                snprintf(undo_log_msg, sizeof(undo_log_msg), "Undo performed on %s by %s", 
                         msg.filename, msg.username);
                log_message("storage_server", undo_log_msg);
                
                printf("  ✓ File restored from backup\n");
                break;
            }
            
            
            default:
                msg.error_code = ERR_INVALID_REQUEST;
                snprintf(msg.data, sizeof(msg.data), "Invalid request");
                send_message(client_socket, &msg);
                printf("→ Invalid request type: %d\n", msg.type);
        }
    }
    
    close(client_socket);
    return NULL;
}

// Handle naming server commands
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
                } else if (result == ERR_FILE_EXISTS) {
                    snprintf(msg.data, sizeof(msg.data), "File already exists");
                    printf("  ✗ File already exists\n");
                } else {
                    snprintf(msg.data, sizeof(msg.data), "Failed to create file");
                    printf("  ✗ Failed to create file\n");
                }
                break;
                
            case MSG_DELETE:
                printf("→ DELETE command for '%s'\n", msg.filename);
                result = delete_file(msg.filename);
                if (result == RESP_SUCCESS) {
                    snprintf(msg.data, sizeof(msg.data), "File deleted from storage server");
                    printf("  ✓ File deleted\n");
                } else if (result == ERR_FILE_NOT_FOUND) {
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    printf("  ✗ File not found\n");
                } else {
                    snprintf(msg.data, sizeof(msg.data), "Failed to delete file");
                    printf("  ✗ Failed to delete file\n");
                }
                break;
                
            case MSG_CREATEFOLDER: {
                printf("→ CREATEFOLDER command for '%s'\n", msg.filename);
                
                // Create directory path under storage/
                char dirpath[MAX_PATH];
                snprintf(dirpath, sizeof(dirpath), "%s%s", storage_dir, msg.filename);
                
                // Create directory recursively (like mkdir -p)
                char *path_copy = strdup(dirpath);
                char *p = path_copy + strlen(storage_dir); // Skip "storage/" prefix
                
                for (; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(path_copy, 0777);
                        *p = '/';
                    }
                }
                mkdir(path_copy, 0777); // Create final directory
                
                free(path_copy);
                
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Folder created on storage server");
                printf("  ✓ Directory created: %s\n", dirpath);
                break;
            }
            
            case MSG_MOVE: {
                printf("→ MOVE command: '%s' to folder '%s'\n", msg.filename, msg.folder);
                
                // Build old and new paths
                char old_path[MAX_PATH], new_path[MAX_PATH];
                snprintf(old_path, sizeof(old_path), "%s%s", storage_dir, msg.filename);
                
                if (strlen(msg.folder) == 0) {
                    // Move to root
                    snprintf(new_path, sizeof(new_path), "%s%s", storage_dir, msg.filename);
                } else {
                    // Move to folder (ensure folder directory exists)
                    char folder_path[MAX_PATH];
                    snprintf(folder_path, sizeof(folder_path), "%s%s", storage_dir, msg.folder);
                    mkdir(folder_path, 0777); // Create if doesn't exist
                    
                    snprintf(new_path, sizeof(new_path), "%s%s/%s", storage_dir, msg.folder, msg.filename);
                }
                
                // Move file using rename
                if (rename(old_path, new_path) == 0) {
                    result = RESP_SUCCESS;
                    printf("  ✓ File moved from %s to %s\n", old_path, new_path);
                } else {
                    result = ERR_SERVER_ERROR;
                    printf("  ✗ Failed to move file\n");
                }
                
                snprintf(msg.data, sizeof(msg.data), "File moved on storage server");
                break;
            }

            case MSG_CHECKPOINT: {
                printf("→ CHECKPOINT command: '%s' with tag '%s'\n", msg.filename, msg.checkpoint_tag);
                
                // Create checkpoints directory if it doesn't exist
                char checkpoints_dir[MAX_PATH];
                snprintf(checkpoints_dir, sizeof(checkpoints_dir), "%scheckpoints/", storage_dir);
                mkdir(checkpoints_dir, 0777);
                
                // Build source and checkpoint paths
                char src_path[MAX_PATH], checkpoint_path[MAX_PATH];
                snprintf(src_path, sizeof(src_path), "%s%s", storage_dir, msg.filename);
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s", 
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                // Copy file to checkpoint (read and write)
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
                
                // Copy data
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
                break;
            }

            case MSG_VIEWCHECKPOINT: {
                printf("→ VIEWCHECKPOINT command: '%s' with tag '%s'\n", msg.filename, msg.checkpoint_tag);
                
                // Build checkpoint path
                char checkpoint_path[MAX_PATH];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s",
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                // Read checkpoint file
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
                
                // Build paths
                char file_path[MAX_PATH], checkpoint_path[MAX_PATH];
                snprintf(file_path, sizeof(file_path), "%s%s", storage_dir, msg.filename);
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%scheckpoints/%s.%s",
                         storage_dir, msg.filename, msg.checkpoint_tag);
                
                // Read checkpoint
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
                
                // Overwrite file with checkpoint data
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
                break;
            }

            case MSG_HEARTBEAT: {
                // Simple heartbeat response
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "alive");
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
                // Handle replication of file operations from other SS
                // This would contain the actual operation to replicate
                printf("→ REPLICATE command: replicating data\n");
                result = RESP_SUCCESS;
                snprintf(msg.data, sizeof(msg.data), "Replication received");
                // In a full implementation, this would execute the replicated operation
                break;
            }

            case MSG_INFO: {
                printf("→ INFO request for '%s' from naming server\n", msg.filename);
                
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
                
                // Check if file exists
                if (!file_exists(filepath)) {
                    result = ERR_FILE_NOT_FOUND;
                    snprintf(msg.data, sizeof(msg.data), "File not found");
                    printf("  ✗ File not found\n");
                    break;
                }
                
                // Read file and calculate statistics
                FILE *fp = fopen(filepath, "r");
                if (!fp) {
                    result = ERR_SERVER_ERROR;
                    snprintf(msg.data, sizeof(msg.data), "Failed to open file");
                    printf("  ✗ Failed to open file\n");
                    break;
                }
                
                // Calculate file size, word count, and character count
                long size = 0;
                int word_count = 0;
                int char_count = 0;
                int in_word = 0;
                int c;
                
                while ((c = fgetc(fp)) != EOF) {
                    size++;
                    
                    // Count characters (excluding newlines for consistency)
                    if (c != '\n' && c != '\r') {
                        char_count++;
                    }
                    
                    // Count words (space-separated)
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
                
                // Send statistics back to naming server
                // Format: "size:word_count:char_count"
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
    nm_port = client_port + 1000;  // Separate port for NS communication
    
    printf("=== Storage Server %s ===\n", ss_id);
    printf("NS: %s:%d\n", ns_ip, ns_port);
    printf("Client Port: %d\n", client_port);
    printf("NM Port: %d\n", nm_port);
    
    // Initialize storage
    init_storage();
    
    // Register with naming server and get persistent connection
    int ns_socket = register_with_ns();
    if (ns_socket < 0) {
        fprintf(stderr, "Failed to register with Naming Server\n");
        return 1;
    }
    
    // Start thread to handle NS commands on the persistent connection
    int *ns_sock_ptr = malloc(sizeof(int));
    *ns_sock_ptr = ns_socket;
    pthread_t ns_thread;
    if (pthread_create(&ns_thread, NULL, handle_ns_commands, ns_sock_ptr) != 0) {
        fprintf(stderr, "Failed to create NS command handler thread\n");
        close(ns_socket);
        return 1;
    }
    pthread_detach(ns_thread);
    
    // Create socket for client connections
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
    printf("Type 'DISCONNECT' to disconnect from Naming Server and shutdown\n\n");
    log_message("storage_server", "Server started successfully");
    
    // Make stdin non-blocking
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    
    // Accept client connections
    while (1) {
        // Check for DISCONNECT command from terminal
        char cmd[256];
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            // Remove newline
            cmd[strcspn(cmd, "\n")] = 0;
            
            if (strcmp(cmd, "DISCONNECT") == 0) {
                printf("\n⚠️  Disconnecting from Naming Server...\n");
                printf("✓ Storage server %s shutting down\n", ss_id);
                close(client_listener);
                if (ns_socket >= 0) close(ns_socket);
                exit(0);
            }
        }
        
        // Set timeout for accept to check for commands periodically
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_listener, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
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
