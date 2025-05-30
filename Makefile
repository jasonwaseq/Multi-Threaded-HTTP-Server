CC = clang
CFLAGS = -Wall -Wextra -Werror -pedantic -pthread

# Object files
OBJS = httpserver.o

# Target executable
TARGET = httpserver

# Default target
all: $(TARGET)

# Build the httpserver executable
httpserver: $(OBJS) asgn4_helper_funcs.a
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) asgn4_helper_funcs.a

# Compile httpserver.c
httpserver.o: httpserver.c connection.h listener_socket.h queue.h rwlock.h request.h response.h
	$(CC) $(CFLAGS) -c httpserver.c

# Format source files with clang-format
format:
	clang-format -i -style=file *.c *.h

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean format
