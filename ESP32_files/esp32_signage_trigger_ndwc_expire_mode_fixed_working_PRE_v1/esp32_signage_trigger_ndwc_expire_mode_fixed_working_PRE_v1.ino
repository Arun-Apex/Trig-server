/******************************************************
 * Ethernet Signage Trigger (NDWC CAP -> JSON proxy, single source)
 * + WiFi Setup Portal
 *
 * NEW: TRIG Server Heartbeat + Remote Config Update
 * - POST /api/heartbeat
 * - Server may respond with {update:true, version:N, desired:{...}}
 *
 * WiFi AP:
 *   SSID: Signage-Setup
 *   PASS: 12345678
 *   Portal: http://192.168.4.1
 *a
 * Ethernet:
 *   W5500 over SPI
 *
 * LED (WS2812 on GPIO21):
 *   Blue    = boot
 *   Green   = ethernet OK / idle or normal
 *   Red     = ethernet down or trigger error
 *   Orange blinking = event playlist active
 ******************************************************/

#include <Arduino.h>
#include <time.h>
#include <SPI.h>
#include <Ethernet.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

#include <WiFi.h>
#include <WebServer.h>

/* ================= Types ================= */
struct EventDef { const char* key; const char* th; const char* en; };
static const uint8_t EVENT_COUNT = 28;
static const EventDef EVENT_DEFS[EVENT_COUNT] = {
  {"PRA", "การแจ้งเตือนระดับชาติ", "National Alert"},
  {"EXA", "การแจ้งเตือนภัยขั้นรุนแรง", "Extreme Alert"},
  {"ASA", "การแจ้งเตือนเหตุกราดยิง", "Active Shooter Alert"},
  {"ACI", "การแจ้งเตือนเพื่อให้ข้อมูล", "AllClear/Informational Alert"},
  {"CAM", "การแจ้งเตือนการลักพาตัว", "Child Abduction"},
  {"RMT", "การแจ้งเตือนเพื่อทดสอบประจำเดือน", "Required Monthly Test"},
  {"DUMMY", "DUMMY", "DUMMY"},
  {"E10", "1-2.9 เกิดการสั่นไหวเล็กน้อย", "1–2.9 (Minor): Minor earthquakes are often felt, but rarely cause damage."},
  {"E11", "3-3.9 เกิดการสั่นไหวเล็กน้อย", "3–3.9 (Light): Light earthquakes are often felt, but rarely causes significant damage."},
  {"E12", "4-4.9 เกิดการสั่นไหวปานกลาง", "4–4.9 (Moderate): A moderate earthquake causes a noticeable shaking of indoor items, accompanied by rattling noises. Significant damage is unlikely."},
  {"E13", "5-5.9 เกิดการสั่นไหวรุนแรงเป็นบริเวณกว้าง", "5–5.9 (Strong): Strong earthquakes potentially cause significant damage to buildings and other structures."},
  {"E14", "6-6.9 เกิดการสั่นไหวรุนแรงมาก", "6.0–6.9 (Major): Major earthquakes cause a lot of damage in populated areas."},
  {"E15", "7.0 ขึ้นไป เกิดการสั่นไหวร้ายแรง", "7.0 and higher (Great): These earthquakes cause serious damage."},
  {"E16", "สึนามิ", "Tsunami"},
  {"E17", "พายุดีเปรสชันเขตร้อน", "Tropical Depression"},
  {"E18", "พายุโซนร้อน", "Tropical Storm"},
  {"E19", "ไต้ฝุ่น", "Typhoon"},
  {"E20", "อุทกภัย", "Flood"},
  {"E21", "ดินโคลนถล่ม", "Landslide"},
  {"E22", "ภัยหนาว", "Cold Weather"},
  {"E23", "ภัยจากฝุ่นละอองขนาดเล็ก (PM2.5)", "Fine Particulate Matter (PM2.5)"},
  {"E24", "กรณีเหตุรุนแรงในพื้นที่สาธารณะ", "Mass Violence Incident in Public Areas"},
  {"E25", "ภัยจากโรคระบาดในมนุษย์", "Human Epidemics"},
  {"E26", "อัคคีภัยและภัยจากสารเคมีและวัตถุอันตราย", "Fire, Chemical, and Hazardous Material"},
  {"E27", "ภัยจากการรุกรานจากภายนอกประเทศ", "Military Invasion"},
  {"E28", "ภัยจากการจราจรและขนส่ง", "Traffic and Transport Incidents"},
  {"E29", "ภัยคุกคามทางไซเบอร์", "Cyber Threats"},
  {"E30", "ภัยอื่นๆ", "Other Hazards"},
};

struct Config {
  String playerIp = "192.168.1.166";
  String basicAuth = "Basic b2N0b3B1czpvY3RvcHVz";
  String normalPlaylist = "Station-CC02";

  // Single NDWC JSON endpoint (proxy)
  String capUrl = "http://27.131.170.216:8081/ndwc/latest.json";

  // Device location filter (ISO3166-2 province code like TH-10). Empty = no filtering.
  String deviceIso = ""; 

  uint16_t pollSec = 30;

  // cycle controls
  uint8_t cycles = 1;
  uint16_t eventPlaySec = 60;
  uint8_t eventPlayMode = 0; // 0=fixed duration, 1=until alert expires
  uint16_t normalHoldSec = 30;

  // dedupe controls
  bool useIdentifier = true;
  bool allowStaleTrigger = false;
  String lastDedupeKey = "";

  // per-event playlist mapping (from Excel column B)
  bool evEnabled[EVENT_COUNT];
  String evPlaylist[EVENT_COUNT];
  uint8_t evMinRank[EVENT_COUNT]; // 0..4 (Unknown..Extreme)

  // ===== TRIG server (device monitoring + config) =====
  String trigUrl = "http://27.131.170.216:8082";
  String trigToken = "32198767ddb0b65323f073690b174d50cead84557624616119698262b8e6f876";
  uint16_t hbSec = 30;
  uint32_t appliedConfigVersion = 0;

  Config() {
    for (uint8_t i=0;i<EVENT_COUNT;i++){ evEnabled[i]=true; evPlaylist[i]=""; }
  }
};


struct CapResult {
  bool ok = false;

  bool decisionActive = false;
  bool upstreamOk = false;
  bool stale = false;
  String reason = "";

  String identifier = "";
  String sent = "";

  String effective = "";
  String expires = "";

  String playlist = "";     // server-provided (optional)
  String playlistKey = "";  // used to map to local playlist list


  String severity = "Unknown";
  uint8_t rank = 0;
  // for device filtering
  String isoCsv = "";       // "TH-10,TH-14"
};

