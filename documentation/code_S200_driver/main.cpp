/**
 * S200 Wind Sensor — hardware verification sketch
 *
 * Covers HW-S200-001 through HW-S200-004.
 * Two diagnostic sections run first:
 *
 *   DIAG-1  Raw receive test
 *           Sends the exact 8-byte FC04 request (addr=44, FC04, reg=0x0008,
 *           count=6) and prints every raw byte that arrives within 300 ms.
 *           Bypasses all Modbus framing so even a garbled response is visible.
 *
 *   DIAG-2  Address scan (1–60)
 *           Tries FC04 read of registers 0x0008–0x0013 on every address in
 *           that range and reports which address (if any) responds.  Useful
 *           when the default address (44) has been changed.
 *
 * Prerequisites:
 *   - S200 wired to the RS-485 bus:
 *       Brown (A+) → RS-485 A+    White (B−) → RS-485 B−
 *       Red         → 9–30 V DC    Black       → GND
 *   - Sensor at Modbus address 44 (factory default).
 *   - SIT65HVD08P transceiver:
 *       DI  → GPIO 17   (UART1 TX, PIN_RS485_TX)
 *       RO  → GPIO 18   (UART1 RX, PIN_RS485_RX)
 *       DE+RE → GPIO 8  (PIN_RS485_DE_RE)
 *   - Serial monitor at 115200 baud on UART0 (GPIO 43 TX / 44 RX).
 *
 * Build and upload:  pio run -e lolin_s3 --target upload
 * Read output:       pio device monitor  (or open COM port at 115200)
 *
 * @author Greenhouse Controller project
 * @version 0.1.0
 */

#include <Arduino.h>
#include "gpio_util.h"
#include "modbus_rtu.h"
#include "s200.h"

static const uint8_t SENSOR_ADDR = S200_DEFAULT_ADDR;  /* 44 */

/* ---------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------- */
static int pass_count = 0;
static int fail_count = 0;

/**
 * @brief Record a pass/fail test result and print it to the serial console.
 *
 * Increments pass_count or fail_count, then prints
 * @c "[PASS] id: description" or @c "[FAIL] id: description".
 *
 * @param id          Short test identifier (e.g. "HW-S200-001").
 * @param description Human-readable description of what is being checked.
 * @param condition   @c true → pass, @c false → fail.
 */
static void check(const char *id, const char *description, bool condition)
{
    if (condition) {
        Serial.print("[PASS] ");
        pass_count++;
    } else {
        Serial.print("[FAIL] ");
        fail_count++;
    }
    Serial.print(id);
    Serial.print(": ");
    Serial.println(description);
}

/**
 * @brief Convert an s200_status_t code to a human-readable string.
 *
 * @param s  Status code returned by an s200_* API function.
 * @return   Pointer to a string literal describing the status.
 */
