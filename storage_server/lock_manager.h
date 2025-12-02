#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <pthread.h>
#include "../common/protocol.h"

// Sentence lock structure
typedef struct SentenceLock {
    char filename[MAX_FILENAME];
    int sentence_num;
    char username[MAX_USERNAME];
    time_t locked_at;
    struct SentenceLock *next;
} SentenceLock;

// Lock management functions
SentenceLock* find_sentence_lock(const char *filename, int sentence_num);
int add_sentence_lock(const char *filename, int sentence_num, const char *username);
void remove_sentence_lock(const char *filename, int sentence_num, const char *username);
void cleanup_locks();

// External global variables
extern SentenceLock *locks;
extern pthread_mutex_t lock_mutex;

#endif // LOCK_MANAGER_H