/* ================= RGB LED ================= */
#define RGB_PIN 21
Adafruit_NeoPixel led(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

static void ledColor(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}

/* ================= W5500 PINS ================= */
#define W5500_MISO  12
#define W5500_MOSI  11
#define W5500_SCK   13
#define W5500_CS    14
#define W5500_RST    9

/* ================= Ethernet ================= */
byte ETH_MAC[] = {0x02,0xAA,0xBB,0xCC,0xDD,0x01}; // can be left; W5500 works fine in your environment

/* ================= WiFi AP Setup Portal ================= */
static const char* AP_SSID = "Signage-Setup";
static const char* AP_PASS = "12345678";
WebServer wifiServer(80);

/* ================= Helpers ================= */
static uint16_t clampU16(long v, long minV, long maxV) {
  if (v < minV) v = minV;
  if (v > maxV) v = maxV;
  return (uint16_t)v;
}
static uint8_t clampU8(long v, long minV, long maxV) {
  if (v < minV) v = minV;
  if (v > maxV) v = maxV;
  return (uint8_t)v;
}
static uint32_t clampU32(long long v, long long minV, long long maxV) {
  if (v < minV) v = minV;
  if (v > maxV) v = maxV;
  return (uint32_t)v;
}

/* ================= Time helpers (ISO8601 +TZ like +07:00) ================= */
static bool parseIsoWithTzToEpoch(const String& iso, time_t &outEpoch) {
  // Expected: YYYY-MM-DDTHH:MM:SS+07:00 (or Z)
  if (iso.length() < 19) return false;
  String s = iso;

  // If ends with 'Z', treat as UTC
  if (s.endsWith("Z")) {
    s.remove(s.length()-1);
    s += "+0000";
  } else {
    // Convert timezone "+07:00" -> "+0700" (remove the last colon in TZ)
    int plus = s.lastIndexOf('+');
    int minus = s.lastIndexOf('-');
    int tzPos = max(plus, minus);
    if (tzPos > 18 && s.length() >= tzPos + 6 && s.charAt(tzPos+3) == ':') {
      s.remove(tzPos+3, 1);
    }
  }

  struct tm tm{};
  char buf[48];
  s.toCharArray(buf, sizeof(buf));

  char *p = strptime(buf, "%Y-%m-%dT%H:%M:%S%z", &tm);
  if (!p) return false;

  // Convert tm (UTC) to epoch safely on ESP32 (timegm may be unavailable)
// mktime assumes local time, so temporarily set TZ=UTC
char oldTZ[32];
const char* curTZ = getenv("TZ");
strncpy(oldTZ, curTZ ? curTZ : "", sizeof(oldTZ));
oldTZ[sizeof(oldTZ)-1] = ' ';

setenv("TZ", "UTC0", 1);
tzset();

time_t t = mktime(&tm);

if (strlen(oldTZ)) {
  setenv("TZ", oldTZ, 1);
} else {
  unsetenv("TZ");
}
tzset();

if (t == (time_t)-1) return false;
  outEpoch = t;
  return true;
}

static uint32_t computeHoldSecondsUntilExpire(const String& effective, const String& expires) {
  time_t eff=0, exp=0;
  if (!parseIsoWithTzToEpoch(effective, eff)) return 0;
  if (!parseIsoWithTzToEpoch(expires, exp))   return 0;
  if (exp <= eff) return 0;

  time_t now = time(nullptr);
  bool clockValid = (now > 1700000000); // sanity threshold (~2023)
  time_t base = clockValid ? now : eff;

  if (exp <= base) return 0;
  time_t diff = exp - base;

  // clamp to avoid crazy values (max 6 hours)
  if (diff > 6 * 60 * 60) diff = 6 * 60 * 60;
  return (uint32_t)diff;
}


/* ================= Globals ================= */
Config cfg;

// CAP currently being processed in a run
CapResult g_runCap;

// state machine
enum RunState {
  IDLE,
  EVENT_TRIGGER,
  EVENT_WAIT,
  NORMAL_TRIGGER,
  NORMAL_WAIT
};

RunState runState = IDLE;

uint32_t lastPollMs = 0;
uint32_t stateTs = 0;
uint32_t eventWaitMs = 0; // duration to stay in EVENT_WAIT (ms)
uint32_t lastTriggerAttemptMs = 0;

uint8_t cyclesLeft = 0;
String selectedEventName = "";
String selectedEventPlaylist = "";
String pendingIdentifier = "";
bool identifierCommitted = false;

String pendingSource = "";   // "national" or "regional"
String pendingAreaDesc = "";

/* ================= LED blink timing ================= */
bool orangeBlinkOn = false;
uint32_t lastBlinkMs = 0;

/* ================= NEW: Heartbeat state ================= */
uint32_t lastHbMs = 0;
String g_deviceId = "";            // trig-<efusemac>
String lastAlertIdSent = "";
String lastEventSent = "";
String lastSevSent = "";

/* ================= Small helpers ================= */
static uint8_t severityRank(const String& s) {
  if (s == "Extreme") return 4;
  if (s == "Severe")  return 3;
  if (s == "Moderate")return 2;
  if (s == "Minor")   return 1;
  return 0;
}
static String sevLabel(uint8_t r) {
  switch (r) {
    case 4: return "Extreme";
    case 3: return "Severe";
    case 2: return "Moderate";
    case 1: return "Minor";
    default: return "Unknown";
  }
}
static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}
static String upperTrim(String s) { s.trim(); s.toUpperCase(); return s; }
static bool containsIgnoreCase(const String& hay, const String& needle) {
  String h = hay; h.toLowerCase();
  String n = needle; n.toLowerCase();
  if (n.length() == 0) return true;
  return h.indexOf(n) >= 0;
}

/* ================= Event playlist lookup ================= */
static int eventIndexByKey(const String& key) {
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    if (key.equalsIgnoreCase(EVENT_DEFS[i].key)) return (int)i;
  }
  return -1;
}

static String playlistForKey(const String& key) {
  int idx = eventIndexByKey(key);
  if (idx < 0) return String("");
  if (!cfg.evEnabled[idx]) return String("");
  return cfg.evPlaylist[idx];
}

static String makeFallbackKey(const CapResult& r) {
  return r.sent + "|" + r.playlistKey + "|" + r.playlist;
}
static String chooseDedupeKey(const CapResult& r) {
  if (cfg.useIdentifier && r.identifier.length()) return r.identifier;
  return makeFallbackKey(r);
}

static bool matchesDeviceIso(const CapResult& r) {
  if (cfg.deviceIso.length() == 0) return true;
  if (r.isoCsv.length() == 0) return false;
  return containsIgnoreCase(r.isoCsv, cfg.deviceIso);
}



/* ================= Device ID ================= */
static String makeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[32];
  // use lower-case hex, stable
  snprintf(buf, sizeof(buf), "trig-%04x%08x",
           (uint16_t)(mac >> 32),
           (uint32_t)(mac & 0xFFFFFFFF));
  return String(buf);
}

/* ================= Config FS ================= */
static void saveConfig() {
  DynamicJsonDocument d(24576);

  d["playerIp"] = cfg.playerIp;
  d["basicAuth"] = cfg.basicAuth;
  d["normalPlaylist"] = cfg.normalPlaylist;

  d["capUrl"] = cfg.capUrl;
  d["deviceIso"] = cfg.deviceIso;

  d["pollSec"] = cfg.pollSec;

  d["cycles"] = cfg.cycles;
  d["eventPlaySec"] = cfg.eventPlaySec;
  d["eventPlayMode"] = cfg.eventPlayMode;
  d["eventPlayMode"] = cfg.eventPlayMode;
  d["normalHoldSec"] = cfg.normalHoldSec;

  d["useIdentifier"] = cfg.useIdentifier;
  d["allowStaleTrigger"] = cfg.allowStaleTrigger;
  d["lastDedupeKey"] = cfg.lastDedupeKey;

  // per-event mappings
  JsonArray arr = d.createNestedArray("events");
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    JsonObject o = arr.createNestedObject();
    o["key"] = EVENT_DEFS[i].key;
    o["en"]  = cfg.evEnabled[i];
    o["pl"]  = cfg.evPlaylist[i];
  
    o["min"] = cfg.evMinRank[i];
  }

  // TRIG server
  d["trigUrl"] = cfg.trigUrl;
  d["trigToken"] = cfg.trigToken;
  d["hbSec"] = cfg.hbSec;
  d["appliedConfigVersion"] = cfg.appliedConfigVersion;

  File f = LittleFS.open("/cfg.json", "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
}

