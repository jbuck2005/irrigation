# Makefile for Irrigation Controller Project

CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -Iinclude
LDFLAGS  := 

SRC_DIR  := src
INC_DIR  := include
BUILD_DIR:= build
BIN_DIR  := bin
SYSTEMD_DIR := systemd

# Programs to build
PROGRAMS := irrigation irrigationctl irrigationd

# Source files
SRC := $(wildcard $(SRC_DIR)/*.c)
OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRC))

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
	$(CC) $(CFLAGS) $^ -o $(BIN_DIR)/$@

irrigationctl: $(IRRIGATIONCTL_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $(BIN_DIR)/$@

irrigationd: $(IRRIGATIOND_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $(BIN_DIR)/$@

# Clean build files
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Install binaries and systemd unit
install: all
	install -d /usr/local/bin
	install -m 755 $(BIN_DIR)/irrigation /usr/local/bin/
	install -m 755 $(BIN_DIR)/irrigationctl /usr/local/bin/
	install -m 755 $(BIN_DIR)/irrigationd /usr/local/bin/
	install -d /etc/systemd/system
	install -m 644 $(SYSTEMD_DIR)/irrigationd.service /etc/systemd/system/

# Uninstall everything
uninstall:
	rm -f /usr/local/bin/irrigation
	rm -f /usr/local/bin/irrigationctl
	rm -f /usr/local/bin/irrigationd
	rm -f /etc/systemd/system/irrigationd.service

.PHONY: all clean install uninstall