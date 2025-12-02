CC = gcc
CFLAGS = -Wall -Wextra -pthread -I../common
LDFLAGS = -pthread

TARGET = utils.o
SRCS = utils.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
