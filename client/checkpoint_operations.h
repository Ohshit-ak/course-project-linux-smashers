#ifndef CHECKPOINT_OPERATIONS_H
#define CHECKPOINT_OPERATIONS_H

// Checkpoint operation handlers
void handle_checkpoint(const char *filename, const char *tag);
void handle_viewcheckpoint(const char *filename, const char *tag);
void handle_revert(const char *filename, const char *tag);
void handle_listcheckpoints(const char *filename);

// External globals
extern int ns_socket;
extern char username[256];

#endif // CHECKPOINT_OPERATIONS_H
