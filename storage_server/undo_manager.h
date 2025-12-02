#ifndef UNDO_MANAGER_H
#define UNDO_MANAGER_H

#include <pthread.h>
#include "../common/protocol.h"

// Undo state tracking structure
typedef struct UndoState {
    char filename[MAX_FILENAME];
    int undo_performed;
    struct UndoState *next;
} UndoState;

// Undo management functions
UndoState* get_undo_state(const char *filename);
void set_undo_state(const char *filename, int undo_performed);
void cleanup_undo_states();

// External global variables
extern UndoState *undo_states;
extern pthread_mutex_t undo_mutex;

#endif // UNDO_MANAGER_H
