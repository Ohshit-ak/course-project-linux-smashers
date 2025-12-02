#include "folder_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

FolderEntry *folders = NULL;
pthread_mutex_t folder_lock = PTHREAD_MUTEX_INITIALIZER;

// Initialize folders
void init_folders() {
    folders = NULL;
}

// Check if folder exists
int folder_exists(const char *folder_path) {
    pthread_mutex_lock(&folder_lock);
    FolderEntry *current = folders;
    while (current != NULL) {
        if (strcmp(current->name, folder_path) == 0) {
            pthread_mutex_unlock(&folder_lock);
            return 1;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&folder_lock);
    return 0;
}

// Create a new folder
int create_folder(const char *folder_path, const char *owner) {
    // Check if folder already exists
    if (folder_exists(folder_path)) {
        return ERR_FOLDER_EXISTS;
    }
    
    // Create parent folders if needed (e.g., "docs/photos" creates "docs" first)
    char parent_path[MAX_FILENAME] = "";
    char *last_slash = strrchr(folder_path, '/');
    if (last_slash != NULL) {
        size_t parent_len = last_slash - folder_path;
        strncpy(parent_path, folder_path, parent_len);
        parent_path[parent_len] = '\0';
        
        // Recursively create parent if it doesn't exist
        if (!folder_exists(parent_path) && strlen(parent_path) > 0) {
            int result = create_folder(parent_path, owner);
            if (result != RESP_SUCCESS) {
                return result;
            }
        }
    }
    
    pthread_mutex_lock(&folder_lock);
    
    FolderEntry *new_folder = malloc(sizeof(FolderEntry));
    strncpy(new_folder->name, folder_path, sizeof(new_folder->name) - 1);
    strncpy(new_folder->owner, owner, sizeof(new_folder->owner) - 1);
    new_folder->created_at = time(NULL);
    new_folder->next = folders;
    folders = new_folder;
    
    pthread_mutex_unlock(&folder_lock);
    
    printf("📁 Folder created: %s by %s\n", folder_path, owner);
    return RESP_SUCCESS;
}

// List all files in a folder
char* list_folder_files(const char *folder_path) {
    static char file_list[MAX_DATA];
    memset(file_list, 0, sizeof(file_list));
    
    extern FileEntry *file_table[];
    extern pthread_mutex_t table_lock;
    
    pthread_mutex_lock(&table_lock);
    
    int count = 0;
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *current = file_table[i];
        while (current != NULL) {
            // Check if file is in this folder
            if (strcmp(current->info.folder, folder_path) == 0) {
                if (count > 0) {
                    strncat(file_list, "\n", sizeof(file_list) - strlen(file_list) - 1);
                }
                strncat(file_list, current->info.name, sizeof(file_list) - strlen(file_list) - 1);
                count++;
            }
            current = current->next;
        }
    }
    
    pthread_mutex_unlock(&table_lock);
    
    if (count == 0) {
        strncpy(file_list, "(empty folder)", sizeof(file_list));
    }
    
    return file_list;
}

// Move file to folder (caller must provide valid FileEntry)
int move_file_to_folder(FileEntry *entry, const char *folder_path) {
    if (entry == NULL) {
        return ERR_FILE_NOT_FOUND;
    }
    
    extern pthread_mutex_t table_lock;
    pthread_mutex_lock(&table_lock);
    
    // Update file's folder
    strncpy(entry->info.folder, folder_path, sizeof(entry->info.folder) - 1);
    entry->info.folder[sizeof(entry->info.folder) - 1] = '\0';
    
    pthread_mutex_unlock(&table_lock);
    return RESP_SUCCESS;
}

// Cleanup folders (call on shutdown)
void cleanup_folders() {
    pthread_mutex_lock(&folder_lock);
    
    FolderEntry *current = folders;
    while (current != NULL) {
        FolderEntry *next = current->next;
        free(current);
        current = next;
    }
    folders = NULL;
    
    pthread_mutex_unlock(&folder_lock);
}
