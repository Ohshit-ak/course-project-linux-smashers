#include "checkpoint_operations.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <string.h>

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
