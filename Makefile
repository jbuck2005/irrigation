# -------------------------------------------------------------------
# Makefile for irrigationd project (hardened build)
# -------------------------------------------------------------------
# Targets:
#   make            -> build irrigationd daemon
#   make tests      -> build unit tests
#   make install    -> install daemon, service, defaults file, create user
#   make uninstall  -> remove installed files
#   make clean      -> remove all binaries/objects
#
# Flags:
#   DEBUG=1         -> enable debug symbols and disable optimizations
# -------------------------------------------------------------------

CC      := gcc

# Base warnings and C standard
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 -Iinclude

# Compiler hardening flags
CFLAGS  += -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Wformat -Wformat-security

# Linker hardening flags
LDFLAGS := -lpthread -lrt -pie -Wl,-z,relro,-z,now,-z,noexecstack

# Debug vs release
ifeq ($(DEBUG),1)
  CFLAGS  += -g -O0
else
  CFLAGS  += -O2
endif

SRC_DIR := src
INC_DIR := include

# Source files
DAEMON_SRCS := $(SRC_DIR)/irrigationd.c \
               $(SRC_DIR)/mcp23017.c \
               $(SRC_DIR)/rate_limit.c \
               $(SRC_DIR)/auth.c
DAEMON_OBJS := $(DAEMON_SRCS:.c=.o)

TEST_AUTH_SRCS := test_auth.c $(SRC_DIR)/auth.c
TEST_AUTH_OBJS := $(TEST_AUTH_SRCS:.c=.o)

TEST_RL_SRCS   := test_rate_limit.c $(SRC_DIR)/rate_limit.c
TEST_RL_OBJS   := $(TEST_RL_SRCS:.c=.o)

# Binaries
DAEMON_BIN := irrigationd
TEST_BINS  := test_auth test_rate_limit

# Installation paths
PREFIX      ?= /usr/local
BINDIR      := $(PREFIX)/bin
SYSTEMD_DIR ?= /etc/systemd
DEFAULTS_DIR?= /etc/default

SERVICE_FILE := irrigationd.service
DEFAULTS_FILE:= irrigationd.defaults

# -------------------------------------------------------------------
# Default target
# -------------------------------------------------------------------
all: $(DAEMON_BIN)

# -------------------------------------------------------------------
# Build daemon
# -------------------------------------------------------------------
$(DAEMON_BIN): $(DAEMON_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# -------------------------------------------------------------------
# Build tests
# -------------------------------------------------------------------
tests: $(TEST_BINS)

test_auth: $(TEST_AUTH_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

test_rate_limit: $(TEST_RL_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# -------------------------------------------------------------------
# Generic rule for .o
# -------------------------------------------------------------------
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# -------------------------------------------------------------------
# Install / uninstall
# -------------------------------------------------------------------
install: $(DAEMON_BIN)
# Create system user/group if missing
	@if ! id -u irrigationd >/dev/null 2>&1; then \
		echo "Creating system user irrigationd..."; \
		useradd -r -s /usr/sbin/nologin -d /var/lib/irrigationd -M irrigationd; \
	fi

# Install daemon binary
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(DAEMON_BIN) $(DESTDIR)$(BINDIR)

# Install systemd service file
	install -d $(DESTDIR)$(SYSTEMD_DIR)
	install -m 0644 $(SERVICE_FILE) $(DESTDIR)$(SYSTEMD_DIR)/$(SERVICE_FILE)

# Install /etc/default/irrigationd if not present
	install -d $(DESTDIR)$(DEFAULTS_DIR)
	@if [ ! -f $(DESTDIR)$(DEFAULTS_DIR)/irrigationd ]; then \
		install -m 0640 $(DEFAULTS_FILE) $(DESTDIR)$(DEFAULTS_DIR)/irrigationd; \
	fi
	@chown root:irrigationd $(DESTDIR)$(DEFAULTS_DIR)/irrigationd

	@echo "Install complete."
	@echo "Run the following to enable service:"
	@echo ""
	@echo "sudo systemctl daemon-reexec"
	@echo "sudo systemctl enable irrigationd"
	@echo "sudo systemctl start irrigationd"
	@echo ""
	@echo "sudo vi sudo vi /etc/default/irrigationd"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(DAEMON_BIN)
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/$(SERVICE_FILE)
	rm -f $(DESTDIR)$(DEFAULTS_DIR)/irrigationd

# -------------------------------------------------------------------
# Clean
# -------------------------------------------------------------------
clean:
	rm -f $(DAEMON_OBJS) $(TEST_AUTH_OBJS) $(TEST_RL_OBJS) \
	      $(DAEMON_BIN) $(TEST_BINS)

.PHONY: all tests clean install uninstall