static void loadConfig() {
  if (!LittleFS.exists("/cfg.json")) return;
  File f = LittleFS.open("/cfg.json", "r");
  if (!f) return;

  DynamicJsonDocument d(24576);
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) return;

  cfg.playerIp       = (const char*)(d["playerIp"] | cfg.playerIp.c_str());
  cfg.basicAuth      = (const char*)(d["basicAuth"] | cfg.basicAuth.c_str());
  cfg.normalPlaylist = (const char*)(d["normalPlaylist"] | cfg.normalPlaylist.c_str());

  cfg.capUrl   = (const char*)(d["capUrl"] | cfg.capUrl.c_str());
  cfg.deviceIso= (const char*)(d["deviceIso"] | cfg.deviceIso.c_str());

  cfg.pollSec = (uint16_t)(d["pollSec"] | cfg.pollSec);

  cfg.cycles        = (uint8_t)(d["cycles"] | cfg.cycles);
  cfg.eventPlaySec  = (uint16_t)(d["eventPlaySec"] | cfg.eventPlaySec);
  cfg.eventPlayMode = (uint8_t)(d["eventPlayMode"] | cfg.eventPlayMode);
  cfg.normalHoldSec = (uint16_t)(d["normalHoldSec"] | cfg.normalHoldSec);

  cfg.useIdentifier = (bool)(d["useIdentifier"] | cfg.useIdentifier);
  cfg.allowStaleTrigger = (bool)(d["allowStaleTrigger"] | cfg.allowStaleTrigger);
  cfg.lastDedupeKey = (const char*)(d["lastDedupeKey"] | cfg.lastDedupeKey.c_str());

  // TRIG server
  cfg.trigUrl = (const char*)(d["trigUrl"] | cfg.trigUrl.c_str());
  cfg.trigToken = (const char*)(d["trigToken"] | cfg.trigToken.c_str());
  cfg.hbSec = (uint16_t)(d["hbSec"] | cfg.hbSec);
  cfg.appliedConfigVersion = (uint32_t)(d["appliedConfigVersion"] | cfg.appliedConfigVersion);

  // per-event mappings
  if (d.containsKey("events")) {
    JsonArray arr = d["events"].as<JsonArray>();
    uint8_t i=0;
    for (JsonObject o : arr) {
      if (i >= EVENT_COUNT) break;
      cfg.evEnabled[i] = (bool)(o["en"] | cfg.evEnabled[i]);
      cfg.evPlaylist[i] = String((const char*)(o["pl"] | cfg.evPlaylist[i].c_str()));
      
      cfg.evMinRank[i] = (uint8_t)(o["min"] | cfg.evMinRank[i]);
      i++;
    }
  }
}

/* ================= Player Trigger ================= */
static bool triggerPlaylist(const String& pl) {
  if (pl.length() == 0) return false;

  IPAddress ip;
  if (!ip.fromString(cfg.playerIp)) {
    Serial.println("Trigger: invalid player IP");
    ledColor(40, 0, 0);
    return false;
  }

  EthernetClient c;
  if (!c.connect(ip, 8000)) {
    Serial.println("Trigger: connect failed to player");
    ledColor(40, 0, 0);
    return false;
  }

  String body = "{\"play\":true}";
  c.print("POST /api/play/playlists/" + pl + " HTTP/1.1\r\n");
  c.print("Host: " + cfg.playerIp + "\r\n");
  c.print("Authorization: " + cfg.basicAuth + "\r\n");
  c.print("Content-Type: application/json\r\n");
  c.print("Content-Length: " + String(body.length()) + "\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(body);

  uint32_t t0 = millis();
  while (!c.available() && millis() - t0 < 2500) delay(5);

  String status = c.readStringUntil('\n');
  status.trim();
  Serial.println("Trigger -> " + status);

  c.stop();

  bool ok = status.startsWith("HTTP/1.1 2") || status.startsWith("HTTP/1.0 2");
  if (!ok) ledColor(40, 0, 0);
  return ok;
}

/* ================= HTTP URL parse (http://IP:port/path) ================= */
static bool parseHttpUrl(const String& url, IPAddress& hostIp, uint16_t& port, String& path) {
  String u = url;
  u.trim();
  if (!u.startsWith("http://")) return false;

  String rest = u.substring(7);
  int slash = rest.indexOf('/');
  String hostport = (slash < 0) ? rest : rest.substring(0, slash);
  path = (slash < 0) ? "/" : rest.substring(slash);
  if (path.length() == 0) path = "/";

  int colon = hostport.indexOf(':');
  String host = hostport;
  port = 80;

  if (colon >= 0) {
    host = hostport.substring(0, colon);
    port = (uint16_t)hostport.substring(colon + 1).toInt();
    if (port == 0) port = 80;
  }

  host.trim();
  if (!hostIp.fromString(host)) return false;
  return true;
}

/* ================= Ethernet bring-up ================= */
static bool startEthernet() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(80);
  digitalWrite(W5500_RST, HIGH);
  delay(200);

  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);

  Serial.println("Ethernet DHCP...");
  int dhcp = Ethernet.begin(ETH_MAC);
  delay(200);

  uint32_t t0 = millis();
  while (millis() - t0 < 6000) {
    if (Ethernet.linkStatus() == LinkON && Ethernet.localIP() != INADDR_NONE) break;
    delay(200);
  }

  if (dhcp == 0 || Ethernet.localIP() == INADDR_NONE) {
    Serial.println("DHCP failed -> static fallback 192.168.1.250");
    IPAddress ip(192, 168, 1, 250);
    IPAddress dns(192, 168, 1, 1);
    IPAddress gw(192, 168, 1, 1);
    IPAddress mask(255, 255, 255, 0);
    Ethernet.begin(ETH_MAC, ip, dns, gw, mask);
    delay(300);
  }

  Serial.print("ETH link: ");
  Serial.println(Ethernet.linkStatus() == LinkON ? "ON" : "OFF");
  Serial.print("ETH IP  : ");
  Serial.println(Ethernet.localIP());

  return (Ethernet.linkStatus() == LinkON && Ethernet.localIP() != INADDR_NONE);
}
static bool ethernetOk() {
  return (Ethernet.linkStatus() == LinkON && Ethernet.localIP() != INADDR_NONE);
}


/* ================= Poll CAP JSON ================= */
static CapResult pollCapUrl(const String& url, const String& sourceName) {
  CapResult r;

  IPAddress hostIp;
  uint16_t port;
  String path;

  if (!parseHttpUrl(url, hostIp, port, path)) {
    Serial.println(sourceName + ": URL must be http://<IP>:<port>/path");
    return r;
  }

  EthernetClient c;
  if (!c.connect(hostIp, port)) {
    Serial.println(sourceName + ": connect failed");
    return r;
  }

  c.print("GET " + path + " HTTP/1.1\r\n");
  c.print("Host: " + hostIp.toString() + "\r\n");
  c.print("User-Agent: signage-trigger\r\n");
  c.print("Accept: application/json\r\n");
  c.print("Connection: close\r\n\r\n");

  String status = c.readStringUntil('\n');
  status.trim();
  if (!status.startsWith("HTTP/1.1 200")) {
    Serial.println(sourceName + ": bad status: " + status);
    c.stop();
    return r;
  }

  // skip headers
  while (c.connected()) {
    String line = c.readStringUntil('\n');
    if (line == "\r" || line.length() == 1) break;
  }

  // Filter to avoid huge CAP polygon payloads
  StaticJsonDocument<512> filter;
  filter["decision"]["active"] = true;
  filter["decision"]["reason"] = true;

  filter["alert"]["upstreamOk"] = true;
  filter["alert"]["stale"] = true;
  filter["alert"]["reason"] = true;
  filter["alert"]["identifier"] = true;
  filter["alert"]["sent"] = true;
  filter["alert"]["effective"] = true;
  filter["alert"]["expires"] = true;
  filter["alert"]["info"]["playlist"] = true;
  filter["alert"]["info"]["playlistKey"] = true;
  filter["alert"]["info"]["severity"] = true;
  filter["alert"]["info"]["effective"] = true;
  filter["alert"]["info"]["expires"] = true;
  filter["alert"]["geocodes"]["iso3166_2"] = true;

  DynamicJsonDocument d(4096);
  DeserializationError err = deserializeJson(d, c, DeserializationOption::Filter(filter));
  c.stop();

  if (err) {
    Serial.print(sourceName + ": JSON parse error: ");
    Serial.println(err.c_str());
    return r;
  }

  r.ok = true;
  r.decisionActive = d["decision"]["active"] | false;
  r.reason = String((const char*)(d["decision"]["reason"] | ""));

  r.upstreamOk = d["alert"]["upstreamOk"] | false;
  r.stale = d["alert"]["stale"] | false;
  String aReason = String((const char*)(d["alert"]["reason"] | ""));
  if (aReason.length()) r.reason = aReason;

  r.identifier = String((const char*)(d["alert"]["identifier"] | ""));
r.sent = String((const char*)(d["alert"]["sent"] | ""));

r.effective = String((const char*)(d["alert"]["effective"] | ""));
r.expires   = String((const char*)(d["alert"]["expires"] | ""));
if (!r.effective.length()) r.effective = String((const char*)(d["alert"]["info"]["effective"] | ""));
if (!r.expires.length())   r.expires   = String((const char*)(d["alert"]["info"]["expires"] | ""));
  r.playlist = String((const char*)(d["alert"]["info"]["playlist"] | ""));
  r.playlistKey = String((const char*)(d["alert"]["info"]["playlistKey"] | ""));


  r.severity = String((const char*)(d["alert"]["info"]["severity"] | "Unknown"));
  r.rank = severityRank(r.severity);
  // geocodes list
  if (d["alert"]["geocodes"]["iso3166_2"].is<JsonArray>()) {
    String csv;
    for (JsonVariant v : d["alert"]["geocodes"]["iso3166_2"].as<JsonArray>()) {
      String s = String((const char*)v);
      if (!s.length()) continue;
      if (csv.length()) csv += ",";
      csv += s;
    }
    r.isoCsv = csv;
  }

  return r;
}


