#include "user_session_manager.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

UserEntry *registered_users = NULL;
ActiveSession *active_sessions = NULL;
pthread_mutex_t user_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

// Initialize users and sessions
void init_users_and_sessions() {
    registered_users = NULL;
    active_sessions = NULL;
}

// Register user if not already registered
void register_user(const char *username) {
    pthread_mutex_lock(&user_lock);
    
    // Check if user already exists
    UserEntry *current = registered_users;
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&user_lock);
            return;  // User already registered
        }
        current = current->next;
    }
    
    // Add new user
    UserEntry *new_user = malloc(sizeof(UserEntry));
    strncpy(new_user->username, username, sizeof(new_user->username));
    new_user->registered_at = time(NULL);
    new_user->next = registered_users;
    registered_users = new_user;
    
    pthread_mutex_unlock(&user_lock);
}

// Get all registered users
char* get_all_users() {
    pthread_mutex_lock(&user_lock);
    
    static char user_list[MAX_DATA * 2];
    memset(user_list, 0, sizeof(user_list));
    
    int count = 0;
    UserEntry *current = registered_users;
    
    while (current != NULL) {
        if (count > 0) {
            strncat(user_list, "\n", sizeof(user_list) - strlen(user_list) - 1);
        }
        strncat(user_list, current->username, sizeof(user_list) - strlen(user_list) - 1);
        count++;
        current = current->next;
    }
    
    pthread_mutex_unlock(&user_lock);
    
    if (count == 0) {
        strncpy(user_list, "(no users registered)", sizeof(user_list));
    }
    
    return user_list;
}

// Check if user already has active session
ActiveSession* find_active_session(const char *username) {
    pthread_mutex_lock(&session_lock);
    
    ActiveSession *current = active_sessions;
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&session_lock);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&session_lock);
    return NULL;
}

// Add active session
int add_active_session(const char *username, int client_socket, const char *client_ip) {
    pthread_mutex_lock(&session_lock);
    
    // Check if user already has active session
    ActiveSession *existing = active_sessions;
    while (existing != NULL) {
        if (strcmp(existing->username, username) == 0) {
            pthread_mutex_unlock(&session_lock);
            return 0;  // Session already exists
        }
        existing = existing->next;
    }
    
    // Create new session
    ActiveSession *new_session = malloc(sizeof(ActiveSession));
    strncpy(new_session->username, username, sizeof(new_session->username));
    new_session->client_socket = client_socket;
    strncpy(new_session->client_ip, client_ip, sizeof(new_session->client_ip));
    new_session->login_time = time(NULL);
    new_session->next = active_sessions;
    active_sessions = new_session;
    
    pthread_mutex_unlock(&session_lock);
    return 1;  // Session added successfully
}

// Remove active session
void remove_active_session(const char *username) {
    pthread_mutex_lock(&session_lock);
    
    ActiveSession *current = active_sessions;
    ActiveSession *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->username, username) == 0) {
            if (prev == NULL) {
                active_sessions = current->next;
            } else {
                prev->next = current->next;
            }
            
            printf("✓ Session ended for user: %s\n", username);
            free(current);
            pthread_mutex_unlock(&session_lock);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&session_lock);
}

// Cleanup users and sessions (call on shutdown)
void cleanup_users_and_sessions() {
    pthread_mutex_lock(&user_lock);
    UserEntry *user = registered_users;
    while (user != NULL) {
        UserEntry *next = user->next;
        free(user);
        user = next;
    }
    registered_users = NULL;
    pthread_mutex_unlock(&user_lock);
    
    pthread_mutex_lock(&session_lock);
    ActiveSession *session = active_sessions;
    while (session != NULL) {
        ActiveSession *next = session->next;
        free(session);
        session = next;
    }
    active_sessions = NULL;
    pthread_mutex_unlock(&session_lock);
}
