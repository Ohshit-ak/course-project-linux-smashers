#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <time.h>
#include "protocol.h"

// Logging functions
void log_message(const char *component, const char *message);
void log_error(const char *component, const char *message);
void log_request(const char *from, const char *to, const char *request);

// String utilities
char** split_string(const char *str, const char *delim, int *count);
void free_split_string(char **strings, int count);
char* trim_whitespace(char *str);

// Network utilities
int send_message(int socket, struct Message *msg);
int recv_message(int socket, struct Message *msg);

// File utilities
int file_exists(const char *filename);
long get_file_size(const char *filename);
int count_words(const char *filename);
int count_chars(const char *filename);

// Time utilities
char* format_time(time_t t);

#endif // UTILS_H
