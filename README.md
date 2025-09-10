# Irrigation Controller

This project provides a simple irrigation controller for Raspberry Pi using the **MCP23017 I²C GPIO expander**.  
It includes a daemon service, command-line utilities, and systemd integration so it can run automatically on boot.

## Features
- Control irrigation zones via **MCP23017** I²C GPIO expander.
- Daemon (`irrigationd`) that listens for commands.
- CLI utilities:
  - `irrigation` — manual zone control
  - `irrigationctl` — send commands to the daemon
- `systemd` unit for automatic start & management.

## Quick start

### Prerequisites
- Linux (tested on Raspberry Pi OS)
- GCC and build tools:
```bash
sudo apt-get update
sudo apt-get install build-essential i2c-tools
```
- I²C enabled on your Raspberry Pi (use `raspi-config` or enable via `/boot/config.txt`).

### Build
Clone the repository and build:
```bash
git clone https://github.com/YOURNAME/irrigation.git
cd irrigation
make
```
Binaries are created in the `bin/` directory:
```
bin/irrigation
bin/irrigationctl
bin/irrigationd
```

### Install
Install binaries system-wide and install the `systemd` unit:
```bash
sudo make install
sudo systemctl enable irrigationd
sudo systemctl start irrigationd
```

## Usage

Run the daemon manually (for testing):
```bash
irrigationd
```

Control a zone directly:
```bash
irrigation ZONE=1 TIME=30
```

Send a command to the running daemon:
```bash
irrigationctl ZONE=1 TIME=30
```

### Example
Start irrigation on zone 2 for 5 minutes:
```bash
irrigationctl ZONE=2 TIME=5
```

## Project layout
```
.
├── include/        # Header files (mcp23017.h)
├── src/            # Source files (irrigation.c, irrigationctl.c, irrigationd.c, mcp23017.c)
├── systemd/        # Systemd service unit (irrigationd.service)
├── scripts/        # Helper scripts (diagnostics.sh)
├── examples/       # Usage examples (examples.txt)
├── Makefile        # Build system
└── README.md       # This file
```

## Systemd management
Check status:
```bash
systemctl status irrigationd
```
Stop / restart:
```bash
sudo systemctl stop irrigationd
sudo systemctl restart irrigationd
```
View logs:
```bash
journalctl -u irrigationd -f
```

## Development & maintenance
Clean build artifacts:
```bash
make clean
```

Uninstall installed files:
```bash
sudo make uninstall
```

If you modify headers in `include/` or sources in `src/`, re-run `make`.

## Contributing
Contributions are welcome. When opening a PR, please:
- Make small, focused changes.
- Include a short description and testing steps.
- Update this README if you change user-facing behavior.

## License
This project is intended to be released under the MIT License.

---
