CC = gcc
CFLAGS = -Wall -Wextra -pthread -I../common
LDFLAGS = -pthread

# Original monolithic version
TARGET = naming_server
SRCS = naming_server.c
OBJS = $(SRCS:.c=.o) ../common/utils.o

# Modular version
TARGET_MODULAR = naming_server_modular
MODULE_SRCS = naming_server_modular.c \
              file_manager.c \
              access_control.c \
              storage_server_manager.c \
              folder_manager.c \
              checkpoint_manager.c \
              search_manager.c \
              user_session_manager.c \
              persistence.c
MODULE_OBJS = $(MODULE_SRCS:.c=.o) ../common/utils.o

# Default target: build both versions
all: $(TARGET) $(TARGET_MODULAR)

# Build original version
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Build modular version
$(TARGET_MODULAR): $(MODULE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile pattern rule
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build only original
original: $(TARGET)

# Build only modular
modular: $(TARGET_MODULAR)

clean:
	rm -f $(OBJS) $(MODULE_OBJS) $(TARGET) $(TARGET_MODULAR) *.log

.PHONY: all original modular clean
