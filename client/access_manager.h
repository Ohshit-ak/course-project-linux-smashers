#ifndef ACCESS_MANAGER_H
#define ACCESS_MANAGER_H

// Access control handlers
void handle_addaccess(const char *flag, const char *filename, const char *target_user);
void handle_remaccess(const char *filename, const char *target_user);
void handle_requestaccess(const char *filename, const char *access_type);
void handle_viewrequests(const char *filename);
void handle_respondrequest(const char *filename, int request_id, int approve);

// External globals
extern int ns_socket;
extern char username[256];

#endif // ACCESS_MANAGER_H
