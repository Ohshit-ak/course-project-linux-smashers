CC = gcc
CFLAGS = -Wall -Wextra -pthread -I../common
LDFLAGS = -pthread

# Original monolithic version
TARGET = storage_server
SRCS = storage_server.c
OBJS = $(SRCS:.c=.o) ../common/utils.o

# Modular version
TARGET_MODULAR = storage_server_modular
MODULAR_SRCS = storage_server_modular.c file_operations.c sentence_parser.c lock_manager.c undo_manager.c
MODULAR_OBJS = $(MODULAR_SRCS:.c=.o) ../common/utils.o

# Build both versions
all: $(TARGET) $(TARGET_MODULAR)

# Original storage server
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Modular storage server
$(TARGET_MODULAR): $(MODULAR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(MODULAR_OBJS) $(TARGET) $(TARGET_MODULAR) *.log *.o
	rm -rf storage/ backups/
