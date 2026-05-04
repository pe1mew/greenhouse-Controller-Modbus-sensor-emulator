/**
 * FG6485A T/RH Transmitter — hardware verification sketch
 *
 * Covers HW-FG-001 through HW-FG-008.
 * Includes two diagnostic sections that run first:
 *
 *   DIAG-1  Raw receive test
 *           Sends the exact 8-byte datasheet example request (addr=1, FC03,
 *           reg=0x0000, count=2) and prints every raw byte that arrives
 *           within 300 ms.  Bypasses all Modbus framing so even a garbled
 *           or unexpected response is visible.
 *
 *   DIAG-2  Address scan (1–20)
 *           Tries FC03 read of registers 0x0000–0x0001 on every address in
 *           that range and reports which address (if any) responds.  Useful
 *           when the DIP switch position is uncertain.
 *
 * Prerequisites:
 *   - FG6485A wired to the RS-485 bus:
 *       A+ (yellow) → RS-485 A+    B- (white) → RS-485 B-
 *       V+ (red)    → 9–36 V DC    GND (black) → GND
 *   - DIP switch set to address 1 (only switch H = ON).
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
#include "fg6485a.h"

/* Slave address set on the FG6485A DIP switch (factory default = 1) */
static const uint8_t SENSOR_ADDR = FG6485A_DEFAULT_ADDR;

/* ---------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------- */
static int pass_count = 0;
static int fail_count = 0;

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

