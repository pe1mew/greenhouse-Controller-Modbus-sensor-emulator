# modbusTestClient

A Modbus RTU slave that satisfies the greenhouse-Controller hardware verification test suite (LIB-6 / HW-MB-001 through HW-MB-004). Connect it to the greenhouse-Controller board over RS485 and it will reply with the exact register values the test expects.

## Purpose

Replaces a real sensor network during hardware bring-up. The test master (running on the greenhouse-Controller board, UART1 TX=GPIO17 RX=GPIO18 DE/RE=GPIO8 at 9600 baud) sends three probes; this slave answers the first two and stays silent for the third, making all four test cases pass.

## Hardware

| Component | Detail |
|---|---|
| Board | M5Stack Atom Lite (ESP32-PICO-D4) |
| RS485 adapter | Atomic RS485 Base |
| RS485 RX | GPIO 22 (UART2 RX ← RS485 RO) |
| RS485 TX | GPIO 19 (UART2 TX → RS485 DI) |
| Direction control | Hardware transistor on RS485 Base (automatic) |
| LED | WS2812B on GPIO 27 |

## Register map

| Address | Function | Registers | Values |
|---|---|---|---|
| 1 | FC03 Read Holding Registers | 0, 1 | `0x1234`, `0x5678` |
| 2 | FC04 Read Input Registers | 0, 1 | `0x00E6`, `0x028F` |

Address 99 is silently ignored — the master receives a timeout, satisfying HW-MB-004.

## Behaviour

- Collects bytes with a 5 ms inter-character silence timeout to detect frame boundaries.
- Validates CRC before acting on any request.
- Broadcasts (address 0) and unknown addresses are silently ignored.
- Out-of-range register requests receive Modbus exception 0x02 (Illegal Data Address).
- Unsupported function codes receive Modbus exception 0x01 (Illegal Function).
- After each reply the RS485 echo bytes are drained from the RX buffer.

### LED colours

| Colour | Meaning |
|---|---|
| Blue | Idle, listening |
| Green flash | Request answered successfully |
| Yellow flash | Request ignored (unknown address or wrong FC for address) |
| Red flash | CRC error in received request |

## Serial output example

```
=================================================
  Modbus RTU Test Client (Slave)
  M5Stack Atom + Atomic RS485 Base
-------------------------------------------------
  Baud rate    : 9600
-------------------------------------------------
  Addr 1    FC03  Holding regs 0-1 : 0x1234  0x5678
  Addr 2    FC04  Input   regs 0-1 : 0x00E6  0x028F
-------------------------------------------------
  LED: Blue=idle  Green=answered  Red=CRC err
=================================================

Waiting for master requests...

>> addr=1  fc=0x03  startReg=0  qty=2
>> addr=2  fc=0x04  startReg=0  qty=2
```

## Build & Flash

```
pio run -e modbusTestClient --target upload
pio device monitor -e modbusTestClient
```

## Dependencies

- [FastLED](https://github.com/FastLED/FastLED)
