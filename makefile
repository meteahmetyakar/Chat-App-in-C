# Makefile

# Compiler and flags
CC       := gcc
CFLAGS   := -std=gnu11 -Wall -Wextra -O2 -pthread \
             -Iclient/include -Iserver/include

# Client and Server source/build directories
CLIENT_SRCDIR   := client/src
CLIENT_BUILDDIR := client/build
CLIENT_BINDIR   := client
CLIENT_BIN      := $(CLIENT_BINDIR)/chatclient

SERVER_SRCDIR   := server/src
SERVER_BUILDDIR := server/build
SERVER_BINDIR   := server
SERVER_BIN      := $(SERVER_BINDIR)/chatserver

# Collect all .c files under client/src and server/src
CLIENT_SRCS := $(wildcard $(CLIENT_SRCDIR)/*.c)
SERVER_SRCS := $(wildcard $(SERVER_SRCDIR)/*.c)

# Map each .c → corresponding .o under the build directories
CLIENT_OBJS := $(patsubst $(CLIENT_SRCDIR)/%.c,$(CLIENT_BUILDDIR)/%.o,$(CLIENT_SRCS))
SERVER_OBJS := $(patsubst $(SERVER_SRCDIR)/%.c,$(SERVER_BUILDDIR)/%.o,$(SERVER_SRCS))

.PHONY: all clean

# Default target builds both client and server
all: $(CLIENT_BIN) $(SERVER_BIN)

# ------------------------------------------------------------
# 1) Build chatclient executable into client/ directory
# ------------------------------------------------------------
$(CLIENT_BIN): $(CLIENT_OBJS) | $(CLIENT_BUILDDIR)
	@echo "[LD] $@"
	$(CC) $(CFLAGS) -o $@ $^

# Compile each client .c → client/build/*.o
$(CLIENT_BUILDDIR)/%.o: $(CLIENT_SRCDIR)/%.c | $(CLIENT_BUILDDIR)
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Ensure client/build directory exists
$(CLIENT_BUILDDIR):
	mkdir -p $@

# ------------------------------------------------------------
# 2) Build chatserver executable into server/ directory
# ------------------------------------------------------------
$(SERVER_BIN): $(SERVER_OBJS) | $(SERVER_BUILDDIR)
	@echo "[LD] $@"
	$(CC) $(CFLAGS) -o $@ $^

# Compile each server .c → server/build/*.o
$(SERVER_BUILDDIR)/%.o: $(SERVER_SRCDIR)/%.c | $(SERVER_BUILDDIR)
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Ensure server/build directory exists
$(SERVER_BUILDDIR):
	mkdir -p $@

# ------------------------------------------------------------
# Clean up everything: remove build dirs and binaries in client/ and server/
# ------------------------------------------------------------
clean:
	@echo "[CLEAN] Removing build artifacts and executables..."
	rm -rf $(CLIENT_BUILDDIR) $(CLIENT_BIN)
	rm -rf $(SERVER_BUILDDIR) $(SERVER_BIN)

