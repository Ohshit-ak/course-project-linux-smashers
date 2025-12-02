#ifndef CHECKPOINT_MANAGER_H
#define CHECKPOINT_MANAGER_H

#include "file_manager.h"
#include "../common/protocol.h"

// Function declarations
int add_checkpoint(FileEntry *entry, const char *tag, const char *creator);
CheckpointEntry* find_checkpoint(FileEntry *entry, const char *tag);
char* list_checkpoints(FileEntry *entry);

#endif // CHECKPOINT_MANAGER_H
