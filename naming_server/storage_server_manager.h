#ifndef STORAGE_SERVER_MANAGER_H
#define STORAGE_SERVER_MANAGER_H

#include <time.h>
#include "../common/protocol.h"

// Storage server structure
typedef struct StorageServer {
    char id[64];
    char ip[16];
    int nm_port;
    int client_port;
    int ss_socket;  // Persistent connection socket
    int is_active;
    time_t last_heartbeat;
    int failed;  // 1 if detected as failed
    struct StorageServer *next;
} StorageServer;

// Function declarations
void init_storage_servers();
StorageServer* get_available_ss();
StorageServer* find_ss_by_id(const char *ss_id);
void register_storage_server(struct SSRegistration *reg);
void replicate_to_all_ss(struct Message *msg);
void* heartbeat_monitor(void *arg);

// External global variables
extern StorageServer *storage_servers;
extern volatile int shutdown_flag;

#endif // STORAGE_SERVER_MANAGER_H
