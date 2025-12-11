# 📁 Docs++ - Distributed File System

A robust, distributed file system implementation in C that enables collaborative file management with advanced features like access control, file streaming, command execution, and comprehensive logging.

---

## 🌟 Features at a Glance

### Core File Operations
- ✅ **CREATE** - Create new files on any storage server
- ✅ **READ** - Read file contents with permission checking
- ✅ **WRITE** - Collaborative sentence-level editing with locking
- ✅ **DELETE** - Remove files (owner-only)
- ✅ **INFO** - View detailed file metadata
- ✅ **VIEW** - List files with multiple display modes

### Advanced Features
- 🔄 **STREAM** - Word-by-word streaming with 0.1s delay
- ⚡ **EXEC** - Execute file content as shell commands on naming server
- ↩️ **UNDO** - Revert to previous file version (no consecutive undos)
- 🔍 **SEARCH** - Fast file search with caching
- 📂 **Folder Management** - Create and organize files in folders
- 🔖 **Checkpoints** - Create, view, and revert to file snapshots
- 🔐 **Access Control** - Fine-grained read/write permissions
- 📊 **Access Requests** - Request and manage file access permissions
- 🎯 **USE Command** - Switch between storage servers with validation
- 📋 **LISTSS Command** - View all storage servers and their status

### Fault Tolerance & Caching
- 💾 **Automatic Backups** - Files synced to NS after every WRITE
- 🗄️ **Cache System** - Temporary file storage when SS is down
- 🔄 **3-Tier Failover** - Cache → Backup → Alternative SS
- 💓 **Heartbeat Monitoring** - Detects SS failures within 5 seconds
- 🔁 **Seamless Recovery** - READ operations work even when SS is offline

### System Features
- 🔗 **Multiple Storage Servers** - Scalable storage architecture
- 👥 **Multi-User Support** - Concurrent user sessions
- 🔒 **Sentence-Level Locking** - Prevent write conflicts
- 📝 **Comprehensive Logging** - All operations logged with timestamps
- 🔄 **Persistent Connections** - Efficient NS-SS communication
- ⚡ **Hash Table Lookup** - O(1) file access
- 💪 **Fault Tolerance** - Heartbeat monitoring, cache, and automatic failover
- 🛡️ **High Availability** - Files accessible even when storage servers are down

---

## 📋 Table of Contents

