#include "file_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

FileEntry *file_table[HASH_TABLE_SIZE];
pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

// Initialize file table
void init_file_table() {
    memset(file_table, 0, sizeof(file_table));
}

// Hash function for file lookup
unsigned int hash_function(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_TABLE_SIZE;
}

// Add file to hash table
void add_file(struct FileInfo *info, const char *ss_id) {
    unsigned int index = hash_function(info->name);
    
    pthread_mutex_lock(&table_lock);
    
    FileEntry *entry = malloc(sizeof(FileEntry));
    memcpy(&entry->info, info, sizeof(struct FileInfo));
    strncpy(entry->info.storage_server_id, ss_id, sizeof(entry->info.storage_server_id));
    entry->acl = NULL;
    entry->checkpoints = NULL;
    entry->access_requests = NULL;
    entry->next = file_table[index];
    file_table[index] = entry;
    
    pthread_mutex_unlock(&table_lock);
    
    log_message("naming_server", "Added file to registry");
}

// Lookup file in hash table
FileEntry* lookup_file(const char *filename) {
    unsigned int index = hash_function(filename);
    
    pthread_mutex_lock(&table_lock);
    
    FileEntry *current = file_table[index];
    while (current != NULL) {
        if (strcmp(current->info.name, filename) == 0) {
            pthread_mutex_unlock(&table_lock);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    return NULL;
}

// Delete file entry from hash table
int delete_file_entry(const char *filename) {
    unsigned int index = hash_function(filename);
    
    pthread_mutex_lock(&table_lock);
    
    FileEntry *prev = NULL;
    FileEntry *current = file_table[index];
    
    while (current != NULL) {
        if (strcmp(current->info.name, filename) == 0) {
            if (prev == NULL) {
                file_table[index] = current->next;
            } else {
                prev->next = current->next;
            }
            
            // Free ACL entries
            AccessControl *acl = current->acl;
            while (acl != NULL) {
                AccessControl *next_acl = acl->next;
                free(acl);
                acl = next_acl;
            }
            
            // Free checkpoint entries
            CheckpointEntry *cp = current->checkpoints;
            while (cp != NULL) {
                CheckpointEntry *next_cp = cp->next;
                free(cp);
                cp = next_cp;
            }
            
            // Free access request entries
            AccessRequestNode *req = current->access_requests;
            while (req != NULL) {
                AccessRequestNode *next_req = req->next;
                free(req);
                req = next_req;
            }
            
            free(current);
            pthread_mutex_unlock(&table_lock);
            return 1;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    return 0;
}

// Cleanup file table (call on shutdown)
void cleanup_file_table() {
    pthread_mutex_lock(&table_lock);
    
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *entry = file_table[i];
        while (entry != NULL) {
            FileEntry *next_entry = entry->next;
            
            // Free ACL
            AccessControl *acl = entry->acl;
            while (acl != NULL) {
                AccessControl *next_acl = acl->next;
                free(acl);
                acl = next_acl;
            }
            
            // Free checkpoints
            CheckpointEntry *cp = entry->checkpoints;
            while (cp != NULL) {
                CheckpointEntry *next_cp = cp->next;
                free(cp);
                cp = next_cp;
            }
            
            // Free access requests
            AccessRequestNode *req = entry->access_requests;
            while (req != NULL) {
                AccessRequestNode *next_req = req->next;
                free(req);
                req = next_req;
            }
            
            free(entry);
            entry = next_entry;
        }
        file_table[i] = NULL;
    }
    
    pthread_mutex_unlock(&table_lock);
}
