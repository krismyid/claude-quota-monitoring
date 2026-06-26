# Claude Code Quota Monitor — Wemos D1 Mini + 16x2 LCD

## Hardware
- Wemos D1 Mini (ESP8266)
- 16x2 LCD with I2C backpack (PCF8574)

### Wiring
```
LCD SDA → D2 (GPIO4)
LCD SCL → D1 (GPIO5)
LCD VCC → 5V  (or 3.3V depending on backpack)
LCD GND → GND
```

---

## Architecture

```
Claude Code
  │ statusLine hook (after every response)
  ▼
statusline_hook.sh
  │ writes /tmp/claude-quota.json
  │ {five_hour: {used_percentage, resets_at}, seven_day: {...}}
  │
cron (every 1 min)
  ▼
push_to_wemos.py
  │ reads /tmp/claude-quota.json
  │ computes resets_in_minutes from epoch
  │ HTTP POST → http://<wemos-ip>/update
  ▼
Wemos D1 Mini
  │ tiny HTTP server on port 80
  │ receives JSON, updates display state
  │ NTP for clock
  ▼
16x2 LCD (rotates screens every 4s)
```

### Push payload (PC → Wemos)
```json
{
  "five_hour_pct": 12.5,
  "five_hour_resets_min": 264,
  "weekly_pct": 5.2,
  "weekly_resets_min": 418
}
```

---

## Display — 16x2 LCD

Last character (col 15) on every row is reserved as **update indicator**:
- `|` `/` `-` `\` spinning — within 60s of last push
- `·` static — no recent update / stale (>5 min)

### Screen 1 — Date & Time (via NTP)
```
2026-06-26 10:30|
Thursday        ·|
```
Rotates in every 4s.

### Screen 2 — Usage % + bar
```
5H:████░░░░ 12%|
7D:█░░░░░░░  5%|
```
Layout per row: `XX:` (3) + bar (8) + ` XX%` (4) + indicator (1) = 16

**Bar expand animation:** when this screen rotates in, bar fills left→right from 0 to actual value over ~1 second (one block per ~125ms, 8 blocks total).

Alert: backlight blinks when either value ≥ 80%.

### Screen 3 — Time to reset
```
5H reset: 4h20m|
7D reset: 6h50m·|
```

---

## Files to create

### 1. `statusline_hook.sh`
- Receives Claude Code env JSON on stdin
- Extracts `rate_limits.five_hour` and `rate_limits.seven_day`
- Writes to `/tmp/claude-quota.json`
- Prints `5h:12% 7d:5%` to stdout (shown in Claude Code CLI status bar)

### 2. `push_to_wemos.py`
- Reads `/tmp/claude-quota.json`
- Converts `resets_at` epoch → minutes remaining
- HTTP POST to Wemos
- Configured via `WEMOS_URL` env var

### 3. `wemos_monitor/wemos_monitor.ino`
- WiFi connect
- NTP sync for clock
- HTTP server on port 80, `POST /update` → store state
- Rotate Screen 1 → 2 → 3 every 4s
- Screen 2: bar expand animation on entry
- Indicator: spin for 60s after push, then `·` if stale
- Backlight blink when ≥ 80%

### 4. Arduino libraries needed
- LiquidCrystal_I2C
- ArduinoJson (v6 or v7)
- NTPClient
- WiFiUdp
- ESP8266WiFi (bundled)
- ESP8266WebServer (bundled)

---

## Config changes

### `~/.claude/settings.json` — add statusLine
```json
{
  "model": "sonnet",
  "effortLevel": "high",
  "theme": "dark",
  "statusLine": "/path/to/claude-quota-monitoring/statusline_hook.sh"
}
```

### Cron entry (every 1 min)
```
* * * * * WEMOS_URL=http://<wemos-ip>/update /path/to/claude-quota-monitoring/push_to_wemos.py
```

### Wemos: assign static DHCP lease in router
So the IP never changes between reboots.

---

## Cleanup
Delete old files created during architecture exploration:
- `quota_server.py`
- `start_server.sh`

---

## Source of quota data
Found by reading Claude Code binary strings (`/opt/claude-code/bin/claude`).
The `statusLine` command receives this JSON on stdin after every API response:
```json
{
  "rate_limits": {
    "five_hour": {
      "used_percentage": 12.5,
      "resets_at": 1234567890
    },
    "seven_day": {
      "used_percentage": 5.2,
      "resets_at": 1234567890
    }
  }
}
```
No token math or hardcoded limits — exact percentages from Anthropic's servers.
