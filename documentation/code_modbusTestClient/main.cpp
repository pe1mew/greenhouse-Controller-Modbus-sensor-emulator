/**
 * Modbus RTU Test Client (Slave)
 *
 * Acts as a Modbus RTU slave that satisfies the greenhouse-Controller
 * hardware verification test (LIB-6 / HW-MB-001 through HW-MB-004).
 *
 * Responds to:
 *   Address 1  FC03 (Read Holding Registers)  regs 0-1 → {0x1234, 0x5678}
 *   Address 2  FC04 (Read Input Registers)    regs 0-1 → {0x00E6, 0x028F}
 *
 * Address 99 is silently ignored — no reply causes the master to time out,
 * which satisfies HW-MB-004.
 *
 * Any request for out-of-range registers gets a Modbus exception 0x02
 * (Illegal Data Address).  Unsupported function codes get exception 0x01
 * (Illegal Function).  Broadcast (address 0) is always ignored.
 *
 * Hardware:
 *   - M5Stack Atom Lite (ESP32-PICO-D4)
 *   - Atomic RS485 Base
 *     - G22: UART2 RX  ← RS485 RO
 *     - G19: UART2 TX  → RS485 DI (direction auto-controlled by HW transistor)
 *
 * Serial output : 115200 baud (USB)
 * RS485 baud    : 9600  (must match master)
 *
 * LED colours:
 *   Blue        — idle, listening
 *   Green flash — request received and answered successfully
 *   Yellow flash— request received but ignored (wrong address / broadcast)
 *   Red flash   — CRC error in received request
 */

#include <Arduino.h>
#include <FastLED.h>

// === Pin definitions ===
#define LED_PIN     27
#define NUM_LEDS    1
#define RS485_RX    22
#define RS485_TX    19

// === Bus settings ===
#define MODBUS_BAUD_RATE   9600
#define FRAME_TIMEOUT_MS   5      // Inter-character silence → end of frame

// === Slave identities ===
#define ADDR_HOLDING   1   // Responds to FC03 (holding registers)
#define ADDR_INPUT     2   // Responds to FC04 (input registers)

// === Register data — must match master's expected values ===
static const uint16_t holdingRegs[] = { 0x1234, 0x5678 };   // regs 0, 1
static const uint16_t inputRegs[]   = { 0x00E6, 0x028F };   // regs 0, 1
static const uint8_t  HOLDING_COUNT = sizeof(holdingRegs) / sizeof(holdingRegs[0]);
static const uint8_t  INPUT_COUNT   = sizeof(inputRegs)   / sizeof(inputRegs[0]);

CRGB leds[NUM_LEDS];

