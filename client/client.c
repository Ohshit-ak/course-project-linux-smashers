/*
 * Client - Main Entry Point
 * 
 * The Client provides a command-line interface for users to interact
 * with the distributed file system. It:
 * - Connects to the Naming Server
 * - Sends commands from user
 * - Establishes direct connections with Storage Servers when needed
 * - Displays results to user
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include "../common/protocol.h"
#include "../common/utils.h"

#define BUFFER_SIZE 4096

// Global state
int ns_socket = -1;
char username[MAX_USERNAME];
char ns_ip[16] = "127.0.0.1";
int ns_port = 8080;
volatile int ns_alive = 1;  // Flag to track NS connection status
char selected_ss_id[64] = "";  // Currently selected storage server (empty = use most recent)

// Background thread to monitor naming server connection
void* monitor_ns_connection(void *arg) {
    while (ns_alive) {
        sleep(2);  // Check every 2 seconds
        
        if (ns_socket < 0) {
            ns_alive = 0;
            break;
        }
        
        // Try to check socket status
        int error = 0;
        socklen_t len = sizeof(error);
        int retval = getsockopt(ns_socket, SOL_SOCKET, SO_ERROR, &error, &len);
        
        if (retval != 0 || error != 0) {
            ns_alive = 0;
            printf("\n\n✗ Naming Server connection lost\n");
            printf("✗ System shutting down...\n\n");
            fflush(stdout);
            exit(1);
        }
        
        // Try a small peek to detect disconnection
        char buf[1];
        int result = recv(ns_socket, buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) {
            // Connection closed by server
            ns_alive = 0;
            printf("\n\n✗ Naming Server shut down\n");
            printf("✗ Client exiting...\n\n");
            fflush(stdout);
            close(ns_socket);
            exit(0);
        }
    }
    return NULL;
}

// Check if naming server is still alive
int check_ns_alive() {
    if (!ns_alive || ns_socket < 0) {
        printf("\n✗ Naming Server connection lost\n");
        printf("✗ System shutting down...\n");
        if (ns_socket >= 0) close(ns_socket);
        exit(1);
        return 0;
    }
    return 1;
}

// Connect to naming server
int connect_to_ns() {
    ns_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ns_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    inet_pton(AF_INET, ns_ip, &ns_addr.sin_addr);
    
    if (connect(ns_socket, (struct sockaddr*)&ns_addr, sizeof(ns_addr)) < 0) {
        perror("Connection to Naming Server failed");
        close(ns_socket);
        ns_socket = -1;
        return -1;
    }
    
    printf("Connected to Naming Server at %s:%d\n", ns_ip, ns_port);
    
    // Register with NS
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REGISTER_CLIENT;
    strncpy(msg.username, username, sizeof(msg.username));
    send_message(ns_socket, &msg);
    
    // Wait for registration ACK from server
    memset(&msg, 0, sizeof(msg));
    if (recv_message(ns_socket, &msg) > 0) {
        if (msg.error_code == RESP_SUCCESS) {
            // Registration successful
            printf("\n%s\n\n", msg.data);  // Display welcome message
            ns_alive = 1;
            
            // Start background monitoring thread
            pthread_t monitor_thread;
            pthread_create(&monitor_thread, NULL, monitor_ns_connection, NULL);
            pthread_detach(monitor_thread);  // Detach so it runs independently
        } else if (msg.error_code == ERR_FILE_LOCKED) {
            // User already logged in elsewhere
            printf("\n\u2717 Login Failed\n");
            printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
            printf("%s\n", msg.data);
            printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
            printf("Please close the other session first or use a different username.\n\n");
            close(ns_socket);
            ns_socket = -1;
            return -1;
        } else {
            printf("\n\u2717 Login Failed: Server returned error code %d\n", msg.error_code);
            close(ns_socket);
            ns_socket = -1;
            return -1;
        }
    } else {
        printf("\n\u2717 Login Failed: No response from server\n");
        close(ns_socket);
        ns_socket = -1;
        return -1;
    }
    
    return 0;
}

// Connect to storage server
int connect_to_ss(const char *ip, int port) {
    int ss_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &ss_addr.sin_addr);
    
    if (connect(ss_socket, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Connection to Storage Server failed");
        close(ss_socket);
        return -1;
    }
    
    return ss_socket;
}

// Handle USE SS command - select storage server for operations
void handle_use_ss(const char *ss_id) {
    if (ss_id == NULL || strlen(ss_id) == 0) {
        // Show current selection
        if (strlen(selected_ss_id) > 0) {
            printf("Currently using storage server: %s\n", selected_ss_id);
        } else {
            printf("Currently using: Most recent storage server (default)\n");
        }
        fflush(stdout);
        return;
    }
    
    // Set new storage server selection
    strncpy(selected_ss_id, ss_id, sizeof(selected_ss_id) - 1);
    selected_ss_id[sizeof(selected_ss_id) - 1] = '\0';
    printf("✓ Now using storage server: %s\n", selected_ss_id);
    printf("  (Future CREATE operations will use this server)\n");
    fflush(stdout);
}

// Handle CREATE command
void handle_create(const char *filename) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CREATE;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    // Include selected SS ID (empty string = use most recent)
    strncpy(msg.data, selected_ss_id, sizeof(msg.data) - 1);
    
    printf("Creating file '%s'...\n", filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to send request\n");
        fflush(stdout);
        return;
    }
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to receive response\n");
        fflush(stdout);
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
        fflush(stdout);
    } else if (msg.error_code == ERR_FILE_EXISTS) {
        printf("✗ Error: File already exists\n");
        fflush(stdout);
    } else {
        printf("✗ Error: %s (code: %d)\n", msg.data, msg.error_code);
        fflush(stdout);
    }
}

// Handle READ command
void handle_read(const char *filename) {
    // Request SS info from NS
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_READ;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Reading file '%s'...\n", filename);
    fflush(stdout);
    
    printf("[DEBUG] Sending READ request (type: %d, file: %s)\n", msg.type, msg.filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to send request\n");
        fflush(stdout);
        return;
    }
    
    printf("[DEBUG] Waiting for response from NS...\n");
    fflush(stdout);
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to receive response\n");
        fflush(stdout);
        return;
    }
    
    printf("[DEBUG] Received response: code=%d, type=%d, data='%s'\n", 
           msg.error_code, msg.type, msg.data);
    fflush(stdout);
    
    if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
        fflush(stdout);
        return;
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: Permission denied\n");
        fflush(stdout);
        return;
    } else if (msg.error_code == ERR_SS_UNAVAILABLE) {
        printf("✗ Error: %s\n", msg.data);
        fflush(stdout);
        return;
    } else if (msg.error_code == RESP_SUCCESS) {
        // NS served content directly from cache/backup (SS was down)
        printf("\n╔════════════════════════════════════════╗\n");
        printf("║ Content of: %-24s║\n", filename);
        printf("║ (served from NS cache/backup)          ║\n");
        printf("╚════════════════════════════════════════╝\n");
        if (strlen(msg.data) > 0) {
            printf("%s\n", msg.data);
        } else {
            printf("(empty file)\n");
        }
        printf("────────────────────────────────────────\n");
        fflush(stdout);
        return;
    } else if (msg.error_code != RESP_SS_INFO) {
        printf("✗ Error: Unexpected response (code: %d)\n", msg.error_code);
        printf("   %s\n", msg.data);
        fflush(stdout);
        return;
    }
    
    // Connect to SS
    printf("✓ Got SS address: %s:%d\n", msg.ss_ip, msg.ss_port);
    fflush(stdout);
    int ss_socket = connect_to_ss(msg.ss_ip, msg.ss_port);
    if (ss_socket < 0) {
        printf("✗ Failed to connect to Storage Server\n");
        return;
    }
    
    // Request file content
    struct Message read_msg;
    memset(&read_msg, 0, sizeof(read_msg));
    read_msg.type = MSG_READ;
    strncpy(read_msg.filename, filename, sizeof(read_msg.filename));
    
    if (send_message(ss_socket, &read_msg) < 0) {
        printf("Error: Failed to send read request to SS\n");
        close(ss_socket);
        return;
    }
    
    // Clear message buffer before receiving to avoid stale data
    memset(&read_msg, 0, sizeof(read_msg));
    
    if (recv_message(ss_socket, &read_msg) < 0) {
        printf("Error: Failed to receive data from SS\n");
        close(ss_socket);
        return;
    }
    
    if (read_msg.error_code == RESP_SUCCESS) {
        printf("\n╔════════════════════════════════════════╗\n");
        printf("║ Content of: %-24s║\n", filename);
        printf("╚════════════════════════════════════════╝\n");
        if (strlen(read_msg.data) > 0) {
            printf("%s\n", read_msg.data);
        } else {
            printf("(empty file)\n");
        }
        printf("────────────────────────────────────────\n");
    } else {
        printf("✗ Error reading file: %s (code: %d)\n", 
               read_msg.data, read_msg.error_code);
    }
    
    close(ss_socket);
}

// Handle DELETE command
void handle_delete(const char *filename) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_DELETE;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Deleting file '%s'...\n", filename);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to send request\n");
        return;
    }
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
        remove(filename);  // Local cleanup
    } else if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: Only the owner can delete this file\n");
    } else {
        printf("✗ Error: %s (code: %d)\n", msg.data, msg.error_code);
    }
}

// Handle VIEW command
void handle_view(int show_all, int show_details) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_VIEW;
    strncpy(msg.username, username, sizeof(msg.username));
    msg.flags = (show_all ? 1 : 0) | (show_details ? 2 : 0);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to send request\n");
        return;
    }
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("Error: Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("\n╔════════════════════════════════════════╗\n");
        printf("║ Available Files                        ║\n");
        printf("╚════════════════════════════════════════╝\n");
        printf("%s", msg.data);
        printf("────────────────────────────────────────\n");
        fflush(stdout);
    } else {
        printf("✗ Error: %s (code: %d)\n", msg.data, msg.error_code);
        fflush(stdout);
    }
}

// Handle LIST command
void handle_list() {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LIST_USERS;
    strncpy(msg.username, username, sizeof(msg.username));
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to send request\n");
        return;
    }
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("\n╔════════════════════════════════════════╗\n");
        printf("║ Registered Users                       ║\n");
        printf("╚════════════════════════════════════════╝\n");
        
        // Parse and display users
        char *user_list = strdup(msg.data);
        char *user = strtok(user_list, "\n");
        int count = 0;
        
        while (user != NULL) {
            count++;
            printf("  %d. %s\n", count, user);
            user = strtok(NULL, "\n");
        }
        
        free(user_list);
        printf("────────────────────────────────────────\n");
        printf("Total: %d user(s)\n\n", count);
    } else {
        printf("✗ Error: %s\n", msg.data);
    }
}

// Handle INFO command
void handle_info(const char *filename) {
    struct Message msg;
    msg.type = MSG_INFO;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    send_message(ns_socket, &msg);
    
    // Clear message buffer before receiving to avoid stale data
    memset(&msg, 0, sizeof(msg));
    
    recv_message(ns_socket, &msg);
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("\n--- File Information ---\n%s\n", msg.data);
    } else {
        printf("Error: %d\n", msg.error_code);
    }
}

// Handle ADDACCESS command
void handle_addaccess(const char *flag, const char *filename, const char *target_user) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_ADD_ACCESS;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.data, target_user, sizeof(msg.data));
    
    // Set flags: 1 = read, 2 = write
    if (strcmp(flag, "-R") == 0) {
        msg.flags = 1;  // Read access
    } else if (strcmp(flag, "-W") == 0) {
        msg.flags = 2;  // Write access (implies read)
    } else {
        printf("✗ Error: Invalid flag. Use -R for read or -W for write access\n");
        return;
    }
    
    printf("Granting %s access to '%s' for user '%s'...\n", 
           (msg.flags == 2) ? "write" : "read", filename, target_user);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to send request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle REMACCESS command
void handle_remaccess(const char *filename, const char *target_user) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REM_ACCESS;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.data, target_user, sizeof(msg.data));
    
    printf("Removing access to '%s' for user '%s'...\n", filename, target_user);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to send request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle STREAM command - stream word-by-word from SS with 0.1s delay
void handle_stream(const char *filename) {
    // Request SS info from NS
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_STREAM;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));

    printf("Streaming file '%s'...\n", filename);
    fflush(stdout);

    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send STREAM request to NS\n");
        return;
    }

    // Clear and wait for NS response
    memset(&msg, 0, sizeof(msg));
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response from NS\n");
        return;
    }

    if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
        return;
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: You don't have permission to stream this file\n");
        return;
    } else if (msg.error_code != RESP_SS_INFO) {
        printf("✗ Error: %s\n", msg.data);
        return;
    }

    // Connect to SS
    printf("✓ Got SS address: %s:%d\n", msg.ss_ip, msg.ss_port);
    int ss_socket = connect_to_ss(msg.ss_ip, msg.ss_port);
    if (ss_socket < 0) {
        printf("✗ Failed to connect to storage server\n");
        return;
    }

    // Send STREAM request to SS
    struct Message stream_msg;
    memset(&stream_msg, 0, sizeof(stream_msg));
    stream_msg.type = MSG_STREAM;
    strncpy(stream_msg.filename, filename, sizeof(stream_msg.filename));

    if (send_message(ss_socket, &stream_msg) < 0) {
        printf("✗ Failed to send STREAM request to SS\n");
        close(ss_socket);
        return;
    }

    // Receive streamed words until RESP_SUCCESS
    printf("\n--- Stream Output ---\n");
    int word_count = 0;
    while (1) {
        // Check NS connection every 10 words
        if (word_count % 10 == 0) {
            if (!check_ns_alive()) {
                close(ss_socket);
                return;
            }
        }
        
        struct Message in;
        memset(&in, 0, sizeof(in));
        if (recv_message(ss_socket, &in) <= 0) {
            printf("\n✗ Connection lost while streaming\n");
            check_ns_alive();  // Check if NS is down
            break;
        }

        if (in.error_code == RESP_DATA) {
            word_count++;
            // Check if it's a newline marker
            if (strcmp(in.data, "\n") == 0) {
                printf("\n");
            } else {
                // Print word with space
                printf("%s ", in.data);
            }
            fflush(stdout);
        } else if (in.error_code == RESP_SUCCESS) {
            // End of stream
            printf("\n--- End of Stream ---\n");
            break;
        } else {
            // Some error occurred
            printf("\n✗ Stream error: %s\n", in.data);
            break;
        }
    }

    close(ss_socket);
}

// Handle WRITE command
void handle_write(const char *filename, int sentence_num) {
    // Request SS info from NS and lock the sentence
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_WRITE;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    msg.sentence_num = sentence_num;
    
    printf("Requesting write access to sentence %d in '%s'...\n", sentence_num, filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send WRITE request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
        return;
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: You don't have write permission\n");
        return;
    } else if (msg.error_code != RESP_SS_INFO) {
        printf("✗ Error: %s\n", msg.data);
        return;
    }
    
    // Connect to SS
    printf("✓ Got write permission. Connecting to SS at %s:%d\n", msg.ss_ip, msg.ss_port);
    int ss_socket = connect_to_ss(msg.ss_ip, msg.ss_port);
    if (ss_socket < 0) {
        printf("✗ Failed to connect to storage server\n");
        return;
    }
    
    // Send WRITE request to SS to lock the sentence
    struct Message write_msg;
    memset(&write_msg, 0, sizeof(write_msg));
    write_msg.type = MSG_WRITE;
    strncpy(write_msg.filename, filename, sizeof(write_msg.filename));
    strncpy(write_msg.username, username, sizeof(write_msg.username));
    write_msg.sentence_num = sentence_num;
    
    if (send_message(ss_socket, &write_msg) < 0) {
        printf("✗ Failed to send lock request\n");
        close(ss_socket);
        return;
    }
    
    // Receive lock response
    memset(&write_msg, 0, sizeof(write_msg));
    if (recv_message(ss_socket, &write_msg) < 0) {
        printf("✗ Failed to receive lock response\n");
        close(ss_socket);
        return;
    }
    
    if (write_msg.error_code == ERR_FILE_LOCKED) {
        printf("✗ Sentence %d is locked by another user: %s\n", sentence_num, write_msg.data);
        close(ss_socket);
        return;
    } else if (write_msg.error_code == ERR_SENTENCE_OUT_OF_RANGE) {
        printf("✗ Sentence %d does not exist. File has %d sentences.\n", sentence_num, write_msg.word_index);
        close(ss_socket);
        return;
    } else if (write_msg.error_code != RESP_SUCCESS) {
        printf("✗ Error: %s\n", write_msg.data);
        close(ss_socket);
        return;
    }
    
    // Display current sentence
    printf("\n✓ Sentence locked successfully!\n");
    printf("Current sentence: %s\n", write_msg.data);
    printf("\nEnter word updates in format: <word_index> <content>\n");
    printf("Type 'ETIRW' on a new line when done.\n");
    printf("────────────────────────────────────────\n");
    
    // Read word updates from user
    char line[BUFFER_SIZE];
    int update_count = 0;
    while (1) {
        // Check NS connection every 3 updates
        if (update_count > 0 && update_count % 3 == 0) {
            if (!check_ns_alive()) {
                close(ss_socket);
                return;
            }
        }
        
        printf("> ");
        fflush(stdout);
        
        if (fgets(line, sizeof(line), stdin) == NULL) {
            check_ns_alive();  // Check if NS is down
            break;
        }
        
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Check for ETIRW (end of write)
        if (strcmp(line, "ETIRW") == 0) {
            printf("\n✓ Finalizing changes...\n");
            
            // Send ETIRW signal to SS
            memset(&write_msg, 0, sizeof(write_msg));
            write_msg.type = MSG_WRITE;
            strncpy(write_msg.data, "ETIRW", sizeof(write_msg.data));
            
            if (send_message(ss_socket, &write_msg) < 0) {
                printf("✗ Failed to send ETIRW\n");
                break;
            }
            
            // Receive final response
            memset(&write_msg, 0, sizeof(write_msg));
            if (recv_message(ss_socket, &write_msg) < 0) {
                printf("✗ Failed to receive final response\n");
                break;
            }
            
            if (write_msg.error_code == RESP_SUCCESS) {
                printf("✓ Changes saved successfully!\n");
                printf("Updated sentence: %s\n", write_msg.data);
            } else {
                printf("✗ Error saving changes: %s\n", write_msg.data);
            }
            break;
        }
        
        // Parse word_index and content
        char *word_idx_str = strtok(line, " ");
        char *content = strtok(NULL, "");  // Get rest of line
        
        if (!word_idx_str || !content) {
            printf("Invalid format. Use: <word_index> <content>\n");
            continue;
        }
        
        int word_idx = atoi(word_idx_str);
        
        // Send update to SS
        memset(&write_msg, 0, sizeof(write_msg));
        write_msg.type = MSG_WRITE;
        write_msg.word_index = word_idx;
        strncpy(write_msg.data, content, sizeof(write_msg.data) - 1);
        
        if (send_message(ss_socket, &write_msg) < 0) {
            printf("✗ Failed to send update\n");
            check_ns_alive();  // Check if NS is down
            continue;
        }
        
        update_count++;
        
        // Receive acknowledgment
        memset(&write_msg, 0, sizeof(write_msg));
        if (recv_message(ss_socket, &write_msg) < 0) {
            printf("✗ Failed to receive acknowledgment\n");
            check_ns_alive();  // Check if NS is down
            continue;
        }
        
        if (write_msg.error_code == RESP_SUCCESS) {
            printf("  ✓ Updated. New sentence: %s\n", write_msg.data);
        } else if (write_msg.error_code == ERR_WORD_OUT_OF_RANGE) {
            printf("  ✗ Word index %d out of range (max: %d)\n", word_idx, write_msg.word_index);
        } else {
            printf("  ✗ Error: %s\n", write_msg.data);
        }
    }
    
    close(ss_socket);
}

// Handle UNDO command
void handle_undo(const char *filename) {
    // Request NS for file info and permission check
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_UNDO;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Requesting undo for '%s'...\n", filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send UNDO request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
        return;
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: You don't have write permission\n");
        return;
    } else if (msg.error_code != RESP_SS_INFO) {
        printf("✗ Error: %s\n", msg.data);
        return;
    }
    
    // Connect to SS
    printf("✓ Permission granted. Connecting to SS at %s:%d\n", msg.ss_ip, msg.ss_port);
    int ss_socket = connect_to_ss(msg.ss_ip, msg.ss_port);
    if (ss_socket < 0) {
        printf("✗ Failed to connect to storage server\n");
        return;
    }
    
    // Send UNDO request to SS
    struct Message undo_msg;
    memset(&undo_msg, 0, sizeof(undo_msg));
    undo_msg.type = MSG_UNDO;
    strncpy(undo_msg.filename, filename, sizeof(undo_msg.filename));
    strncpy(undo_msg.username, username, sizeof(undo_msg.username));
    
    if (send_message(ss_socket, &undo_msg) < 0) {
        printf("✗ Failed to send undo request\n");
        close(ss_socket);
        return;
    }
    
    // Receive undo response
    memset(&undo_msg, 0, sizeof(undo_msg));
    if (recv_message(ss_socket, &undo_msg) < 0) {
        printf("✗ Failed to receive undo response\n");
        close(ss_socket);
        return;
    }
    
    if (undo_msg.error_code == RESP_SUCCESS) {
        printf("✓ Undo successful! File reverted to previous version.\n");
        if (strlen(undo_msg.data) > 0) {
            printf("  Info: %s\n", undo_msg.data);
        }
    } else {
        printf("✗ Undo failed: %s\n", undo_msg.data);
    }
    
    close(ss_socket);
}

// Handle EXEC command - execute file content as shell commands on naming server
void handle_exec(const char *filename) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_EXEC;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Executing file '%s' on naming server...\n", filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send EXEC request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == ERR_FILE_NOT_FOUND) {
        printf("✗ Error: File not found\n");
        return;
    } else if (msg.error_code == ERR_PERMISSION_DENIED) {
        printf("✗ Error: You don't have read permission to execute this file\n");
        return;
    } else if (msg.error_code == RESP_SUCCESS) {
        printf("\n╔════════════════════════════════════════╗\n");
        printf("║ Execution Output: %-21s║\n", filename);
        printf("╚════════════════════════════════════════╝\n");
        if (strlen(msg.data) > 0) {
            printf("%s", msg.data);
            // Add newline if output doesn't end with one
            if (msg.data[strlen(msg.data) - 1] != '\n') {
                printf("\n");
            }
        } else {
            printf("(no output)\n");
        }
        printf("────────────────────────────────────────\n");
    } else {
        printf("✗ Error executing file: %s\n", msg.data);
    }
}

// Handle SEARCH command
void handle_search(const char *pattern) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SEARCH;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.data, pattern, sizeof(msg.data));  // Send pattern in data field
    
    printf("Searching for files matching '%s'...\n", pattern);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send SEARCH request\n");
        return;
    }
    
    // Clear message buffer before receiving
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("\n%s\n", msg.data);
    } else {
        printf("✗ Error searching: %s\n", msg.data);
    }
}

void handle_createfolder(const char *foldername) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CREATEFOLDER;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, foldername, sizeof(msg.filename));
    
    // Include selected storage server ID if specified
    if (strlen(selected_ss_id) > 0) {
        strncpy(msg.data, selected_ss_id, sizeof(msg.data));
        printf("Creating folder '%s' on %s...\n", foldername, selected_ss_id);
    } else {
        printf("Creating folder '%s'...\n", foldername);
    }
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send CREATEFOLDER request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

void handle_viewfolder(const char *foldername) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_VIEWFOLDER;
    strncpy(msg.username, username, sizeof(msg.username));
    
    // Empty string for root folder
    if (foldername) {
        strncpy(msg.filename, foldername, sizeof(msg.filename));
    }
    
    // Note: VIEWFOLDER queries NS which tracks all folders across all SS
    // selected_ss_id doesn't apply here as folders are virtual in NS
    if (!foldername || strlen(foldername) == 0) {
        printf("Viewing root folder...\n");
    } else {
        printf("Viewing folder '%s'...\n", foldername);
    }
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send VIEWFOLDER request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("\n%s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

void handle_move(const char *filename, const char *foldername) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_MOVE;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    // Empty string for root folder
    if (foldername) {
        strncpy(msg.folder, foldername, sizeof(msg.folder));
    }
    
    // Note: MOVE operates on files which already have an SS assignment
    // selected_ss_id doesn't change which SS the file is on
    if (!foldername || strlen(foldername) == 0) {
        printf("Moving '%s' to root folder...\n", filename);
    } else {
        printf("Moving '%s' to folder '%s'...\n", filename, foldername);
    }
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send MOVE request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle CHECKPOINT command
void handle_checkpoint(const char *filename, const char *tag) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CHECKPOINT;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.checkpoint_tag, tag, sizeof(msg.checkpoint_tag));
    
    printf("Creating checkpoint '%s' for '%s'...\n", tag, filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send CHECKPOINT request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle VIEWCHECKPOINT command
void handle_viewcheckpoint(const char *filename, const char *tag) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_VIEWCHECKPOINT;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.checkpoint_tag, tag, sizeof(msg.checkpoint_tag));
    
    printf("Viewing checkpoint '%s' for '%s'...\n", tag, filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send VIEWCHECKPOINT request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("═══════════════════════════════════════════════════════════\n");
        printf("Checkpoint '%s' content:\n", tag);
        printf("═══════════════════════════════════════════════════════════\n");
        printf("%s\n", msg.data);
        printf("═══════════════════════════════════════════════════════════\n");
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle REVERT command
void handle_revert(const char *filename, const char *tag) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REVERT;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.checkpoint_tag, tag, sizeof(msg.checkpoint_tag));
    
    printf("Reverting '%s' to checkpoint '%s'...\n", filename, tag);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send REVERT request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle LISTCHECKPOINTS command
void handle_listcheckpoints(const char *filename) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LISTCHECKPOINTS;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Listing checkpoints for '%s'...\n", filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send LISTCHECKPOINTS request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("═══════════════════════════════════════════════════════════\n");
        printf("%s\n", msg.data);
        printf("═══════════════════════════════════════════════════════════\n");
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle REQUESTACCESS command
void handle_requestaccess(const char *filename, const char *access_type) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REQUESTACCESS;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    // Parse access type: -R (read), -W (write), -RW (both)
    if (strcmp(access_type, "-R") == 0) {
        msg.flags = 1;  // Read only
    } else if (strcmp(access_type, "-W") == 0) {
        msg.flags = 2;  // Write only
    } else if (strcmp(access_type, "-RW") == 0 || strcmp(access_type, "-WR") == 0) {
        msg.flags = 3;  // Read and write
    } else {
        printf("Usage: REQUESTACCESS -R|-W|-RW <filename>\n");
        return;
    }
    
    printf("Requesting %s access to '%s'...\n", access_type, filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send REQUESTACCESS request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle VIEWREQUESTS command
void handle_viewrequests(const char *filename) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_VIEWREQUESTS;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    
    printf("Viewing access requests for '%s'...\n", filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send VIEWREQUESTS request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("═══════════════════════════════════════════════════════════\n");
        printf("%s\n", msg.data);
        printf("═══════════════════════════════════════════════════════════\n");
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Handle RESPONDREQUEST command
void handle_respondrequest(const char *filename, int request_id, int approve) {
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_RESPONDREQUEST;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    msg.request_id = request_id;
    msg.flags = approve;  // 1=approve, 0=deny
    
    printf("%s request %d for '%s'...\n", approve ? "Approving" : "Denying", request_id, filename);
    fflush(stdout);
    
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send RESPONDREQUEST request\n");
        return;
    }
    
    memset(&msg, 0, sizeof(msg));
    
    if (recv_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
    
    if (msg.error_code == RESP_SUCCESS) {
        printf("✓ %s\n", msg.data);
    } else {
        printf("✗ %s\n", msg.data);
    }
}

// Parse and execute command
void execute_command(char *command) {
    char *cmd = strtok(command, " \n");
    if (!cmd) return;
    
    if (strcmp(cmd, "CREATE") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_create(filename);
        } else {
            printf("Usage: CREATE <filename>\n");
        }
    }
    else if (strcmp(cmd, "READ") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_read(filename);
        } else {
            printf("Usage: READ <filename>\n");
        }
    }
    else if (strcmp(cmd, "DELETE") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_delete(filename);
        } else {
            printf("Usage: DELETE <filename>\n");
        }
    }
    else if (strcmp(cmd, "VIEW") == 0) {
        char *flags = strtok(NULL, " \n");
        int show_all = 0, show_details = 0;
        if (flags) {
            if (strchr(flags, 'a')) show_all = 1;
            if (strchr(flags, 'l')) show_details = 1;
        }
        handle_view(show_all, show_details);
    }
    else if (strcmp(cmd, "INFO") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_info(filename);
        } else {
            printf("Usage: INFO <filename>\n");
        }
    }
    else if (strcmp(cmd, "WRITE") == 0) {
        char *filename = strtok(NULL, " \n");
        char *sentence_num_str = strtok(NULL, " \n");
        if (filename && sentence_num_str) {
            int sentence_num = atoi(sentence_num_str);
            handle_write(filename, sentence_num);
        } else {
            printf("Usage: WRITE <filename> <sentence_number>\n");
        }
    }
    else if (strcmp(cmd, "STREAM") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_stream(filename);
        } else {
            printf("Usage: STREAM <filename>\n");
        }
    }
    else if (strcmp(cmd, "UNDO") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_undo(filename);
        } else {
            printf("Usage: UNDO <filename>\n");
        }
    }
    else if (strcmp(cmd, "LIST") == 0) {
        handle_list();
    }
    else if (strcmp(cmd, "LISTSS") == 0) {
        // List storage servers
        struct Message msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_LIST_SS;
        strncpy(msg.username, username, sizeof(msg.username));
        
        if (send_message(ns_socket, &msg) < 0 || recv_message(ns_socket, &msg) < 0) {
            printf("✗ Error: Failed to get storage server list\n");
        } else if (msg.error_code == RESP_SUCCESS) {
            printf("\n╔════════════════════════════════════════════════════════════════╗\n");
            printf("║               Storage Servers                                  ║\n");
            printf("╠════════════════════════════════════════════════════════════════╣\n");
            printf("║ ID         Address           Status                           ║\n");
            printf("╠════════════════════════════════════════════════════════════════╣\n");
            printf("%s", msg.data);
            printf("╚════════════════════════════════════════════════════════════════╝\n");
        } else {
            printf("✗ Error: %s\n", msg.data);
        }
    }
    else if (strcmp(cmd, "ADDACCESS") == 0) {
        char *flag = strtok(NULL, " \n");
        char *filename = strtok(NULL, " \n");
        char *target_user = strtok(NULL, " \n");
        if (flag && filename && target_user) {
            handle_addaccess(flag, filename, target_user);
        } else {
            printf("Usage: ADDACCESS -R/-W <filename> <username>\n");
            printf("  -R: Grant read access\n");
            printf("  -W: Grant write access (includes read)\n");
        }
    }
    else if (strcmp(cmd, "REMACCESS") == 0) {
        char *filename = strtok(NULL, " \n");
        char *target_user = strtok(NULL, " \n");
        if (filename && target_user) {
            handle_remaccess(filename, target_user);
        } else {
            printf("Usage: REMACCESS <filename> <username>\n");
        }
    }
    else if (strcmp(cmd, "EXEC") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_exec(filename);
        } else {
            printf("Usage: EXEC <filename>\n");
        }
    }
    else if (strcmp(cmd, "SEARCH") == 0) {
        char *pattern = strtok(NULL, "\n");  // Get rest of line as pattern
        if (pattern) {
            // Trim leading whitespace
            while (*pattern == ' ' || *pattern == '\t') pattern++;
            if (strlen(pattern) > 0) {
                handle_search(pattern);
            } else {
                printf("Usage: SEARCH <pattern>\n");
            }
        } else {
            printf("Usage: SEARCH <pattern>\n");
        }
    }
    else if (strcmp(cmd, "USE") == 0) {
        char *ss_id = strtok(NULL, " \n");
        handle_use_ss(ss_id);  // NULL shows current, non-NULL sets new SS
    }
    else if (strcmp(cmd, "CREATEFOLDER") == 0) {
        char *foldername = strtok(NULL, " \n");
        if (foldername) {
            handle_createfolder(foldername);
        } else {
            printf("Usage: CREATEFOLDER <foldername>\n");
        }
    }
    else if (strcmp(cmd, "VIEWFOLDER") == 0) {
        char *foldername = strtok(NULL, " \n");
        // If no foldername provided, view root (pass NULL)
        handle_viewfolder(foldername);
    }
    else if (strcmp(cmd, "MOVE") == 0) {
        char *filename = strtok(NULL, " \n");
        char *foldername = strtok(NULL, " \n");
        if (filename) {
            // If no foldername provided, move to root (pass NULL)
            handle_move(filename, foldername);
        } else {
            printf("Usage: MOVE <filename> [foldername]\n");
            printf("       MOVE <filename>          - Move to root folder\n");
            printf("       MOVE <filename> <folder> - Move to specified folder\n");
        }
    }
    else if (strcmp(cmd, "CHECKPOINT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_checkpoint(filename, tag);
        } else {
            printf("Usage: CHECKPOINT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "VIEWCHECKPOINT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_viewcheckpoint(filename, tag);
        } else {
            printf("Usage: VIEWCHECKPOINT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "REVERT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_revert(filename, tag);
        } else {
            printf("Usage: REVERT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "LISTCHECKPOINTS") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_listcheckpoints(filename);
        } else {
            printf("Usage: LISTCHECKPOINTS <filename>\n");
        }
    }
    else if (strcmp(cmd, "REQUESTACCESS") == 0) {
        char *access_type = strtok(NULL, " \n");
        char *filename = strtok(NULL, " \n");
        if (access_type && filename) {
            handle_requestaccess(filename, access_type);
        } else {
            printf("Usage: REQUESTACCESS -R|-W|-RW <filename>\n");
            printf("  -R:  Request read access\n");
            printf("  -W:  Request write access\n");
            printf("  -RW: Request read and write access\n");
        }
    }
    else if (strcmp(cmd, "VIEWREQUESTS") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_viewrequests(filename);
        } else {
            printf("Usage: VIEWREQUESTS <filename>\n");
        }
    }
    else if (strcmp(cmd, "APPROVEREQUEST") == 0) {
        char *filename = strtok(NULL, " \n");
        char *request_id_str = strtok(NULL, " \n");
        if (filename && request_id_str) {
            int request_id = atoi(request_id_str);
            handle_respondrequest(filename, request_id, 1);  // 1 = approve
        } else {
            printf("Usage: APPROVEREQUEST <filename> <request_id>\n");
        }
    }
    else if (strcmp(cmd, "DENYREQUEST") == 0) {
        char *filename = strtok(NULL, " \n");
        char *request_id_str = strtok(NULL, " \n");
        if (filename && request_id_str) {
            int request_id = atoi(request_id_str);
            handle_respondrequest(filename, request_id, 0);  // 0 = deny
        } else {
            printf("Usage: DENYREQUEST <filename> <request_id>\n");
        }
    }
    else if (strcmp(cmd, "HELP") == 0) {
        printf("\n╔════════════════════════════════════════════════════════════════╗\n");
        printf("║                    Available Commands                          ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Basic Operations:                                              ║\n");
        printf("║  CREATE <filename>          - Create a new file                ║\n");
        printf("║  READ <filename>            - Read file content                ║\n");
        printf("║  DELETE <filename>          - Delete a file                    ║\n");
        printf("║  VIEW [-a] [-l]             - List files                       ║\n");
        printf("║  INFO <filename>            - Get file information             ║\n");
        printf("║  LIST                       - List all users                   ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Advanced Operations:                                           ║\n");
        printf("║  WRITE <file> <sent#>       - Write to file (interactive)      ║\n");
        printf("║  STREAM <filename>          - Stream file content              ║\n");
        printf("║  UNDO <filename>            - Undo last change                 ║\n");
        printf("║  EXEC <filename>            - Execute file as commands         ║\n");
        printf("║  SEARCH <pattern>           - Search for files by name         ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Storage Server Selection:                                      ║\n");
        printf("║  USE <SS_ID>                - Select storage server for files  ║\n");
        printf("║  USE                        - Show current storage server      ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Folder Operations:                                             ║\n");
        printf("║  CREATEFOLDER <folder>      - Create a new folder              ║\n");
        printf("║  VIEWFOLDER [folder]        - View folder contents             ║\n");
        printf("║  MOVE <file> [folder]       - Move file to folder              ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Checkpoint Operations:                                         ║\n");
        printf("║  CHECKPOINT <file> <tag>    - Create checkpoint with tag       ║\n");
        printf("║  VIEWCHECKPOINT <file> <tag>- View checkpoint content          ║\n");
        printf("║  REVERT <file> <tag>        - Revert to checkpoint             ║\n");
        printf("║  LISTCHECKPOINTS <file>     - List all checkpoints             ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║ Access Control:                                                ║\n");
        printf("║  ADDACCESS -R/-W <file> <user>  - Grant access                ║\n");
        printf("║  REMACCESS <file> <user>        - Remove access               ║\n");
        printf("║  REQUESTACCESS -R|-W|-RW <file> - Request access              ║\n");
        printf("║  VIEWREQUESTS <file>            - View pending requests (owner)║\n");
        printf("║  APPROVEREQUEST <file> <id>     - Approve request (owner)     ║\n");
        printf("║  DENYREQUEST <file> <id>        - Deny request (owner)        ║\n");
        printf("╠════════════════════════════════════════════════════════════════╣\n");
        printf("║  EXIT                       - Quit client                      ║\n");
        printf("╚════════════════════════════════════════════════════════════════╝\n\n");
    }
    else if (strcmp(cmd, "EXIT") == 0 || strcmp(cmd, "QUIT") == 0) {
        printf("Goodbye!\n");
        if (ns_socket >= 0) {
            close(ns_socket);
        }
        exit(0);
    }
    else {
        printf("Unknown command. Type HELP for available commands.\n");
    }
}

int main(int argc, char *argv[]) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Docs++ Distributed File System       ║\n");
    printf("║  Client v1.0                           ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    // Get username
    printf("Enter your username: ");
    fflush(stdout);
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "Failed to read username\n");
        return 1;
    }
    username[strcspn(username, "\n")] = 0;  // Remove newline
    
    if (strlen(username) == 0) {
        fprintf(stderr, "Username cannot be empty\n");
        return 1;
    }
    
    printf("\n✓ Hello, %s!\n\n", username);
    
    // Parse command line arguments for NS address
    if (argc >= 3) {
        strncpy(ns_ip, argv[1], sizeof(ns_ip));
        ns_port = atoi(argv[2]);
    }
    
    // Connect to naming server
    printf("Connecting to Naming Server at %s:%d...\n", ns_ip, ns_port);
    if (connect_to_ns() < 0) {
        fprintf(stderr, "✗ Failed to connect to Naming Server\n");
        fprintf(stderr, "  Make sure the Naming Server is running!\n");
        return 1;
    }
    
    printf("✓ Connected successfully!\n\n");
    printf("Type HELP for available commands\n");
    printf("────────────────────────────────────────\n\n");
    
    // Command loop
    char command[BUFFER_SIZE];
    while (1) {
        printf("%s> ", username);
        fflush(stdout);
        
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        
        // Remove trailing newline
        command[strcspn(command, "\n")] = 0;
        
        // Skip empty commands
        if (strlen(command) == 0) {
            continue;
        }
        
        execute_command(command);
        printf("\n");
    }
    
    if (ns_socket >= 0) {
        close(ns_socket);
    }
    
    printf("\nGoodbye!\n");
    return 0;
}
