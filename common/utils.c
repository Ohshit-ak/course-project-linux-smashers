#include "utils.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <ctype.h>

// Logging functions
void log_message(const char *component, const char *message) {
    char filename[128];
    snprintf(filename, sizeof(filename), "%s.log", component);
    
    FILE *log = fopen(filename, "a");
    if (log) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';  // Remove newline
        fprintf(log, "[%s] %s\n", time_str, message);
        fclose(log);
    }
}

void log_error(const char *component, const char *message) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "ERROR: %s", message);
    log_message(component, msg);
    fprintf(stderr, "[%s] %s\n", component, msg);
}

void log_request(const char *from, const char *to, const char *request) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "Request from %s to %s: %s", from, to, request);
    log_message("system", msg);
}

// String utilities
char** split_string(const char *str, const char *delim, int *count) {
    char *copy = strdup(str);
    char **result = NULL;
    *count = 0;
    
    char *token = strtok(copy, delim);
    while (token != NULL) {
        result = realloc(result, sizeof(char*) * (*count + 1));
        result[*count] = strdup(token);
        (*count)++;
        token = strtok(NULL, delim);
    }
    
    free(copy);
    return result;
}

void free_split_string(char **strings, int count) {
    for (int i = 0; i < count; i++) {
        free(strings[i]);
    }
    free(strings);
}

char* trim_whitespace(char *str) {
    char *end;
    
    // Trim leading space
    while(isspace((unsigned char)*str)) str++;
    
    if(*str == 0)
        return str;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    
    end[1] = '\0';
    return str;
}

// Network utilities
int send_message(int socket, struct Message *msg) {
    int bytes_sent = send(socket, msg, sizeof(struct Message), 0);
    if (bytes_sent < 0) {
        log_error("network", "Failed to send message");
        return -1;
    }
    return bytes_sent;
}

int recv_message(int socket, struct Message *msg) {
    // Clear the message structure first to avoid stale data
    memset(msg, 0, sizeof(struct Message));
    
    // Use MSG_WAITALL to ensure we receive the complete message
    int bytes_received = recv(socket, msg, sizeof(struct Message), MSG_WAITALL);
    if (bytes_received < 0) {
        log_error("network", "Failed to receive message");
        return -1;
    }
    if (bytes_received == 0) {
        // Connection closed
        return 0;
    }
    return bytes_received;
}

// File utilities
int file_exists(const char *filename) {
    struct stat buffer;
    return (stat(filename, &buffer) == 0);
}

long get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return -1;
}

int count_words(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;
    
    int count = 0;
    char ch;
    int in_word = 0;
    
    while ((ch = fgetc(fp)) != EOF) {
        if (isspace(ch)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            count++;
        }
    }
    
    fclose(fp);
    return count;
}

int count_chars(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;
    
    int count = 0;
    while (fgetc(fp) != EOF) {
        count++;
    }
    
    fclose(fp);
    return count;
}

// Time utilities
char* format_time(time_t t) {
    static char buffer[64];
    struct tm *tm_info = localtime(&t);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return buffer;
}