/* ================= Pick event rule ================= */
/* ================= Area match for regional alerts ================= */
/* ================= NEW: HTTP POST JSON helper ================= */
static bool httpPostJson(const String& url, const String& jsonBody, String& outBody, String& outStatus) {
  IPAddress hostIp;
  uint16_t port;
  String path;

  if (!parseHttpUrl(url, hostIp, port, path)) {
    outStatus = "bad-url";
    return false;
  }

  EthernetClient c;
  if (!c.connect(hostIp, port)) {
    outStatus = "connect-fail";
    return false;
  }

  c.print("POST " + path + " HTTP/1.1\r\n");
  c.print("Host: " + hostIp.toString() + "\r\n");
  c.print("User-Agent: signage-trigger\r\n");
  c.print("Accept: application/json\r\n");
  c.print("Content-Type: application/json\r\n");
  if (cfg.trigToken.length()) {
    c.print("X-Auth-Token: " + cfg.trigToken + "\r\n");
  }
  c.print("Content-Length: " + String(jsonBody.length()) + "\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(jsonBody);

  uint32_t t0 = millis();
  while (!c.available() && millis() - t0 < 4000) delay(5);

  outStatus = c.readStringUntil('\n');
  outStatus.trim();

  // headers
  while (c.connected()) {
    String line = c.readStringUntil('\n');
    if (line == "\r" || line.length() == 1) break;
  }

  outBody = "";
  uint32_t t1 = millis();
  while ((c.connected() || c.available()) && (millis() - t1 < 5000)) {
    while (c.available()) outBody += (char)c.read();
    delay(1);
  }
  c.stop();

  return outStatus.startsWith("HTTP/1.1 200") || outStatus.startsWith("HTTP/1.1 2");
}

/* ================= NEW: Apply desired config from server ================= */
static void applyDesiredConfig(JsonObject desired) {
  if (desired.containsKey("playerIp")) cfg.playerIp = String((const char*)desired["playerIp"]);
  if (desired.containsKey("basicAuth")) cfg.basicAuth = String((const char*)desired["basicAuth"]);
  if (desired.containsKey("normalPlaylist")) cfg.normalPlaylist = String((const char*)desired["normalPlaylist"]);

  if (desired.containsKey("capUrl")) cfg.capUrl = String((const char*)desired["capUrl"]);
  if (desired.containsKey("deviceIso")) cfg.deviceIso = String((const char*)desired["deviceIso"]);

  if (desired.containsKey("pollSec")) cfg.pollSec = clampU16((long)desired["pollSec"], 5, 3600);
  if (desired.containsKey("cycles")) cfg.cycles = clampU8((long)desired["cycles"], 1, 10);
  if (desired.containsKey("eventPlaySec")) cfg.eventPlaySec = clampU16((long)desired["eventPlaySec"], 5, 36000);
  if (desired.containsKey("eventPlayMode")) cfg.eventPlayMode = clampU8((long)desired["eventPlayMode"], 0, 1);
  if (desired.containsKey("normalHoldSec")) cfg.normalHoldSec = clampU16((long)desired["normalHoldSec"], 5, 36000);

  if (desired.containsKey("useIdentifier")) cfg.useIdentifier = (bool)desired["useIdentifier"];
  if (desired.containsKey("allowStaleTrigger")) cfg.allowStaleTrigger = (bool)desired["allowStaleTrigger"];

  if (desired.containsKey("trigUrl")) cfg.trigUrl = String((const char*)desired["trigUrl"]);
  if (desired.containsKey("trigToken")) cfg.trigToken = String((const char*)desired["trigToken"]);
  if (desired.containsKey("hbSec")) cfg.hbSec = clampU16((long)desired["hbSec"], 10, 3600);

  // optional: update event playlists via server
  // Supports both formats:
  //  A) events: [ {key:"PRA", enabled:true, playlist:"X", minRank:2}, ... ]
  //  B) events: [ {key:"PRA", en:true, pl:"X", min:2}, ... ]  (legacy)
  //
  // Applies by matching "key" to EVENT_DEFS[*].key, so ordering doesn't matter.
  if (desired.containsKey("events") && desired["events"].is<JsonArray>()) {
    JsonArray arr = desired["events"].as<JsonArray>();

    for (JsonObject o : arr) {
      const char* k = o["key"] | nullptr;
      if (!k || !*k) continue;

      // Find matching index
      int idx = -1;
      for (uint8_t i = 0; i < EVENT_COUNT; i++) {
        if (String(EVENT_DEFS[i].key) == String(k)) { idx = i; break; }
      }
      if (idx < 0) continue;

      // enabled (support both names)
      if (o.containsKey("enabled")) cfg.evEnabled[idx] = (bool)o["enabled"];
      else if (o.containsKey("en")) cfg.evEnabled[idx] = (bool)o["en"];

      // playlist (support both names)
      if (o.containsKey("playlist")) cfg.evPlaylist[idx] = String((const char*)o["playlist"]);
      else if (o.containsKey("pl")) cfg.evPlaylist[idx] = String((const char*)o["pl"]);

      // min severity rank (support both names)
      if (o.containsKey("minRank")) cfg.evMinRank[idx] = clampU8((long)o["minRank"], 0, 4);
      else if (o.containsKey("min")) cfg.evMinRank[idx] = clampU8((long)o["min"], 0, 4);
    }
  }

  // Debug (helps confirm remote apply is really changing values)
  Serial.printf("CFG: applied remote desired. normalPlaylist=%s EXA(pl)=%s EXA(min)=%u EXA(en)=%u\n",
              cfg.normalPlaylist.c_str(),
              cfg.evPlaylist[1].c_str(),
              (unsigned)cfg.evMinRank[1],
              (unsigned)cfg.evEnabled[1]);


  saveConfig();
}

/* ================= NEW: Send heartbeat and handle config update ================= */
static void sendHeartbeat() {
  if (cfg.trigUrl.length() == 0) return;

  // Basic player connectivity test (non-blocking-ish)
  bool playerOk = false;
  {
    IPAddress ip;
    if (ip.fromString(cfg.playerIp)) {
      EthernetClient c;
      if (c.connect(ip, 8000)) { playerOk = true; }
      c.stop();
    }
  }

  // Build JSON body
  DynamicJsonDocument d(2048);
  d["deviceId"] = g_deviceId;
  d["deviceIso"] = cfg.deviceIso;
  d["ethIp"] = (Ethernet.localIP() == INADDR_NONE) ? "0.0.0.0" : Ethernet.localIP().toString();
  d["ethLink"] = (Ethernet.linkStatus() == LinkON);

  d["playerIp"] = cfg.playerIp;
  d["playerOk"] = playerOk;

  d["fwVersion"] = "2.0.0";
  d["appliedConfigVersion"] = cfg.appliedConfigVersion;

  // last alert fields (best-effort)
  if (pendingIdentifier.length()) d["lastAlertId"] = pendingIdentifier;
  if (selectedEventName.length()) d["lastEvent"] = selectedEventName;

  // If we have last sent values, keep them too (helps server)
  if (lastAlertIdSent.length()) d["lastAlertId"] = lastAlertIdSent;
  if (lastEventSent.length()) d["lastEvent"] = lastEventSent;
  if (lastSevSent.length()) d["lastSeverity"] = lastSevSent;

   // ===== CONFIG SNAPSHOT FOR TRIG ADMIN UI =====
  JsonObject cfgSnap = d.createNestedObject("config");

  cfgSnap["playerIp"] = cfg.playerIp;
  cfgSnap["normalPlaylist"] = cfg.normalPlaylist;
  cfgSnap["capUrl"] = cfg.capUrl;
  cfgSnap["deviceIso"] = cfg.deviceIso;

  cfgSnap["pollSec"] = cfg.pollSec;
  cfgSnap["cycles"] = cfg.cycles;
  cfgSnap["eventPlaySec"] = cfg.eventPlaySec;
  cfgSnap["eventPlayMode"] = cfg.eventPlayMode;
  cfgSnap["normalHoldSec"] = cfg.normalHoldSec;

  cfgSnap["useIdentifier"] = cfg.useIdentifier;
  cfgSnap["allowStaleTrigger"] = cfg.allowStaleTrigger;

  JsonArray ev = cfgSnap.createNestedArray("events");
  for (uint8_t i = 0; i < EVENT_COUNT; i++) {
    JsonObject o = ev.createNestedObject();
    o["key"] = EVENT_DEFS[i].key;
    o["enabled"] = cfg.evEnabled[i];
    o["playlist"] = cfg.evPlaylist[i];
  }

  // ===== END CONFIG SNAPSHOT =====

  String body;
  serializeJson(d, body);

  String respBody, statusLine;
  String url = cfg.trigUrl;
  url.trim();
  if (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/api/heartbeat";

  bool ok = httpPostJson(url, body, respBody, statusLine);
  Serial.println("HB -> " + statusLine);

  if (!ok) return;

  DynamicJsonDocument rdoc(8192);
  DeserializationError err = deserializeJson(rdoc, respBody);
  if (err) {
    Serial.print("HB: response parse error: ");
    Serial.println(err.c_str());
    return;
  }

  bool upd = rdoc["update"] | false;
  if (!upd) return;

  uint32_t ver = (uint32_t)(rdoc["version"] | 0);
  JsonObject desired = rdoc["desired"].as<JsonObject>();
  if (ver == 0 || desired.isNull()) return;

  Serial.println("HB: applying remote config version=" + String(ver));
  applyDesiredConfig(desired);
  cfg.appliedConfigVersion = ver;
  saveConfig();
}

/* ================= WiFi portal HTML ================= */
static String buildPage(const String& msg) {
  String ethIp = (Ethernet.localIP() == INADDR_NONE) ? String("0.0.0.0") : Ethernet.localIP().toString();
  String ethLink = (Ethernet.linkStatus() == LinkON) ? "ON" : "OFF";

  String html;
  html.reserve(32000);

  html += "<!doctype html><html><head><meta charset='utf-8'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<title>Signage Setup</title>";
  html += "<style>"
          "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:18px;background:#0b1220;color:#e8eefc}"
          ".wrap{max-width:980px;margin:0 auto}"
          "h1{font-size:20px;margin:0 0 10px 0}"
          ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
          ".card{background:#0f1a33;border:1px solid rgba(255,255,255,.10);border-radius:14px;padding:14px;margin:12px 0}"
          "label{display:block;font-size:12px;color:#b8c6ff;margin-top:10px}"
          "input,select{width:100%;padding:10px;border-radius:12px;border:1px solid rgba(255,255,255,.14);background:#0b1220;color:#e8eefc}"
          ".btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}"
          "button{padding:10px 14px;border-radius:12px;border:1px solid rgba(255,255,255,.14);background:#111c3a;color:#e8eefc;cursor:pointer}"
          "button.primary{background:#2b5cff;border-color:#2b5cff}"
          "button.danger{background:#ff5a5f;border-color:#ff5a5f}"
          ".muted{color:#9fb0ff;font-size:12px;opacity:.92;margin-top:8px;line-height:1.35}"
          ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.10);font-size:12px;margin-right:8px;margin-top:8px}"
          ".msg{padding:10px 12px;border-radius:12px;background:rgba(43,92,255,.14);border:1px solid rgba(43,92,255,.35);margin:12px 0;font-size:13px}"
          "table{width:100%;border-collapse:collapse;margin-top:10px}"
          "th,td{border-bottom:1px solid rgba(255,255,255,.10);padding:8px;vertical-align:top;font-size:13px}"
          "th{color:#b8c6ff;text-align:left;font-size:12px;font-weight:600}"
          ".k{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;font-size:12px;color:#c7d2ff}"
          "@media(max-width:760px){.row{grid-template-columns:1fr}}"
          "</style></head><body><div class='wrap'>";

  html += "<h1>Signage Setup Portal</h1>";
  html += "<div class='muted'>Device ID: <b>" + htmlEscape(g_deviceId) + "</b></div>";
  html += "<div class='muted'>Connect to WiFi <b>" + String(AP_SSID) + "</b> and open <b>http://192.168.4.1</b></div>";

  html += "<div>";
  html += "<span class='pill'>Ethernet Link: " + ethLink + "</span>";
  html += "<span class='pill'>Ethernet IP: " + ethIp + "</span>";
  html += "<span class='pill'>WiFi AP IP: " + WiFi.softAPIP().toString() + "</span>";
  html += "<span class='pill'>State: " + String((int)runState) + "</span>";
  if (pendingIdentifier.length()) html += "<span class='pill'>Alert: " + htmlEscape(pendingIdentifier) + "</span>";
  if (pendingSource.length()) html += "<span class='pill'>Source: " + htmlEscape(pendingSource) + "</span>";
  html += "</div>";

  if (msg.length()) html += "<div class='msg'>" + htmlEscape(msg) + "</div>";

  html += "<div class='card'>";
  html += "<form method='POST' action='/save'>";

  html += "<div class='row'>";

  html += "<div>";
  html += "<label>Player IP (LAN)</label>";
  html += "<input name='playerIp' value='" + htmlEscape(cfg.playerIp) + "'>";
  html += "<label>Authorization (Basic ...)</label>";
  html += "<input name='basicAuth' value='" + htmlEscape(cfg.basicAuth) + "'>";
  html += "<label>Normal Playlist</label>";
  html += "<input name='normalPlaylist' value='" + htmlEscape(cfg.normalPlaylist) + "'>";
  html += "</div>";

  html += "<div>";
  html += "<label>NDWC JSON URL</label>";
  html += "<input name='capUrl' value='" + htmlEscape(cfg.capUrl) + "'>";
  html += "<label>Device ISO3166-2 (example TH-10). Leave empty = no filtering</label>";
  html += "<input name='deviceIso' value='" + htmlEscape(cfg.deviceIso) + "' placeholder='TH-10'>";
  html += "<label>Use Identifier for dedupe</label><select name='useIdentifier'>";
  html += String("<option value='1' ") + (cfg.useIdentifier ? "selected" : "") + ">ON</option>";
  html += String("<option value='0' ") + (!cfg.useIdentifier ? "selected" : "") + ">OFF</option>";
  html += "</select>";
  html += "<label>Allow trigger when upstream timeout/stale</label><select name='allowStaleTrigger'>";
  html += String("<option value='1' ") + (cfg.allowStaleTrigger ? "selected" : "") + ">ON</option>";
  html += String("<option value='0' ") + (!cfg.allowStaleTrigger ? "selected" : "") + ">OFF</option>";
  html += "</select>";
  html += "</div>";

  html += "</div>";

  html += "<div class='row'>";
  html += "<div>";
  html += "<label>Poll Interval (seconds)</label>";
  html += "<input name='pollSec' value='" + String(cfg.pollSec) + "'>";
  html += "<label>Cycles per NEW alert (1..10)</label>";
  html += "<input name='cycles' value='" + String(cfg.cycles) + "'>";
  html += "</div>";
  html += "<div>";
  html += "<label>Event Play Duration (seconds)</label>";
  html += "<input name='eventPlaySec' value='" + String(cfg.eventPlaySec) + "'>";
  html += "<label>Event Playlist Mode</label>";
  html += "<select name='eventPlayMode'>";
  html += "<option value='0'" + String(cfg.eventPlayMode==0?" selected":"") + ">Fixed duration</option>";
  html += "<option value='1'" + String(cfg.eventPlayMode==1?" selected":"") + ">Until alert expires</option>";
  html += "</select>";
  html += "<label>Normal Hold Duration (seconds)</label>";
  html += "<input name='normalHoldSec' value='" + String(cfg.normalHoldSec) + "'>";
  html += "</div>";
  html += "</div>";

  // TRIG server fields
  html += "<div class='row'>";
  html += "<div>";
  html += "<label>TRIG Server URL (base)</label>";
  html += "<input name='trigUrl' value='" + htmlEscape(cfg.trigUrl) + "' placeholder='http://x.x.x.x:8082'>";
  html += "<label>TRIG Token (X-Auth-Token)</label>";
  html += "<input name='trigToken' value='" + htmlEscape(cfg.trigToken) + "'>";
  html += "</div>";
  html += "<div>";
  html += "<label>Heartbeat Interval (seconds)</label>";
  html += "<input name='hbSec' value='" + String(cfg.hbSec) + "'>";
  html += "<div class='muted'>Applied Config Version: <b>" + String(cfg.appliedConfigVersion) + "</b></div>";
  html += "</div>";
  html += "</div>";

  // Event mapping table
  html += "<div class='card' style='margin:12px 0 0 0'>";
  html += "<h3 style='margin:0 0 6px 0;font-size:16px'>Event → Playlist Mapping</h3>";
  html += "<div class='muted'>Each event in the Excel list has its own playlist. Key comes from server as <b>playlistKey</b>.</div>";
  html += "<table>";
  html += "<tr><th style='width:80px'>Key</th><th>Event (TH / EN)</th><th style='width:110px'>Enable</th><th style='width:140px'>Min Severity</th><th>Playlist</th></tr>";
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    html += "<tr>";
    html += "<td class='k'>" + String(EVENT_DEFS[i].key) + "</td>";
    html += "<td>" + htmlEscape(String(EVENT_DEFS[i].th)) + "<div class='muted'>" + htmlEscape(String(EVENT_DEFS[i].en)) + "</div></td>";
    html += "<td><select name='evEn" + String(i) + "'>";
    html += String("<option value='1' ") + (cfg.evEnabled[i] ? "selected" : "") + ">ON</option>";
    html += String("<option value='0' ") + (!cfg.evEnabled[i] ? "selected" : "") + ">OFF</option>";
    html += "</select></td>";
    html += "<td><select name='evMin" + String(i) + "'>";
    for (int r=0; r<=4; r++) {
      html += "<option value='" + String(r) + "' " + String(cfg.evMinRank[i]==r ? "selected" : "") + ">" + sevLabel((uint8_t)r) + "</option>";
    }
    html += "</select></td>";
    html += "<td><input name='evPl" + String(i) + "' value='" + htmlEscape(cfg.evPlaylist[i]) + "' placeholder='Playlist name in signage'></td>";
    html += "</tr>";
  }
  html += "</table>";
  html += "</div>";

  html += "<div class='btns'>";
  html += "<button class='primary' type='submit'>Save Settings</button>";
  html += "</form>";

  html += "<form method='POST' action='/action' style='margin:0'><input type='hidden' name='cmd' value='poll'><button type='submit'>Poll Now</button></form>";
  html += "<form method='POST' action='/action' style='margin:0'><input type='hidden' name='cmd' value='hb'><button type='submit'>Send Heartbeat Now</button></form>";
  html += "<form method='POST' action='/action' style='margin:0'><input type='hidden' name='cmd' value='normal'><button type='submit'>Test: Play Normal</button></form>";
  html += "<form method='POST' action='/action' style='margin:0'><input type='hidden' name='cmd' value='testcap'><button class='danger' type='submit'>Test: Play From CAP</button></form>";
  html += "<form method='POST' action='/action' style='margin:0'><input type='hidden' name='cmd' value='clear'><button type='submit'>Clear last dedupe</button></form>";
  html += "</div>";

  html += "<div class='muted'>Device filtering uses <b>deviceIso</b> against alert geocodes (ISO3166-2). If empty, all devices trigger.</div>";
  html += "<div class='muted'>Auto trigger commits the dedupe key only after a successful event trigger.</div>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

