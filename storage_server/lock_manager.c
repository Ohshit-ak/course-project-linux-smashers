#include "lock_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Global lock list
SentenceLock *locks = NULL;
pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;

// Check if sentence is locked
SentenceLock* find_sentence_lock(const char *filename, int sentence_num) {
    pthread_mutex_lock(&lock_mutex);
    
    SentenceLock *current = locks;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num) {
            pthread_mutex_unlock(&lock_mutex);
            return current;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&lock_mutex);
    return NULL;
}

// Add sentence lock
int add_sentence_lock(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    
    // Check if already locked
    SentenceLock *current = locks;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num) {
            pthread_mutex_unlock(&lock_mutex);
            return 0;  // Already locked
        }
        current = current->next;
    }
    
    // Add new lock
    SentenceLock *new_lock = malloc(sizeof(SentenceLock));
    strncpy(new_lock->filename, filename, sizeof(new_lock->filename));
    new_lock->sentence_num = sentence_num;
    strncpy(new_lock->username, username, sizeof(new_lock->username));
    new_lock->locked_at = time(NULL);
    new_lock->next = locks;
    locks = new_lock;
    
    pthread_mutex_unlock(&lock_mutex);
    return 1;  // Lock acquired
}

// Remove sentence lock
void remove_sentence_lock(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    
    SentenceLock *current = locks;
    SentenceLock *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0 && 
            current->sentence_num == sentence_num &&
            strcmp(current->username, username) == 0) {
            
            if (prev == NULL) {
                locks = current->next;
            } else {
                prev->next = current->next;
            }
            
            free(current);
            pthread_mutex_unlock(&lock_mutex);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&lock_mutex);
}
