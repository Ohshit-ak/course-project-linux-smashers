#ifndef USER_SESSION_MANAGER_H
#define USER_SESSION_MANAGER_H

#include <time.h>
#include <pthread.h>
#include "../common/protocol.h"

// User registry structure
typedef struct UserEntry {
    char username[MAX_USERNAME];
    time_t registered_at;
    struct UserEntry *next;
} UserEntry;

// Active session structure
typedef struct ActiveSession {
    char username[MAX_USERNAME];
    int client_socket;
    char client_ip[16];
    time_t login_time;
    struct ActiveSession *next;
} ActiveSession;

// Function declarations
void init_users_and_sessions();
void register_user(const char *username);
char* get_all_users();
ActiveSession* find_active_session(const char *username);
int add_active_session(const char *username, int client_socket, const char *client_ip);
void remove_active_session(const char *username);
void cleanup_users_and_sessions();

// External global variables
extern UserEntry *registered_users;
extern ActiveSession *active_sessions;
extern pthread_mutex_t user_lock;
extern pthread_mutex_t session_lock;

#endif // USER_SESSION_MANAGER_H
