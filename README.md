# Arduino GPS Online Tracker

A GPS tracker built on the SIM808 GPS/GPRS/GSM module and an Arduino Nano
(ATmega328), which acquires its position over GPS and reports it to a
backend server over the cellular network in near real time. It can also
send an SMS notification once tracking is confirmed live.

## Hardware

- Arduino Nano (or any ATmega328 board - `platformio.ini` targets `board = uno`)
- SIM808 GPS/GPRS/GSM module, wired to the Arduino over `SoftwareSerial`:
  - Arduino pin 10 -> SIM808 RX
  - Arduino pin 11 -> SIM808 TX
- A GSM antenna (for network/SMS) and a separate active GPS antenna (3-5V bias)
- A SIM card with an active data plan
- A power supply capable of the SIM808's current spikes (up to ~2A) - a
  computer's USB port is usually not enough; use a proper wall adapter or
  battery. See `notes/SIM808_DEBUGGING.md` for what under-powering looks like.

## How it works

1. On boot, the sketch initializes the SIM808 (`sim808.init()`), joins the
   cellular data network (`sim808.join()`), opens a TCP connection to the
   backend server, and powers on the GPS engine.
2. `loop()` polls the GPS position every ~2 seconds via `AT+CGNSINF` (a
   direct AT command, not the DFRobot library's own GPS parser - see
   `notes/SIM808_DEBUGGING.md` for why).
3. Once a fix is acquired, the coordinates and a UTC timestamp are POSTed to
   the backend as HTTP query parameters (see `notes/API_REQUEST_EXAMPLE.md`
   for the exact request format), and an SMS notification is sent once, the
   first time a fix is confirmed.
4. If a POST gets no response (e.g. the connection dropped), the sketch
   reconnects and retries on the next cycle.

## Setup

1. Install [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).
2. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   own values:
   - `API_KEY` - your backend's API key
   - `NOTIFY_PHONE_NUMBER` - phone number for the SMS notification, e.g. `+44...`
   - `APN` / `APN_USER` / `APN_PASS` - your SIM card's mobile network APN credentials
   - `SERVER_HOST` / `SERVER_PORT` / `API_PATH` - your backend's address and endpoint

   `include/secrets.h` is gitignored and never committed - see
   `include/secrets.h.example` for the exact format.
3. Wire up the hardware as described above.
4. Build and upload:
   ```
   pio run --target upload
   ```
5. Open the serial monitor at 9600 baud to watch it connect and start tracking.

Set `#define DEBUG 1` near the top of `src/main.cpp` for full raw AT
command/HTTP request-response logging if you need to diagnose a comms
issue; `DEBUG 0` (the default) prints concise one-line status only.
