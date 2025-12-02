#ifndef FILE_OPERATIONS_H
#define FILE_OPERATIONS_H

#include "../common/protocol.h"

// File operations
void init_storage();
int list_files(char files[][MAX_FILENAME]);
int create_file(const char *filename);
int read_file(const char *filename, char *buffer, int buffer_size);
int delete_file(const char *filename);
int file_info(const char *filename);

// External global variables (defined in main)
extern char storage_dir[MAX_PATH];
extern char backup_dir[MAX_PATH];
extern char ss_id[64];

#endif // FILE_OPERATIONS_H
