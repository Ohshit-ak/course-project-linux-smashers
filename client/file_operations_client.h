#ifndef FILE_OPERATIONS_CLIENT_H
#define FILE_OPERATIONS_CLIENT_H

// File operation handlers
void handle_create(const char *filename);
void handle_read(const char *filename);
void handle_delete(const char *filename);
void handle_view(int show_all, int show_details);
void handle_info(const char *filename);
void handle_list();
void handle_use_ss(const char *ss_id);

// External globals
extern int ns_socket;
extern char username[256];
extern char selected_ss_id[64];

#endif // FILE_OPERATIONS_CLIENT_H