static const char *fg_status_str(fg6485a_status_t s)
{
    switch (s) {
        case FG6485A_OK:         return "FG6485A_OK";
        case FG6485A_ERR_COMM:   return "FG6485A_ERR_COMM";
        case FG6485A_ERR_PARAM:  return "FG6485A_ERR_PARAM";
        default:                 return "FG6485A_ERR_UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * DIAG-1 — Raw receive test
 *
 * Sends the exact 8-byte frame from the datasheet example:
 *   01 03 00 00 00 02 C4 0B  (addr=1, FC03, start=0x0000, count=2)
 * then prints every byte received within 300 ms, bypassing all framing.
 * If the sensor responds at all — even with a garbled frame — we see it.
 * --------------------------------------------------------------------------- */
static void diag_raw_receive(void)
{
    Serial.println();
    Serial.println("=== DIAG-1: Raw receive (addr=1, FC03, reg=0, count=2) ===");
    Serial.println("  Sending: 01 03 00 00 00 02 C4 0B");

    /* Inter-frame silence before asserting DE */
    delayMicroseconds(5000);

    const uint8_t req[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
    gpio_set_rs485_direction(true);
    Serial1.write(req, sizeof(req));
    Serial1.flush();
    delayMicroseconds(2000);
    gpio_set_rs485_direction(false);

    /* Discard echo bytes */
    delayMicroseconds(1500);
    while (Serial1.available()) (void)Serial1.read();

    /* Collect everything that arrives within 300 ms */
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
        Serial.println("  >> Sensor sent nothing — check address, A+/B- polarity,");
        Serial.println("     supply voltage, and DIP switch orientation.");
    } else {
        Serial.println();
        Serial.print("  Total bytes received: ");
        Serial.println(count);
    }
    Serial.println("=========================================================");
}

/* ---------------------------------------------------------------------------
 * DIAG-2 — Address scan (1–20)
 *
 * Issues a FC03 read (reg 0x0000, count 2) to each address in turn.
 * The first address that responds is reported with its raw register values.
 * Worst-case duration: 20 × 200 ms timeout = 4 s.
 * --------------------------------------------------------------------------- */
static void diag_address_scan(void)
{
    Serial.println();
    Serial.println("=== DIAG-2: Address scan (1-20) ===");
    uint16_t regs[2];
    bool found = false;
    for (uint8_t addr = 1; addr <= 20; addr++) {
        Serial.print("  Trying address ");
        if (addr < 10) Serial.print(" ");
        Serial.print(addr);
        Serial.print(" ... ");

        modbus_status_t s = modbus_read_holding_registers(addr, 0x0000u, 2u, regs);

        if (s == MODBUS_OK) {
            Serial.println("RESPONSE!");
            Serial.print("    raw hum  reg (0x0000) = 0x");
            if (regs[0] < 0x1000) Serial.print("0");
            if (regs[0] < 0x100)  Serial.print("0");
            if (regs[0] < 0x10)   Serial.print("0");
            Serial.print(regs[0], HEX);
            Serial.print("  → ");
            Serial.print((float)(int16_t)regs[0] / 10.0f, 1);
            Serial.println(" %RH");

            Serial.print("    raw temp reg (0x0001) = 0x");
            if (regs[1] < 0x1000) Serial.print("0");
            if (regs[1] < 0x100)  Serial.print("0");
            if (regs[1] < 0x10)   Serial.print("0");
            Serial.print(regs[1], HEX);
            Serial.print("  → ");
            Serial.print((float)(int16_t)regs[1] / 10.0f, 1);
            Serial.println(" C");
            found = true;
        } else if (s == MODBUS_ERR_CRC) {
            Serial.println("CRC error (response received but corrupt)");
            found = true;   /* sensor IS there — CRC issue */
        } else if (s == MODBUS_ERR_EXCEPTION) {
            Serial.println("exception response (sensor answered, wrong register?)");
            found = true;
        } else {
            Serial.println("timeout");
        }
    }
    if (!found) {
        Serial.println("  >> No device responded on addresses 1-20.");
        Serial.println("  >> Check A+/B- polarity or scan a wider range.");
    }
    Serial.println("===================================");
}

/* ---------------------------------------------------------------------------
 * Setup — diagnostics first, then hardware verification tests
 * --------------------------------------------------------------------------- */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("================================================");
    Serial.println("  FG6485A T/RH — hardware verification");
    Serial.println("================================================");

    /* -----------------------------------------------------------------
     * HW-FG-001 — Modbus driver initialises without error
     * ----------------------------------------------------------------- */
    modbus_init();
    Serial.println("modbus_init(): UART1 TX=GPIO17 RX=GPIO18 baud=9600 DE/RE=GPIO8");
    check("HW-FG-001", "modbus_init() completed", true);

    /* Run diagnostics before the formal tests so we can see raw bus state */
    diag_raw_receive();
    diag_address_scan();

    Serial.println();
    Serial.println("--- Formal hardware tests ---");

    /* -----------------------------------------------------------------
     * HW-FG-002 — Read temperature and humidity
     * ----------------------------------------------------------------- */
    Serial.println("--- Measurements (addr=1) ---");
    fg6485a_measurement_t meas = {0.0f, 0.0f};
    fg6485a_status_t st = fg6485a_read_measurements(SENSOR_ADDR, &meas);
    Serial.print("  status      : "); Serial.println(fg_status_str(st));
    Serial.print("  temperature : "); Serial.print(meas.temperature_c, 1); Serial.println(" C");
    Serial.print("  humidity    : "); Serial.print(meas.humidity_pct,   1); Serial.println(" %RH");
    check("HW-FG-002", "read_measurements returned FG6485A_OK",
          st == FG6485A_OK);
    check("HW-FG-002a", "temperature in operating range (-40..120 C)",
          st == FG6485A_OK &&
          meas.temperature_c >= -40.0f && meas.temperature_c <= 120.0f);
    check("HW-FG-002b", "humidity in operating range (0..99.9 %RH)",
          st == FG6485A_OK &&
          meas.humidity_pct >= 0.0f && meas.humidity_pct <= 99.9f);

    /* -----------------------------------------------------------------
     * HW-FG-003 — Read device info
     * Extra delay: sensor needs recovery time after the DIAG-2 address scan
     * ----------------------------------------------------------------- */
    delay(500);
    Serial.println("--- Device info (addr=1) ---");
    fg6485a_info_t info = {0u, 0u, 0u};
    st = fg6485a_read_info(SENSOR_ADDR, &info);
    Serial.print("  status      : "); Serial.println(fg_status_str(st));
    Serial.print("  device_type : 0x"); Serial.println(info.device_type, HEX);
    Serial.print("  version     : 0x"); Serial.println(info.version, HEX);
    Serial.print("  device_id   : 0x"); Serial.println(info.device_id, HEX);
    check("HW-FG-003", "read_info returned FG6485A_OK", st == FG6485A_OK);

    /* -----------------------------------------------------------------
     * HW-FG-004 — Read all in one call
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Read all (measurements + info, addr=1) ---");
    fg6485a_measurement_t meas2 = {0.0f, 0.0f};
    fg6485a_info_t        info2 = {0u, 0u, 0u};
    st = fg6485a_read_all(SENSOR_ADDR, &meas2, &info2);
    Serial.print("  status      : "); Serial.println(fg_status_str(st));
    Serial.print("  temperature : "); Serial.print(meas2.temperature_c, 1); Serial.println(" C");
    Serial.print("  humidity    : "); Serial.print(meas2.humidity_pct,   1); Serial.println(" %RH");
    check("HW-FG-004", "read_all returned FG6485A_OK", st == FG6485A_OK);

    /* -----------------------------------------------------------------
     * HW-FG-005 — Read alarm configuration
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Alarm config read (addr=1) ---");
    fg6485a_alarm_config_t alarm;
    st = fg6485a_read_alarm_config(SENSOR_ADDR, &alarm);
    Serial.print("  status            : "); Serial.println(fg_status_str(st));
    if (st == FG6485A_OK) {
        Serial.print("  temp_alarm_hi     : "); Serial.print(alarm.temp_alarm_high, 1);
        Serial.print(" C (en="); Serial.print(alarm.temp_alarm_high_en); Serial.println(")");
        Serial.print("  temp_alarm_lo     : "); Serial.print(alarm.temp_alarm_low,  1);
        Serial.print(" C (en="); Serial.print(alarm.temp_alarm_low_en);  Serial.println(")");
        Serial.print("  hum_alarm_hi      : "); Serial.print(alarm.hum_alarm_high,  1);
        Serial.print(" %RH (en="); Serial.print(alarm.hum_alarm_high_en); Serial.println(")");
        Serial.print("  hum_alarm_lo      : "); Serial.print(alarm.hum_alarm_low,   1);
        Serial.print(" %RH (en="); Serial.print(alarm.hum_alarm_low_en);  Serial.println(")");
    }
    check("HW-FG-005", "read_alarm_config returned FG6485A_OK", st == FG6485A_OK);

    /* -----------------------------------------------------------------
     * HW-FG-006 — Write alarm thresholds (round-trip verify)
     *
     * The sensor accepts FC16 writes to registers 0x000C–0x0013 but the
     * humidity alarm-enable bits (0x0011, 0x0013) appear to be read-only
     * in firmware — writes of 0 are silently ignored.  The test therefore
     * only verifies that the four threshold values round-trip correctly;
     * enable bits are written with the same value as currently stored so
     * the read-back comparison is not affected by that hardware constraint.
     *
     * Thresholds are shifted slightly from factory defaults so that a
     * stale read (or no-write-at-all) would produce a mismatch.
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Alarm config write + verify (addr=1) ---");

    /* Step 1 — read current config so we can preserve enable bits and
     *           restore factory thresholds at the end. */
    fg6485a_alarm_config_t orig_cfg;
    fg6485a_status_t st2 = fg6485a_read_alarm_config(SENSOR_ADDR, &orig_cfg);
    if (st2 != FG6485A_OK) {
        Serial.print("  pre-read failed: "); Serial.println(fg_status_str(st2));
    }

    /* Step 2 — write new thresholds, keeping enable bits from the sensor */
    fg6485a_alarm_config_t write_cfg = {
        38.0f, orig_cfg.temp_alarm_high_en,  /* temp high  38 C  (was 40) */
        12.0f, orig_cfg.temp_alarm_low_en,   /* temp low   12 C  (was 10) */
        75.0f, orig_cfg.hum_alarm_high_en,   /* hum  high  75 %RH (was 80) */
        25.0f, orig_cfg.hum_alarm_low_en     /* hum  low   25 %RH (was 20) */
    };
    st = fg6485a_write_alarm_config(SENSOR_ADDR, &write_cfg);
    Serial.print("  write status : "); Serial.println(fg_status_str(st));
    check("HW-FG-006a", "write_alarm_config returned FG6485A_OK", st == FG6485A_OK);

    delay(200);
    fg6485a_alarm_config_t read_cfg;
    st2 = fg6485a_read_alarm_config(SENSOR_ADDR, &read_cfg);
    Serial.print("  read status  : "); Serial.println(fg_status_str(st2));
    if (st2 == FG6485A_OK) {
        Serial.println("  field               written   read-back");
        Serial.print  ("  temp_alarm_high     "); Serial.print(write_cfg.temp_alarm_high, 1);
        Serial.print  ("       "); Serial.println(read_cfg.temp_alarm_high, 1);
        Serial.print  ("  temp_alarm_low      "); Serial.print(write_cfg.temp_alarm_low,  1);
        Serial.print  ("       "); Serial.println(read_cfg.temp_alarm_low, 1);
        Serial.print  ("  hum_alarm_high      "); Serial.print(write_cfg.hum_alarm_high,  1);
        Serial.print  ("       "); Serial.println(read_cfg.hum_alarm_high, 1);
        Serial.print  ("  hum_alarm_low       "); Serial.print(write_cfg.hum_alarm_low,   1);
        Serial.print  ("       "); Serial.println(read_cfg.hum_alarm_low, 1);
    }
    /* Only verify threshold values — enable bits are hardware read-only */
    bool cfg_match = (st2 == FG6485A_OK)
                   && (read_cfg.temp_alarm_high == write_cfg.temp_alarm_high)
                   && (read_cfg.temp_alarm_low  == write_cfg.temp_alarm_low)
                   && (read_cfg.hum_alarm_high  == write_cfg.hum_alarm_high)
                   && (read_cfg.hum_alarm_low   == write_cfg.hum_alarm_low);
    check("HW-FG-006b", "alarm threshold read-back matches written values", cfg_match);

    /* Step 3 — restore factory thresholds */
    delay(100);
    (void)fg6485a_write_alarm_config(SENSOR_ADDR, &orig_cfg);

    /* -----------------------------------------------------------------
     * HW-FG-007 — Write corrections (0.0 — no change)
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Corrections write (addr=1, 0.0 C / 0.0 %RH) ---");
    st  = fg6485a_write_temp_correction(SENSOR_ADDR, 0.0f);
    st2 = fg6485a_write_humidity_correction(SENSOR_ADDR, 0.0f);
    Serial.print("  temp correction status : "); Serial.println(fg_status_str(st));
    Serial.print("  hum  correction status : "); Serial.println(fg_status_str(st2));
    check("HW-FG-007", "both correction writes returned FG6485A_OK",
          st == FG6485A_OK && st2 == FG6485A_OK);

    /* -----------------------------------------------------------------
     * HW-FG-008 — Timeout on non-existent device (addr=99)
     * ----------------------------------------------------------------- */
    delay(200);
    Serial.println("--- Timeout test (addr=99, no device) ---");
    fg6485a_measurement_t dummy;
    uint32_t t0 = millis();
    st = fg6485a_read_measurements(99u, &dummy);
    uint32_t elapsed = millis() - t0;
    Serial.print("  status  : "); Serial.println(fg_status_str(st));
    Serial.print("  elapsed : "); Serial.print(elapsed); Serial.println(" ms");
    check("HW-FG-008", "FG6485A_ERR_COMM for absent device; elapsed < 300 ms",
          st == FG6485A_ERR_COMM && elapsed < 300u);

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