/* ================= WiFi portal handlers ================= */
static void handleRoot() { wifiServer.send(200, "text/html", buildPage("")); }

static void handleSave() {
  if (wifiServer.hasArg("playerIp")) cfg.playerIp = wifiServer.arg("playerIp");
  if (wifiServer.hasArg("basicAuth")) cfg.basicAuth = wifiServer.arg("basicAuth");
  if (wifiServer.hasArg("normalPlaylist")) cfg.normalPlaylist = wifiServer.arg("normalPlaylist");

  if (wifiServer.hasArg("capUrl")) cfg.capUrl = wifiServer.arg("capUrl");
  if (wifiServer.hasArg("deviceIso")) cfg.deviceIso = wifiServer.arg("deviceIso");

  if (wifiServer.hasArg("useIdentifier")) cfg.useIdentifier = (wifiServer.arg("useIdentifier") == "1");
  if (wifiServer.hasArg("allowStaleTrigger")) cfg.allowStaleTrigger = (wifiServer.arg("allowStaleTrigger") == "1");

  if (wifiServer.hasArg("pollSec")) cfg.pollSec = clampU16(wifiServer.arg("pollSec").toInt(), 5, 3600);
  if (wifiServer.hasArg("cycles")) cfg.cycles = clampU8(wifiServer.arg("cycles").toInt(), 1, 10);
  if (wifiServer.hasArg("eventPlaySec")) cfg.eventPlaySec = clampU16(wifiServer.arg("eventPlaySec").toInt(), 5, 36000);
  if (wifiServer.hasArg("eventPlayMode")) cfg.eventPlayMode = clampU8(wifiServer.arg("eventPlayMode").toInt(), 0, 1);
  if (wifiServer.hasArg("normalHoldSec")) cfg.normalHoldSec = clampU16(wifiServer.arg("normalHoldSec").toInt(), 5, 36000);

  // TRIG server fields
  if (wifiServer.hasArg("trigUrl")) cfg.trigUrl = wifiServer.arg("trigUrl");
  if (wifiServer.hasArg("trigToken")) cfg.trigToken = wifiServer.arg("trigToken");
  if (wifiServer.hasArg("hbSec")) cfg.hbSec = clampU16(wifiServer.arg("hbSec").toInt(), 10, 3600);

  // event rows
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    String enKey = "evEn" + String(i);
    String plKey = "evPl" + String(i);
    if (wifiServer.hasArg(enKey)) cfg.evEnabled[i] = (wifiServer.arg(enKey) == "1");
    if (wifiServer.hasArg(plKey)) cfg.evPlaylist[i] = wifiServer.arg(plKey);
  
    String mnKey = "evMin" + String(i);
    if (wifiServer.hasArg(mnKey)) cfg.evMinRank[i] = clampU8(wifiServer.arg(mnKey).toInt(), 0, 4);
}

  saveConfig();
  wifiServer.send(200, "text/html", buildPage("Saved successfully."));
}

