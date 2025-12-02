/*
 * Client - Main Entry Point (Modular Version)
 * 
 * This modular version separates different client functionalities into
 * specialized modules for better maintainability and code organization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Common includes
#include "../common/protocol.h"
#include "../common/utils.h"

// Module includes
#include "connection_manager.h"
#include "file_operations_client.h"
#include "access_manager.h"
#include "folder_operations.h"
#include "checkpoint_operations.h"
#include "advanced_operations.h"
#include "command_parser.h"

#define BUFFER_SIZE 4096

// Global state
int ns_socket = -1;
char username[MAX_USERNAME];
char ns_ip[16] = "127.0.0.1";
int ns_port = 8080;
volatile int ns_alive = 1;
char selected_ss_id[64] = "";  // Currently selected storage server

int main(int argc, char *argv[]) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Docs++ Distributed File System       ║\n");
    printf("║  Client v1.0 (Modular)                 ║\n");
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
