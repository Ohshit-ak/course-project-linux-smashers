#ifndef FOLDER_OPERATIONS_H
#define FOLDER_OPERATIONS_H

// Folder operation handlers
void handle_createfolder(const char *foldername);
void handle_viewfolder(const char *foldername);
void handle_move(const char *filename, const char *foldername);

// External globals
extern int ns_socket;
extern char username[256];
extern char selected_ss_id[64];

#endif // FOLDER_OPERATIONS_H