static void handleAction() {
  String cmd = wifiServer.arg("cmd");

  if (cmd == "poll") {
    CapResult r = pollCapUrl(cfg.capUrl, "ndwc");
    if (!r.ok) { wifiServer.send(200, "text/html", buildPage("poll failed: json-parse")); return; }
    String m = "ndwc: decisionActive=" + String(r.decisionActive ? "true" : "false")
             + " upstreamOk=" + String(r.upstreamOk ? "true" : "false")
             + " stale=" + String(r.stale ? "true" : "false")
             + " reason=" + r.reason
             + " id=" + r.identifier
             + " sev=" + r.severity + " rank=" + String(r.rank)
             + " key=" + r.playlistKey
             + " iso=" + r.isoCsv;
    wifiServer.send(200, "text/html", buildPage(m));
    return;
  }

  if (cmd == "hb") {
    sendHeartbeat();
    wifiServer.send(200, "text/html", buildPage("Heartbeat sent. Check /api/devices on server."));
    return;
  }

  if (cmd == "normal") {
    wifiServer.send(200, "text/html", buildPage(triggerPlaylist(cfg.normalPlaylist) ? "Normal playlist triggered." : "Normal trigger failed."));
    return;
  }

  if (cmd == "clear") {
    cfg.lastDedupeKey = "";
    saveConfig();
    wifiServer.send(200, "text/html", buildPage("last dedupe cleared."));
    return;
  }

  if (cmd == "testcap") {
    CapResult r = pollCapUrl(cfg.capUrl, "ndwc");
    if (!r.ok) { wifiServer.send(200, "text/html", buildPage("CAP poll failed.")); return; }

    if (!r.decisionActive) { wifiServer.send(200, "text/html", buildPage("No active alert (decision.active=false).")); return; }
    if (!matchesDeviceIso(r)) { wifiServer.send(200, "text/html", buildPage("Alert not for this deviceIso=" + cfg.deviceIso + " (iso=" + r.isoCsv + ")")); return; }

    if ((!r.upstreamOk || r.stale) && !cfg.allowStaleTrigger) {
      wifiServer.send(200, "text/html", buildPage("Upstream not OK / stale, trigger blocked (enable Allow Stale to override)."));
      return;
    }

    String pl = playlistForKey(r.playlistKey);
    if (pl.length() == 0) pl = r.playlist;


    int idx = eventIndexByKey(r.playlistKey);
    uint8_t minR = (idx >= 0) ? cfg.evMinRank[idx] : 0;
    if (r.rank < minR) {
      wifiServer.send(200, "text/html", buildPage("Severity " + r.severity + " below min " + sevLabel(minR) + " for key=" + r.playlistKey));
      return;
    }
    if (pl.length() == 0) {
      wifiServer.send(200, "text/html", buildPage("No playlist configured for key=" + r.playlistKey));
      return;
    }

    wifiServer.send(200, "text/html", buildPage(triggerPlaylist(pl) ? ("Triggered: " + pl) : "Trigger failed."));
    return;
  }

  wifiServer.send(200, "text/html", buildPage("Unknown action."));
}

