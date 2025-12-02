#include "folder_operations.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <string.h>

// Handle CREATEFOLDER command
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

// Handle VIEWFOLDER command
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

// Handle MOVE command
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
