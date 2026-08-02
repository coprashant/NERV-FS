CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -D_GNU_SOURCE
SRC = src/main.c src/net.c src/threadpool.c src/protocol.c src/fileops.c src/handlers.c src/logging.c
OBJ = $(SRC:.c=.o)
TARGET = nervfs_server

CLIENT_SRC = client/nervfs_client.c src/protocol.c
CLIENT_TARGET = nervfs_client

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -Iinclude -o $(CLIENT_TARGET) $(CLIENT_SRC)

clean:
	rm -f $(OBJ) $(TARGET) $(CLIENT_TARGET)

.PHONY: all clean client