static void handleStatus() {
  DynamicJsonDocument d(8192);
  d["deviceId"] = g_deviceId;

  d["wifiApIp"] = WiFi.softAPIP().toString();
  d["ethIp"] = (Ethernet.localIP() == INADDR_NONE) ? "0.0.0.0" : Ethernet.localIP().toString();
  d["ethLink"] = (Ethernet.linkStatus() == LinkON);

  d["runState"] = (int)runState;

  d["pendingIdentifier"] = pendingIdentifier;

  d["playerIp"] = cfg.playerIp;
  d["normalPlaylist"] = cfg.normalPlaylist;

  d["capUrl"] = cfg.capUrl;
  d["deviceIso"] = cfg.deviceIso;

  d["pollSec"] = cfg.pollSec;
  d["cycles"] = cfg.cycles;
  d["eventPlaySec"] = cfg.eventPlaySec;
  d["normalHoldSec"] = cfg.normalHoldSec;

  d["useIdentifier"] = cfg.useIdentifier;
  d["allowStaleTrigger"] = cfg.allowStaleTrigger;
  d["lastDedupeKey"] = cfg.lastDedupeKey;

  // events
  JsonArray arr = d.createNestedArray("events");
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    JsonObject o = arr.createNestedObject();
    o["key"] = EVENT_DEFS[i].key;
    o["enabled"] = cfg.evEnabled[i];
    o["playlist"] = cfg.evPlaylist[i];

    o["minRank"] = cfg.evMinRank[i];
  }

  d["trigUrl"] = cfg.trigUrl;
  d["hbSec"] = cfg.hbSec;
  d["appliedConfigVersion"] = cfg.appliedConfigVersion;

  String out;
  serializeJson(d, out);
  wifiServer.send(200, "application/json", out);
}

void handleApiConfig() {
  // Live config view (useful for confirming remote push applied)
  DynamicJsonDocument d(4096);
  d["playerIp"] = cfg.playerIp;
  d["normalPlaylist"] = cfg.normalPlaylist;
  d["capUrl"] = cfg.capUrl;
  d["deviceIso"] = cfg.deviceIso;
  d["pollSec"] = cfg.pollSec;
  d["cycles"] = cfg.cycles;
  d["eventPlaySec"] = cfg.eventPlaySec;
  d["normalHoldSec"] = cfg.normalHoldSec;
  d["useIdentifier"] = cfg.useIdentifier;
  d["allowStaleTrigger"] = cfg.allowStaleTrigger;
  d["trigUrl"] = cfg.trigUrl;
  d["hbSec"] = cfg.hbSec;
  d["appliedConfigVersion"] = cfg.appliedConfigVersion;

  JsonArray ev = d.createNestedArray("events");
  for (uint8_t i=0;i<EVENT_COUNT;i++) {
    JsonObject o = ev.createNestedObject();
    o["key"] = EVENT_DEFS[i].key;
    o["enabled"] = cfg.evEnabled[i];
    o["minRank"] = cfg.evMinRank[i];
    o["playlist"] = cfg.evPlaylist[i];
  }

  String out;
  serializeJson(d, out);
  wifiServer.send(200, "application/json", out);
}


static void startWifiPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gw(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gw, mask);

  Serial.print("WiFi AP SSID: "); Serial.println(AP_SSID);
  Serial.print("WiFi AP IP  : "); Serial.println(WiFi.softAPIP());

  wifiServer.on("/", HTTP_GET, handleRoot);
  wifiServer.on("/save", HTTP_POST, handleSave);
  wifiServer.on("/action", HTTP_POST, handleAction);
  wifiServer.on("/status", HTTP_GET, handleStatus);
  wifiServer.on("/api/config", HTTP_GET, handleApiConfig);
  wifiServer.begin();
}

/* ================= Auto trigger / cycles logic ================= */
static void startNewAlertRun(const CapResult& r, const String& eventKey, const String& playlist) {
  g_runCap = r;

  pendingIdentifier = r.identifier;
  pendingSource = "ndwc";
  pendingAreaDesc = "";

  // also store last values for heartbeat
  lastAlertIdSent = chooseDedupeKey(r);
  lastEventSent = eventKey;
  lastSevSent = r.severity;

  identifierCommitted = false;
  cyclesLeft = cfg.cycles;
  selectedEventName = eventKey;
  selectedEventPlaylist = playlist;

  Serial.println("AUTO: new alert -> key=" + chooseDedupeKey(r) +
                 " id=" + r.identifier +
                 " playlist=" + playlist +
                 " sent=" + r.sent +
                 " cycles=" + String(cyclesLeft));

  runState = EVENT_TRIGGER;
  stateTs = millis();
  lastTriggerAttemptMs = 0;
}

