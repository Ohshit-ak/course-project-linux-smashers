#include "persistence.h"
#include "file_manager.h"
#include "access_control.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REGISTRY_FILE "../naming_server/registry.dat"
#define CACHE_DIR "../cache"

extern FileEntry *file_table[];
extern pthread_mutex_t table_lock;

// Save file registry to disk
int save_file_registry(const char *filename) {
    pthread_mutex_lock(&table_lock);
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        pthread_mutex_unlock(&table_lock);
        log_error("naming_server", "Failed to save file registry");
        return -1;
    }
    
    // Count total files
    int file_count = 0;
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *entry = file_table[i];
        while (entry != NULL) {
            file_count++;
            entry = entry->next;
        }
    }
    
    // Write header
    fprintf(fp, "REGISTRY_V1\n");
    fprintf(fp, "%d\n", file_count);
    
    // Write each file entry
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileEntry *entry = file_table[i];
        while (entry != NULL) {
            // Write file info
            fprintf(fp, "FILE:%s:%s:%s:%ld:%ld:%ld:%ld:%d:%d\n",
                    entry->info.name,
                    entry->info.owner,
                    entry->info.storage_server_id,
                    entry->info.created_at,
                    entry->info.last_modified,
                    entry->info.last_accessed,
                    entry->info.size,
                    entry->info.word_count,
                    entry->info.char_count);
            
            // Write ACLs
            AccessControl *acl = entry->acl;
            while (acl != NULL) {
                fprintf(fp, "ACL:%s:%d:%d\n",
                        acl->username,
                        acl->can_read,
                        acl->can_write);
                acl = acl->next;
            }
            
            fprintf(fp, "END\n");
            entry = entry->next;
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&table_lock);
    
    log_message("naming_server", "File registry saved to disk");
    printf("✓ File registry saved (%d files)\n", file_count);
    return 0;
}

// Load file registry from disk
int load_file_registry(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        log_message("naming_server", "No existing registry found - starting fresh");
        return 0; // Not an error - just no existing registry
    }
    
    char line[1024];
    
    // Read header
    if (!fgets(line, sizeof(line), fp) || strncmp(line, "REGISTRY_V1", 11) != 0) {
        fclose(fp);
        log_error("naming_server", "Invalid registry format");
        return -1;
    }
    
    int file_count = 0;
    if (!fgets(line, sizeof(line), fp) || sscanf(line, "%d", &file_count) != 1) {
        fclose(fp);
        log_error("naming_server", "Invalid registry header");
        return -1;
    }
    
    printf("Loading %d files from registry...\n", file_count);
    
    // Read file entries
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "FILE:", 5) == 0) {
            struct FileInfo info;
            char *token = strtok(line + 5, ":");
            
            // Parse file info
            if (token) strncpy(info.name, token, sizeof(info.name));
            token = strtok(NULL, ":");
            if (token) strncpy(info.owner, token, sizeof(info.owner));
            token = strtok(NULL, ":");
            if (token) strncpy(info.storage_server_id, token, sizeof(info.storage_server_id));
            token = strtok(NULL, ":");
            if (token) info.created_at = atol(token);
            token = strtok(NULL, ":");
            if (token) info.last_modified = atol(token);
            token = strtok(NULL, ":");
            if (token) info.last_accessed = atol(token);
            token = strtok(NULL, ":");
            if (token) info.size = atol(token);
            token = strtok(NULL, ":");
            if (token) info.word_count = atoi(token);
            token = strtok(NULL, ":");
            if (token) info.char_count = atoi(token);
            
            // Add file to registry
            add_file(&info, info.storage_server_id);
            
            // Get the newly added file entry
            FileEntry *entry = lookup_file(info.name);
            
            // Read ACLs until END
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "ACL:", 4) == 0) {
                    char username[MAX_USERNAME];
                    int can_read, can_write;
                    
                    token = strtok(line + 4, ":");
                    if (token) strncpy(username, token, sizeof(username));
                    token = strtok(NULL, ":");
                    if (token) can_read = atoi(token);
                    token = strtok(NULL, ":");
                    if (token) can_write = atoi(token);
                    
                    if (entry) {
                        add_access(entry, username, can_read, can_write);
                    }
                } else if (strncmp(line, "END", 3) == 0) {
                    break;
                }
            }
            
            printf("  ✓ Loaded: %s (owner: %s, ACLs preserved)\n", info.name, info.owner);
        }
    }
    
    fclose(fp);
    log_message("naming_server", "File registry loaded from disk");
    return 0;
}
