# Makefile for Irrigation Controller Project

CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -Iinclude
LDFLAGS  := -pthread
LDLIBS   := -lrt

SRC_DIR  := src
INC_DIR  := include
BUILD_DIR:= build
BIN_DIR  := bin
SYSTEMD_DIR := systemd
EXAMPLES_DIR := examples

# Programs to build
PROGRAMS := irrigation irrigationctl irrigationd

# Source files
SRC := $(wildcard $(SRC_DIR)/*.c)

# Object files grouped by program
IRRIGATION_OBJ    := $(BUILD_DIR)/irrigation.o    $(BUILD_DIR)/mcp23017.o
IRRIGATIONCTL_OBJ := $(BUILD_DIR)/irrigationctl.o
IRRIGATIOND_OBJ   := $(BUILD_DIR)/irrigationd.o   $(BUILD_DIR)/mcp23017.o

# Default target
all: $(BIN_DIR) $(PROGRAMS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Pattern rule for building object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build programs
irrigation: $(IRRIGATION_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $(BIN_DIR)/$@

irrigationctl: $(IRRIGATIONCTL_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $(BIN_DIR)/$@

irrigationd: $(IRRIGATIOND_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $(BIN_DIR)/$@

# Install target
install: all
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(BIN_DIR)/* $(DESTDIR)/usr/local/bin/
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 $(SYSTEMD_DIR)/irrigationd.service $(DESTDIR)/etc/systemd/system/
	install -d $(DESTDIR)/etc/default
	install -m 644 $(EXAMPLES_DIR)/irrigationd $(DESTDIR)/etc/default/irrigationd

# Enable target (reload systemd, enable + start the service)
enable:
	systemctl daemon-reexec
	systemctl enable --now irrigationd.service

# Uninstall target
uninstall:
	rm -f $(DESTDIR)/usr/local/bin/irrigation
	rm -f $(DESTDIR)/usr/local/bin/irrigationctl
	rm -f $(DESTDIR)/usr/local/bin/irrigationd
	rm -f $(DESTDIR)/etc/systemd/system/irrigationd.service
	rm -f $(DESTDIR)/etc/default/irrigationd

# Clean target
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)