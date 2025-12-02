#include "connection_manager.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

// Background thread to monitor naming server connection
void* monitor_ns_connection(void *arg) {
    (void)arg;  // Unused parameter
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
            printf("\n✗ Login Failed\n");
            printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
            printf("%s\n", msg.data);
            printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
            printf("Please close the other session first or use a different username.\n\n");
            close(ns_socket);
            ns_socket = -1;
            return -1;
        } else {
            printf("\n✗ Login Failed: Server returned error code %d\n", msg.error_code);
            close(ns_socket);
            ns_socket = -1;
            return -1;
        }
    } else {
        printf("\n✗ Login Failed: No response from server\n");
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
