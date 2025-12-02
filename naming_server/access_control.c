#include "access_control.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

int next_request_id = 1;
pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;

// Check if user has permission
int check_permission(FileEntry *entry, const char *username, int need_write) {
    // Owner always has full access
    if (strcmp(entry->info.owner, username) == 0)
        return 1;
    
    // Check ACL
    AccessControl *acl = entry->acl;
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            if (need_write)
                return acl->can_write;
            else
                return acl->can_read;
        }
        acl = acl->next;
    }
    
    return 0;  // No access
}

// Add access control entry to a file
int add_access(FileEntry *entry, const char *username, int can_read, int can_write) {
    // Check if user already has access
    AccessControl *acl = entry->acl;
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            // Update existing access
            acl->can_read = can_read;
            acl->can_write = can_write;
            return 1;  // Updated
        }
        acl = acl->next;
    }
    
    // Add new ACL entry
    AccessControl *new_acl = malloc(sizeof(AccessControl));
    strncpy(new_acl->username, username, sizeof(new_acl->username));
    new_acl->can_read = can_read;
    new_acl->can_write = can_write;
    new_acl->next = entry->acl;
    entry->acl = new_acl;
    
    return 0;  // Added new
}

// Remove access control entry from a file
int remove_access(FileEntry *entry, const char *username) {
    AccessControl *acl = entry->acl;
    AccessControl *prev = NULL;
    
    while (acl != NULL) {
        if (strcmp(acl->username, username) == 0) {
            // Found the entry to remove
            if (prev == NULL) {
                entry->acl = acl->next;
            } else {
                prev->next = acl->next;
            }
            free(acl);
            return 1;  // Removed
        }
        prev = acl;
        acl = acl->next;
    }
    
    return 0;  // Not found
}

// Add access request
int add_access_request(FileEntry *entry, const char *requester, int access_type) {
    pthread_mutex_lock(&request_lock);
    
    // Check if request already exists and is pending
    AccessRequestNode *req = entry->access_requests;
    while (req != NULL) {
        if (strcmp(req->requester, requester) == 0 && req->status == 0) {
            pthread_mutex_unlock(&request_lock);
            return -1;  // Request already pending
        }
        req = req->next;
    }
    
    // Create new request
    AccessRequestNode *new_req = malloc(sizeof(AccessRequestNode));
    new_req->request_id = next_request_id++;
    strncpy(new_req->requester, requester, sizeof(new_req->requester));
    new_req->access_type = access_type;
    new_req->requested_at = time(NULL);
    new_req->status = 0;  // Pending
    new_req->next = entry->access_requests;
    entry->access_requests = new_req;
    
    pthread_mutex_unlock(&request_lock);
    return new_req->request_id;
}

// List pending access requests for a file
char* list_access_requests(FileEntry *entry) {
    static char results[MAX_DATA];
    memset(results, 0, sizeof(results));
    
    pthread_mutex_lock(&request_lock);
    
    AccessRequestNode *req = entry->access_requests;
    int count = 0;
    
    while (req != NULL) {
        if (req->status == 0) {  // Only show pending requests
            if (count > 0) {
                strncat(results, "\n", sizeof(results) - strlen(results) - 1);
            }
            
            char line[512];
            const char *access_str = (req->access_type == 1) ? "Read" : 
                                    (req->access_type == 2) ? "Write" : "Read+Write";
            snprintf(line, sizeof(line), "  [ID:%d] %s requests %s access at %s",
                     req->request_id, req->requester, access_str, format_time(req->requested_at));
            strncat(results, line, sizeof(results) - strlen(results) - 1);
            count++;
        }
        req = req->next;
    }
    
    pthread_mutex_unlock(&request_lock);
    
    if (count == 0) {
        snprintf(results, sizeof(results), "No pending access requests");
    } else {
        char header[256];
        snprintf(header, sizeof(header), "Pending access requests for '%s' (%d total):\n", entry->info.name, count);
        char temp[MAX_DATA];
        strncpy(temp, results, sizeof(temp) - 1);
        snprintf(results, sizeof(results), "%s%s", header, temp);
    }
    
    return results;
}

// Find and respond to access request
int respond_to_request(FileEntry *entry, int request_id, int approve) {
    pthread_mutex_lock(&request_lock);
    
    AccessRequestNode *req = entry->access_requests;
    while (req != NULL) {
        if (req->request_id == request_id && req->status == 0) {
            req->status = approve ? 1 : 2;  // 1=approved, 2=denied
            
            // If approved, add to ACL
            if (approve) {
                int can_read = (req->access_type == 1 || req->access_type == 3);
                int can_write = (req->access_type == 2 || req->access_type == 3);
                add_access(entry, req->requester, can_read, can_write);
            }
            
            pthread_mutex_unlock(&request_lock);
            return 0;
        }
        req = req->next;
    }
    
    pthread_mutex_unlock(&request_lock);
    return -1;  // Request not found
}
