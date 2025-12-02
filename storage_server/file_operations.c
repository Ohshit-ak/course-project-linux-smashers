#include "file_operations.h"
#include "../common/utils.h"
#include "../common/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#define BASE_STORAGE_DIR "./storage/"
#define BASE_BACKUP_DIR "./backups/"

// External globals (defined in storage_server_modular.c)
extern char ss_id[64];
extern char storage_dir[MAX_PATH];
extern char backup_dir[MAX_PATH];

// Initialize storage directories
void init_storage() {
    // Get current working directory for visibility
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("📂 Storage Server Working Directory: %s\n", cwd);
    }
    
    // Create base directories if they don't exist
    mkdir(BASE_STORAGE_DIR, 0777);
    mkdir(BASE_BACKUP_DIR, 0777);
    
    // Create SS-specific subdirectories: ./storage/SS1/ and ./backups/SS1/
    snprintf(storage_dir, sizeof(storage_dir), "%s%s/", BASE_STORAGE_DIR, ss_id);
    snprintf(backup_dir, sizeof(backup_dir), "%s%s/", BASE_BACKUP_DIR, ss_id);
    
    mkdir(storage_dir, 0777);
    mkdir(backup_dir, 0777);
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Storage directories initialized: %s and %s", storage_dir, backup_dir);
    log_message("storage_server", log_msg);
    
    // Display full absolute paths for visibility
    char abs_storage[1024], abs_backup[1024];
    realpath(storage_dir, abs_storage);
    realpath(backup_dir, abs_backup);
    
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║ 📁 STORAGE SERVER DIRECTORIES                                  ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Storage ID: %-50s ║\n", ss_id);
    printf("║ Storage:    %-50s ║\n", abs_storage);
    printf("║ Backups:    %-50s ║\n", abs_backup);
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
}

// List all files in storage directory
int list_files(char files[][MAX_FILENAME]) {
    DIR *dir = opendir(storage_dir);
    if (!dir) {
        log_error("storage_server", "Could not open storage directory");
        return 0;
    }
    
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Regular file
            strncpy(files[count], entry->d_name, MAX_FILENAME);
            count++;
            if (count >= MAX_FILES) break;
        }
    }
    
    closedir(dir);
    return count;
}

// Create a file
int create_file(const char *filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (file_exists(filepath)) {
        return ERR_FILE_EXISTS;
    }
    
    // Ensure parent directory exists
    char *last_slash = strrchr(filepath, '/');
    if (last_slash != NULL && last_slash != filepath) {
        char dirpath[MAX_PATH];
        size_t dir_len = last_slash - filepath;
        strncpy(dirpath, filepath, dir_len);
        dirpath[dir_len] = '\0';
        
        // Create directory recursively
        char *p = dirpath + strlen(storage_dir);
        for (; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(dirpath, 0777);
                *p = '/';
            }
        }
        mkdir(dirpath, 0777);
    }
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        log_error("storage_server", "Failed to create file");
        return ERR_SERVER_ERROR;
    }
    
    fclose(fp);
    
    // Create initial backup copy
    char backup_path[MAX_PATH];
    snprintf(backup_path, sizeof(backup_path), "%s%s", backup_dir, filename);
    
    // Ensure backup parent directory exists
    char *backup_slash = strrchr(backup_path, '/');
    if (backup_slash != NULL && backup_slash != backup_path) {
        char backup_dirpath[MAX_PATH];
        size_t dir_len = backup_slash - backup_path;
        strncpy(backup_dirpath, backup_path, dir_len);
        backup_dirpath[dir_len] = '\0';
        
        char *p = backup_dirpath + strlen(backup_dir);
        for (; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(backup_dirpath, 0777);
                *p = '/';
            }
        }
        mkdir(backup_dirpath, 0777);
    }
    
    // Create empty backup file
    FILE *backup_fp = fopen(backup_path, "w");
    if (backup_fp) {
        fclose(backup_fp);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Created file: %s (with backup)", filename);
    log_message("storage_server", msg);
    
    return RESP_SUCCESS;
}

// Read file content
int read_file(const char *filename, char *buffer, int buffer_size) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        return ERR_SERVER_ERROR;
    }
    
    int bytes_read = fread(buffer, 1, buffer_size - 1, fp);
    buffer[bytes_read] = '\0';
    fclose(fp);
    
    return RESP_SUCCESS;
}

// Delete a file
int delete_file(const char *filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    if (remove(filepath) != 0) {
        log_error("storage_server", "Failed to delete file");
        return ERR_SERVER_ERROR;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Deleted file: %s", filename);
    log_message("storage_server", msg);
    
    return RESP_SUCCESS;
}

// Get file information
int file_info(const char* filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    if (!file_exists(filepath)) {
        return ERR_FILE_NOT_FOUND;
    }
    
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return ERR_SERVER_ERROR;
    }
    
    printf("File: %s\n", filename);
    printf("Size: %ld bytes\n", st.st_size);
    printf("Created: %s", ctime(&st.st_ctime));
    printf("Last Modified: %s", ctime(&st.st_mtime));
    printf("Last Accessed: %s", ctime(&st.st_atime));
    
    return RESP_SUCCESS;
}
