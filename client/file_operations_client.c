#include "file_operations_client.h"
#include "connection_manager.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    
    // Validate SS exists by querying NS
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LIST_SS;  // Get list of storage servers
    strncpy(msg.username, username, sizeof(msg.username));
    
    if (send_message(ns_socket, &msg) < 0 || recv_message(ns_socket, &msg) < 0) {
        printf("✗ Error: Cannot validate storage server\n");
        fflush(stdout);
        return;
    }
    
    // Check if SS exists and is active in response
    if (strstr(msg.data, ss_id) == NULL) {
        printf("✗ Error: Storage server '%s' not found\n", ss_id);
        printf("   Use LISTSS command to see available storage servers\n");
        fflush(stdout);
        return;
    }
    
    // Check if SS is active (appears in first column followed by \t)
    char search_pattern[100];
    snprintf(search_pattern, sizeof(search_pattern), "%s\t", ss_id);
    char *ss_line = strstr(msg.data, search_pattern);
    if (ss_line && strstr(ss_line, "Inactive")) {
        printf("✗ Error: Storage server '%s' is currently inactive\n", ss_id);
        printf("   Use LISTSS command to see active storage servers\n");
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
