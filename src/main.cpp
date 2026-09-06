#include <DFRobot_SIM808.h>
#include <SoftwareSerial.h>

#define PIN_TX 10
#define PIN_RX 11

#include "secrets.h"  // API_KEY, NOTIFY_PHONE_NUMBER, APN*, SERVER_*, API_PATH - gitignored, see secrets.h.example

#define GPS_POLL_INTERVAL_MS 2000

// 1 = full raw AT/HTTP dumps on every cycle (useful when debugging comms
// issues); 0 = concise one-line status only. Flip and reflash if something
// breaks in the field and the detailed logs are needed again.
#define DEBUG 0

SoftwareSerial mySerial(PIN_RX, PIN_TX);  // RX, TX from Arduino's perspective
DFRobot_SIM808 sim808(&mySerial);

// sized for the response only, not for high-volume data - this device just
// POSTs a single point and prints whatever comes back for visibility
char http_buffer[128];

// Send a raw AT command, capture the modem's exact reply into buf, and echo it
// to Serial - this is the capture loop proven reliable during GPS diagnostics
// (repeated clean AT+CGNSINF reads), reused here instead of a second one-off
// implementation since the DFRobot_SIM808 library's own helpers only return
// true/false and hide the actual response text.
void sendATRaw(const __FlashStringHelper *cmd, char *buf, size_t bufSize, unsigned long timeoutMs = 5000) {
  while (mySerial.available()) mySerial.read();
  mySerial.print(cmd);
  mySerial.print("\r\n");

  size_t len = 0;
  unsigned long start = millis();
  while (millis() - start < timeoutMs && len < bufSize - 1) {
    while (mySerial.available() && len < bufSize - 1) {
      buf[len++] = mySerial.read();
      start = millis();
    }
  }
  buf[len] = '\0';

#if DEBUG
  Serial.print("> ");
  Serial.println(cmd);
  Serial.println(buf);
#endif
}

// Splits on ',' without collapsing adjacent delimiters, unlike strtok - needed
// because AT+CGNSINF leaves unset fields empty (e.g. "1,0,,,,,,,,,,,,,,,,,,,"),
// and strtok would silently skip those and misalign every field after them.
char *nextField(char **cursor) {
  if (!*cursor) return NULL;
  char *start = *cursor;
  char *comma = strchr(start, ',');
  if (comma) {
    *comma = '\0';
    *cursor = comma + 1;
  } else {
    *cursor = NULL;
  }
  return start;
}

struct GNSSFix {
  char utcDateTime[20];  // yyyyMMddhhmmss.sss
  float lat;
  float lon;
};

// Parses a raw AT+CGNSINF response (as captured by sendATRaw) into a GNSSFix.
// Fields: <run status>,<fix status>,<UTC time>,<lat>,<lon>,<alt>,<speed>,
// <course>,<fix mode>,<reserved>,<HDOP>,<PDOP>,<VDOP>,<reserved>,
// <GPS satellites in view>,<GNSS satellites used>,...
bool parseCGNSINF(char *buf, GNSSFix *fix) {
  char *p = strstr(buf, "+CGNSINF:");
  if (!p) return false;
  p += strlen("+CGNSINF:");

  nextField(&p);  // GNSS run status (unused)
  char *fixStatus = nextField(&p);
  char *utcTime = nextField(&p);
  char *lat = nextField(&p);
  char *lon = nextField(&p);

  if (!fixStatus || fixStatus[0] != '1') return false;
  if (!utcTime || !lat || !lon || !*utcTime || !*lat || !*lon) return false;
  if (strlen(utcTime) < 14) return false;  // yyyyMMddhhmmss - shorter means a corrupted/dropped read

  float parsedLat = atof(lat);
  float parsedLon = atof(lon);
  // SoftwareSerial occasionally drops/corrupts bytes mid-response (worse right
  // after GSM transmit bursts); a corrupted read can still parse as *some*
  // float, so sanity-check the range instead of trusting whatever atof() gives.
  if (parsedLat < -90 || parsedLat > 90 || parsedLon < -180 || parsedLon > 180) return false;

  strncpy(fix->utcDateTime, utcTime, sizeof(fix->utcDateTime) - 1);
  fix->utcDateTime[sizeof(fix->utcDateTime) - 1] = '\0';
  fix->lat = parsedLat;
  fix->lon = parsedLon;
  return true;
}

//************* setup() steps *************

void silenceLeftoverGPSStream() {
  // The SIM808 module isn't power-cycled when the Arduino resets, so NMEA
  // streaming left on from a previous run keeps flooding the UART and
  // corrupts every AT command response until it's turned off. Deliberately
  // NOT sending AT+CGNSPWR=0 here: that fully powers down the GNSS engine
  // and forces a cold start next time it's turned back on, throwing away
  // any satellite lock across every re-upload during testing. Streaming
  // alone is enough to quiet the UART for init()/join()/connect() below.
  mySerial.print(F("AT+CGNSTST=0\r\n"));
  delay(500);
  while (mySerial.available()) mySerial.read();
}

void initModem() {
  while (!sim808.init()) {
    delay(1000);
    Serial.print("Sim808 init error\r\n");
  }
  Serial.println("Sim808 OK");
}

