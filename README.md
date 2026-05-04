# Modbus Sensor Emulator

An ESP32-based Modbus RTU slave device that emulates two greenhouse sensors concurrently on a single RS485 bus: an **ASAIR FG6485A** temperature/humidity transmitter and a **Seeed SenseCAP S200** wind speed and direction meter. Intended as a development and test tool for systems that query these sensors.

## Features

- Concurrent emulation of FG6485A (Modbus FC03, T/RH) and S200 (Modbus FC04, wind speed + direction)
- Three independent modes per sensor:
  - **Manual** — values set directly via the web interface (slider + input box with Apply)
  - **Live** — values fetched live from [Open-Meteo](https://open-meteo.com/) weather API (internet required; ≤ 1 000 calls/day free tier)
  - **Replay** — timestamped `.csv` file replayed at wall-clock speed
- Location auto-detected from public IP (priority 1) or set manually in the web interface (priority 2)
- WiFi: automatic AP/STA switching; AP SSID `SensorEmulator-AABB` (open, no password)
- mDNS: reachable as `http://emulator.local` when connected to a WiFi network
- NTP time synchronisation (configurable server); manual time fallback
- Live-updating web interface (HTTP + WebSocket), available in both AP and STA mode
- Modbus activity log streamed live to the web interface
- Modbus slave address per sensor configurable in the web interface
- All settings persisted in NVS (survive reboot)
- RGB LED status feedback: blue steady (idle), green blink (valid frame), red blink (CRC error), red solid (fault)
- Uses FreeRTOS for concurrent task management

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | M5Stack Atom Lite (ESP32-PICO-D4) |
| RS485 interface | M5Stack Atomic RS485 Base — UART2 RX G22 / TX G19 (hardware direction control) |
| Baud rate | 9600 baud, 8N1 (matches both emulated sensors) |
| RGB LED | G27 (FastLED) |

## Emulated Modbus Register Maps

### FG6485A — Modbus RTU slave (default address 1, FC03)

| Register | Description | Encoding |
|----------|-------------|---------|
| 0x0000 | Relative humidity | uint16 × 10 (%RH) |
| 0x0001 | Temperature | int16 × 10 (°C) |
| 0x0008–0x000B | Device info | read-only static values |
| 0x000C–0x0013 | Alarm config | FC03 read / FC16 write |

### S200 — Modbus RTU slave (default address 44, FC04)

| Register | Description | Encoding |
|----------|-------------|---------|
| 0x0008–0x000D | Wind direction min/max/avg | int32 × 1000 (°) |
| 0x000E–0x0013 | Wind speed min/max/avg | int32 × 1000 (m/s) |
| 0x001C–0x001D | Heating temperature | int32 × 1000 (°C) |
| 0x1000–0x1001 | Device address / baud config | FC03 read / FC16 write |

## Repository Structure

```
greenhouse-Controller-Modbus-sensor-emulator/
│
├── firmware/                   ← PlatformIO / ESP-IDF project
│   ├── platformio.ini
│   ├── data/                   ← SPIFFS web assets (index.html, style.css, app.js)
│   ├── sensorEmulator/         ← Application source code
│   └── implementationPlan.md
│
├── webMock/                     ← Desktop web mock (Python / Flask)
│   ├── server.py               ← Emulates the Atom Lite HTTP + WebSocket interface
│   ├── requirements.txt
│   └── README.md
│
├── design/
│   ├── modbusSensorEmulator.md ← Full design plan
│   └── webMock.md               ← Web mock implementation plan
│
├── documentation/              ← Driver source and datasheet references
│   ├── code_FG6485A_driver/    ← FG6485A Modbus RTU driver (reference)
│   ├── code_S200_driver/       ← S200 Modbus RTU driver (reference)
│   ├── code_modbusTestClient/  ← Working Atom Lite RS485 slave example
│   ├── T-RH_ FG6485A/          ← FG6485A sensor notes
│   └── W-Sensecap-S200/        ← S200 sensor notes
│
├── README.md
├── LICENSE
├── license.md
├── changelog.md
├── contributing.md
└── code_of_conduct.md
```

## Getting Started

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git

### Web Mock (no hardware required)

A desktop mock server lets you develop and review the web UI without any hardware.
It serves the same `firmware/data/` assets and replicates all HTTP + WebSocket
endpoints with simulated sensor state.

```
cd webMock
pip install -r requirements.txt
python server.py
# Open http://127.0.0.1:5000
```

See [webMock/README.md](webMock/README.md) for full details.

### Build and Flash

1. Clone the repository:
   ```
   git clone https://github.com/pe1mew/greenhouse-Controller-Modbus-sensor-emulator.git
   cd greenhouse-Controller-Modbus-sensor-emulator/firmware
   ```
2. Open the `firmware/` folder in VS Code (PlatformIO will detect `platformio.ini` automatically).
3. Connect the Atom Lite via USB-C.
4. Click **Upload** in the PlatformIO toolbar (or run `pio run -t upload` in the terminal).
5. Open the **Serial Monitor** (115200 baud) to view startup diagnostics.

### First Use

On first boot the device starts as an open WiFi access point named `SensorEmulator-AABB` (AABB = last two bytes of the WiFi MAC address in hex). Connect to that AP and navigate to `http://192.168.4.1` to open the web interface and configure WiFi, time, and sensor modes.

Once connected to a WiFi network the web interface is also reachable at `http://emulator.local`.

## Design

See [design/modbusSensorEmulator.md](design/modbusSensorEmulator.md) for the full design plan, including FreeRTOS task architecture, Modbus register maps, NVS key table, CSV replay file format, WiFi/mDNS/NTP management, and the development sequence.

## License

See the [license.md](license.md) file for full details.

**Software** (firmware and all code): Source-available, non-commercial. Free to use and modify for personal/non-commercial purposes; redistribution and commercial use are not permitted.

**Documentation and design files**: Licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.

<a rel="license" href="https://creativecommons.org/licenses/by-nc-nd/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc-nd/4.0/88x31.png" /></a>

## Disclaimer

This project is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.