// ---------------------------------------------------------------------------
// CRC-16/IBM (Modbus RTU)
// ---------------------------------------------------------------------------
static uint16_t modbusCRC16(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// LED helpers
// ---------------------------------------------------------------------------
static void setLED(CRGB color)
{
    leds[0] = color;
    FastLED.show();
}

static void flashLED(CRGB color, uint16_t ms = 80)
{
    setLED(color);
    delay(ms);
    setLED(CRGB::Blue);
}

// ---------------------------------------------------------------------------
// Transmit a response frame and drain the RS485 echo from RX
// ---------------------------------------------------------------------------
static void sendResponse(const uint8_t *resp, uint8_t respLen)
{
    Serial2.write(resp, respLen);
    Serial2.flush();   // Wait until last bit is shifted out

    // The Atomic RS485 Base auto-enables TX — our own bytes echo back on RX.
    // Wait for all echo bytes to arrive then drain them exactly.
    // At 9600 baud 1 byte ≈ 1.04 ms; add generous margin.
    delay(respLen * 2 + 5);
    uint8_t avail = Serial2.available();
    uint8_t drain = (avail < respLen) ? avail : respLen;
    for (uint8_t i = 0; i < drain; i++) Serial2.read();
}

// ---------------------------------------------------------------------------
// Send a Modbus exception response
// ---------------------------------------------------------------------------
static void sendException(uint8_t addr, uint8_t fc, uint8_t exceptionCode)
{
    uint8_t resp[5];
    resp[0] = addr;
    resp[1] = fc | 0x80;
    resp[2] = exceptionCode;
    uint16_t crc = modbusCRC16(resp, 3);
    resp[3] = crc & 0xFF;
    resp[4] = crc >> 8;

    Serial.printf("   >> Exception 0x%02X for FC 0x%02X to addr %u\n",
                  exceptionCode, fc, addr);
    sendResponse(resp, 5);
}

// ---------------------------------------------------------------------------
// Send a FC03 / FC04 register read response
// ---------------------------------------------------------------------------
static void sendRegisterResponse(uint8_t addr, uint8_t fc,
                                  const uint16_t *regs, uint8_t regCount,
                                  uint16_t startReg, uint16_t quantity)
{
    // Validate range
    if (quantity == 0 || quantity > 125 || (startReg + quantity) > regCount) {
        sendException(addr, fc, 0x02);   // Illegal Data Address
        return;
    }

    // Response: [addr, fc, byteCount, data..., crcL, crcH]
    uint8_t byteCount = (uint8_t)(quantity * 2);
    uint8_t resp[256];
    resp[0] = addr;
    resp[1] = fc;
    resp[2] = byteCount;
    for (uint16_t i = 0; i < quantity; i++) {
        resp[3 + i * 2]     = (uint8_t)(regs[startReg + i] >> 8);
        resp[3 + i * 2 + 1] = (uint8_t)(regs[startReg + i] & 0xFF);
    }
    uint8_t frameLen = 3 + byteCount;
    uint16_t crc = modbusCRC16(resp, frameLen);
    resp[frameLen]     = crc & 0xFF;
    resp[frameLen + 1] = crc >> 8;

    sendResponse(resp, frameLen + 2);
}

// ---------------------------------------------------------------------------
// Process a complete received frame
// ---------------------------------------------------------------------------
static void processFrame(const uint8_t *buf, uint8_t len)
{
    if (len < 4) return;

    uint8_t  addr = buf[0];
    uint8_t  fc   = buf[1];

    // Validate CRC before doing anything else
    uint16_t rxCRC   = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    uint16_t calcCRC = modbusCRC16(buf, len - 2);
    if (rxCRC != calcCRC) {
        Serial.printf("[CRC ERROR] addr=%u fc=0x%02X rx=0x%04X calc=0x%04X — ignored\n",
                      addr, fc, rxCRC, calcCRC);
        flashLED(CRGB::Red, 150);
        return;
    }

    // Ignore broadcasts and addresses we don't serve
    if (addr == 0 || (addr != ADDR_HOLDING && addr != ADDR_INPUT)) {
        flashLED(CRGB::Yellow, 60);
        return;
    }

    // Standard read request must be 8 bytes: addr+fc+regH+regL+qtyH+qtyL+crcL+crcH
    if (len < 8) {
        sendException(addr, fc, 0x03);   // Illegal Data Value
        return;
    }

    uint16_t startReg = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t quantity = ((uint16_t)buf[4] << 8) | buf[5];

    Serial.printf(">> addr=%u  fc=0x%02X  startReg=%u  qty=%u\n",
                  addr, fc, startReg, quantity);

    if (addr == ADDR_HOLDING && fc == 0x03) {
        sendRegisterResponse(addr, fc, holdingRegs, HOLDING_COUNT, startReg, quantity);
        flashLED(CRGB::Green);
    } else if (addr == ADDR_INPUT && fc == 0x04) {
        sendRegisterResponse(addr, fc, inputRegs, INPUT_COUNT, startReg, quantity);
        flashLED(CRGB::Green);
    } else {
        // Wrong function code for this address
        sendException(addr, fc, 0x01);   // Illegal Function
        flashLED(CRGB::Yellow);
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(50);
    setLED(CRGB::Blue);

    Serial2.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RS485_RX, RS485_TX);

    delay(500);

    Serial.println();
    Serial.println("=================================================");
    Serial.println("  Modbus RTU Test Client (Slave)");
    Serial.println("  M5Stack Atom + Atomic RS485 Base");
    Serial.println("-------------------------------------------------");
    Serial.printf( "  Baud rate    : %d\n", MODBUS_BAUD_RATE);
    Serial.println("-------------------------------------------------");
    Serial.printf( "  Addr %-3u  FC03  Holding regs 0-1 : 0x%04X  0x%04X\n",
                   ADDR_HOLDING, holdingRegs[0], holdingRegs[1]);
    Serial.printf( "  Addr %-3u  FC04  Input   regs 0-1 : 0x%04X  0x%04X\n",
                   ADDR_INPUT,   inputRegs[0],   inputRegs[1]);
    Serial.println("-------------------------------------------------");
    Serial.println("  LED: Blue=idle  Green=answered  Red=CRC err");
    Serial.println("=================================================");
    Serial.println();
    Serial.println("Waiting for master requests...");
    Serial.println();
}

// ---------------------------------------------------------------------------
// Loop — collect bytes into frames, process each complete frame
// ---------------------------------------------------------------------------
void loop()
{
    static uint8_t  frameBuf[256];
    static uint8_t  frameLen     = 0;
    static uint32_t lastByteTime = 0;

    if (Serial2.available()) {
        frameBuf[frameLen++] = (uint8_t)Serial2.read();
        lastByteTime = millis();

        if (frameLen >= sizeof(frameBuf)) {
            // Buffer overflow — discard and restart
            Serial.println("[WARN] Frame buffer overflow — discarding");
            frameLen = 0;
        }
    } else if (frameLen > 0 && (millis() - lastByteTime) >= FRAME_TIMEOUT_MS) {
        processFrame(frameBuf, frameLen);
        frameLen = 0;
    }
}
