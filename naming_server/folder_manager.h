#ifndef FOLDER_MANAGER_H
#define FOLDER_MANAGER_H

#include <time.h>
#include <pthread.h>
#include "../common/protocol.h"
#include "file_manager.h"

// Folder structure
typedef struct FolderEntry {
    char name[MAX_FILENAME];
    char owner[MAX_USERNAME];
    time_t created_at;
    struct FolderEntry *next;
} FolderEntry;

// Function declarations
void init_folders();
int folder_exists(const char *folder_path);
int create_folder(const char *folder_path, const char *owner);
char* list_folder_files(const char *folder_path);
int move_file_to_folder(FileEntry *entry, const char *folder_path);
void cleanup_folders();

// External global variables
extern FolderEntry *folders;
extern pthread_mutex_t folder_lock;

#endif // FOLDER_MANAGER_H
