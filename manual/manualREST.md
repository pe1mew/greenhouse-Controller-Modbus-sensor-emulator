# REST API Mode — User Manual

## Table of Contents

1. [Overview](#1-overview)
2. [Prerequisites](#2-prerequisites)
3. [Enabling REST Mode](#3-enabling-rest-mode)
   - 3.1 [Via the web interface](#31-via-the-web-interface)
   - 3.2 [Via the sensor POST endpoint](#32-via-the-sensor-post-endpoint)
4. [The Data Endpoint](#4-the-data-endpoint)
   - 4.1 [Request format](#41-request-format)
   - 4.2 [Field reference](#42-field-reference)
   - 4.3 [Physical ranges](#43-physical-ranges)
5. [Response Format](#5-response-format)
   - 5.1 [Field status values](#51-field-status-values)
   - 5.2 [Full response schema](#52-full-response-schema)
6. [Behaviour Details](#6-behaviour-details)
   - 6.1 [Partial updates](#61-partial-updates)
   - 6.2 [Range checking and rejection](#62-range-checking-and-rejection)
   - 6.3 [Mode guard](#63-mode-guard)
   - 6.4 [Independent sensor control](#64-independent-sensor-control)
   - 6.5 [Value persistence](#65-value-persistence)
7. [HTTP Examples](#7-http-examples)
   - 7.1 [curl — full update](#71-curl--full-update)
   - 7.2 [curl — temperature and humidity only](#72-curl--temperature-and-humidity-only)
   - 7.3 [curl — wind station only](#73-curl--wind-station-only)
   - 7.4 [curl — out-of-range rejection](#74-curl--out-of-range-rejection)
   - 7.5 [curl — mixed mode (S200 not in REST)](#75-curl--mixed-mode-s200-not-in-rest)
   - 7.6 [Python — periodic push loop](#76-python--periodic-push-loop)
   - 7.7 [PowerShell — single push](#77-powershell--single-push)
   - 7.8 [Node.js — event-driven push](#78-nodejs--event-driven-push)
8. [Switching Away From REST Mode](#8-switching-away-from-rest-mode)
9. [Integration with the Web Interface](#9-integration-with-the-web-interface)
10. [Limitations](#10-limitations)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. Overview

REST mode is a fourth operating mode for each sensor (alongside Manual, Live,
and Replay).  When a sensor is placed in REST mode an external system can push
live measurement values to the emulator via a single HTTP POST endpoint.  The
emulator immediately makes those values available on the Modbus RS-485 bus.

**Key characteristics:**

- Any HTTP client (curl, Python, Node.js, PLC, SCADA, etc.) that can send a
  JSON POST request can supply values.
- The JSON body contains named fields — `T`, `RH`, `Direction`, `Speed`,
  `Heating`.  Any subset may be sent; missing fields are simply ignored.
- Each submitted field is independently range-checked.  Fields that pass are
  applied; fields outside the physical sensor range are rejected with
  diagnostic information in the response.
- The FG6485A and S200 sensors are controlled independently.  You can put only
  one sensor in REST mode while the other remains in Manual, Live, or Replay.
- Values are held until a new POST overwrites them or the mode is changed.
  There is no automatic timeout — if no new data arrives the Modbus bus
  continues to serve the last injected values.
- The endpoint path is `POST /api/data` and is always available over HTTP on
  port 80, regardless of which Wi-Fi mode the device is in (AP or STA).

---

## 2. Prerequisites

| Requirement | Notes |
|:---|:---|
| Device reachable over IP | Connect to the emulator's AP (`SensorEmulator` SSID) or to the same LAN if the device is in STA mode. |
| Know the device IP | AP mode: always `192.168.4.1`. STA mode: shown in the web UI Status card or via your router's DHCP table. mDNS: `http://sensor-emulator.local` (if your OS supports mDNS). |
| Sensor(s) set to REST mode | Must be done before pushing data — see [Section 3](#3-enabling-rest-mode). |
| HTTP client | `curl`, `wget`, Python `requests`, Node.js `fetch`, PowerShell `Invoke-RestMethod`, or any equivalent. |

---

## 3. Enabling REST Mode

### 3.1 Via the web interface

1. Open `http://<device-ip>/` in a browser.
2. In the **FG6485A — Temperature & Humidity** section, locate the **Mode**
   row and click the **REST** radio button.
3. In the **S200 — Wind & Heating** section, do the same if you also want to
   push wind and heating values via REST.
4. The mode is saved to NVS immediately and survives a power cycle.

> **Note:** The manual-values sliders are automatically disabled while a sensor
> is in REST mode.  They become active again when you switch back to Manual.

### 3.2 Via the sensor POST endpoint

You can also switch mode programmatically by posting to `/config/sensor`.
Mode value `3` maps to REST.

```http
POST /config/sensor HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{"sensor":"fg6485a","mode":3}
```

```http
POST /config/sensor HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{"sensor":"s200","mode":3}
```

With curl:

```bash
# Set FG6485A to REST mode
curl -s -X POST http://192.168.4.1/config/sensor \
  -H "Content-Type: application/json" \
  -d '{"sensor":"fg6485a","mode":3}'

# Set S200 to REST mode
curl -s -X POST http://192.168.4.1/config/sensor \
  -H "Content-Type: application/json" \
  -d '{"sensor":"s200","mode":3}'
```

---

## 4. The Data Endpoint

```
POST /api/data
Content-Type: application/json
```

### 4.1 Request format

The request body is a JSON object.  All fields are optional; include only the
ones you want to update.

```json
{
  "T":         <float>,
  "RH":        <float>,
  "Direction": <float>,
  "Speed":     <float>,
  "Heating":   <float>
}
```

Field names are **case-sensitive**.

### 4.2 Field reference

| Field | Sensor | Physical quantity | Unit |
|:---|:---|:---|:---|
| `T` | FG6485A | Air temperature | °C |
| `RH` | FG6485A | Relative humidity | %RH |
| `Direction` | S200 | Wind direction | ° (0–360) |
| `Speed` | S200 | Wind speed | m/s |
| `Heating` | S200 | Heating element temperature | °C |

### 4.3 Physical ranges

Values outside these ranges are **rejected** — they are not applied and the
response will indicate `"rejected"` for that field.

| Field | Minimum | Maximum | Resolution |
|:---|---:|---:|:---|
| `T` | −40.0 °C | 120.0 °C | 0.1 °C |
| `RH` | 0.0 %RH | 99.9 %RH | 0.1 %RH |
| `Direction` | 0.0 ° | 360.0 ° | 0.001 ° |
| `Speed` | 0.0 m/s | 60.0 m/s | 0.001 m/s |
| `Heating` | −40.0 °C | 85.0 °C | 0.001 °C |

> **Encoding note:** Internally FG6485A values are stored ×10 (int16) and
> S200 values are stored ×1000 (int32).  The API always accepts and returns
> engineering-unit floats — no scaling is required on the caller side.

---

## 5. Response Format

The response is a JSON object with one key per field that was **present** in
the request.  Fields that were absent from the request are absent from the
response.

```json
{
  "<field>": {
    "status": "accepted" | "rejected" | "skipped",
    ...
  }
}
```

HTTP status is always `200 OK` as long as the body is valid JSON.  A `400 Bad
Request` is returned only if the body cannot be parsed at all.

### 5.1 Field status values

| Status | Meaning | Extra keys in the object |
|:---|:---|:---|
| `accepted` | Value was within range and the sensor was in REST mode — applied. | `value` (number): the engineering-unit value that was actually stored (after raw-encode round-trip). |
| `rejected` | Value was outside the physical range — **not** applied. | `reason` (`"out_of_range"`), `min` (number), `max` (number). |
| `skipped` | The target sensor was **not** in REST mode — **not** applied. | `reason` (`"not_rest_mode"`). |

### 5.2 Full response schema

```json
{
  "T": {
    "status": "accepted",
    "value": 22.5
  },
  "RH": {
    "status": "rejected",
    "reason": "out_of_range",
    "min": 0.0,
    "max": 99.9
  },
  "Direction": {
    "status": "accepted",
    "value": 270.0
  },
  "Speed": {
    "status": "accepted",
    "value": 3.2
  },
  "Heating": {
    "status": "skipped",
    "reason": "not_rest_mode"
  }
}
```

---

## 6. Behaviour Details

### 6.1 Partial updates

You can send any subset of fields in a single request.  Fields not included in
the request are not touched — the Modbus registers keep whatever value they had
before.

```json
{"Speed": 5.0}
```

Only `Speed` is updated.  `Direction` and `Heating` keep their previous
values.

### 6.2 Range checking and rejection

Range checking happens **before** the value is written.  If a value is outside
the allowed range, the register is **not modified** and the response reports
`"rejected"` with the allowed `min` and `max`.  No clamping is performed — you
receive an exact report of what was refused and why.

### 6.3 Mode guard

Each FG6485A field (`T`, `RH`) is only applied when the **FG6485A mode** is
`REST`.  Each S200 field (`Direction`, `Speed`, `Heating`) is only applied when
the **S200 mode** is `REST`.

If a sensor is in any other mode the affected fields are reported as `"skipped"`
with `"reason": "not_rest_mode"`.  This lets a single external system send a
full weather payload at all times; the emulator itself decides which values to
apply based on the configured mode.

### 6.4 Independent sensor control

The two sensors are completely independent:

- FG6485A in REST, S200 in Manual → `T` and `RH` are accepted; `Direction`,
  `Speed`, `Heating` are skipped.
- FG6485A in Live, S200 in REST → `T` and `RH` are skipped; S200 fields
  are accepted.
- Both in REST → all five fields are accepted (subject to range).

### 6.5 Value persistence

Accepted values stay on the Modbus bus until one of the following happens:

- A new `POST /api/data` with updated values is received.
- The sensor is switched to a different mode (Manual, Live, or Replay), which
  may overwrite the registers.
- The device is rebooted (registers revert to the NVS-persisted Manual values
  on startup; REST mode injects no value until the first POST arrives).

---

## 7. HTTP Examples

All examples below assume the device is reachable at `192.168.4.1` (AP mode).
Replace with the actual IP or `sensor-emulator.local` as appropriate.

---

### 7.1 curl — full update

Push all five fields in one request.  Both sensors must be in REST mode.

```bash
curl -s -X POST http://192.168.4.1/api/data \
  -H "Content-Type: application/json" \
  -d '{
    "T":         22.5,
    "RH":        65.0,
    "Direction": 270.0,
    "Speed":     3.2,
    "Heating":   18.0
  }' | python3 -m json.tool
```

Example response:

```json
{
  "T":         { "status": "accepted", "value": 22.5  },
  "RH":        { "status": "accepted", "value": 65.0  },
  "Direction": { "status": "accepted", "value": 270.0 },
  "Speed":     { "status": "accepted", "value": 3.2   },
  "Heating":   { "status": "accepted", "value": 18.0  }
}
```

---

### 7.2 curl — temperature and humidity only

Only the FG6485A fields are sent.  S200 registers are not touched.

```bash
curl -s -X POST http://192.168.4.1/api/data \
  -H "Content-Type: application/json" \
  -d '{"T": 18.3, "RH": 72.0}'
```

Example response:

```json
{
  "T":  { "status": "accepted", "value": 18.3 },
  "RH": { "status": "accepted", "value": 72.0 }
}
```

---

### 7.3 curl — wind station only

Only wind direction and speed are pushed.  Heating stays at its previous value.

```bash
curl -s -X POST http://192.168.4.1/api/data \
  -H "Content-Type: application/json" \
  -d '{"Direction": 135.5, "Speed": 12.8}'
```

Example response:

```json
{
  "Direction": { "status": "accepted", "value": 135.5 },
  "Speed":     { "status": "accepted", "value": 12.8  }
}
```

---

### 7.4 curl — out-of-range rejection

Temperature of 150 °C exceeds the 120 °C maximum and humidity of −5 %RH is
below 0.  Neither register is modified.

```bash
curl -s -X POST http://192.168.4.1/api/data \
  -H "Content-Type: application/json" \
  -d '{"T": 150.0, "RH": -5.0}'
```

Example response:

```json
{
  "T":  { "status": "rejected", "reason": "out_of_range", "min": -40.0, "max": 120.0 },
  "RH": { "status": "rejected", "reason": "out_of_range", "min":   0.0, "max":  99.9 }
}
```

---

### 7.5 curl — mixed mode (S200 not in REST)

FG6485A is in REST mode, S200 is in Manual mode.  S200 fields in the payload
are silently skipped.

```bash
curl -s -X POST http://192.168.4.1/api/data \
  -H "Content-Type: application/json" \
  -d '{"T": 20.0, "RH": 60.0, "Speed": 5.0, "Direction": 90.0}'
```

Example response:

```json
{
  "T":         { "status": "accepted", "value": 20.0 },
  "RH":        { "status": "accepted", "value": 60.0 },
  "Speed":     { "status": "skipped",  "reason": "not_rest_mode" },
  "Direction": { "status": "skipped",  "reason": "not_rest_mode" }
}
```

---

### 7.6 Python — periodic push loop

A script that reads sensor data from a local source and pushes it to the
emulator every 10 seconds.

```python
import time
import requests

EMULATOR_URL = "http://192.168.4.1/api/data"

def read_local_sensors():
    """Replace with your actual sensor reading logic."""
    return {
        "T":         21.4,
        "RH":        58.0,
        "Direction": 225.0,
        "Speed":     4.7,
        "Heating":   20.0,
    }

def push(payload):
    try:
        resp = requests.post(EMULATOR_URL, json=payload, timeout=5)
        resp.raise_for_status()
        result = resp.json()
        for field, info in result.items():
            status = info.get("status")
            if status == "accepted":
                print(f"  {field}: {info['value']}")
            elif status == "rejected":
                print(f"  {field}: REJECTED — out of range "
                      f"[{info['min']}, {info['max']}]")
            elif status == "skipped":
                print(f"  {field}: skipped (sensor not in REST mode)")
    except requests.RequestException as e:
        print(f"Push failed: {e}")

if __name__ == "__main__":
    while True:
        data = read_local_sensors()
        print(f"Pushing: {data}")
        push(data)
        time.sleep(10)
```

---

### 7.7 PowerShell — single push

```powershell
$body = @{
    T         = 19.5
    RH        = 55.0
    Direction = 180.0
    Speed     = 2.3
    Heating   = 17.5
} | ConvertTo-Json

$response = Invoke-RestMethod `
    -Method Post `
    -Uri "http://192.168.4.1/api/data" `
    -ContentType "application/json" `
    -Body $body

$response | ConvertTo-Json -Depth 3
```

---

### 7.8 Node.js — event-driven push

```javascript
const http = require('http');

const EMULATOR_HOST = '192.168.4.1';
const EMULATOR_PORT = 80;

function pushData(payload) {
  const body = JSON.stringify(payload);
  const options = {
    hostname: EMULATOR_HOST,
    port:     EMULATOR_PORT,
    path:     '/api/data',
    method:   'POST',
    headers:  {
      'Content-Type':   'application/json',
      'Content-Length': Buffer.byteLength(body),
    },
  };

  return new Promise((resolve, reject) => {
    const req = http.request(options, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try { resolve(JSON.parse(data)); }
        catch { reject(new Error('Invalid JSON response')); }
      });
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

// Example: push once
pushData({ T: 23.1, RH: 48.0, Speed: 6.5, Direction: 310.0 })
  .then((result) => {
    for (const [field, info] of Object.entries(result)) {
      if (info.status === 'accepted') {
        console.log(`${field}: accepted → ${info.value}`);
      } else {
        console.log(`${field}: ${info.status} — ${info.reason}`);
      }
    }
  })
  .catch(console.error);
```

---

## 8. Switching Away From REST Mode

Switch a sensor back to another mode at any time via the web UI radio buttons
or via `POST /config/sensor`.

```bash
# Return FG6485A to Manual mode
curl -s -X POST http://192.168.4.1/config/sensor \
  -H "Content-Type: application/json" \
  -d '{"sensor":"fg6485a","mode":0}'

# Return S200 to Live mode
curl -s -X POST http://192.168.4.1/config/sensor \
  -H "Content-Type: application/json" \
  -d '{"sensor":"s200","mode":1}'
```

Mode codes:

| Code | Mode |
|:---:|:---|
| `0` | Manual |
| `1` | Live (Open-Meteo API) |
| `2` | Replay (CSV playback) |
| `3` | REST (this mode) |

After switching, any subsequent `POST /api/data` calls targeting that sensor
will return `"skipped"` for its fields until REST mode is re-enabled.

---

## 9. Integration with the Web Interface

The web UI reflects the current sensor state regardless of how values were
last set.  When a sensor is in REST mode:

- The **Mode** indicator in the Status section shows **REST**.
- The manual-value sliders are disabled (greyed out).
- The slider and number-input values update every second via the WebSocket
  push, so you can observe arriving REST values in real time.

The live-fetch countdown bar is hidden while neither sensor is in Live mode.

---

## 10. Limitations

| Limitation | Detail |
|:---|:---|
| No authentication | The endpoint has no API key or authentication. On a private network this is acceptable; do not expose port 80 to the public internet. |
| Body size limit | The HTTP body is capped at 512 bytes. A full five-field payload is well within this limit. |
| No TLS | The server runs plain HTTP on port 80; HTTPS is not supported by the ESP-IDF httpd server on this firmware build. |
| No push notification | The emulator does not initiate outbound connections in REST mode; it only responds to incoming HTTP POSTs. |
| No timestamping | The emulator does not record when a value was last pushed. Monitor via the WebSocket status stream if needed. |
| Single concurrent write | The JSON body is read atomically into a 512-byte buffer; concurrent POSTs will be serialised by the httpd. |
| No NVS persistence of REST values | Values injected via REST are held in RAM only. After a reboot, Modbus registers revert to the NVS-persisted Manual defaults until the first REST push arrives. |

---

## 11. Troubleshooting

**`400 Bad Request` — "JSON parse error"**

The body was not valid JSON. Check for missing quotes, trailing commas, or
incorrect field names. Remember field names are case-sensitive (`T` not `t`,
`Direction` not `direction`).

**All fields return `"skipped"` with `"not_rest_mode"`**

The sensor has not been switched to REST mode. Use the web UI or `POST
/config/sensor` with `"mode":3` — see [Section 3](#3-enabling-rest-mode).

**FG6485A fields accepted but S200 fields skipped (or vice versa)**

Only the sensor(s) set to REST mode will accept values. Check the current mode
of each sensor in the web UI Status section.

**`rejected` with `"out_of_range"` for values that look correct**

Verify units. `Direction` is in degrees (0–360), not radians. `Speed` is in m/s,
not km/h. `T` and `Heating` are in °C.

**No response / connection refused**

- Confirm you are connected to the same network as the emulator.
- In AP mode the IP is always `192.168.4.1`.
- In STA mode find the assigned IP in your router or in the web UI.
- Verify the device has booted (the LED on the M5Stack Atom should be lit).

**Values appear correct in the API response but Modbus reads old data**

The mode task dispatchers run at low priority. Allow up to one FreeRTOS tick
(~1 ms) after a POST before the Modbus slave serves the new value. In practice
the update is instantaneous for any Modbus polling rate above 10 ms.
