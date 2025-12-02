#include "undo_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Global undo state list
UndoState *undo_states = NULL;
pthread_mutex_t undo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Get undo state for a file
UndoState* get_undo_state(const char *filename) {
    pthread_mutex_lock(&undo_mutex);
    UndoState *current = undo_states;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            pthread_mutex_unlock(&undo_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&undo_mutex);
    return NULL;
}

// Set undo state for a file (1 = undo performed, 0 = file modified)
void set_undo_state(const char *filename, int undo_performed) {
    pthread_mutex_lock(&undo_mutex);
    
    UndoState *current = undo_states;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            current->undo_performed = undo_performed;
            pthread_mutex_unlock(&undo_mutex);
            return;
        }
        current = current->next;
    }
    
    // Create new undo state entry
    UndoState *new_state = malloc(sizeof(UndoState));
    strncpy(new_state->filename, filename, sizeof(new_state->filename) - 1);
    new_state->filename[sizeof(new_state->filename) - 1] = '\0';
    new_state->undo_performed = undo_performed;
    new_state->next = undo_states;
    undo_states = new_state;
    
    pthread_mutex_unlock(&undo_mutex);
}
