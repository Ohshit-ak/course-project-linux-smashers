#include "advanced_operations.h"
#include "connection_manager.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

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
