#ifndef ADVANCED_OPERATIONS_H
#define ADVANCED_OPERATIONS_H

// Advanced operation handlers
void handle_write(const char *filename, int sentence_num);
void handle_stream(const char *filename);
void handle_undo(const char *filename);
void handle_exec(const char *filename);
void handle_search(const char *pattern);

// External globals
extern int ns_socket;
extern char username[256];

#endif // ADVANCED_OPERATIONS_H
