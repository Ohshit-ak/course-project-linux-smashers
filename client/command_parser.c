#include "command_parser.h"
#include "file_operations_client.h"
#include "access_manager.h"
#include "folder_operations.h"
#include "checkpoint_operations.h"
#include "advanced_operations.h"
#include "../common/protocol.h"
#include "../common/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// External globals
extern int ns_socket;
extern char username[];

// Print help menu
void print_help() {
    printf("\n╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                    Available Commands                          ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Basic Operations:                                              ║\n");
    printf("║  CREATE <filename>          - Create a new file                ║\n");
    printf("║  READ <filename>            - Read file content                ║\n");
    printf("║  DELETE <filename>          - Delete a file                    ║\n");
    printf("║  VIEW [-a] [-l]             - List files                       ║\n");
    printf("║  INFO <filename>            - Get file information             ║\n");
    printf("║  LIST                       - List all users                   ║\n");
    printf("║  LISTSS                     - List storage servers             ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Advanced Operations:                                           ║\n");
    printf("║  WRITE <file> <sent#>       - Write to file (interactive)      ║\n");
    printf("║  STREAM <filename>          - Stream file content              ║\n");
    printf("║  UNDO <filename>            - Undo last change                 ║\n");
    printf("║  EXEC <filename>            - Execute file as commands         ║\n");
    printf("║  SEARCH <pattern>           - Search for files by name         ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Storage Server Selection:                                      ║\n");
    printf("║  USE <SS_ID>                - Select storage server for files  ║\n");
    printf("║  USE                        - Show current storage server      ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Folder Operations:                                             ║\n");
    printf("║  CREATEFOLDER <folder>      - Create a new folder              ║\n");
    printf("║  VIEWFOLDER [folder]        - View folder contents             ║\n");
    printf("║  MOVE <file> [folder]       - Move file to folder              ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Checkpoint Operations:                                         ║\n");
    printf("║  CHECKPOINT <file> <tag>    - Create checkpoint with tag       ║\n");
    printf("║  VIEWCHECKPOINT <file> <tag>- View checkpoint content          ║\n");
    printf("║  REVERT <file> <tag>        - Revert to checkpoint             ║\n");
    printf("║  LISTCHECKPOINTS <file>     - List all checkpoints             ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ Access Control:                                                ║\n");
    printf("║  ADDACCESS -R/-W <file> <user>  - Grant access                ║\n");
    printf("║  REMACCESS <file> <user>        - Remove access               ║\n");
    printf("║  REQUESTACCESS -R|-W|-RW <file> - Request access              ║\n");
    printf("║  VIEWREQUESTS <file>            - View pending requests (owner)║\n");
    printf("║  APPROVEREQUEST <file> <id>     - Approve request (owner)     ║\n");
    printf("║  DENYREQUEST <file> <id>        - Deny request (owner)        ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║  EXIT                       - Quit client                      ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");
}

