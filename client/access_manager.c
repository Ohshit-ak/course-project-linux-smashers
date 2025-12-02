#include "access_manager.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <string.h>

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
