
# Smart Home Automation Node using ESP32 and FreeRTOS

## Overview
An ESP32-based smart home automation firmware project demonstrating
FreeRTOS multitasking, MQTT communication, sensor monitoring,
automatic control logic, and manual relay commands.

## Features
- Separate FreeRTOS tasks for sensor acquisition, control,
  MQTT communication, and health monitoring.
- MQTT telemetry publishing.
- MQTT commands for AUTO/MANUAL mode and relay control.
- Simulated temperature, humidity, light, and motion readings.
- Sensor freshness and free-heap monitoring.

## Architecture
- Sensor Task: generates demo sensor data.
- Control Task: evaluates automation rules and relay commands.
- MQTT Task: connects to Wi-Fi, subscribes to commands,
  and publishes telemetry.
- Health Task: reports sensor freshness and system statistics.

## MQTT Topics
- Telemetry: `student/smart-home-01/telemetry`
- Status: `student/smart-home-01/status`
- Commands: `student/smart-home-01/command`

## Commands
- `MODE:AUTO`
- `MODE:MANUAL`
- `RELAY:ON`
- `RELAY:OFF`

Relay ON/OFF commands are accepted only in MANUAL mode.

## Build
1. Open this project in VS Code with PlatformIO installed.
2. Enter Wi-Fi credentials in `src/main.cpp`.
3. Build the `esp32dev` environment.
4. Review serial output at 115200 baud.

## Current Status
Firmware starter implementation. Sensor data is simulated.
Compilation and runtime behavior must be verified in the
development environment. Physical sensors, relay operation,
and actual ESP32 behavior have not yet been hardware-tested.

## Safety and Security
- The public MQTT broker is for demonstration only.
- Do not send sensitive data or use it for real home control.
- Use an authenticated broker for deployment.
- Never connect mains voltage during software-only testing.
- Verify relay polarity and wiring before hardware use.

