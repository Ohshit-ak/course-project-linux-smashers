#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <time.h>
#include <pthread.h>
#include "../common/protocol.h"

#define HASH_TABLE_SIZE 1024

// Forward declarations
typedef struct AccessControl AccessControl;
typedef struct CheckpointEntry CheckpointEntry;
typedef struct AccessRequestNode AccessRequestNode;

// Access control entry
struct AccessControl {
    char username[MAX_USERNAME];
    int can_read;
    int can_write;
    struct AccessControl *next;
};

// Checkpoint structure
struct CheckpointEntry {
    char tag[MAX_FILENAME];
    char creator[MAX_USERNAME];
    time_t created_at;
    long size;
    struct CheckpointEntry *next;
};

// Access request structure (internal linked list node)
struct AccessRequestNode {
    int request_id;
    char requester[MAX_USERNAME];
    int access_type;  // 1=read, 2=write, 3=both
    time_t requested_at;
    int status;  // 0=pending, 1=approved, 2=denied
    struct AccessRequestNode *next;
};

// File metadata structure
typedef struct FileEntry {
    struct FileInfo info;
    struct AccessControl *acl;
    struct CheckpointEntry *checkpoints;
    struct AccessRequestNode *access_requests;
    struct FileEntry *next;  // For hash table chaining
} FileEntry;

// Function declarations
void init_file_table();
unsigned int hash_function(const char *str);
void add_file(struct FileInfo *info, const char *ss_id);
FileEntry* lookup_file(const char *filename);
int delete_file_entry(const char *filename);
void cleanup_file_table();

// External global variables
extern FileEntry *file_table[HASH_TABLE_SIZE];
extern pthread_mutex_t table_lock;

#endif // FILE_MANAGER_H
