#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <time.h>

// Message types
#define MSG_REGISTER_SS 1
#define MSG_REGISTER_CLIENT 2
#define MSG_CREATE 10
#define MSG_READ 11
#define MSG_WRITE 12
#define MSG_DELETE 13
#define MSG_VIEW 14
#define MSG_INFO 15
#define MSG_STREAM 16
#define MSG_LIST_USERS 17
#define MSG_ADD_ACCESS 18
#define MSG_REM_ACCESS 19
#define MSG_EXEC 20
#define MSG_UNDO 21
#define MSG_SEARCH 22
#define MSG_CREATEFOLDER 23
#define MSG_MOVE 24
#define MSG_VIEWFOLDER 25
#define MSG_CHECKPOINT 26
#define MSG_VIEWCHECKPOINT 27
#define MSG_REVERT 28
#define MSG_LISTCHECKPOINTS 29
#define MSG_REQUESTACCESS 30
#define MSG_VIEWREQUESTS 31
#define MSG_RESPONDREQUEST 32
#define MSG_HEARTBEAT 33
#define MSG_SHUTDOWN 34
#define MSG_REPLICATE 35
#define MSG_LIST_SS 36

// Response types
#define RESP_SUCCESS 200
#define RESP_SS_INFO 201
#define RESP_DATA 202
#define RESP_ACK 203

// Error codes
#define ERR_FILE_NOT_FOUND 404
#define ERR_PERMISSION_DENIED 403
#define ERR_FILE_LOCKED 423
#define ERR_FILE_EXISTS 409
#define ERR_INVALID_REQUEST 400
#define ERR_SERVER_ERROR 500
#define ERR_SS_UNAVAILABLE 503
#define ERR_SENTENCE_OUT_OF_RANGE 422
#define ERR_WORD_OUT_OF_RANGE 421
#define ERR_FOLDER_NOT_FOUND 424
#define ERR_FOLDER_EXISTS 425
#define ERR_CHECKPOINT_NOT_FOUND 426
#define ERR_NO_PENDING_REQUESTS 427
#define ERR_REQUEST_NOT_FOUND 428

// Constants
#define MAX_FILENAME 256
#define MAX_USERNAME 256
#define MAX_PATH 512
#define MAX_DATA 4096
#define MAX_FILES 1024

// Message structure for communication
struct Message {
    int type;                   // Message type constant
    char username[MAX_USERNAME]; // Requesting user
    char filename[MAX_FILENAME]; // Target file (or folder name)
    char folder[MAX_FILENAME];  // Folder path for MOVE/VIEWFOLDER
    char checkpoint_tag[MAX_FILENAME]; // For checkpoint operations
    int sentence_num;           // For WRITE operations
    int word_index;             // For WRITE operations
    int flags;                  // For VIEW (-a, -l flags), also used for access type (1=read, 2=write, 3=both)
    int request_id;             // For access request operations
    int data_length;            // Length of payload
    char data[MAX_DATA];        // Payload
    int error_code;             // Error/success code
    char ss_ip[16];            // Storage server IP (for responses)
    int ss_port;               // Storage server port (for responses)
};

// File information structure
struct FileInfo {
    char name[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char folder[MAX_FILENAME];  // Parent folder path (empty string = root)
    char storage_server_id[64];
    time_t created_at;
    time_t last_modified;
    time_t last_accessed;
    long size;
    int word_count;
    int char_count;
};

// Storage server registration info
struct SSRegistration {
    char ss_id[64];
    char ip[16];
    int nm_port;
    int client_port;
    int file_count;
    char files[MAX_FILES][MAX_FILENAME];
};

// Checkpoint information
struct CheckpointInfo {
    char filename[MAX_FILENAME];
    char tag[MAX_FILENAME];
    char creator[MAX_USERNAME];
    time_t created_at;
    long size;
};

// Access request information
struct AccessRequest {
    int request_id;
    char filename[MAX_FILENAME];
    char requester[MAX_USERNAME];
    char owner[MAX_USERNAME];
    int access_type;  // 1=read, 2=write, 3=both
    time_t requested_at;
    int status;  // 0=pending, 1=approved, 2=denied
};

#endif // PROTOCOL_H