void connectGPRS() {
  while (!sim808.join(F(APN), F(APN_USER), F(APN_PASS))) {
    int signalStrength;
    sim808.getSignalStrength(&signalStrength);
    Serial.print("GPRS join error, signal (AT+CSQ raw, 0-31, 99=unknown): ");
    Serial.println(signalStrength);
    delay(1000);
  }
  Serial.println("GPRS OK");
}

void connectServer() {
  // Plain HTTP on port 80 - the SIM808 module has no reliable TLS/HTTPS
  // support, so the backend needs to accept plain HTTP for this endpoint.
  while (!sim808.connect(TCP, SERVER_HOST, SERVER_PORT)) {
    Serial.println("Connect error");
    delay(1000);
  }
  Serial.println("Server OK");
}

void powerOnGPS() {
  // power the GNSS engine only - streaming (AT+CGNSTST) is deliberately left
  // off, we poll AT+CGNSINF for fixes instead (see parseCGNSINF)
  mySerial.print(F("AT+CGNSPWR=1\r\n"));
  delay(2000);
  while (mySerial.available()) mySerial.read();
  Serial.println("GPS power success");
}

void setup() {
  mySerial.begin(9600);
  Serial.begin(9600);

  silenceLeftoverGPSStream();
  initModem();
  connectGPRS();
  connectServer();
  powerOnGPS();
}

//************* loop() steps *************

bool pollGPSFix(GNSSFix *fix) {
  // static: keeps this large buffer off the call stack, which was
  // overflowing on this 2KB-RAM chip when it was a local alive at the same
  // time as snprintf()/dtostrf()'s own stack usage - see SIM808_DEBUGGING.md
  static char atBuf[128];
  sendATRaw(F("AT+CGNSINF"), atBuf, sizeof(atBuf), 3000);
  return parseCGNSINF(atBuf, fix);
}

void postFixToServer(const GNSSFix &fix) {
  // AT+CGNSINF's UTC datetime (yyyyMMddhhmmss.sss) reformatted to ISO 8601
  static char isoTimestamp[21];
  snprintf(isoTimestamp, sizeof(isoTimestamp), "%.4s-%.2s-%.2sT%.2s:%.2s:%.2sZ",
      fix.utcDateTime, fix.utcDateTime + 4, fix.utcDateTime + 6,
      fix.utcDateTime + 8, fix.utcDateTime + 10, fix.utcDateTime + 12);

  static char latStr[12];
  static char lonStr[12];
  dtostrf(fix.lat, 1, 6, latStr);
  dtostrf(fix.lon, 1, 6, lonStr);

  static char http_post[256];
  int post_length = snprintf(http_post, sizeof(http_post),
      "POST " API_PATH "?lat=%s&lon=%s&timestamp=%s HTTP/1.1\r\n"
      "Host: " SERVER_HOST "\r\n"
      "X-API-KEY: " API_KEY "\r\n"
      "Content-Length: 0\r\n"
      "\r\n",
      latStr, lonStr, isoTimestamp);

#if DEBUG
  Serial.println("[POST] Request:");
  Serial.println(http_post);
#endif
  sim808.send(http_post, post_length);

  int ret = sim808.recv(http_buffer, sizeof(http_buffer) - 1);
  if (ret <= 0) {
    Serial.println("[POST] No response received, reconnecting...");
    // The socket only gets opened once, in connectServer() at boot; if the
    // server (or an idle timeout on the connection) closes it, every POST
    // after that fails the same way forever unless we reopen it here.
    sim808.close();
    connectServer();
  } else {
    http_buffer[ret] = '\0';
#if DEBUG
    Serial.print("[POST] Recv ");
    Serial.print(ret);
    Serial.print(" bytes: ");
    Serial.println(http_buffer);
#else
    char *statusStart = strstr(http_buffer, "HTTP/1.1 ");
    Serial.print("[POST] ");
    if (statusStart) {
      Serial.write(statusStart + 9, 3);  // 3-digit status code
      Serial.println();
    } else {
      Serial.println("response received");
    }
#endif
  }
}

void loop() {
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < GPS_POLL_INTERVAL_MS) return;
  lastPoll = millis();

  static GNSSFix fix;
  if (pollGPSFix(&fix)) {
#if !DEBUG
    Serial.print("[GPS] Fix: ");
    Serial.print(fix.lat, 6);
    Serial.print(",");
    Serial.println(fix.lon, 6);
#endif

    // one-time notification once network + GPS are both confirmed working -
    // sent here rather than at the end of setup() because setup() only
    // confirms the network, not GPS. Attempted once only, success or not:
    // the library doesn't document whether SMS is safe with the TCP socket
    // already open (see SIM808_DEBUGGING.md), so this also doubles as the
    // one real-world test of that - watch that postFixToServer() below
    // still gets a response right after this, on this same fix.
    static bool trackingStartedNotified = false;
    if (!trackingStartedNotified) {
      trackingStartedNotified = true;
      bool sent = sim808.sendSMS((char *)NOTIFY_PHONE_NUMBER, (char *)"GPS tracker: tracking started");
      Serial.println(sent ? "[SMS] Tracking-started notification sent" : "[SMS] Notification failed, continuing");
    }

    postFixToServer(fix);
  } else {
#if DEBUG
    Serial.println("[GPS] No fix yet, retrying...");
#endif
  }
}
