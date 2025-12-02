# 🔄 Docs++ Execution Flow - Line by Line Guide

> **Purpose:** Complete execution flow explanation for project evaluation  
> **Date:** December 2, 2025  
> **Format:** Sequential runtime flow with line-by-line explanations

---

## 📋 Table of Contents

1. [System Startup Flow](#1-system-startup-flow)
2. [Naming Server Initialization](#2-naming-server-initialization)
3. [Storage Server Registration](#3-storage-server-registration)
4. [Client Connection & Authentication](#4-client-connection--authentication)
5. [Command Execution Flow](#5-command-execution-flow)
6. [Complete Request-Response Cycles](#6-complete-request-response-cycles)
7. [Thread Management](#7-thread-management)
8. [Shutdown & Cleanup](#8-shutdown--cleanup)

---

## 1. System Startup Flow

### Overview
The distributed file system starts in this order:
1. **Naming Server** (NS) - Port 8080
2. **Storage Server(s)** (SS) - Port 8081+
3. **Client(s)** - Connects to NS

---

## 2. Naming Server Initialization

### File: `naming_server/naming_server.c`

#### **Line 2654: `int main()`**
- **Entry point** for Naming Server process
- Declares local variables for socket handling

```c
int server_socket, *client_socket;
struct sockaddr_in server_addr, client_addr;
socklen_t addr_len = sizeof(client_addr);
```

**Explanation:**
- `server_socket`: Will hold the listening socket for incoming connections
- `client_socket`: Pointer to dynamically allocated socket for each client (thread-safe)
- `server_addr`: Server's own address configuration
- `client_addr`: Will store connecting client's address
- `addr_len`: Size of client address structure

---

#### **Lines 2657-2659: Print startup banner**

```c
printf("=== Naming Server ===\n");
printf("Starting on port %d...\n", NS_PORT);
```

**Execution:**
- Prints visual header to console
- `NS_PORT` is defined as 8080 (line 28)
- User sees this first when NS starts

---

#### **Lines 2662-2664: Register signal handlers**

```c
signal(SIGINT, shutdown_system);   // Ctrl+C
signal(SIGTERM, shutdown_system);  // kill command
signal(SIGHUP, shutdown_system);   // Terminal hangup
```

**Execution:**
- `signal()` registers interrupt handlers
- When user presses Ctrl+C, `shutdown_system()` function is called
- Ensures graceful cleanup (close sockets, notify SS, etc.)
- **Key concept:** Signal handling for robustness

---

#### **Line 2667: Initialize hash table**

```c
memset(file_table, 0, sizeof(file_table));
```

**Execution:**
- `file_table` is a global array: `FileEntry *file_table[HASH_TABLE_SIZE]` (1024 slots)
- `memset()` zeros out all 1024 pointers
- This hash table will store file metadata for O(1) lookup
- **Data structure:** Hash table with chaining for collision resolution

---

#### **Lines 2670-2675: Start heartbeat monitor thread**

```c
pthread_t heartbeat_thread;
if (pthread_create(&heartbeat_thread, NULL, heartbeat_monitor, NULL) != 0) {
    perror("Failed to create heartbeat thread");
    exit(EXIT_FAILURE);
}
pthread_detach(heartbeat_thread);
```

**Execution Flow:**
1. `pthread_create()` spawns a new thread
2. New thread executes `heartbeat_monitor()` function in parallel
3. Main thread continues without waiting
4. `pthread_detach()` means thread cleans up itself when done
5. **Purpose:** Background thread checks if Storage Servers are alive every 5 seconds

**What heartbeat_monitor does:**
- Runs infinite loop: `while (!shutdown_flag)`
- Every 5 seconds, checks each registered SS's `last_heartbeat` timestamp
- If SS hasn't sent heartbeat in 15 seconds, marks it as failed
- Logged message: "Storage server SS1 marked as failed (no heartbeat)"

---

#### **Lines 2678-2682: Create TCP socket**

```c
server_socket = socket(AF_INET, SOCK_STREAM, 0);
if (server_socket < 0) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
}
```

**Execution:**
- `socket()` system call creates endpoint for communication
- `AF_INET`: IPv4 address family
- `SOCK_STREAM`: TCP (reliable, connection-oriented)
- Returns file descriptor (typically 3, since 0=stdin, 1=stdout, 2=stderr)
- If fails, prints error and exits

---

#### **Lines 2685-2686: Set socket options**

```c
int opt = 1;
setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

**Execution:**
- `SO_REUSEADDR` allows reusing port 8080 immediately after restart
- Without this, you'd get "Address already in use" error
- Useful during development when restarting NS frequently
- **Network programming:** Essential socket option

---

#### **Lines 2689-2692: Bind socket to address**

```c
server_addr.sin_family = AF_INET;
server_addr.sin_addr.s_addr = INADDR_ANY;
server_addr.sin_port = htons(NS_PORT);
```

**Execution:**
- `sin_family = AF_INET`: IPv4
- `sin_addr.s_addr = INADDR_ANY`: Listen on all network interfaces (0.0.0.0)
- `htons(NS_PORT)`: Converts 8080 to network byte order (big-endian)
  - **Why?** Network protocols use big-endian, x86 CPUs use little-endian
  - `htons()` = "host to network short"

```c
if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("Bind failed");
    exit(EXIT_FAILURE);
}
```

**Execution:**
- `bind()` associates socket with IP address and port
- Now this process owns port 8080
- Other processes can't use port 8080
- Fails if port already in use

---

#### **Lines 2698-2702: Listen for connections**

```c
if (listen(server_socket, MAX_CLIENTS) < 0) {
    perror("Listen failed");
    exit(EXIT_FAILURE);
}
```

**Execution:**
- `listen()` marks socket as passive (waiting for connections)
- `MAX_CLIENTS = 100`: Backlog queue size
- Socket is now ready to accept incoming connections
- **State change:** Socket becomes a "listening socket"

---

#### **Lines 2704-2706: Ready message**

```c
printf("Naming Server is running and waiting for connections...\n");
printf("Type 'SHUTDOWN' to gracefully shutdown the server\n\n");
log_message("naming_server", "Server started successfully");
```

**Execution:**
- Prints status to console
- `log_message()` writes to `naming_server/ns.log` file
- Log format: `[2024-12-02 10:30:45] Server started successfully`
- NS is now fully initialized and ready

---

#### **Line 2709: Make stdin non-blocking**

```c
fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
```

**Execution:**
- `fcntl()` changes file descriptor flags
- `F_GETFL`: Get current flags
- `| O_NONBLOCK`: Add non-blocking flag
- **Purpose:** Check for "SHUTDOWN" command without blocking accept()
- Now `fgets()` returns NULL immediately if no input available

---

#### **Lines 2712-2778: Main accept loop**

```c
while (!shutdown_flag) {
```

**Execution:**
- Infinite loop until `shutdown_flag` is set
- `shutdown_flag` is a global variable, set by signal handlers
- This is the **heart of the server** - processes all incoming connections

---

#### **Lines 2714-2724: Check for SHUTDOWN command**

```c
char cmd[256];
if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
    cmd[strcspn(cmd, "\n")] = 0;
    
    if (strcmp(cmd, "SHUTDOWN") == 0) {
        printf("\n⚠️  Initiating Naming Server shutdown...\n");
        // ... shutdown logic
    }
}
```

**Execution:**
1. `fgets()` tries to read from stdin (non-blocking)
2. Returns NULL if no input (no blocking!)
3. If user typed something, remove newline with `strcspn()`
4. Compare with "SHUTDOWN" string
5. If match, start graceful shutdown procedure

**Shutdown procedure:**
- Iterate through all registered Storage Servers
- Send `MSG_SHUTDOWN` message to each
- Close their sockets
- Print confirmation messages
- Exit cleanly

---

#### **Lines 2744-2752: Select for timeout**

```c
fd_set readfds;
FD_ZERO(&readfds);
FD_SET(server_socket, &readfds);

struct timeval tv;
tv.tv_sec = 0;
tv.tv_usec = 100000; // 100ms timeout

int activity = select(server_socket + 1, &readfds, NULL, NULL, &tv);
```

**Execution:**
1. `fd_set` is a set of file descriptors to monitor
2. `FD_ZERO()` clears the set
3. `FD_SET()` adds server_socket to the set
4. `timeval` sets timeout to 100 milliseconds
5. `select()` waits up to 100ms for incoming connection
6. Returns:
   - **> 0** if socket has incoming connection
   - **0** if timeout (no connection in 100ms)
   - **< 0** if error

**Why timeout?**
- Without timeout, `accept()` blocks forever
- Can't check shutdown_flag or stdin
- 100ms timeout allows responsive shutdown

---

#### **Lines 2754-2778: Accept and spawn thread**

```c
if (activity > 0 && FD_ISSET(server_socket, &readfds)) {
    client_socket = malloc(sizeof(int));
    *client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
```

**Execution:**
1. Check if there's actually a connection waiting
2. `malloc()` allocates memory for socket number
   - **Why malloc?** Each thread needs its own copy
   - If we used local variable, all threads would share same memory
3. `accept()` extracts first connection from queue
4. Fills in `client_addr` with client's IP and port
5. Returns new socket file descriptor for this specific client

```c
printf("New connection from %s:%d\n", 
       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
```

**Execution:**
- `inet_ntoa()` converts IP address to string (e.g., "127.0.0.1")
- `ntohs()` converts port from network to host byte order
- Prints: "New connection from 127.0.0.1:54321"

```c
pthread_t thread;
if (pthread_create(&thread, NULL, handle_client, client_socket) != 0) {
    perror("Thread creation failed");
    free(client_socket);
    continue;
}

pthread_detach(thread);
```

**Execution:**
1. `pthread_create()` spawns new thread
2. New thread executes `handle_client()` function
3. Passes `client_socket` as argument to thread
4. **Concurrent processing:** Main thread continues accepting, worker thread handles client
5. `pthread_detach()` means thread auto-cleans when done
6. **Result:** Each client gets its own thread

---

## 3. Storage Server Registration

### File: `storage_server/storage_server.c`

#### **Line 1741: `int main(int argc, char *argv[])`**

**Command line:** `./storage_server SS1 127.0.0.1 8080 8081`

```c
if (argc != 5) {
    printf("Usage: %s <ss_id> <ns_ip> <ns_port> <client_port>\n", argv[0]);
    return 1;
}
```

**Execution:**
- Validates exactly 5 arguments (program name + 4 args)
- If wrong, prints usage and exits

---

#### **Lines 1748-1751: Parse arguments**

```c
strncpy(ss_id, argv[1], sizeof(ss_id));       // ss_id = "SS1"
strncpy(ns_ip, argv[2], sizeof(ns_ip));       // ns_ip = "127.0.0.1"
ns_port = atoi(argv[3]);                      // ns_port = 8080
client_port = atoi(argv[4]);                  // client_port = 8081
nm_port = client_port + 1000;                 // nm_port = 9081
```

**Execution:**
- Copies command-line args to global variables
- `atoi()` converts string to integer
- `nm_port = client_port + 1000`: NS communication port
  - **Design:** Separate ports for client ops vs NS commands
  - Client operations: 8081
  - NS commands: 9081

---

#### **Lines 1753-1757: Print configuration**

```c
printf("=== Storage Server %s ===\n", ss_id);
printf("NS: %s:%d\n", ns_ip, ns_port);
printf("Client Port: %d\n", client_port);
printf("NM Port: %d\n", nm_port);
```

**Execution:**
- Prints startup configuration
- Example output:
  ```
  === Storage Server SS1 ===
  NS: 127.0.0.1:8080
  Client Port: 8081
  NM Port: 9081
  ```

---

#### **Line 1760: `init_storage()`**

Let me jump to this function (around line 65):

```c
void init_storage() {
    mkdir(BASE_STORAGE_DIR, 0777);              // Create ../storage/
    mkdir(BASE_BACKUP_DIR, 0777);               // Create ../backups/
    
    snprintf(storage_dir, sizeof(storage_dir), "%s%s/", BASE_STORAGE_DIR, ss_id);
    snprintf(backup_dir, sizeof(backup_dir), "%s%s/", BASE_BACKUP_DIR, ss_id);
    
    mkdir(storage_dir, 0777);                   // Create ../storage/SS1/
    mkdir(backup_dir, 0777);                    // Create ../backups/SS1/
```

**Execution:**
1. `mkdir()` creates directories (ignore error if exists)
2. `snprintf()` builds paths: `../storage/SS1/` and `../backups/SS1/`
3. Creates SS-specific subdirectories
4. Sets permissions: 0777 (rwxrwxrwx)
5. **Result:** File system structure created:
   ```
   storage/
     SS1/
   backups/
     SS1/
   ```

```c
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Storage directories initialized: %s and %s", 
             storage_dir, backup_dir);
    log_message("storage_server", log_msg);
    printf("Storage: %s\n", storage_dir);
    printf("Backups: %s\n", backup_dir);
}
```

**Execution:**
- Formats log message with directory paths
- Writes to `storage_server/ss1.log`
- Prints to console for user feedback

---

#### **Lines 1763-1767: Register with NS**

```c
sleep(1);  // Give NS time to start
int ns_socket = register_with_ns();
if (ns_socket < 0) {
    fprintf(stderr, "Failed to register with Naming Server\n");
    return 1;
}
```

**Execution:**
1. `sleep(1)` waits 1 second for NS to be ready
2. Calls `register_with_ns()` function

**Inside `register_with_ns()` (around line 143):**

```c
int register_with_ns() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    inet_pton(AF_INET, ns_ip, &ns_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&ns_addr, sizeof(ns_addr)) < 0) {
        return -1;
    }
```

**Execution:**
1. Creates TCP socket
2. Configures NS address (127.0.0.1:8080)
3. `inet_pton()` converts IP string to binary
4. `connect()` establishes connection to NS
5. **3-way TCP handshake happens here**

```c
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SS_REGISTER;
    strncpy(msg.ss_info.id, ss_id, sizeof(msg.ss_info.id));
    strncpy(msg.ss_info.ip, ns_ip, sizeof(msg.ss_info.ip));
    msg.ss_info.nm_port = nm_port;
    msg.ss_info.client_port = client_port;
```

**Execution:**
1. Creates Message structure (defined in `protocol.h`)
2. `memset()` zeros all fields
3. Sets message type to `MSG_SS_REGISTER`
4. Fills in SS information:
   - ID: "SS1"
   - IP: "127.0.0.1"
   - NM Port: 9081
   - Client Port: 8081

```c
    list_files(msg.ss_info.files);
    
    send_message(sock, &msg);
```

**Execution:**
1. `list_files()` scans `../storage/SS1/` directory
2. Adds all file names to `msg.ss_info.files` array
3. `send_message()` serializes and sends to NS
4. **Network transmission:** Message sent over TCP

```c
    struct Message response;
    if (receive_message(sock, &response) < 0) {
        return -1;
    }
    
    if (response.type == MSG_SUCCESS) {
        printf("✓ Registered with Naming Server\n");
        return sock;  // Return the persistent connection
    }
    
    return -1;
}
```

**Execution:**
1. `receive_message()` waits for NS response
2. Checks if response is `MSG_SUCCESS`
3. **Returns socket** (stays connected!)
4. **Key design:** This socket remains open for NS commands

---

#### **Lines 1770-1777: Start NS command handler thread**

```c
int *ns_sock_ptr = malloc(sizeof(int));
*ns_sock_ptr = ns_socket;
pthread_t ns_thread;
if (pthread_create(&ns_thread, NULL, handle_ns_commands, ns_sock_ptr) != 0) {
    fprintf(stderr, "Failed to create NS command handler thread\n");
    close(ns_socket);
    return 1;
}
pthread_detach(ns_thread);
```

**Execution:**
1. Allocate memory for socket (thread-safe)
2. Create new thread running `handle_ns_commands()`
3. **Parallel execution:** Main thread continues, new thread listens for NS commands
4. `pthread_detach()` for auto-cleanup

**What `handle_ns_commands()` does:**
- Infinite loop waiting for messages from NS
- Receives commands like `MSG_HEARTBEAT_REQUEST`, `MSG_SHUTDOWN`
- Responds to heartbeat to prove it's alive
- Runs in background forever

---

#### **Lines 1780-1806: Create client listener socket**

```c
int client_listener = socket(AF_INET, SOCK_STREAM, 0);
// ... configure and bind ...
client_addr.sin_port = htons(client_port);  // 8081

if (listen(client_listener, 10) < 0) {
    perror("Client listen failed");
    return 1;
}

printf("Storage Server is running and ready for client connections...\n");
```

**Execution:**
1. Creates **second socket** for client connections
2. Binds to port 8081 (different from NS port 9081)
3. `listen()` with backlog of 10
4. **Result:** SS has TWO sockets:
   - One connected to NS (for commands)
   - One listening for clients (for file operations)

---

## 4. Client Connection & Authentication

### File: `client/client.c`

#### **Line 1652: `int main(int argc, char *argv[])`**

```c
printf("╔════════════════════════════════════════╗\n");
printf("║  Docs++ Distributed File System       ║\n");
printf("║  Client v1.0                           ║\n");
printf("╚════════════════════════════════════════╝\n\n");
```

**Execution:**
- Prints ASCII art banner
- Creates professional CLI appearance

---

#### **Lines 1659-1671: Get username**

```c
printf("Enter your username: ");
fflush(stdout);
if (fgets(username, sizeof(username), stdin) == NULL) {
    fprintf(stderr, "Failed to read username\n");
    return 1;
}
username[strcspn(username, "\n")] = 0;  // Remove newline

if (strlen(username) == 0) {
    fprintf(stderr, "Username cannot be empty\n");
    return 1;
}

printf("\n✓ Hello, %s!\n\n", username);
```

**Execution:**
1. Prompts for username
2. `fflush(stdout)` ensures prompt is displayed immediately
3. `fgets()` reads line from stdin (blocking)
4. Removes trailing newline character
5. Validates username is not empty
6. Stores in global variable `username`
7. **Result:** User identity established

---

#### **Lines 1674-1678: Parse NS address**

```c
if (argc >= 3) {
    strncpy(ns_ip, argv[1], sizeof(ns_ip));
    ns_port = atoi(argv[2]);
}
```

**Execution:**
- If command-line args provided: `./client 127.0.0.1 8080`
- Override defaults (127.0.0.1:8080)
- Allows connecting to remote NS

---

#### **Lines 1681-1688: Connect to NS**

```c
printf("Connecting to Naming Server at %s:%d...\n", ns_ip, ns_port);
if (connect_to_ns() < 0) {
    fprintf(stderr, "✗ Failed to connect to Naming Server\n");
    fprintf(stderr, "  Make sure the Naming Server is running!\n");
    return 1;
}

printf("✓ Connected successfully!\n\n");
```

**Execution:**
1. Prints connection attempt message
2. Calls `connect_to_ns()` function

**Inside `connect_to_ns()` (around line 86):**

```c
int connect_to_ns() {
    ns_socket = socket(AF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    inet_pton(AF_INET, ns_ip, &ns_addr.sin_addr);
    
    if (connect(ns_socket, (struct sockaddr*)&ns_addr, sizeof(ns_addr)) < 0) {
        perror("Connection to Naming Server failed");
        return -1;
    }
    
    return 0;
}
```

**Execution:**
1. Creates TCP socket
2. Configures NS address
3. `connect()` initiates TCP connection
4. **TCP handshake:** SYN → SYN-ACK → ACK
5. Stores socket in global `ns_socket`
6. **Result:** Persistent connection to NS established

---

#### **Lines 1690-1693: Print help**

```c
printf("Type HELP for available commands\n");
printf("────────────────────────────────────────\n\n");
```

**Execution:**
- Prints horizontal line separator
- User guidance for first-time users

---

#### **Lines 1696-1717: Command loop**

```c
char command[BUFFER_SIZE];
while (1) {
    printf("%s> ", username);
    fflush(stdout);
    
    if (fgets(command, sizeof(command), stdin) == NULL) {
        break;
    }
    
    // Remove trailing newline
    command[strcspn(command, "\n")] = 0;
    
    // Skip empty commands
    if (strlen(command) == 0) {
        continue;
    }
    
    execute_command(command);
    printf("\n");
}
```

**Execution:**
1. **Infinite loop** - heart of client interaction
2. Prints prompt: `alice> `
3. `fflush()` ensures prompt visible immediately
4. `fgets()` blocks waiting for user input
5. User types command and presses Enter
6. Remove newline character
7. Skip if empty (just Enter key)
8. Call `execute_command()` to process
9. Print blank line for spacing
10. Loop back to prompt

**Example interaction:**
```
alice> CREATE hello.txt
✓ File created successfully: hello.txt

alice> VIEW
Files accessible to you:
  hello.txt  [Owner: alice]  [Storage: SS1]

alice> 
```

---

## 5. Command Execution Flow

### File: `client/client.c` - Line 1353

#### **`void execute_command(char *command)`**

**Example command:** `CREATE hello.txt`

```c
void execute_command(char *command) {
    // Parse command
    char cmd[64];
    sscanf(command, "%s", cmd);
    
    // Convert to uppercase for comparison
    for (int i = 0; cmd[i]; i++) {
        cmd[i] = toupper(cmd[i]);
    }
```

**Execution:**
1. `sscanf()` extracts first word: "CREATE"
2. Loop converts to uppercase: "CREATE"
3. **Case-insensitive:** User can type "create", "Create", "CREATE"

```c
    if (strcmp(cmd, "CREATE") == 0) {
        handle_create(command);
    }
```

**Execution:**
1. `strcmp()` compares strings
2. If match, call `handle_create()`
3. Similar `if-else` chain for all commands

---

### **`handle_create()` Function**

**User typed:** `CREATE hello.txt`

```c
void handle_create(char *command) {
    char filename[MAX_FILENAME];
    
    // Parse filename
    if (sscanf(command, "CREATE %s", filename) != 1) {
        printf("Usage: CREATE <filename>\n");
        return;
    }
```

**Execution:**
1. `sscanf()` extracts filename after "CREATE"
2. Returns 1 if successful, 0 if failed
3. Validation: Ensures filename provided

```c
    // Build message
    struct Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_CREATE;
    strncpy(msg.username, username, sizeof(msg.username));
    strncpy(msg.filename, filename, sizeof(msg.filename));
    strncpy(msg.ss_id, selected_ss_id, sizeof(msg.ss_id));
```

**Execution:**
1. Create Message structure
2. Zero all fields for clean state
3. Set type: `MSG_CREATE`
4. Copy username: "alice"
5. Copy filename: "hello.txt"
6. Copy selected SS: "" (empty = NS chooses)

```c
    // Send to naming server
    if (send_message(ns_socket, &msg) < 0) {
        printf("✗ Failed to send request\n");
        return;
    }
```

**Execution:**
1. `send_message()` serializes Message structure
2. Sends over TCP socket to NS
3. **Network:** Client → NS transmission
4. If error, print message and return

```c
    // Wait for response
    struct Message response;
    if (receive_message(ns_socket, &response) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
```

**Execution:**
1. `receive_message()` blocks waiting for NS reply
2. Deserializes bytes into Message structure
3. **Network:** NS → Client transmission
4. Timeout if NS doesn't respond

```c
    // Handle response
    if (response.type == MSG_SUCCESS) {
        printf("✓ File created successfully: %s\n", filename);
        printf("  Storage Server: %s\n", response.data);
    } else {
        printf("✗ Create failed: %s\n", response.data);
    }
}
```

**Execution:**
1. Check response type
2. If `MSG_SUCCESS`, print success message
3. `response.data` contains which SS stored it: "SS1"
4. If `MSG_ERROR`, print error message
5. Return to command loop

---

## 6. Complete Request-Response Cycles

### Example 1: CREATE Operation Flow

**User action:** Types `CREATE hello.txt` and presses Enter

#### **Step 1: Client Side (client.c)**

1. **Line 1699:** Command loop receives "CREATE hello.txt"
2. **Line 1710:** Calls `execute_command("CREATE hello.txt")`
3. **Line 1357:** Parses command, extracts "CREATE"
4. **Line 1365:** Calls `handle_create()`
5. **Lines 149-152:** Parses filename "hello.txt"
6. **Lines 155-160:** Builds MSG_CREATE message
7. **Line 163:** Sends to NS via `ns_socket`

**Network:** TCP packet sent to NS (127.0.0.1:8080)

---

#### **Step 2: Naming Server Side (naming_server.c)**

**NS has thread waiting in `handle_client()` (line 912):**

```c
void* handle_client(void *arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    struct Message msg;
    while (1) {
        if (receive_message(client_socket, &msg) < 0) {
            break;  // Client disconnected
        }
```

**Execution:**
1. Thread was blocked in `receive_message()`
2. **Wakes up** when client's packet arrives
3. Deserializes bytes into `msg` structure
4. `msg.type == MSG_CREATE`

```c
        switch (msg.type) {
            case MSG_CREATE:
                handle_create_request(client_socket, &msg);
                break;
```

**Execution:**
1. Switch statement routes to `handle_create_request()`
2. Passes client socket and message

**Inside `handle_create_request()` (around line 1480):**

```c
void handle_create_request(int client_socket, struct Message *msg) {
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), 
             "CREATE request from '%s' for file '%s'", 
             msg->username, msg->filename);
    log_message("naming_server", log_msg);
```

**Execution:**
1. Formats log message
2. Writes to ns.log: `[2024-12-02 10:30:45] CREATE request from 'alice' for file 'hello.txt'`

```c
    // Check if file already exists
    FileEntry *existing = lookup_file(msg->filename);
    if (existing != NULL) {
        send_error(client_socket, "File already exists");
        return;
    }
```

**Execution:**
1. `lookup_file()` hashes filename and searches hash table
2. If found, file already exists
3. Send error response to client
4. Return early (don't create duplicate)

```c
    // Select storage server
    StorageServer *ss = select_storage_server(msg->ss_id);
    if (!ss) {
        send_error(client_socket, "No active storage servers available");
        return;
    }
```

**Execution:**
1. `select_storage_server()` chooses which SS to use
2. If `msg->ss_id` is empty, picks least-loaded SS
3. If specific SS requested (via USE command), use that one
4. Returns NULL if no SS is active
5. **Load balancing:** Distributes files across multiple SS

```c
    // Forward CREATE request to storage server
    struct Message ss_msg;
    memset(&ss_msg, 0, sizeof(ss_msg));
    ss_msg.type = MSG_CREATE;
    strncpy(ss_msg.filename, msg->filename, sizeof(ss_msg.filename));
    strncpy(ss_msg.username, msg->username, sizeof(ss_msg.username));
    
    send_message(ss->ss_socket, &ss_msg);
```

**Execution:**
1. Build new message for SS
2. Copy filename and username
3. Send to SS via persistent connection
4. **Network:** NS → SS transmission

```c
    // Wait for SS response
    struct Message ss_response;
    if (receive_message(ss->ss_socket, &ss_response) < 0) {
        send_error(client_socket, "Storage server communication error");
        return;
    }
    
    if (ss_response.type != MSG_SUCCESS) {
        send_error(client_socket, ss_response.data);
        return;
    }
```

**Execution:**
1. Block waiting for SS to create file
2. Receive response message
3. Check if successful
4. If error, forward error to client

```c
    // Add file to registry
    FileEntry *entry = malloc(sizeof(FileEntry));
    memset(entry, 0, sizeof(FileEntry));
    strncpy(entry->info.filename, msg->filename, sizeof(entry->info.filename));
    strncpy(entry->info.owner, msg->username, sizeof(entry->info.owner));
    strncpy(entry->info.ss_id, ss->id, sizeof(entry->info.ss_id));
    entry->info.created_at = time(NULL);
    
    add_file_to_table(entry);
```

**Execution:**
1. Allocate memory for new file entry
2. Initialize all fields
3. Set filename: "hello.txt"
4. Set owner: "alice"
5. Set storage server: "SS1"
6. Set creation timestamp: current Unix time
7. `add_file_to_table()` adds to hash table
8. **Data structure:** File now in registry for future lookups

```c
    // Send success to client
    struct Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_SUCCESS;
    snprintf(response.data, sizeof(response.data), "%s", ss->id);
    send_message(client_socket, &response);
    
    snprintf(log_msg, sizeof(log_msg), 
             "File '%s' created successfully on %s for user '%s'", 
             msg->filename, ss->id, msg->username);
    log_message("naming_server", log_msg);
}
```

**Execution:**
1. Build success response
2. Include SS ID in data field: "SS1"
3. Send to client
4. **Network:** NS → Client transmission
5. Log success message to ns.log
6. Function returns, thread waits for next message

---

#### **Step 3: Storage Server Side (storage_server.c)**

**SS has thread waiting in `handle_ns_commands()` (around line 558):**

```c
void* handle_ns_commands(void *arg) {
    int ns_socket = *(int*)arg;
    free(arg);
    
    struct Message msg;
    while (1) {
        if (receive_message(ns_socket, &msg) < 0) {
            break;
        }
        
        switch (msg.type) {
            case MSG_CREATE: {
```

**Execution:**
1. Thread blocked in `receive_message()`
2. Wakes up when NS sends CREATE message
3. Switch routes to MSG_CREATE case

```c
                char filepath[MAX_PATH];
                snprintf(filepath, sizeof(filepath), "%s%s", 
                         storage_dir, msg.filename);
                
                // Create empty file
                FILE *f = fopen(filepath, "w");
                if (f == NULL) {
                    struct Message response;
                    memset(&response, 0, sizeof(response));
                    response.type = MSG_ERROR;
                    snprintf(response.data, sizeof(response.data), 
                             "Failed to create file");
                    send_message(ns_socket, &response);
                    break;
                }
                fclose(f);
```

**Execution:**
1. Build full file path: `../storage/SS1/hello.txt`
2. `fopen()` with "w" mode creates file
3. If fails (permissions, disk full), send error
4. `fclose()` closes file handle
5. **File system:** Empty file created on disk

```c
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), 
                         "File created: %s", msg.filename);
                log_message("storage_server", log_msg);
                
                struct Message response;
                memset(&response, 0, sizeof(response));
                response.type = MSG_SUCCESS;
                send_message(ns_socket, &response);
                break;
            }
```

**Execution:**
1. Log to ss1.log: `File created: hello.txt`
2. Build success response
3. Send to NS
4. **Network:** SS → NS transmission
5. Case ends, loop continues waiting

---

#### **Step 4: Back to Client (client.c)**

```c
    // Wait for response
    struct Message response;
    if (receive_message(ns_socket, &response) < 0) {
        printf("✗ Failed to receive response\n");
        return;
    }
```

**Execution:**
1. Client was blocked in `receive_message()`
2. **Wakes up** when NS sends response
3. Deserializes message

```c
    if (response.type == MSG_SUCCESS) {
        printf("✓ File created successfully: %s\n", filename);
        printf("  Storage Server: %s\n", response.data);
    }
}
```

**Execution:**
1. Check type: `MSG_SUCCESS`
2. Print to console:
   ```
   ✓ File created successfully: hello.txt
     Storage Server: SS1
   ```
3. Function returns
4. Back to command loop
5. **User sees result!**

---

### Example 2: WRITE Operation Flow

**User action:** `WRITE hello.txt 0`

This is more complex because client connects **directly to Storage Server**.

#### **Step 1: Client sends request to NS**

Similar to CREATE, client sends `MSG_WRITE` to NS with filename.

#### **Step 2: NS validates and returns SS info**

```c
void handle_write_request(int client_socket, struct Message *msg) {
    FileEntry *file = lookup_file(msg->filename);
    
    // Check if file exists
    if (!file) {
        send_error(client_socket, "File not found");
        return;
    }
    
    // Check write permission
    if (!has_write_permission(file, msg->username)) {
        send_error(client_socket, "Permission denied");
        return;
    }
    
    // Return storage server info
    struct Message response;
    response.type = MSG_REDIRECT_TO_SS;
    strncpy(response.ss_info.id, file->info.ss_id, ...);
    strncpy(response.ss_info.ip, "127.0.0.1", ...);
    response.ss_info.client_port = 8081;
    send_message(client_socket, &response);
}
```

**Execution:**
1. Lookup file in hash table
2. Verify file exists
3. Check ACL (access control list)
4. If user has write permission, send SS details
5. **Response contains:** SS ID, IP, Port
6. NS's job is done for this request

---

#### **Step 3: Client connects directly to SS**

```c
void handle_write(char *command) {
    // ... parse command, send to NS, get SS info ...
    
    // Connect to storage server
    int ss_socket = socket(AF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(response.ss_info.client_port);
    inet_pton(AF_INET, response.ss_info.ip, &ss_addr.sin_addr);
    
    connect(ss_socket, (struct sockaddr*)&ss_addr, sizeof(ss_addr));
```

**Execution:**
1. Creates **new socket** (not ns_socket!)
2. Configures SS address from NS response
3. `connect()` to SS directly
4. **Network:** Client → SS direct connection
5. **Design:** Offloads data transfer from NS

```c
    // Send WRITE request to SS
    struct Message write_msg;
    write_msg.type = MSG_WRITE;
    strncpy(write_msg.username, username, ...);
    strncpy(write_msg.filename, filename, ...);
    write_msg.sentence_num = sentence_num;
    
    send_message(ss_socket, &write_msg);
```

**Execution:**
1. Build WRITE message
2. Include sentence number to edit
3. Send to SS
4. **Data flow:** Client → SS (bypassing NS)

---

#### **Step 4: SS handles WRITE - Interactive mode**

```c
void* handle_client(void *arg) {
    // ... receive MSG_WRITE ...
    
    case MSG_WRITE: {
        // Check sentence lock
        if (is_sentence_locked(msg.filename, msg.sentence_num, msg.username)) {
            send_error(client_socket, "Sentence is locked by another user");
            break;
        }
        
        // Lock the sentence
        add_sentence_lock(msg.filename, msg.sentence_num, msg.username);
```

**Execution:**
1. Check if sentence already locked by another user
2. **Concurrency control:** Prevents two users editing same sentence
3. If locked, send error
4. If free, add lock with username

```c
        // Create backup for undo
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg.filename);
        
        char backup_path[MAX_PATH];
        snprintf(backup_path, sizeof(backup_path), "%s%s.backup", 
                 backup_dir, msg.filename);
        
        copy_file(filepath, backup_path);
```

**Execution:**
1. Build paths: original and backup
2. `copy_file()` duplicates entire file
3. **Undo system:** Snapshot before modification
4. Stored in `../backups/SS1/hello.txt.backup`

```c
        // Send ready signal
        struct Message ready;
        ready.type = MSG_READY_FOR_INPUT;
        send_message(client_socket, &ready);
```

**Execution:**
1. Tell client SS is ready for input
2. **Network:** SS → Client signal
3. Client displays interactive prompt

---

#### **Step 5: Client interactive input loop**

```c
    // Client receives READY signal
    printf("\nEnter words (format: <position> <word1> [word2] ...):\n");
    printf("Type 'ETIRW' on a new line to finish\n\n");
    
    char line[1024];
    while (1) {
        printf("> ");
        fgets(line, sizeof(line), stdin);
        line[strcspn(line, "\n")] = 0;
        
        if (strcmp(line, "ETIRW") == 0) {
            break;
        }
        
        // Send line to SS
        struct Message input_msg;
        input_msg.type = MSG_WRITE_DATA;
        strncpy(input_msg.data, line, sizeof(input_msg.data));
        send_message(ss_socket, &input_msg);
    }
```

**Execution:**
1. Client enters interactive mode
2. Reads lines from user
3. Each line sent to SS immediately
4. **User types:**
   ```
   > 1 Hello world.
   > 5 This is great!
   > ETIRW
   ```
5. Loop until "ETIRW" (WRITE backwards)

---

#### **Step 6: SS processes each word insertion**

```c
        case MSG_WRITE_DATA: {
            int position;
            char words[512];
            sscanf(msg.data, "%d %[^\n]", &position, words);
            
            // Insert words at position
            insert_words_at_position(msg.filename, msg.sentence_num, 
                                    position, words);
```

**Execution:**
1. Parse position and words from input
2. `insert_words_at_position()` modifies file
3. **Algorithm:** 
   - Read entire sentence
   - Split into words
   - Insert new words at position
   - Shift existing words right
   - Check for delimiter (. ! ?)
   - If delimiter added, split into new sentences
   - Write back to file

```c
            // Send acknowledgment
            struct Message ack;
            ack.type = MSG_SUCCESS;
            send_message(client_socket, &ack);
            break;
        }
```

**Execution:**
1. Send ACK to client
2. Ready for next input line

---

#### **Step 7: Finalize write**

When client sends "ETIRW":

```c
    // Client sends end signal
    struct Message end_msg;
    end_msg.type = MSG_WRITE_END;
    send_message(ss_socket, &end_msg);
```

**SS receives:**

```c
        case MSG_WRITE_END: {
            // Release sentence lock
            remove_sentence_lock(msg.filename, msg.sentence_num);
            
            // Update undo state (last operation was NOT undo)
            update_undo_state(msg.filename, 0);
            
            // Log
            log_message("storage_server", "WRITE operation completed");
            
            // Send final success
            struct Message response;
            response.type = MSG_SUCCESS;
            send_message(client_socket, &response);
            
            close(client_socket);
            break;
        }
```

**Execution:**
1. Remove sentence lock (other users can now edit)
2. Update undo state (prevents consecutive undo)
3. Log completion
4. Send final success
5. **Close SS connection** (write complete)
6. Client returns to command loop

---

## 7. Thread Management

### Threads in the System

#### **Naming Server Threads**

1. **Main Thread** - Accept loop (line 2712)
2. **Heartbeat Monitor Thread** - Checks SS health (line 2670)
3. **Client Handler Threads** - One per connected client/SS (line 2767)

**Example with 2 clients and 2 SS:**
- 1 main thread
- 1 heartbeat thread
- 2 client threads
- 2 SS threads
- **Total: 6 threads running concurrently**

#### **Storage Server Threads**

1. **Main Thread** - Accept client connections (line 1821)
2. **NS Command Thread** - Handle NS commands (line 1770)
3. **Client Handler Threads** - One per connected client (line 1860)

**Example with 3 clients:**
- 1 main thread
- 1 NS command thread
- 3 client threads
- **Total: 5 threads**

#### **Client Threads**

1. **Main Thread** - Command loop
2. **NS Monitor Thread** - Detect NS shutdown (optional, line 35)

**Total: 1-2 threads**

### Thread Synchronization

**Mutexes used:**
- `lock_mutex` - Protects sentence locks in SS
- `undo_mutex` - Protects undo state in SS
- `file_table_mutex` - Protects hash table in NS (if implemented)

**Example critical section:**

```c
pthread_mutex_lock(&lock_mutex);

// Check if sentence is locked
SentenceLock *current = locks;
while (current) {
    if (strcmp(current->filename, filename) == 0 &&
        current->sentence_num == sentence_num) {
        found = current;
        break;
    }
    current = current->next;
}

pthread_mutex_unlock(&lock_mutex);
```

**Why needed?**
- Multiple threads accessing shared `locks` linked list
- Without mutex: **race condition**
- Thread A checks lock while Thread B modifies it
- Result: Corruption, crashes, data loss

---

## 8. Shutdown & Cleanup

### Graceful Shutdown Sequence

#### **NS Shutdown (User types "SHUTDOWN")**

```c
if (strcmp(cmd, "SHUTDOWN") == 0) {
    printf("\n⚠️  Initiating Naming Server shutdown...\n");
    
    // Notify all storage servers
    StorageServer *ss = storage_servers;
    while (ss != NULL) {
        if (ss->is_active && ss->ss_socket >= 0) {
            printf("  Notifying storage server %s to disconnect...\n", ss->id);
            
            struct Message shutdown_msg;
            shutdown_msg.type = MSG_SHUTDOWN;
            send_message(ss->ss_socket, &shutdown_msg);
            close(ss->ss_socket);
        }
        ss = ss->next;
    }
    
    close(server_socket);
    exit(0);
}
```

**Execution:**
1. Iterate through all registered SS
2. Send `MSG_SHUTDOWN` to each
3. Close their sockets
4. Close listening socket
5. Exit process
6. **Result:** All SS notified, can shutdown gracefully

---

#### **SS Receives Shutdown**

```c
void* handle_ns_commands(void *arg) {
    while (1) {
        receive_message(ns_socket, &msg);
        
        switch (msg.type) {
            case MSG_SHUTDOWN:
                printf("\n⚠️  Naming Server has shut down\n");
                printf("✓ Storage server exiting...\n");
                exit(0);
                break;
```

**Execution:**
1. Receive shutdown message
2. Print notification
3. Exit immediately
4. **OS cleanup:** 
   - Closes all file descriptors
   - Frees all memory
   - Terminates all threads

---

#### **Client Detects NS Shutdown**

```c
void* monitor_ns_connection(void *arg) {
    while (ns_alive) {
        sleep(2);
        
        char buf[1];
        int result = recv(ns_socket, buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) {
            // Connection closed by server
            ns_alive = 0;
            printf("\n\n✗ Naming Server shut down\n");
            printf("✗ Client exiting...\n\n");
            exit(0);
        }
    }
}
```

**Execution:**
1. Background thread checks every 2 seconds
2. `recv()` with `MSG_PEEK`: read without consuming
3. `MSG_DONTWAIT`: non-blocking
4. Return value 0 means connection closed
5. Print message and exit
6. **User experience:** Automatic detection of NS shutdown

---

## 🎯 Complete Execution Timeline Example

### Scenario: User creates and writes to a file

```
T=0.0s  : ./naming_server/naming_server
          - Main thread starts
          - Initializes hash table
          - Spawns heartbeat thread
          - Creates socket, binds to 8080
          - Enters accept() loop
          
T=1.0s  : ./storage_server/storage_server SS1 127.0.0.1 8080 8081
          - Parses arguments
          - Creates directories: storage/SS1/, backups/SS1/
          - Connects to NS (127.0.0.1:8080)
          - Sends MSG_SS_REGISTER
          - NS receives, adds SS to registry
          - NS responds MSG_SUCCESS
          - SS spawns NS command handler thread
          - SS creates socket, binds to 8081
          - SS enters accept() loop
          
T=2.0s  : ./client/client
          - Prompts for username
          - User types: alice
          - Connects to NS (127.0.0.1:8080)
          - NS spawns new thread for alice
          - Client shows prompt: alice>
          
T=3.0s  : User types: CREATE hello.txt
          - Client parses command
          - Client sends MSG_CREATE to NS
          - NS receives in alice's thread
          - NS checks if file exists (hash lookup)
          - NS selects storage server (SS1)
          - NS sends MSG_CREATE to SS1
          - SS1 creates file: storage/SS1/hello.txt
          - SS1 responds MSG_SUCCESS to NS
          - NS adds file to hash table
          - NS responds MSG_SUCCESS to alice
          - Alice sees: ✓ File created successfully
          
T=4.0s  : User types: WRITE hello.txt 0
          - Client sends MSG_WRITE to NS
          - NS looks up file in hash table
          - NS checks ACL (alice is owner, has write permission)
          - NS responds MSG_REDIRECT_TO_SS with SS1 info
          - Client creates NEW socket
          - Client connects to SS1:8081 directly
          - Client sends MSG_WRITE to SS1
          - SS1 checks sentence lock (not locked)
          - SS1 adds lock for alice on sentence 0
          - SS1 creates backup: backups/SS1/hello.txt.backup
          - SS1 responds MSG_READY_FOR_INPUT
          
T=4.5s  : Client shows interactive prompt
          User types: 1 Hello world.
          - Client sends MSG_WRITE_DATA to SS1
          - SS1 parses: position=1, words="Hello world."
          - SS1 reads file (empty)
          - SS1 inserts words at position 1
          - SS1 detects delimiter '.'
          - SS1 creates sentence: "Hello world."
          - SS1 writes to file
          - SS1 responds MSG_SUCCESS
          
T=5.0s  : User types: ETIRW
          - Client sends MSG_WRITE_END to SS1
          - SS1 removes sentence lock
          - SS1 updates undo state (last_op = WRITE)
          - SS1 responds MSG_SUCCESS
          - SS1 closes connection
          - Client closes SS socket
          - Client returns to command loop
          - Client shows: alice>
          
T=6.0s  : Heartbeat monitor wakes up
          - Checks SS1 last_heartbeat timestamp
          - Last heartbeat was at T=5.5s (0.5s ago)
          - SS1 is healthy (< 15s threshold)
          - Goes back to sleep for 5s
          
T=7.0s  : User types: EXIT
          - Client sends MSG_EXIT to NS
          - NS removes alice from active sessions
          - NS closes alice's socket
          - NS thread terminates
          - Client closes ns_socket
          - Client prints "Goodbye!"
          - Client exits
```

---

## 🔍 Key Execution Patterns

### Pattern 1: Request-Response

```
Client                  NS                    SS
  |                     |                     |
  |---MSG_CREATE------->|                     |
  |                     |---MSG_CREATE------->|
  |                     |                     | (create file)
  |                     |<--MSG_SUCCESS-------|
  | (add to registry)   |                     |
  |<--MSG_SUCCESS-------|                     |
  |                     |                     |
```

### Pattern 2: Direct Client-SS (after NS redirect)

```
Client                  NS                    SS
  |                     |                     |
  |---MSG_READ--------->|                     |
  |                     | (check permission)  |
  |<--MSG_REDIRECT------|                     |
  |                     |                     |
  |------------MSG_READ----------------->    |
  |                     |                     | (read file)
  |<-----------FILE_DATA-----------------|    |
  |                     |                     |
```

### Pattern 3: Heartbeat

```
NS                                          SS
 |                                          |
 |---MSG_HEARTBEAT_REQUEST----------------->|
 |                                          |
 |<--MSG_HEARTBEAT_RESPONSE-----------------|
 |                                          |
 (updates last_heartbeat timestamp)        |
 |                                          |
```

---

## 📊 Performance Characteristics

### Time Complexity

| Operation | NS Lookup | SS Operation | Total |
|-----------|-----------|--------------|-------|
| CREATE | O(1) | O(1) | O(1) |
| READ | O(1) | O(n) | O(n) |
| WRITE | O(1) | O(n*m) | O(n*m) |
| DELETE | O(1) | O(1) | O(1) |
| SEARCH | O(n) | - | O(n) |

Where:
- n = file size (bytes)
- m = number of words in sentence

### Space Complexity

| Component | Memory Usage |
|-----------|--------------|
| NS Hash Table | O(files) |
| NS User Registry | O(users) |
| NS Search Cache | O(CACHE_SIZE * MAX_DATA) |
| SS Sentence Locks | O(concurrent_writes) |
| SS Undo Backups | O(files) |

---

## 🎓 Learning Points for Evaluation

### 1. **Concurrency**
- Multi-threading with pthreads
- Mutex locks for critical sections
- Thread-safe data structures
- Race condition prevention

### 2. **Network Programming**
- TCP sockets (SOCK_STREAM)
- Client-server architecture
- Direct peer-to-peer (Client-SS)
- Non-blocking I/O with select()

### 3. **Data Structures**
- Hash tables for O(1) lookup
- Linked lists for chaining
- LRU cache for search optimization
- ACL (Access Control Lists)

### 4. **System Design**
- Separation of concerns (NS vs SS)
- Load balancing across SS
- Fault tolerance (heartbeat)
- Graceful shutdown

### 5. **File System Operations**
- POSIX file I/O (fopen, fread, fwrite)
- Directory management (mkdir, opendir)
- File copying for undo
- Atomic operations

### 6. **Protocol Design**
- Message-based communication
- Type-length-value encoding
- Error handling
- State management

---

## 🔧 Debugging Tips

### View Logs in Real-Time

```bash
# Terminal 1 - NS logs
tail -f naming_server/ns.log

# Terminal 2 - SS logs
tail -f storage_server/ss1.log

# Terminal 3 - Run client
./client/client
```

### Trace Execution Flow

Add debug prints:
```c
printf("[DEBUG] Entering handle_create_request\n");
printf("[DEBUG] Filename: %s, Username: %s\n", msg->filename, msg->username);
printf("[DEBUG] Selected SS: %s\n", ss->id);
```

### Check Thread Status

```bash
# See all threads
ps -eLf | grep naming_server

# Count threads
ps -L -p <pid> | wc -l
```

---

**This guide covers the complete execution flow from system startup to shutdown. Each line is explained with its purpose, execution details, and relationship to the overall system architecture.**

**Good luck with your evaluation! 🚀**