static void commitIdentifierIfNeeded() {
  if (identifierCommitted) return;

  cfg.lastDedupeKey = chooseDedupeKey(g_runCap);
  saveConfig();
  identifierCommitted = true;

  Serial.println("AUTO: committed lastDedupeKey=" + cfg.lastDedupeKey);
}


/* ================= setup / loop ================= */
void setup() {
  Serial.begin(115200);
  delay(300);

  led.begin();
  led.setBrightness(40);
  ledColor(0, 0, 20); // blue boot

  LittleFS.begin(true);
  loadConfig();

  g_deviceId = makeDeviceId();

  startWifiPortal();

  if (!startEthernet()) ledColor(40, 0, 0);
  else ledColor(0, 40, 0);

  Serial.println("Device ID   : " + g_deviceId);
  Serial.println("CAP URL     : " + cfg.capUrl);
  Serial.println("Device ISO  : " + cfg.deviceIso);
        Serial.println("Player IP   : " + cfg.playerIp);
  Serial.println("TRIG URL    : " + cfg.trigUrl);
}

void loop() {
  wifiServer.handleClient();
  uint32_t now = millis();

  // LED behavior
  if (runState == EVENT_WAIT) {
    if (now - lastBlinkMs >= 500) {
      lastBlinkMs = now;
      orangeBlinkOn = !orangeBlinkOn;
      if (orangeBlinkOn) ledColor(255, 90, 0);
      else ledColor(0, 0, 0);
    }
  } else {
    if (ethernetOk() && (runState == IDLE || runState == NORMAL_WAIT || runState == NORMAL_TRIGGER)) {
      static uint32_t lastGreen = 0;
      if (now - lastGreen > 1500) { lastGreen = now; ledColor(0, 40, 0); }
    }
  }

  // Ethernet down protection
  if (!ethernetOk()) {
    if (runState != IDLE) {
      Serial.println("AUTO: Ethernet down -> stop run");
      runState = IDLE;
      cyclesLeft = 0;
      pendingIdentifier = "";
      pendingSource = "";
      pendingAreaDesc = "";
      selectedEventName = "";
      selectedEventPlaylist = "";
      identifierCommitted = false;
    }
    ledColor(40, 0, 0);
    delay(10);
    return;
  }

  // ===== NEW: Heartbeat tick =====
  if ((now - lastHbMs) >= (uint32_t)cfg.hbSec * 1000UL) {
    lastHbMs = now;
    sendHeartbeat();
  }

  // ========= IDLE: poll NDWC JSON =========
  if (runState == IDLE) {
    if ((now - lastPollMs) >= (uint32_t)cfg.pollSec * 1000UL) {
      lastPollMs = now;

      CapResult r = pollCapUrl(cfg.capUrl, "ndwc");
      if (!r.ok) {
        Serial.println("CAP: poll failed: " + r.reason);
        return;
      }

      Serial.println("CAP: decisionActive=" + String(r.decisionActive ? "true" : "false") +
                    " upstreamOk=" + String(r.upstreamOk ? "true" : "false") +
                    " stale=" + String(r.stale ? "true" : "false") +
                    " id=" + r.identifier +
                    " key=" + r.playlistKey +
                    " iso=" + r.isoCsv +
                    " reason=" + r.reason);

      if (!r.decisionActive) return;

      if ((!r.upstreamOk || r.stale) && !cfg.allowStaleTrigger) {
        Serial.println("CAP: upstream not ok / stale -> blocked");
        return;
      }

      if (!matchesDeviceIso(r)) {
        Serial.println("CAP: not for this deviceIso=" + cfg.deviceIso + " (iso=" + r.isoCsv + ")");
        return;
      }

      // Determine playlist from key -> local mapping (preferred), fallback to server playlist
      String eventKey = r.playlistKey;
      if (eventKey.length() == 0) eventKey = "UNKNOWN";
      String pl = playlistForKey(eventKey);
      if (pl.length() == 0) pl = r.playlist;


      int idx = eventIndexByKey(eventKey);
      uint8_t minR = (idx >= 0) ? cfg.evMinRank[idx] : 0;
      if (r.rank < minR) {
        Serial.println("CAP: severity " + r.severity + " below min " + sevLabel(minR) + " for key=" + eventKey);
        return;
      }
      if (pl.length() == 0) {
        Serial.println("CAP: no playlist configured for key=" + eventKey);
        return;
      }

      String dedupeKey = chooseDedupeKey(r);
      if (cfg.lastDedupeKey.length() && dedupeKey == cfg.lastDedupeKey) {
        Serial.println("CAP: duplicate (dedupeKey=" + dedupeKey + ")");
        return;
      }

      // start run (commit dedupe key only after successful trigger)
      startNewAlertRun(r, eventKey, pl);
      return;
    }
    return;
  }

  // ========= EVENT_TRIGGER =========

// ========= EVENT_TRIGGER =========
  if (runState == EVENT_TRIGGER) {
    if (now - lastTriggerAttemptMs < 5000 && lastTriggerAttemptMs != 0) return;
    lastTriggerAttemptMs = now;

    Serial.println("AUTO: trigger event playlist -> " + selectedEventPlaylist);
    bool ok = triggerPlaylist(selectedEventPlaylist);
    if (!ok) {
      Serial.println("AUTO: event trigger failed (will retry).");
      return;
    }

    commitIdentifierIfNeeded();

    runState = EVENT_WAIT;
    stateTs = now;
    // duration for event playlist
    if (cfg.eventPlayMode == 1) {
      uint32_t holdSec = computeHoldSecondsUntilExpire(g_runCap.effective, g_runCap.expires);
      if (holdSec == 0) holdSec = cfg.eventPlaySec; // fallback
      eventWaitMs = holdSec * 1000UL;
    } else {
      eventWaitMs = (uint32_t)cfg.eventPlaySec * 1000UL;
    }
    orangeBlinkOn = false;
    lastBlinkMs = now;
    return;
  }

  // ========= EVENT_WAIT =========
  if (runState == EVENT_WAIT) {
    if (now - stateTs >= eventWaitMs) {
      runState = NORMAL_TRIGGER;
      lastTriggerAttemptMs = 0;
    }
    return;
  }

  // ========= NORMAL_TRIGGER =========
  if (runState == NORMAL_TRIGGER) {
    if (now - lastTriggerAttemptMs < 5000 && lastTriggerAttemptMs != 0) return;
    lastTriggerAttemptMs = now;

    Serial.println("AUTO: trigger normal playlist -> " + cfg.normalPlaylist);
    bool ok = triggerPlaylist(cfg.normalPlaylist);
    if (!ok) {
      Serial.println("AUTO: normal trigger failed (will retry).");
      return;
    }

    runState = NORMAL_WAIT;
    stateTs = now;
    return;
  }

  // ========= NORMAL_WAIT =========
  if (runState == NORMAL_WAIT) {
    if (now - stateTs >= (uint32_t)cfg.normalHoldSec * 1000UL) {
      if (cyclesLeft > 0) cyclesLeft--;

      if (cyclesLeft > 0) {
        Serial.println("AUTO: next cycle, remaining=" + String(cyclesLeft));
        runState = EVENT_TRIGGER;
        lastTriggerAttemptMs = 0;
        return;
      }

      Serial.println("AUTO: cycles complete, back to IDLE");
      runState = IDLE;
      pendingIdentifier = "";
      pendingSource = "";
      pendingAreaDesc = "";
      selectedEventName = "";
      selectedEventPlaylist = "";
      identifierCommitted = false;
      cyclesLeft = 0;
      return;
    }
  }
}
