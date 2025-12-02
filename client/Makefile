CC = gcc
CFLAGS = -Wall -Wextra -pthread -I../common
LDFLAGS = -pthread

TARGET = client
TARGET_MODULAR = client_modular

# Original monolithic build
SRCS = client.c
OBJS = $(SRCS:.c=.o) ../common/utils.o

# Modular build
MODULAR_SRCS = client_modular.c connection_manager.c file_operations_client.c \
               access_manager.c folder_operations.c checkpoint_operations.c \
               advanced_operations.c command_parser.c
MODULAR_OBJS = $(MODULAR_SRCS:.c=.o) ../common/utils.o

all: $(TARGET) $(TARGET_MODULAR)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(TARGET_MODULAR): $(MODULAR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(MODULAR_OBJS) $(TARGET) $(TARGET_MODULAR) *.log
