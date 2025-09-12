#!/bin/bash

# MCP23017 I2C address and register definitions
I2C_ADDR=0x20           # Default I2C address for MCP23017 (can be 0x20 or 0x21 depending on address pins)
REG_OLATA=0x14          # OLATA register (output latch for port A)
REG_OLATB=0x15          # OLATB register (output latch for port B)

# Function to turn all zones ON
turn_all_on() {
    # Set all bits in OLATA and OLATB to 1 (turning all zones on)
    i2cset -y 1 $I2C_ADDR $REG_OLATA 0xff b
    i2cset -y 1 $I2C_ADDR $REG_OLATB 0xff b
    echo "All zones turned ON"
}

# Function to turn all zones OFF
turn_all_off() {
    # Clear all bits in OLATA and OLATB to 0 (turning all zones off)
    i2cset -y 1 $I2C_ADDR $REG_OLATA 0x00 b
    i2cset -y 1 $I2C_ADDR $REG_OLATB 0x00 b
    echo "All zones turned OFF"
}

# Check if an action is provided
if [ $# -lt 1 ]; then
    echo "Usage: $0 <on|off>"
    exit 1
fi

action=$1

# Validate the action
if [ "$action" != "on" ] && [ "$action" != "off" ]; then
    echo "Invalid action. Use 'on' or 'off'."
    exit 1
fi

# Perform the action
if [ "$action" == "on" ]; then
    turn_all_on
elif [ "$action" == "off" ]; then
    turn_all_off
fi

