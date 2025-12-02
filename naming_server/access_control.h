#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <pthread.h>
#include "file_manager.h"
#include "../common/protocol.h"

// Function declarations
int check_permission(FileEntry *entry, const char *username, int need_write);
int add_access(FileEntry *entry, const char *username, int can_read, int can_write);
int remove_access(FileEntry *entry, const char *username);

// Access request management
int add_access_request(FileEntry *entry, const char *requester, int access_type);
char* list_access_requests(FileEntry *entry);
int respond_to_request(FileEntry *entry, int request_id, int approve);

// External global variables
extern int next_request_id;
extern pthread_mutex_t request_lock;

#endif // ACCESS_CONTROL_H