static const char *s200_status_str(s200_status_t s)
{
    switch (s) {
        case S200_OK:         return "S200_OK";
        case S200_ERR_COMM:   return "S200_ERR_COMM";
        case S200_ERR_PARAM:  return "S200_ERR_PARAM";
        default:              return "S200_ERR_UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * DIAG-1 — Raw receive test
 *
 * Sends an FC04 request for 6 registers starting at 0x0008 (wind direction
 * min/max/avg) addressed to the default slave (44 = 0x2C).
 * Frame: 2C 04 00 08 00 06 <CRC-low> <CRC-high>
 * CRC for that payload: 2C 04 00 08 00 06 → CRC = 0x57E1 (little-endian: E1 57)
 * --------------------------------------------------------------------------- */

/**
 * @brief Send the hardcoded FC04 request frame and dump all raw bytes received.
 *
 * Bypasses all Modbus framing so even a garbled response is visible.
 * Useful as a first-pass wiring and polarity check before running formal tests.
 * Prints nothing (or "(none)") if the sensor does not respond within 300 ms.
 */
static void diag_raw_receive(void)
{
    Serial.println();
    Serial.println("=== DIAG-1: Raw receive (addr=44, FC04, reg=0x0008, count=6) ===");
    Serial.println("  Sending: 2C 04 00 08 00 06 E1 57");

    delayMicroseconds(5000);

    const uint8_t req[] = {0x2C, 0x04, 0x00, 0x08, 0x00, 0x06, 0xE1, 0x57};
    gpio_set_rs485_direction(true);
    Serial1.write(req, sizeof(req));
    Serial1.flush();
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);

    /* Discard echo bytes */
    delayMicroseconds(1500);
    while (Serial1.available()) (void)Serial1.read();

    Serial.print("  Response bytes:");
    uint32_t t0    = millis();
    int      count = 0;
    while (millis() - t0 < 300u) {
        if (Serial1.available()) {
            uint8_t b = (uint8_t)Serial1.read();
            Serial.print(" ");
            if (b < 0x10) Serial.print("0");
            Serial.print(b, HEX);
            count++;
        }
    }
    if (count == 0) {
        Serial.println(" (none)");
        Serial.println("  >> Sensor sent nothing — check address, A+/B− polarity,");
        Serial.println("     supply voltage, and cable connections.");
    } else {
        Serial.println();
        Serial.print("  Total bytes received: ");
        Serial.println(count);
    }
    Serial.println("=================================================================");
}

/* ---------------------------------------------------------------------------
 * DIAG-2 — Address scan (1–60)
 *
 * Issues an FC04 read (reg 0x0008, count 6) to each address in turn.
 * The first address that responds is reported with raw register values.
 * --------------------------------------------------------------------------- */

/**
 * @brief Scan Modbus addresses 1–60 and report the first one that responds.
 *
 * Issues an FC04 read of registers 0x0008–0x000D (wind direction fields) to
 * each address and prints the raw word pairs for the first responding device.
 * Useful when the factory default address (44) has been changed.
 */
static void diag_address_scan(void)
{
    Serial.println();
    Serial.println("=== DIAG-2: Address scan (1-60) ===");
    uint16_t regs[6];
    bool found = false;
    for (uint8_t addr = 1; addr <= 60; addr++) {
        Serial.print("  Trying address ");
        if (addr < 10)  Serial.print(" ");
        if (addr < 100) Serial.print(" ");
        Serial.print(addr);
        Serial.print(" ... ");

        modbus_status_t s = modbus_read_input_registers(addr, 0x0008u, 6u, regs);

        if (s == MODBUS_OK) {
            Serial.println("RESPONSE!");
            /* Wind direction channels (pairs of uint16 → int32 / 1000) */
            auto print_pair = [](const char *label, const uint16_t *r, const char *unit) {
                int32_t raw = (int32_t)(((uint32_t)r[0] << 16) | r[1]);
                Serial.print("    ");
                Serial.print(label);
                Serial.print(": ");
                Serial.print(raw / 1000.0f, 3);
                Serial.println(unit);
            };
            print_pair("wind_dir_min", &regs[0], " deg");
            print_pair("wind_dir_max", &regs[2], " deg");
            print_pair("wind_dir_avg", &regs[4], " deg");
            found = true;
        } else if (s == MODBUS_ERR_CRC) {
            Serial.println("CRC error (response received but corrupt)");
            found = true;
        } else if (s == MODBUS_ERR_EXCEPTION) {
            Serial.println("exception response (sensor answered)");
            found = true;
        } else {
            Serial.println("timeout");
        }
        if (found) break;
    }
    if (!found) {
        Serial.println("  >> No device responded on addresses 1-60.");
        Serial.println("  >> Check A+/B− polarity or scan a wider range.");
    }
    Serial.println("====================================");
}

/* ---------------------------------------------------------------------------
 * Setup — diagnostics first, then formal hardware verification tests
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("================================================");
    Serial.println("  SenseCAP S200 Wind Sensor — hardware verification");
    Serial.println("================================================");

    /* -----------------------------------------------------------------
     * HW-S200-001 — Modbus driver initialises without error
     * ----------------------------------------------------------------- */
    modbus_init();
    Serial.println("modbus_init(): UART1 TX=GPIO17 RX=GPIO18 baud=9600 DE/RE=GPIO8");
    check("HW-S200-001", "modbus_init() completed", true);

    diag_raw_receive();
    diag_address_scan();

    Serial.println();
    Serial.println("--- Formal hardware tests ---");

    /* -----------------------------------------------------------------
     * HW-S200-002 — Read all measurements
     * ----------------------------------------------------------------- */
    Serial.println("--- Measurements (addr=44) ---");
    s200_measurement_t meas = {};
    s200_status_t st = s200_read_measurements(SENSOR_ADDR, &meas);
    Serial.print("  status                : "); Serial.println(s200_status_str(st));
    Serial.print("  wind_dir_min          : "); Serial.print(meas.wind_dir_min_deg, 3);    Serial.println(" deg");
    Serial.print("  wind_dir_max          : "); Serial.print(meas.wind_dir_max_deg, 3);    Serial.println(" deg");
    Serial.print("  wind_dir_avg          : "); Serial.print(meas.wind_dir_avg_deg, 3);    Serial.println(" deg");
    Serial.print("  wind_speed_min        : "); Serial.print(meas.wind_speed_min_ms, 3);   Serial.println(" m/s");
    Serial.print("  wind_speed_max        : "); Serial.print(meas.wind_speed_max_ms, 3);   Serial.println(" m/s");
    Serial.print("  wind_speed_avg        : "); Serial.print(meas.wind_speed_avg_ms, 3);   Serial.println(" m/s");
    Serial.print("  heating_temperature   : "); Serial.print(meas.heating_temperature_c, 3); Serial.println(" C");
    check("HW-S200-002", "s200_read_measurements returned S200_OK", st == S200_OK);

    /* -----------------------------------------------------------------
     * HW-S200-003 — Values in physical range
     * ----------------------------------------------------------------- */
    if (st == S200_OK) {
        check("HW-S200-003a", "wind direction (min) in 0–360 deg",
              meas.wind_dir_min_deg >= 0.0f && meas.wind_dir_min_deg <= 360.0f);
        check("HW-S200-003b", "wind direction (max) in 0–360 deg",
              meas.wind_dir_max_deg >= 0.0f && meas.wind_dir_max_deg <= 360.0f);
        check("HW-S200-003c", "wind direction (avg) in 0–360 deg",
              meas.wind_dir_avg_deg >= 0.0f && meas.wind_dir_avg_deg <= 360.0f);
        check("HW-S200-003d", "wind speed (min) in 0–60 m/s",
              meas.wind_speed_min_ms >= 0.0f && meas.wind_speed_min_ms <= 60.0f);
        check("HW-S200-003e", "wind speed (max) in 0–60 m/s",
              meas.wind_speed_max_ms >= 0.0f && meas.wind_speed_max_ms <= 60.0f);
        check("HW-S200-003f", "wind speed (avg) in 0–60 m/s",
              meas.wind_speed_avg_ms >= 0.0f && meas.wind_speed_avg_ms <= 60.0f);
        check("HW-S200-003g", "heating temperature in -40..85 C",
              meas.heating_temperature_c >= -40.0f && meas.heating_temperature_c <= 85.0f);
    } else {
        check("HW-S200-003", "skipped (read_measurements failed)", false);
    }

    /* -----------------------------------------------------------------
     * HW-S200-004 — Timeout on non-existent device (addr=99)
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Timeout test (addr=99, no device) ---");
    s200_measurement_t dummy = {};
    uint32_t t0 = millis();
    st = s200_read_measurements(99u, &dummy);
    uint32_t elapsed = millis() - t0;
    Serial.print("  status  : "); Serial.println(s200_status_str(st));
    Serial.print("  elapsed : "); Serial.print(elapsed); Serial.println(" ms");
    check("HW-S200-004", "S200_ERR_COMM for absent device; elapsed < 300 ms",
          st == S200_ERR_COMM && elapsed < 300u);

    /* -----------------------------------------------------------------
     * Summary
     * ----------------------------------------------------------------- */
    Serial.println("================================================");
    Serial.print("  PASSED: "); Serial.println(pass_count);
    Serial.print("  FAILED: "); Serial.println(fail_count);
    Serial.println(fail_count == 0 ? "  RESULT: PASS" : "  RESULT: FAIL");
    Serial.println("================================================");
    Serial.println("Entering idle loop.");
}

void loop()
{
    delay(1000);
}