// Parse and execute command
void execute_command(char *command) {
    char *cmd = strtok(command, " \n");
    if (!cmd) return;
    
    if (strcmp(cmd, "CREATE") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_create(filename);
        } else {
            printf("Usage: CREATE <filename>\n");
        }
    }
    else if (strcmp(cmd, "READ") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_read(filename);
        } else {
            printf("Usage: READ <filename>\n");
        }
    }
    else if (strcmp(cmd, "DELETE") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_delete(filename);
        } else {
            printf("Usage: DELETE <filename>\n");
        }
    }
    else if (strcmp(cmd, "VIEW") == 0) {
        char *flags = strtok(NULL, " \n");
        int show_all = 0, show_details = 0;
        if (flags) {
            if (strchr(flags, 'a')) show_all = 1;
            if (strchr(flags, 'l')) show_details = 1;
        }
        handle_view(show_all, show_details);
    }
    else if (strcmp(cmd, "INFO") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_info(filename);
        } else {
            printf("Usage: INFO <filename>\n");
        }
    }
    else if (strcmp(cmd, "WRITE") == 0) {
        char *filename = strtok(NULL, " \n");
        char *sentence_num_str = strtok(NULL, " \n");
        if (filename && sentence_num_str) {
            int sentence_num = atoi(sentence_num_str);
            handle_write(filename, sentence_num);
        } else {
            printf("Usage: WRITE <filename> <sentence_number>\n");
        }
    }
    else if (strcmp(cmd, "STREAM") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_stream(filename);
        } else {
            printf("Usage: STREAM <filename>\n");
        }
    }
    else if (strcmp(cmd, "UNDO") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_undo(filename);
        } else {
            printf("Usage: UNDO <filename>\n");
        }
    }
    else if (strcmp(cmd, "LIST") == 0) {
        handle_list();
    }
    else if (strcmp(cmd, "LISTSS") == 0) {
        // List storage servers
        struct Message msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_LIST_SS;
        strncpy(msg.username, username, sizeof(msg.username));
        
        if (send_message(ns_socket, &msg) < 0 || recv_message(ns_socket, &msg) < 0) {
            printf("✗ Error: Failed to get storage server list\n");
        } else if (msg.error_code == RESP_SUCCESS) {
            printf("\n╔════════════════════════════════════════════════════════════════╗\n");
            printf("║               Storage Servers                                  ║\n");
            printf("╠════════════════════════════════════════════════════════════════╣\n");
            printf("║ ID         Address           Status                           ║\n");
            printf("╠════════════════════════════════════════════════════════════════╣\n");
            printf("%s", msg.data);
            printf("╚════════════════════════════════════════════════════════════════╝\n");
        } else {
            printf("✗ Error: %s\n", msg.data);
        }
    }
    else if (strcmp(cmd, "ADDACCESS") == 0) {
        char *flag = strtok(NULL, " \n");
        char *filename = strtok(NULL, " \n");
        char *target_user = strtok(NULL, " \n");
        if (flag && filename && target_user) {
            handle_addaccess(flag, filename, target_user);
        } else {
            printf("Usage: ADDACCESS -R/-W <filename> <username>\n");
            printf("  -R: Grant read access\n");
            printf("  -W: Grant write access (includes read)\n");
        }
    }
    else if (strcmp(cmd, "REMACCESS") == 0) {
        char *filename = strtok(NULL, " \n");
        char *target_user = strtok(NULL, " \n");
        if (filename && target_user) {
            handle_remaccess(filename, target_user);
        } else {
            printf("Usage: REMACCESS <filename> <username>\n");
        }
    }
    else if (strcmp(cmd, "EXEC") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_exec(filename);
        } else {
            printf("Usage: EXEC <filename>\n");
        }
    }
    else if (strcmp(cmd, "SEARCH") == 0) {
        char *pattern = strtok(NULL, "\n");  // Get rest of line as pattern
        if (pattern) {
            // Trim leading whitespace
            while (*pattern == ' ' || *pattern == '\t') pattern++;
            if (strlen(pattern) > 0) {
                handle_search(pattern);
            } else {
                printf("Usage: SEARCH <pattern>\n");
            }
        } else {
            printf("Usage: SEARCH <pattern>\n");
        }
    }
    else if (strcmp(cmd, "USE") == 0) {
        char *ss_id = strtok(NULL, " \n");
        handle_use_ss(ss_id);  // NULL shows current, non-NULL sets new SS
    }
    else if (strcmp(cmd, "CREATEFOLDER") == 0) {
        char *foldername = strtok(NULL, " \n");
        if (foldername) {
            handle_createfolder(foldername);
        } else {
            printf("Usage: CREATEFOLDER <foldername>\n");
        }
    }
    else if (strcmp(cmd, "VIEWFOLDER") == 0) {
        char *foldername = strtok(NULL, " \n");
        // If no foldername provided, view root (pass NULL)
        handle_viewfolder(foldername);
    }
    else if (strcmp(cmd, "MOVE") == 0) {
        char *filename = strtok(NULL, " \n");
        char *foldername = strtok(NULL, " \n");
        if (filename) {
            // If no foldername provided, move to root (pass NULL)
            handle_move(filename, foldername);
        } else {
            printf("Usage: MOVE <filename> [foldername]\n");
            printf("       MOVE <filename>          - Move to root folder\n");
            printf("       MOVE <filename> <folder> - Move to specified folder\n");
        }
    }
    else if (strcmp(cmd, "CHECKPOINT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_checkpoint(filename, tag);
        } else {
            printf("Usage: CHECKPOINT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "VIEWCHECKPOINT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_viewcheckpoint(filename, tag);
        } else {
            printf("Usage: VIEWCHECKPOINT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "REVERT") == 0) {
        char *filename = strtok(NULL, " \n");
        char *tag = strtok(NULL, " \n");
        if (filename && tag) {
            handle_revert(filename, tag);
        } else {
            printf("Usage: REVERT <filename> <checkpoint_tag>\n");
        }
    }
    else if (strcmp(cmd, "LISTCHECKPOINTS") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_listcheckpoints(filename);
        } else {
            printf("Usage: LISTCHECKPOINTS <filename>\n");
        }
    }
    else if (strcmp(cmd, "REQUESTACCESS") == 0) {
        char *access_type = strtok(NULL, " \n");
        char *filename = strtok(NULL, " \n");
        if (access_type && filename) {
            handle_requestaccess(filename, access_type);
        } else {
            printf("Usage: REQUESTACCESS -R|-W|-RW <filename>\n");
            printf("  -R:  Request read access\n");
            printf("  -W:  Request write access\n");
            printf("  -RW: Request read and write access\n");
        }
    }
    else if (strcmp(cmd, "VIEWREQUESTS") == 0) {
        char *filename = strtok(NULL, " \n");
        if (filename) {
            handle_viewrequests(filename);
        } else {
            printf("Usage: VIEWREQUESTS <filename>\n");
        }
    }
    else if (strcmp(cmd, "APPROVEREQUEST") == 0) {
        char *filename = strtok(NULL, " \n");
        char *request_id_str = strtok(NULL, " \n");
        if (filename && request_id_str) {
            int request_id = atoi(request_id_str);
            handle_respondrequest(filename, request_id, 1);  // 1 = approve
        } else {
            printf("Usage: APPROVEREQUEST <filename> <request_id>\n");
        }
    }
    else if (strcmp(cmd, "DENYREQUEST") == 0) {
        char *filename = strtok(NULL, " \n");
        char *request_id_str = strtok(NULL, " \n");
        if (filename && request_id_str) {
            int request_id = atoi(request_id_str);
            handle_respondrequest(filename, request_id, 0);  // 0 = deny
        } else {
            printf("Usage: DENYREQUEST <filename> <request_id>\n");
        }
    }
    else if (strcmp(cmd, "HELP") == 0) {
        print_help();
    }
    else if (strcmp(cmd, "EXIT") == 0 || strcmp(cmd, "QUIT") == 0) {
        printf("Goodbye!\n");
        if (ns_socket >= 0) {
            close(ns_socket);
        }
        exit(0);
    }
    else {
        printf("Unknown command. Type HELP for available commands.\n");
    }
}