- [Architecture](#-architecture)
- [Quick Start](#-quick-start)
- [Command Reference](#-command-reference)
- [Project Structure](#-project-structure)
- [Implementation Highlights](#-implementation-highlights)
- [Testing](#-testing)
- [Documentation](#-documentation)
- [Team](#-team)

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        Clients (Multiple)                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │  User 1  │  │  User 2  │  │  User 3  │  │  User N  │    │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘    │
└───────┼─────────────┼─────────────┼─────────────┼───────────┘
        │             │             │             │
        └─────────────┴─────────────┴─────────────┘
                          │
                          ▼
        ┌─────────────────────────────────────────┐
        │       Naming Server (NS)                │
        │  • File metadata & location             │
        │  • User access control (ACL)            │
        │  • Hash table for O(1) lookup           │
        │  • Search cache                         │
        │  • Persistent SS connections            │
        └──────────────┬──────────────────────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
    ┌────────┐    ┌────────┐    ┌────────┐
    │  SS1   │    │  SS2   │    │  SS3   │
    │ Port   │    │ Port   │    │ Port   │
    │ 8081   │    │ 8082   │    │ 8083   │
    └────────┘    └────────┘    └────────┘
    Storage Servers (Multiple)
    • File storage on disk
    • Sentence-level locking
    • Undo/backup management
    • Direct client connections
```

### Component Details

#### 🎯 Naming Server (NS)
The central coordinator managing all file system operations.

**Key Features:**
- **File Registry** - Hash table-based file lookup (O(1) complexity)
- **Access Control** - Per-file ACLs with read/write permissions
- **Storage Mapping** - Tracks which SS stores each file
- **User Management** - Session tracking and authentication
- **Search Optimization** - LRU cache for frequently searched patterns
- **Folder Management** - Hierarchical folder structure
- **Checkpoint Tracking** - Metadata for file snapshots
- **Request Queue** - Manages access permission requests
- **Cache Management** - Temporary file storage in `../cache/` when SS down
- **Backup Storage** - Persistent file copies in `../backups/SS_ID/`
- **Failover Logic** - 3-tier fallback: Cache → Backup → Alternative SS
- **Heartbeat Monitor** - Detects SS failures and marks inactive

**Port:** 8080 (default)
**Cache Directory:** `../cache/` (auto-created)
**Backup Directory:** `../backups/` (auto-created)

#### 💾 Storage Server (SS)
Handles actual file storage and direct client operations.

**Key Features:**
- **File Operations** - CREATE, READ, WRITE, DELETE, STREAM
- **Sentence Parsing** - Intelligent delimiter handling (. ! ?)
- **Write Locking** - Per-sentence locks to prevent conflicts
- **Undo System** - Automatic backup before modifications
- **Dynamic Splitting** - Sentences auto-split when delimiters added
- **Checkpoint Storage** - Snapshot management
- **Heartbeat** - Regular health checks to NS
- **Backup Sync** - Updates NS backup after every WRITE completion
- **Fault Detection** - Notifies NS on startup/shutdown

**Ports:** 
- Client Port: 8081+ (configurable)
- NS Port: Client Port + 1000

**Storage Directories:**
- `storage/SS_ID/` - Primary file storage
- `backups/SS_ID/` - Undo backups (local)
- NS maintains separate backup copies in `../backups/SS_ID/`

#### 💻 Client
Interactive user interface for file system access.

**Key Features:**
- **Interactive Shell** - Command-line interface with help
- **Direct SS Connection** - For data operations (READ, WRITE, STREAM)
- **NS Fallback** - Receives content directly from NS when SS down
- **Session Management** - Single session per user
- **Auto-reconnect** - NS connection monitoring
- **Progress Indicators** - Visual feedback for operations
- **SS Validation** - USE command validates SS existence before switching
- **Cache Indicators** - Shows when content served from NS cache/backup

---

## 🚀 Quick Start

### Prerequisites

```bash
# Required
- GCC 7.5.0 or higher
- Linux/Unix environment (Ubuntu 18.04+ recommended)
- POSIX threads library (pthread)
- Make utility

# Optional for testing
- Bash 4.0+
- netstat (for port checking)
```

### Installation

```bash
# Clone the repository
git clone https://github.com/CS3-OSN-Monsoon-2025/course-project-linux-smashers.git
cd course-project-linux-smashers

# Build all components
make clean && make

# Verify build
ls -lh naming_server/naming_server storage_server/storage_server client/client
```

### Running the System

#### Method 1: Manual (Recommended for Learning)

Open **three separate terminals**:

**Terminal 1 - Naming Server:**
```bash
./naming_server/naming_server
# Listens on port 8080
# Logs to: naming_server/ns.log
```

**Terminal 2 - Storage Server:**
```bash
./storage_server/storage_server SS1 127.0.0.1 8080 8081
# SS1: Server ID
# 127.0.0.1: NS IP
# 8080: NS Port
# 8081: Client Port
# Logs to: storage_server/ss1.log
```

**Terminal 3 - Client:**
```bash
./client/client 127.0.0.1 8080
# Connects to NS at 127.0.0.1:8080
# Enter username when prompted
```

#### Method 2: Using Makefile Targets

```bash
# Terminal 1
make run_ns

# Terminal 2
make run_ss1

# Terminal 3
make run_client
```

#### Method 3: Background Mode (Testing)

```bash
# Start all in background
make run_ns &
sleep 2
make run_ss1 &
make run_ss2 &
sleep 1
make run_client
```

### First Steps Tutorial

```bash
# 1. Login (client will prompt for username)
Username: alice

# 2. View available storage servers
alice> LISTSS
# Shows all storage servers and their status

# 3. Select a storage server (optional)
alice> USE SS1
Switched to storage server: SS1

# 4. Create your first file
alice> CREATE hello.txt

# 5. Write some content
alice> WRITE hello.txt 0
1 Hello world.
2 This is my first file.
ETIRW

# 6. Read the file
alice> READ hello.txt

# 7. Test cache system (advanced)
# Stop SS1 (Ctrl+C in SS terminal)
alice> READ hello.txt
# ✓ Still works! Content served from NS cache/backup

# 8. View all files
alice> VIEW

# 9. Get file details
alice> INFO hello.txt

# 10. Share with another user (in another client)
alice> ADDACCESS -R hello.txt bob

# 11. Stream the file (word-by-word)
alice> STREAM hello.txt

# 12. Exit
alice> EXIT
```

---

## 📝 Command Reference

### File Operations

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **CREATE** | `CREATE <filename>` | Create a new file | `CREATE notes.txt` |
| **READ** | `READ <filename>` | Read file contents | `READ notes.txt` |
| **WRITE** | `WRITE <filename> <sentence#>` | Edit specific sentence | `WRITE notes.txt 0` |
| **DELETE** | `DELETE <filename>` | Delete file (owner only) | `DELETE notes.txt` |
| **INFO** | `INFO <filename>` | Show file metadata | `INFO notes.txt` |
| **VIEW** | `VIEW [-al]` | List accessible files | `VIEW -al` |
| **STREAM** | `STREAM <filename>` | Stream file word-by-word | `STREAM notes.txt` |
| **UNDO** | `UNDO <filename>` | Revert last change | `UNDO notes.txt` |
| **EXEC** | `EXEC <filename>` | Execute file as script | `EXEC script.sh` |

### WRITE Command Details

The WRITE command uses an interactive mode:

```bash
alice> WRITE myfile.txt 0
# Enter words with position and content:
<position> <word1> [word2] [word3]...
ETIRW  # Type ETIRW to finish

# Example:
alice> WRITE story.txt 0
1 Once upon a time.
ETIRW
```

**Rules:**
- Words are inserted at position, shifting existing words right
- No overwriting - always inserts
- Delimiters (. ! ?) create sentence boundaries
- Type `ETIRW` (WRITE backwards) to save

### Access Control

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **LIST** | `LIST` | Show all registered users | `LIST` |
| **ADDACCESS** | `ADDACCESS -[RW] <file> <user>` | Grant access | `ADDACCESS -W notes.txt bob` |
| **REMACCESS** | `REMACCESS <file> <user>` | Revoke access | `REMACCESS notes.txt bob` |
| **REQUESTACCESS** | `REQUESTACCESS -[RW] <file>` | Request access from owner | `REQUESTACCESS -R secret.txt` |
| **VIEWREQUESTS** | `VIEWREQUESTS <file>` | View pending requests | `VIEWREQUESTS myfile.txt` |
| **RESPONDREQUEST** | `RESPONDREQUEST <file> <id> <Y/N>` | Approve/deny request | `RESPONDREQUEST myfile.txt 1 Y` |

**Access Flags:**
- `-R` - Read-only access
- `-W` - Write access (includes read)

### Folder Management

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **CREATEFOLDER** | `CREATEFOLDER <path>` | Create folder | `CREATEFOLDER docs` |
| **MOVE** | `MOVE <file> <folder>` | Move file to folder | `MOVE notes.txt docs` |
| **VIEWFOLDER** | `VIEWFOLDER <path>` | List folder contents | `VIEWFOLDER docs` |

### Checkpoints

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **CHECKPOINT** | `CHECKPOINT <file> <tag>` | Create snapshot | `CHECKPOINT draft.txt v1` |
| **LISTCHECKPOINTS** | `LISTCHECKPOINTS <file>` | List all snapshots | `LISTCHECKPOINTS draft.txt` |
| **VIEWCHECKPOINT** | `VIEWCHECKPOINT <file> <tag>` | View snapshot content | `VIEWCHECKPOINT draft.txt v1` |
| **REVERT** | `REVERT <file> <tag>` | Restore from snapshot | `REVERT draft.txt v1` |

### Search

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **SEARCH** | `SEARCH <pattern>` | Find files by name | `SEARCH .txt` |

**Search Features:**
- Case-insensitive matching
- Substring matching
- Results cached for performance
- Shows owner and storage server

### Storage Server Management

| Command | Syntax | Description | Example |
|---------|--------|-------------|---------|
| **USE** | `USE <server_id>` | Switch active storage server | `USE SS2` |
| **LISTSS** | `LISTSS` | List all storage servers with status | `LISTSS` |

**USE Command Details:**
- Switch which storage server your client operations target
- Validates that the storage server exists and is Active
- Affects subsequent CREATE operations
- Shows error if SS doesn't exist or is Inactive

```bash
# Example: Switch to different storage servers
alice> USE SS1
Switched to storage server: SS1

alice> CREATE file1.txt
File created on SS1

alice> USE SS2
Switched to storage server: SS2

alice> CREATE file2.txt
File created on SS2

# Validation prevents invalid switches
alice> USE SS99
✗ Error: Storage server 'SS99' not found or inactive
```

**LISTSS Command Details:**
- Shows all registered storage servers
- Displays server ID, address (IP:Port), and status (Active/Inactive)
- Useful before using USE command to see available servers

```bash
alice> LISTSS

╔════════════════════════════════════════════════════════════════╗
║               Storage Servers                                  ║
╠════════════════════════════════════════════════════════════════╣
║ ID         Address           Status                           ║
╠════════════════════════════════════════════════════════════════╣
SS1        127.0.0.1:8081    Active
SS2        127.0.0.1:8082    Active
SS3        127.0.0.1:8083    Inactive
╚════════════════════════════════════════════════════════════════╝
```

### System Commands

| Command | Description |
|---------|-------------|
| **HELP** | Show command help |
| **EXIT** | Disconnect from server |

---

## 🗂️ Project Structure

```
course-project-linux-smashers/
│
├── 📁 client/
│   ├── client.c              # 2000+ lines: Client implementation
│   ├── Makefile
│   └── client.log            # Runtime log (auto-generated)
│
├── 📁 naming_server/
│   ├── naming_server.c       # 2600+ lines: NS implementation
│   ├── Makefile
│   └── ns.log               # Runtime log (auto-generated)
│
├── 📁 storage_server/
│   ├── storage_server.c      # 1800+ lines: SS implementation
│   ├── Makefile
│   ├── storage/             # File storage (auto-generated)
│   │   └── SS1/
│   │       ├── file1.txt
│   │       ├── file2.txt
│   │       └── checkpoints/
│   ├── backups/             # Undo backups (auto-generated)
│   │   └── SS1/
│   │       ├── file1.txt.backup
│   │       └── file2.txt.backup
│   └── ss1.log             # Runtime log (auto-generated)
│
├── 📁 cache/                 # NS cache (auto-generated)
│   └── file1.txt            # Temporary copies when SS down
│
├── 📁 backups/               # NS backups (auto-generated)
│   ├── SS1/
│   │   ├── file1.txt        # Synced after every WRITE
│   │   └── file2.txt
│   └── SS2/
│       └── file3.txt
│
├── 📁 common/
│   ├── protocol.h           # Message structures, constants
│   ├── utils.h              # Utility function declarations
│   ├── utils.c              # Logging, network, file utilities
│   └── Makefile
│
├── 📁 tests/
│   ├── basic_test.sh        # Basic functionality tests
│   ├── test_comprehensive.sh # Full feature test suite
│   ├── test_folders.sh      # Folder operations tests
│   ├── test_checkpoints.sh  # Checkpoint tests
│   └── test_files/          # Test data
│
├── 📄 Makefile               # Root makefile (builds all)
├── 📄 README.md              # This file
├── 📄 LOGGING_IMPROVEMENTS.md # Logging documentation
└── 📄 naming_server/
    ├── MODULAR_ARCHITECTURE.md
    └── MODULARIZATION_COMPLETE.md
```

### File Size Reference
- **Client:** ~2000 lines of C code
- **Naming Server:** ~2600 lines of C code
- **Storage Server:** ~1800 lines of C code
- **Common:** ~500 lines of shared code
- **Total:** ~7000+ lines of production code

---

## 💡 Implementation Highlights

### 1. Efficient File Lookup
```c
// Hash table for O(1) file access
unsigned int hash_function(const char *str) {
    unsigned int hash = 5381;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_TABLE_SIZE;
}
```

### 2. Sentence-Level Write Locking
```c
// Multiple users can edit different sentences simultaneously
typedef struct SentenceLock {
    char filename[MAX_FILENAME];
    int sentence_num;
    char locked_by[MAX_USERNAME];
    time_t lock_time;
} SentenceLock;
```

### 3. Intelligent Sentence Parsing
```c
// Handles single delimiters (. ! ?) as sentence endings
// Multiple delimiters (... !!!) treated as words
int sentence_has_delimiter(const char *sentence) {
    // Only single delimiter at end = sentence boundary
    // Prevents false splits on ellipsis (...)
}
```

### 4. Dynamic Sentence Splitting
```c
// Automatically splits sentences when delimiter is added
if (word_contains_delimiter(new_word)) {
    split_into_new_sentences();
    notify_client_of_split();
}
```

### 5. Persistent NS-SS Connections
```c
// Avoids connection overhead for every operation
StorageServer {
    int ss_socket;  // Kept open for NS commands
    int is_active;
    time_t last_heartbeat;
}
```

### 6. Search Result Caching
```c
// LRU cache for frequent searches
typedef struct SearchCacheEntry {
    char query[MAX_FILENAME];
    char results[MAX_DATA];
    time_t timestamp;
} SearchCacheEntry;
```

### 7. Comprehensive Logging
```c
// Every operation logged with timestamps
void log_message(const char *component, const char *message) {
    FILE *log = fopen(logfile, "a");
    fprintf(log, "[%s] %s\n", timestamp, message);
}

// Special logging for stop packets (READ/STREAM)
log_message("storage_server", "READ stop packet sent for 'file.txt'");
```

### 8. Undo System
```c
// Automatic backup before every WRITE
before_write() {
    copy_file(original, backup);
}

on_undo() {
    if (last_operation_was_undo) {
        return ERR_CONSECUTIVE_UNDO;
    }
    restore_from_backup();
}
```

### 9. Cache & Failover System
```c
// 3-tier fallback when storage server is down
handle_read_request() {
    // Tier 1: Check cache (fastest, most recent)
    char cache_path[MAX_PATH];
    snprintf(cache_path, sizeof(cache_path), "../cache/%s", filename);
    if (file_exists(cache_path)) {
        send_file_content(cache_path);  // RESP_SUCCESS
        return;
    }
    
    // Tier 2: Check backup and create cache copy
    char backup_path[MAX_PATH];
    snprintf(backup_path, sizeof(backup_path), "../backups/%s/%s", ss_id, filename);
    if (file_exists(backup_path)) {
        copy_to_cache(backup_path, cache_path);
        send_file_content(cache_path);  // RESP_SUCCESS
        return;
    }
    
    // Tier 3: Failover to another active SS
    StorageServer *alternative_ss = get_available_ss();
    if (alternative_ss) {
        reassign_file(filename, alternative_ss->id);
        send_ss_info(alternative_ss);  // RESP_SS_INFO
        return;
    }
    
    // All tiers failed
    send_error(ERR_SS_UNAVAILABLE);
}
```

### 10. Backup Synchronization
```c
// Update NS backup after every WRITE completion
on_write_complete() {
    char backup_path[MAX_PATH];
    snprintf(backup_path, sizeof(backup_path), "%s%s", backup_dir, filename);
    
    FILE *src = fopen(filepath, "r");
    FILE *dst = fopen(backup_path, "w");
    
    if (src && dst) {
        char buffer[40960];  // 40KB buffer
        size_t bytes = fread(buffer, 1, sizeof(buffer), src);
        fwrite(buffer, 1, bytes, dst);
        fclose(src);
        fclose(dst);
        log("Backup updated for '%s'", filename);
    }
}
```

### 11. Storage Server Validation
```c
// USE command validates SS before switching
handle_use_command(char *ss_id) {
    // Query MSG_LIST_SS from NS
    Message msg = {.type = MSG_LIST_SS};
    send_message(ns_socket, &msg);
    recv_message(ns_socket, &msg);
    
    // Parse SS list and validate
    bool found = false;
    bool active = false;
    char *line = strtok(msg.data, "\n");
    while (line) {
        if (strstr(line, ss_id) && strstr(line, "Active")) {
            found = true;
            active = true;
            break;
        }
        line = strtok(NULL, "\n");
    }
    
    if (!found || !active) {
        printf("Error: Storage server '%s' not found or inactive\n", ss_id);
        return;
    }
    
    // Switch to validated SS
    strcpy(current_ss, ss_id);
    printf("Switched to storage server: %s\n", ss_id);
}
```

---

## 🧪 Testing

### Quick Test
```bash
# Run basic functionality test
./tests/basic_test.sh

# Expected output: All operations should succeed
```

### Comprehensive Test Suite
```bash
# Run all feature tests
./tests/test_comprehensive.sh

# Tests include:
# - CREATE, READ, WRITE, DELETE operations
# - Multi-user access control
# - Folder management
# - Checkpoints
# - Error handling
# - Concurrent operations
```

### Manual Testing Checklist

- [ ] **Basic Operations**
  - [ ] CREATE file
  - [ ] READ file
  - [ ] WRITE to file
  - [ ] DELETE file
  - [ ] VIEW files

- [ ] **Access Control**
  - [ ] Add read access
  - [ ] Add write access
  - [ ] Remove access
  - [ ] Request access

- [ ] **Advanced Features**
  - [ ] STREAM file
  - [ ] EXEC script
  - [ ] UNDO changes
  - [ ] SEARCH files

- [ ] **Folders**
  - [ ] Create folder
  - [ ] Move file to folder
  - [ ] View folder contents

- [ ] **Checkpoints**
  - [ ] Create checkpoint
  - [ ] List checkpoints
  - [ ] View checkpoint
  - [ ] Revert to checkpoint

- [ ] **Storage Server Management**
  - [ ] LISTSS shows all servers
  - [ ] USE validates SS exists
  - [ ] USE rejects inactive SS
  - [ ] USE rejects non-existent SS

- [ ] **Cache & Failover**
  - [ ] READ works when SS down (from cache)
  - [ ] Backup synced after WRITE
  - [ ] Cache created on first access
  - [ ] Failover to alternative SS

- [ ] **Error Handling**
  - [ ] File not found
  - [ ] Permission denied
  - [ ] File locked
  - [ ] Invalid syntax
  - [ ] SS unavailable (all tiers fail)

### Monitoring & Debugging

```bash
# Real-time log monitoring
tail -f naming_server/ns.log
tail -f storage_server/ss1.log

# Check for errors
grep "ERROR" naming_server/ns.log
grep "failed" storage_server/ss1.log

# Port verification
netstat -tulpn | grep 808

# Process status
ps aux | grep -E "naming_server|storage_server|client"
```

---

## 📚 Documentation

### Main Documentation
- **README.md** (this file) - Complete project overview
- **LOGGING_IMPROVEMENTS.md** - Logging system details
- **naming_server/MODULAR_ARCHITECTURE.md** - NS design details
- **naming_server/MODULARIZATION_COMPLETE.md** - Component breakdown

### Quick References
All commands documented with examples in this README

### Protocol Documentation
See `common/protocol.h` for:
- Message type constants
- Error codes
- Data structures
- Communication protocol

---

## 🎯 Key Features Summary

| Feature | Status | Description |
|---------|--------|-------------|
| CREATE | ✅ | Create files on any storage server |
| READ | ✅ | Read with permission checks + stop packet logging |
| WRITE | ✅ | Sentence-level collaborative editing |
| DELETE | ✅ | Owner-only deletion |
| INFO | ✅ | Detailed file metadata |
| VIEW | ✅ | Multiple display modes (-a, -l) |
| STREAM | ✅ | Word-by-word with 0.1s delay + stop packet logging |
| EXEC | ✅ | Execute on NS, output to client |
| UNDO | ✅ | Single-level undo (no consecutive) |
| SEARCH | ✅ | Fast search with LRU caching |
| Folders | ✅ | Create, move, view |
| Checkpoints | ✅ | Create, list, view, revert |
| Access Control | ✅ | Read/write permissions |
| Access Requests | ✅ | Request, view, approve/deny |
| Multi-user | ✅ | Concurrent sessions |
| Logging | ✅ | Comprehensive with stop packets |
| Multiple SS | ✅ | Scalable storage |
| Fault Tolerance | ✅ | Heartbeat monitoring |
| **Cache System** | ✅ | Temporary storage in `../cache/` when SS down |
| **Backup Sync** | ✅ | NS backups updated after every WRITE |
| **3-Tier Failover** | ✅ | Cache → Backup → Alternative SS |
| **LISTSS** | ✅ | View all storage servers with status |
| **USE Validation** | ✅ | Validates SS exists and is Active |
| **High Availability** | ✅ | Files accessible even when SS offline |

---

## 🛠️ Troubleshooting

### Common Issues

**Port already in use:**
```bash
# Kill existing processes
pkill -9 naming_server
pkill -9 storage_server
pkill -9 client

# Check ports are free
netstat -tulpn | grep 808
```

**Connection refused:**
```bash
# Start in order: NS → SS → Client
# Wait 1-2 seconds between starts
```

**Permission denied:**
```bash
# Make binaries executable
chmod +x naming_server/naming_server
chmod +x storage_server/storage_server
chmod +x client/client
```

**Compilation errors:**
```bash
# Clean and rebuild
make clean
make

# Check GCC version
gcc --version  # Need 7.5.0+
```

---

## 👥 Team

**Team Name:** Linux Smashers

**Members:**
- **Akshith**
- **Kartik**

**Course:** CS3301 - Operating Systems and Networks  
**Institution:** IIIT Hyderabad  
**Semester:** Monsoon 2025  

---

## 🙏 Acknowledgments

- **Dr. Karthik Vaidhyanathan** - Course Instructor
- **Teaching Assistants** - Guidance and requirements clarification
- **IIIT Hyderabad** - Infrastructure and support
- **GitHub Copilot** - AI-assisted development

---

## 📄 License

This is an academic project for the CS3301 Operating Systems and Networks course at IIIT Hyderabad. All rights reserved.

**⭐ If you found this project helpful, please give it a star!**

**Last Updated:** December 2025
