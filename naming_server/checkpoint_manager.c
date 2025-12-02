#include "checkpoint_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern pthread_mutex_t table_lock;

// Add checkpoint to file
int add_checkpoint(FileEntry *entry, const char *tag, const char *creator) {
    pthread_mutex_lock(&table_lock);
    
    // Check if checkpoint with this tag already exists
    CheckpointEntry *cp = entry->checkpoints;
    while (cp != NULL) {
        if (strcmp(cp->tag, tag) == 0) {
            pthread_mutex_unlock(&table_lock);
            return -1;  // Checkpoint already exists
        }
        cp = cp->next;
    }
    
    // Create new checkpoint entry
    CheckpointEntry *new_cp = malloc(sizeof(CheckpointEntry));
    strncpy(new_cp->tag, tag, sizeof(new_cp->tag));
    strncpy(new_cp->creator, creator, sizeof(new_cp->creator));
    new_cp->created_at = time(NULL);
    new_cp->size = entry->info.size;
    new_cp->next = entry->checkpoints;
    entry->checkpoints = new_cp;
    
    pthread_mutex_unlock(&table_lock);
    return 0;
}

// Find checkpoint by tag
CheckpointEntry* find_checkpoint(FileEntry *entry, const char *tag) {
    pthread_mutex_lock(&table_lock);
    
    CheckpointEntry *cp = entry->checkpoints;
    while (cp != NULL) {
        if (strcmp(cp->tag, tag) == 0) {
            pthread_mutex_unlock(&table_lock);
            return cp;
        }
        cp = cp->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    return NULL;
}

// List all checkpoints for a file
char* list_checkpoints(FileEntry *entry) {
    static char results[MAX_DATA];
    memset(results, 0, sizeof(results));
    
    pthread_mutex_lock(&table_lock);
    
    CheckpointEntry *cp = entry->checkpoints;
    int count = 0;
    
    while (cp != NULL) {
        if (count > 0) {
            strncat(results, "\n", sizeof(results) - strlen(results) - 1);
        }
        
        char line[512];
        snprintf(line, sizeof(line), "  [%s] Created by %s at %s (size: %ld bytes)",
                 cp->tag, cp->creator, format_time(cp->created_at), cp->size);
        strncat(results, line, sizeof(results) - strlen(results) - 1);
        count++;
        cp = cp->next;
    }
    
    pthread_mutex_unlock(&table_lock);
    
    if (count == 0) {
        snprintf(results, sizeof(results), "No checkpoints found for this file");
    } else {
        char header[256];
        snprintf(header, sizeof(header), "Checkpoints for '%s' (%d total):\n", entry->info.name, count);
        char temp[MAX_DATA];
        strncpy(temp, results, sizeof(temp) - 1);
        snprintf(results, sizeof(results), "%s%s", header, temp);
    }
    
    return results;
}
