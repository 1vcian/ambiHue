// AmbiHue per ESP32-S3 - Ambilight Philips -> lampade Hue
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include <vector>
#include <map>
#include <algorithm>
#include "lwip/sockets.h"
#include "mbedtls/ssl.h"
#include "esp_random.h"
#include "config.h"
#include "web_page.h"

// =====================================================================
// Utilità
// =====================================================================
static SemaphoreHandle_t gLock;
struct Guard {
  Guard() { xSemaphoreTake(gLock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(gLock); }
};

#define LOG(...) do { Serial.printf("[%8.1f] ", millis() / 1000.0); Serial.printf(__VA_ARGS__); Serial.println(); } while (0)

typedef std::vector<String> IdList;

static IdList sortIds(IdList v) {
  std::sort(v.begin(), v.end(), [](const String& a, const String& b) { return a.toInt() < b.toInt(); });
  return v;
}
static bool sameIds(const IdList& a, const IdList& b) { return sortIds(a) == sortIds(b); }
static bool hasId(const IdList& v, const String& id) { return std::find(v.begin(), v.end(), id) != v.end(); }
static String joinIds(const IdList& v) {
  String s;
  for (auto& x : v) { if (s.length()) s += ", "; s += x; }
  return s;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static bool validMode(const String& m) { return m == "auto" || m == "cinema" || m == "off"; }

// =====================================================================
// Stato condiviso (protetto da gLock)
// =====================================================================
struct Rgb { float r = 0, g = 0, b = 0; };
static const char* const SIDE_NAMES[4] = {"left", "top", "right", "bottom"};
static const int MAX_PTS = 24;
struct Layer { uint8_t n[4] = {0, 0, 0, 0}; Rgb p[4][MAX_PTS]; bool valid = false; };

struct Light {
  String id, name;
  // impostazioni
  String mode = "auto";    // "auto" sempre con la TV, "cinema" solo in modalità cinema, "off" mai
  String zone = "all";
  float gain = 1.0f;
  float maxLevel = 1.0f;   // tetto di luminosità (0,05-1)
  // posizione nell'area di intrattenimento
  bool hasLoc = false;
  float loc[3] = {0, 0, 0};
  String rid;        // id del servizio "entertainment" (API v2)
  String positions;  // posizioni v2 in JSON
  // stato
  float cur[3] = {0, 0, 0};
  bool shown = false;      // riceve lo streaming in questo momento
  uint8_t show[3] = {0, 0, 0};
  bool seenOn = false;     // il bridge l'ha data accesa durante lo streaming
  // com'era prima che la prendessimo (solo lampade "cinema"): comandi v1 pronti da rimandare
  String snapOld, snapNew; // penultima e ultima lettura
  String saved;
  bool taken = false;
};

struct Settings {
  bool autoStart = true;   // parte da sola quando la TV si accende
  float smoothing = 0.6f;
  float brightness = 1.0f;
  String restoreScene = DEFAULT_RESTORE_SCENE;
  String cinemaScene = DEFAULT_CINEMA_SCENE;
} gSet;

struct SceneInfo { String id, name, room; };

static std::vector<Light> gLights;
static Layer gLayer;
static uint8_t gCounts[3] = {3, 7, 3};
static int gTvOn = -1;              // -1 sconosciuto, 0 spenta, 1 accesa
static bool gTvReachable = true;
static bool gStreaming = false;
static uint32_t gStreamEpoch = 0;   // cambia a ogni apertura/chiusura dello streaming
static uint32_t gStreamSince = 0;
// sessione: vale finché la TV resta accesa, poi si torna al comportamento automatico
static int gSession = -1;           // -1 automatico, 0 messa in pausa, 1 accesa a mano
static bool gCinema = false;
static uint32_t gCinemaAt = 0;      // ultimo richiamo della scena cinema
static bool gRestart = false;       // il bridge ha chiuso lo streaming: va riaperto
static uint32_t gSessionAt = 0;
static bool gSessionWhileOff = false;
static bool gQuiet = false;         // app/Alexa hanno preso le lampade: fermarsi senza toccarle
static IdList gQuietOff;            // ...e queste vanno lasciate spente
static String gNote;
static String gScenesJson = "[]";
static std::vector<SceneInfo> gScenes;
static String gCinemaAuto;          // scena chiamata "Cinema", se esiste
static String gCfgId;               // id v2 della configurazione dell'area
static bool gDirty = false;
static uint32_t gDirtyAt = 0;

static Preferences prefs;
static WebServer server(80);

static Light* findLight(const String& id) {
  for (auto& l : gLights) if (l.id == id) return &l;
  return nullptr;
}
static void setNote(const String& n) { Guard g; gNote = n; }

// le funzioni seguenti vanno chiamate con gLock preso
static bool runWanted() { return gSession == -1 ? gSet.autoStart : gSession == 1; }
static bool cinemaOn() { return gCinema && gTvOn == 1; }
static bool lightWanted(const Light& l, bool cinema) { return l.mode == "auto" || (l.mode == "cinema" && cinema); }
static void sessionChanged() { gSessionAt = millis(); gSessionWhileOff = gTvOn != 1; }
static String cinemaSceneId() {
  if (gSet.cinemaScene == "-") return "";
  return gSet.cinemaScene.length() ? gSet.cinemaScene : gCinemaAuto;
}
static String sceneName(const String& id) {
  for (auto& s : gScenes) if (s.id == id) return s.name;
  return "";
}

// =====================================================================
// Colori e zone
// =====================================================================
static int sideIndex(const String& s) {
  for (int k = 0; k < 4; k++) if (s == SIDE_NAMES[k]) return k;
  return -1;
}

static bool validZone(const String& z) {
  if (z == "left" || z == "right" || z == "top" || z == "top-left" || z == "top-right" || z == "all") return true;
  int c = z.indexOf(':');
  if (c <= 0) return false;
  if (sideIndex(z.substring(0, c)) < 0) return false;
  String idx = z.substring(c + 1);
  if (!idx.length()) return false;
  for (size_t i = 0; i < idx.length(); i++) if (!isDigit(idx[i])) return false;
  return true;
}

static bool zoneColor(const Layer& L, const String& z, Rgb& out) {
  if (!L.valid) return false;
  int c = z.indexOf(':');
  if (c > 0) {
    int k = sideIndex(z.substring(0, c));
    int i = z.substring(c + 1).toInt();
    if (k < 0 || i < 0 || i >= L.n[k]) return false;
    out = L.p[k][i];
    return true;
  }
  float r = 0, g = 0, b = 0;
  int cnt = 0;
  auto add = [&](int k, int from, int to) {
    for (int i = from; i < to; i++) { r += L.p[k][i].r; g += L.p[k][i].g; b += L.p[k][i].b; cnt++; }
  };
  int nt = L.n[1];
  if (z == "left") add(0, 0, L.n[0]);
  else if (z == "right") add(2, 0, L.n[2]);
  else if (z == "top") add(1, 0, nt);
  else if (z == "top-left") add(1, 0, (nt + 1) / 2);
  else if (z == "top-right") add(1, nt / 2, nt);
  if (cnt == 0) for (int k = 0; k < 4; k++) add(k, 0, L.n[k]);   // "all" o zona vuota
  if (cnt == 0) return false;
  out.r = r / cnt; out.g = g / cnt; out.b = b / cnt;
  return true;
}

static float layerMax(const Layer& L) {
  float m = 0;
  for (int k = 0; k < 4; k++)
    for (int i = 0; i < L.n[k]; i++) m = max(m, max(L.p[k][i].r, max(L.p[k][i].g, L.p[k][i].b)));
  return m;
}

static bool parseLayer(const String& s, Layer& out) {
  JsonDocument d;
  if (deserializeJson(d, s)) return false;
  JsonObject l1 = d["layer1"];
  if (l1.isNull()) return false;
  Layer t;
  for (int k = 0; k < 4; k++) {
    JsonObject o = l1[SIDE_NAMES[k]];
    if (o.isNull()) continue;
    for (JsonPair kv : o) {
      int i = atoi(kv.key().c_str());
      if (i < 0 || i >= MAX_PTS) continue;
      JsonObject p = kv.value();
      t.p[k][i].r = p["r"] | 0;
      t.p[k][i].g = p["g"] | 0;
      t.p[k][i].b = p["b"] | 0;
      if (i + 1 > t.n[k]) t.n[k] = i + 1;
    }
  }
  t.valid = true;
  out = t;
  return true;
}

// =====================================================================
// HTTP verso il Hue Bridge
// =====================================================================
static bool v1Error(const String& s, String* desc = nullptr) {
  JsonDocument d;
  if (deserializeJson(d, s)) { if (desc) *desc = "risposta non valida"; return true; }
  if (d.is<JsonArray>() && d[0]["error"].is<JsonObject>()) {
    if (desc) *desc = d[0]["error"]["description"] | "errore";
    return true;
  }
  return false;
}

class HueApi {
 public:
  void init() { client.setInsecure(); }

  int request(const char* method, const String& url, const String& body, String& out,
              bool appKey = false, String* appIdOut = nullptr) {
    http.setReuse(true);
    http.setTimeout(5000);
    http.setConnectTimeout(5000);
    if (!http.begin(client, url)) { out = ""; return -1; }
    if (appKey) http.addHeader("hue-application-key", ENT_USER);
    if (body.length()) http.addHeader("Content-Type", "application/json");
    const char* keys[] = {"hue-application-id"};
    if (appIdOut) http.collectHeaders(keys, 1);
    int code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());
    out = code > 0 ? http.getString() : String();
    if (appIdOut) *appIdOut = http.header("hue-application-id");
    http.end();
    return code;
  }
  int v1(const char* method, const String& path, const String& body, String& out) {
    return request(method, String("https://") + BRIDGE_IP + "/api/" + ENT_USER + path, body, out);
  }
  int v2(const char* method, const String& path, const String& body, String& out) {
    return request(method, String("https://") + BRIDGE_IP + "/clip/v2/resource/" + path, body, out, true);
  }
  String appId() {
    String out, id;
    request("GET", String("https://") + BRIDGE_IP + "/auth/v1", "", out, true, &id);
    return id;
  }

 private:
  WiFiClientSecure client;
  HTTPClient http;
};

// =====================================================================
// HTTP verso la TV (HTTPS + autenticazione Digest)
// =====================================================================
static String md5hex(const String& s) {
  MD5Builder m;
  m.begin();
  m.add(s);
  m.calculate();
  return m.toString();
}

static String hdrParam(const String& h, const char* key) {
  String k = String(key) + "=";
  int i = 0;
  while (true) {
    i = h.indexOf(k, i);
    if (i < 0) return "";
    if (i > 0 && isAlphaNumeric(h[i - 1])) { i += k.length(); continue; }
    int s = i + k.length();
    if (s < (int)h.length() && h[s] == '"') {
      int e = h.indexOf('"', s + 1);
      return h.substring(s + 1, e < 0 ? h.length() : e);
    }
    int e = h.indexOf(',', s);
    String v = h.substring(s, e < 0 ? h.length() : e);
    v.trim();
    return v;
  }
}

class TvApi {
 public:
  void init() { client.setInsecure(); }

  int get(const String& path, String& out) {
    for (int attempt = 0; attempt < 2; attempt++) {
      http.setReuse(true);
      http.setTimeout(2500);
      http.setConnectTimeout(3000);
      if (!http.begin(client, String("https://") + TV_IP + ":1926" + path)) return -1;
      const char* keys[] = {"WWW-Authenticate"};
      http.collectHeaders(keys, 1);
      if (realm.length()) http.addHeader("Authorization", authHeader("GET", path));
      int code = http.GET();
      if (code == 401) {
        String h = http.header("WWW-Authenticate");
        http.getString();
        http.end();
        realm = hdrParam(h, "realm");
        nonce = hdrParam(h, "nonce");
        opaque = hdrParam(h, "opaque");
        qop = hdrParam(h, "qop");
        nc = 0;
        if (!realm.length()) return 401;
        continue;
      }
      out = code > 0 ? http.getString() : String();
      http.end();
      return code;
    }
    return 401;
  }

  // 1 = configurazione presente (TV accesa), 0 = vuota, -1 = TV irraggiungibile
  int config() {
    String s;
    int code = get("/6/ambilight/currentconfiguration", s);
    if (code < 0) return -1;
    s.trim();
    if (code != 200 || !s.length()) return 0;
    JsonDocument d;
    if (deserializeJson(d, s)) return 0;
    return (d.is<JsonObject>() && d.as<JsonObject>().size() > 0) ? 1 : 0;
  }

 private:
  String authHeader(const char* method, const String& uri) {
    String ha1 = md5hex(String(TV_USER) + ":" + realm + ":" + TV_PASS);
    String ha2 = md5hex(String(method) + ":" + uri);
    String h = String("Digest username=\"") + TV_USER + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
               "\", uri=\"" + uri + "\"";
    if (qop.indexOf("auth") >= 0) {
      char ncs[9];
      snprintf(ncs, sizeof ncs, "%08x", (unsigned)++nc);
      char cn[17];
      snprintf(cn, sizeof cn, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
      String resp = md5hex(ha1 + ":" + nonce + ":" + ncs + ":" + cn + ":auth:" + ha2);
      h += String(", qop=auth, nc=") + ncs + ", cnonce=\"" + cn + "\", response=\"" + resp + "\"";
    } else {
      h += ", response=\"" + md5hex(ha1 + ":" + nonce + ":" + ha2) + "\"";
    }
    if (opaque.length()) h += ", opaque=\"" + opaque + "\"";
    return h;
  }

  WiFiClientSecure client;
  HTTPClient http;
  String realm, nonce, opaque, qop;
  uint32_t nc = 0;
};

// =====================================================================
// Streaming DTLS verso il bridge (API Entertainment)
// =====================================================================
static int dtlsRng(void*, unsigned char* buf, size_t len) { esp_fill_random(buf, len); return 0; }

static int udpSend(void* ctx, const unsigned char* buf, size_t len) {
  int r = lwip_send(*(int*)ctx, buf, len, 0);
  if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;
}

static int udpRecvTimeout(void* ctx, unsigned char* buf, size_t len, uint32_t timeout) {
  int fd = *(int*)ctx;
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(fd, &rf);
  struct timeval tv;
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;
  int r = lwip_select(fd + 1, &rf, NULL, NULL, timeout == 0 ? NULL : &tv);
  if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
  if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  r = lwip_recv(fd, buf, len, 0);
  if (r < 0) return (errno == EAGAIN) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  return r;
}

struct DtlsTimer { uint32_t start = 0, intMs = 0, finMs = 0; };
static void timerSet(void* d, uint32_t intMs, uint32_t finMs) {
  auto t = (DtlsTimer*)d;
  t->intMs = intMs;
  t->finMs = finMs;
  t->start = millis();
}
static int timerGet(void* d) {
  auto t = (DtlsTimer*)d;
  if (t->finMs == 0) return -1;
  uint32_t e = millis() - t->start;
  if (e >= t->finMs) return 2;
  if (e >= t->intMs) return 1;
  return 0;
}

static const int PSK_CIPHERS[] = {MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0};

class DtlsStream {
 public:
  bool active = false;

  bool open(const String& identity) {
    close();
    size_t pskLen = hexToBytes(ENT_KEY, psk, sizeof psk);
    if (!pskLen) { LOG("ENT_KEY non valida"); return false; }

    fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(2100);
    inet_pton(AF_INET, BRIDGE_IP, &a.sin_addr);
    if (lwip_connect(fd, (struct sockaddr*)&a, sizeof a) < 0) { close(); return false; }

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    inited = true;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) { close(); return false; }
    mbedtls_ssl_conf_rng(&conf, dtlsRng, NULL);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ciphersuites(&conf, PSK_CIPHERS);
    mbedtls_ssl_conf_psk(&conf, psk, pskLen, (const unsigned char*)identity.c_str(), identity.length());
    mbedtls_ssl_conf_handshake_timeout(&conf, 400, 4000);
    mbedtls_ssl_conf_read_timeout(&conf, 1000);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) { close(); return false; }
    mbedtls_ssl_set_bio(&ssl, &fd, udpSend, NULL, udpRecvTimeout);
    mbedtls_ssl_set_timer_cb(&ssl, &timer, timerSet, timerGet);

    uint32_t t0 = millis();
    int r;
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
      bool retry = r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT;
      if (!retry || millis() - t0 > 8000) break;
      delay(1);
    }
    if (r != 0) {
      LOG("Connessione cifrata non riuscita (codice -0x%04x)", (unsigned)-r);
      close();
      return false;
    }
    active = true;
    return true;
  }

  bool send(const uint8_t* data, size_t len) {
    if (!active) return false;
    int r = mbedtls_ssl_write(&ssl, data, len);
    return r == (int)len;
  }

  void close() {
    if (inited) {
      if (active) mbedtls_ssl_close_notify(&ssl);
      mbedtls_ssl_free(&ssl);
      mbedtls_ssl_config_free(&conf);
      inited = false;
    }
    if (fd >= 0) { lwip_close(fd); fd = -1; }
    active = false;
  }

 private:
  static size_t hexToBytes(const char* hex, uint8_t* out, size_t max) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > max) return 0;
    for (size_t i = 0; i < n / 2; i++) {
      char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
      char* end;
      out[i] = (uint8_t)strtol(b, &end, 16);
      if (*end) return 0;
    }
    return n / 2;
  }

  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  DtlsTimer timer;
  uint8_t psk[32];
  int fd = -1;
  bool inited = false;
};

// =====================================================================
// Impostazioni salvate nella memoria dell'ESP32
// =====================================================================
static void loadSaved(JsonDocument& d) {
  auto read = [&](const char* key) {
    size_t n = prefs.getBytesLength(key);
    if (!n) return false;
    std::vector<char> buf(n + 1, 0);
    prefs.getBytes(key, buf.data(), n);
    if (deserializeJson(d, buf.data())) { d.clear(); return false; }
    return true;
  };
  if (read("cfg2")) return;
  // versione precedente: si tengono solo le posizioni delle lampade, le impostazioni ripartono da config.h
  if (read("cfg")) {
    JsonDocument area;
    area.set(d["area"]);
    d.clear();
    d["area"] = area;
  }
}

static void saveSettings() {  // da chiamare con gLock preso
  JsonDocument d;
  d["auto_start"] = gSet.autoStart;
  d["smoothing"] = gSet.smoothing;
  d["brightness"] = gSet.brightness;
  d["restore_scene"] = gSet.restoreScene;
  d["cinema_scene"] = gSet.cinemaScene;
  JsonObject ls = d["lights"].to<JsonObject>();
  JsonObject ar = d["area"].to<JsonObject>();
  for (auto& l : gLights) {
    JsonObject o = ls[l.id].to<JsonObject>();
    o["mode"] = l.mode;
    o["zone"] = l.zone;
    o["gain"] = l.gain;
    o["max_level"] = l.maxLevel;
    JsonObject a = ar[l.id].to<JsonObject>();
    a["name"] = l.name;
    if (l.hasLoc) { JsonArray la = a["loc"].to<JsonArray>(); for (int i = 0; i < 3; i++) la.add(l.loc[i]); }
    if (l.rid.length()) a["rid"] = l.rid;
    if (l.positions.length()) a["positions"] = l.positions;
  }
  String s;
  serializeJson(d, s);
  prefs.putBytes("cfg2", s.c_str(), s.length());
}

// =====================================================================
// Scoperta dell'area di intrattenimento
// =====================================================================
static bool discoverArea(HueApi& hue, JsonDocument& saved, IdList& members) {
  String s;
  int code = hue.v1("GET", String("/groups/") + ENT_GROUP, "", s);
  JsonDocument g;
  if (code != 200 || deserializeJson(g, s) || !g["type"].is<const char*>()) {
    LOG("Area %s non leggibile (HTTP %d): %s", ENT_GROUP, code, s.substring(0, 120).c_str());
    return false;
  }
  if (String(g["type"].as<const char*>()) != "Entertainment") {
    LOG("Il gruppo %s non è un'area di intrattenimento", ENT_GROUP);
    return false;
  }
  members.clear();
  for (JsonVariant v : g["lights"].as<JsonArray>()) members.push_back(v.as<String>());

  JsonDocument names, nf;
  nf["*"]["name"] = true;
  hue.v1("GET", "/lights", "", s);
  deserializeJson(names, s, DeserializationOption::Filter(nf));

  std::map<String, String> ridOf, posOf;   // id lampada -> rid ; rid -> posizioni
  if (hue.v2("GET", "entertainment", "", s) == 200) {
    JsonDocument ef, ed;
    ef["data"][0]["id"] = true;
    ef["data"][0]["id_v1"] = true;
    if (!deserializeJson(ed, s, DeserializationOption::Filter(ef))) {
      for (JsonObject e : ed["data"].as<JsonArray>()) {
        String v1 = e["id_v1"] | "";
        if (v1.startsWith("/lights/")) ridOf[v1.substring(8)] = e["id"].as<String>();
      }
    }
  }
  if (hue.v2("GET", "entertainment_configuration", "", s) == 200) {
    JsonDocument cd;
    if (!deserializeJson(cd, s)) {
      for (JsonObject c : cd["data"].as<JsonArray>()) {
        if (String(c["id_v1"] | "") != String("/groups/") + ENT_GROUP) continue;
        gCfgId = c["id"].as<String>();
        JsonArray sls = c["locations"]["service_locations"];
        for (JsonObject sl : sls) {
          String pos;
          if (sl["positions"].is<JsonArray>()) serializeJson(sl["positions"], pos);
          else { JsonDocument pd; pd.add(sl["position"]); serializeJson(pd, pos); }
          posOf[sl["service"]["rid"].as<String>()] = pos;
        }
      }
    }
  }

  Guard lk;
  gLights.clear();
  // prima le lampade ricordate (anche quelle tolte dall'area in una sessione precedente)
  for (JsonPair kv : saved["area"].as<JsonObject>()) {
    Light l;
    l.id = kv.key().c_str();
    JsonObject a = kv.value();
    l.name = a["name"] | String("Lampada " + l.id);
    if (a["loc"].is<JsonArray>()) { l.hasLoc = true; for (int i = 0; i < 3; i++) l.loc[i] = a["loc"][i] | 0.0f; }
    l.rid = a["rid"] | "";
    l.positions = a["positions"] | "";
    gLights.push_back(l);
  }
  // poi quelle trovate ora nell'area
  for (auto& id : members) {
    Light* l = findLight(id);
    if (!l) { Light n; n.id = id; gLights.push_back(n); l = &gLights.back(); }
    l->name = names[id]["name"] | (l->name.length() ? l->name : String("Lampada " + id));
    JsonArray loc = g["locations"][id];
    if (!loc.isNull()) { l->hasLoc = true; for (int i = 0; i < 3; i++) l->loc[i] = loc[i] | 0.0f; }
    if (ridOf.count(id)) {
      l->rid = ridOf[id];
      if (posOf.count(l->rid)) l->positions = posOf[l->rid];
    }
  }
  // impostazioni salvate o predefinite
  for (auto& l : gLights) {
    for (auto& dl : DEFAULT_LIGHTS) {
      if (l.id != dl.id) continue;
      if (validZone(dl.zone)) l.zone = dl.zone;
      if (validMode(dl.mode)) l.mode = dl.mode;
      l.maxLevel = clampf(dl.maxLevel, 0.05f, 1.0f);
    }
    JsonObject o = saved["lights"][l.id];
    if (o.isNull()) continue;
    String m = o["mode"] | "";
    if (validMode(m)) l.mode = m;
    String z = o["zone"] | "";
    if (validZone(z)) l.zone = z;
    l.gain = clampf(o["gain"] | 1.0f, 0.1f, 2.0f);
    l.maxLevel = clampf(o["max_level"] | l.maxLevel, 0.05f, 1.0f);
  }
  if (saved["auto_start"].is<bool>()) gSet.autoStart = saved["auto_start"].as<bool>();
  if (saved["smoothing"].is<float>()) gSet.smoothing = clampf(saved["smoothing"].as<float>(), 0, 0.95f);
  if (saved["brightness"].is<float>()) gSet.brightness = clampf(saved["brightness"].as<float>(), 0.3f, 3.0f);
  if (saved["restore_scene"].is<const char*>()) gSet.restoreScene = saved["restore_scene"].as<String>();
  if (saved["cinema_scene"].is<const char*>()) gSet.cinemaScene = saved["cinema_scene"].as<String>();
  saveSettings();
  return true;
}

// =====================================================================
// Motore: streaming, area, scena (gira sul core 1)
// =====================================================================
class Engine {
 public:
  void begin(const IdList& m) { members = sortIds(m); hue.init(); }

  void run() {
    for (;;) {
      uint32_t t0 = millis();

      // fotografia delle impostazioni
      bool run, quiet = false, restart;
      int tvOn;
      String scene;
      IdList desired, allLights, quietOff;
      {
        Guard g;
        run = runWanted();
        tvOn = gTvOn;
        scene = gSet.restoreScene;
        bool cin = cinemaOn();
        if (gQuiet) { quiet = true; quietOff = gQuietOff; gQuiet = false; gQuietOff.clear(); }
        restart = gRestart;
        gRestart = false;
        for (auto& l : gLights) {
          allLights.push_back(l.id);
          if (!lightWanted(l, cin)) continue;
          desired.push_back(l.id);
          if (l.mode == "cinema" && !l.taken) {   // ci ricordiamo com'era prima di prenderla
            l.saved = l.snapOld.length() ? l.snapOld : l.snapNew;
            l.taken = true;
          }
        }
      }
      desired = sortIds(desired);
      allLights = sortIds(allLights);
      bool want = run && tvOn == 1 && !desired.empty();

      if (quiet && (dtls.active || session)) {
        LOG("Lampade prese dall'app o da Alexa: sincronizzazione in pausa.");
        stopQuiet(quietOff);
      }

      if (restart && dtls.active) { closeStream(); retryAt = millis() + 1000; }

      // lampade incluse/escluse cambiate: si ferma lo streaming e si rifà l'area
      if (want && dtls.active && desired != members && !(hasFailed && desired == failed)) {
        closeStream();
      }

      if (want && !dtls.active && (int32_t)(t0 - retryAt) >= 0) {
        if (desired != members && !(hasFailed && desired == failed)) {
          setMembersSafe(desired);
          delay(300);
          continue;   // si riparte dopo AREA_SETTLE_MS
        }
        if (openStream()) {
          if (members == desired) setNote("");
          Guard g;
          for (auto& l : gLights) { l.cur[0] = l.cur[1] = l.cur[2] = 0; }
          session = true;
          LOG("Sincronizzazione attiva: %s", joinIds(members).c_str());
        } else {
          retryAt = millis() + 10000;
        }
      } else if (!want && (dtls.active || session)) {
        LOG("Sincronizzazione in pausa.");
        stopStream(scene, allLights);
      } else if (!want && !dtls.active && members != allLights && (int32_t)(t0 - fullRetryAt) >= 0) {
        if (!setMembersSafe(allLights)) fullRetryAt = t0 + 30000;
      }

      releaseLights(want ? desired : IdList());

      uint32_t period = 300;
      if (dtls.active) {
        sendFrame();
        period = 1000 / STREAM_FPS;
      }
      uint32_t el = millis() - t0;
      delay(el < period ? period - el : 1);
    }
  }

 private:
  bool setStream(bool on) {
    String out;
    int code = hue.v1("PUT", String("/groups/") + ENT_GROUP, on ? "{\"stream\":{\"active\":true}}" : "{\"stream\":{\"active\":false}}", out);
    String desc;
    if (code != 200 || v1Error(out, &desc)) {
      if (on) setNote("Il bridge ha rifiutato l'attivazione dell'area: " + desc);
      return false;
    }
    return true;
  }

  bool openStream() {
    IdList cands;
    if (identity.length()) cands.push_back(identity);
    String app = hue.appId();
    if (app.length() && !hasId(cands, app)) cands.push_back(app);
    if (!hasId(cands, String(ENT_USER))) cands.push_back(ENT_USER);
    for (auto& id : cands) {
      if (!setStream(true)) return false;
      if (dtls.open(id)) {
        identity = id;
        Guard g;
        gStreaming = true;
        gStreamEpoch++;
        gStreamSince = millis();
        return true;
      }
      setStream(false);
      delay(1000);
    }
    setNote("Connessione cifrata con il bridge non riuscita, nuovo tentativo tra poco.");
    return false;
  }

  void closeStream() {
    {   // prima si avvisa il controllo esterno, così non scambia la nostra chiusura per quella di un'app
      Guard g;
      gStreaming = false;
      gStreamEpoch++;
      for (auto& l : gLights) l.shown = false;
    }
    dtls.close();
    setStream(false);
    closedAt = millis();
  }

  void sendFrame() {
    std::vector<uint8_t> msg = {'H', 'u', 'e', 'S', 't', 'r', 'e', 'a', 'm', 1, 0, 0, 0, 0, 0, 0};
    {
      Guard g;
      float k = gSet.smoothing;
      bool cin = cinemaOn();
      for (auto& id : members) {
        Light* l = findLight(id);
        if (!l) continue;
        bool on = lightWanted(*l, cin);
        float target[3];
        if (on) {
          Rgb c;
          if (zoneColor(gLayer, l->zone, c)) {
            float gn = l->gain * gSet.brightness;
            target[0] = c.r * gn; target[1] = c.g * gn; target[2] = c.b * gn;
            float cap = 255.0f * l->maxLevel;
            float top = max(target[0], max(target[1], target[2]));
            if (top > cap) for (int i = 0; i < 3; i++) target[i] *= cap / top;   // stessa tinta, meno luce
          } else {
            for (int i = 0; i < 3; i++) target[i] = l->cur[i];
          }
        } else {   // solo se il bridge ha rifiutato di toglierla dall'area
          target[0] = 255 * 0.4f; target[1] = 180 * 0.4f; target[2] = 110 * 0.4f;
        }
        for (int i = 0; i < 3; i++) l->cur[i] = l->cur[i] * k + target[i] * (1 - k);
        int lid = l->id.toInt();
        msg.push_back(0);
        msg.push_back((lid >> 8) & 0xff);
        msg.push_back(lid & 0xff);
        for (int i = 0; i < 3; i++) {
          uint16_t v = (uint16_t)(clampf(l->cur[i], 0, 255) / 255.0f * 65535);
          msg.push_back(v >> 8);
          msg.push_back(v & 0xff);
        }
        l->shown = on;
        for (int i = 0; i < 3; i++) l->show[i] = (uint8_t)clampf(l->cur[i], 0, 255);
      }
    }
    if (!dtls.send(msg.data(), msg.size())) {
      setNote("Streaming interrotto, riprovo.");
      LOG("Streaming interrotto");
      closeStream();
      retryAt = millis() + 5000;
    }
  }

  bool membersOk(const IdList& lids) {
    delay(500);
    String s;
    if (hue.v1("GET", String("/groups/") + ENT_GROUP, "", s) != 200) return false;
    JsonDocument g;
    if (deserializeJson(g, s)) return false;
    IdList now;
    for (JsonVariant v : g["lights"].as<JsonArray>()) now.push_back(v.as<String>());
    if (!sameIds(now, lids)) return false;
    if (!gCfgId.length() || hue.v2("GET", "entertainment_configuration/" + gCfgId, "", s) != 200) return true;
    JsonDocument cd;
    if (deserializeJson(cd, s)) return true;
    IdList ch;
    JsonArray chans = cd["data"][0]["channels"];
    for (JsonObject c : chans) {
      JsonArray mems = c["members"];
      for (JsonObject m : mems) {
        String rid = m["service"]["rid"] | "";
        Guard gl;
        for (auto& l : gLights)
          if (l.rid == rid && !hasId(ch, l.id)) ch.push_back(l.id);
      }
    }
    return sameIds(ch, lids);
  }

  bool setMembers(const IdList& lids, String& err) {
    // prima l'API v2, quella che il bridge usa per lo streaming
    bool v2ok = gCfgId.length() > 0;
    JsonDocument b2;
    JsonArray sl = b2["locations"]["service_locations"].to<JsonArray>();
    {
      Guard g;
      for (auto& id : lids) {
        Light* l = findLight(id);
        if (!l || !l->rid.length() || !l->positions.length()) { v2ok = false; break; }
        JsonObject o = sl.add<JsonObject>();
        o["service"]["rid"] = l->rid;
        o["service"]["rtype"] = "entertainment";
        o["positions"] = serialized(l->positions);
      }
    }
    String body, out;
    if (v2ok) {
      serializeJson(b2, body);
      int code = hue.v2("PUT", "entertainment_configuration/" + gCfgId, body, out);
      if (code > 0 && code < 400 && membersOk(lids)) return true;
      err = "v2: HTTP " + String(code);
    }
    // riserva: API v1
    JsonDocument b1;
    JsonArray la = b1["lights"].to<JsonArray>();
    JsonObject lo = b1["locations"].to<JsonObject>();
    {
      Guard g;
      for (auto& id : lids) {
        la.add(id);
        Light* l = findLight(id);
        if (l && l->hasLoc) { JsonArray p = lo[id].to<JsonArray>(); for (int i = 0; i < 3; i++) p.add(l->loc[i]); }
      }
    }
    body = "";
    serializeJson(b1, body);
    int code = hue.v1("PUT", String("/groups/") + ENT_GROUP, body, out);
    String desc;
    if (code == 200 && !v1Error(out, &desc) && membersOk(lids)) return true;
    err += (err.length() ? "; " : "") + String("v1: ") + (desc.length() ? desc : "area non aggiornata");
    return false;
  }

  bool setMembersSafe(const IdList& lids) {
    if (lids == members) return true;
    String err;
    if (setMembers(lids, err)) {
      members = lids;
      hasFailed = false;
      retryAt = millis() + AREA_SETTLE_MS;   // il bridge deve preparare le lampade
      LOG("Area aggiornata: %s", joinIds(lids).c_str());
      return true;
    }
    failed = lids;
    hasFailed = true;
    setNote("Non riesco a liberare le lampade escluse: " + err);
    LOG("Modifica area non riuscita: %s", err.c_str());
    return false;
  }

  String recall(const String& sceneId) {
    String s;
    if (hue.v1("GET", "/scenes/" + sceneId, "", s) != 200) return "";
    JsonDocument f, d;
    f["name"] = true;
    f["group"] = true;
    if (deserializeJson(d, s, DeserializationOption::Filter(f)) || !d["name"].is<const char*>()) {
      setNote("Scena non trovata sul bridge: sceglila di nuovo nell'interfaccia.");
      return "";
    }
    String group = d["group"] | "0";
    String out, desc;
    int code = hue.v1("PUT", "/groups/" + group + "/action", "{\"scene\":\"" + sceneId + "\"}", out);
    if (code != 200 || v1Error(out, &desc)) { setNote("Impossibile richiamare la scena: " + desc); return ""; }
    return d["name"].as<String>();
  }

  void restoreScene(const String& sceneId, bool afterStream) {
    if (!sceneId.length()) { LOG("Nessuna scena da richiamare: sceglila in Altre impostazioni."); return; }
    if (afterStream) delay(1500);   // il bridge prima rimette le lampade com'erano
    String name = recall(sceneId);
    if (name.length() && afterStream) { delay(2500); recall(sceneId); }
    if (name.length()) LOG("Scena \"%s\" richiamata.", name.c_str());
  }

  // Lampade "cinema" non più sincronizzate: tornano com'erano prima (dopo l'eventuale scena)
  void releaseLights(const IdList& keep) {
    if (millis() - closedAt < 2500) return;   // il bridge sta ancora rimettendo a posto le lampade
    std::vector<std::pair<String, String>> todo;
    {
      Guard g;
      for (auto& l : gLights) {
        if (!l.taken || hasId(keep, l.id)) continue;
        if (dtls.active && hasId(members, l.id)) continue;   // ancora nello streaming: prima va tolta
        if (l.saved.length()) todo.push_back({l.id, l.saved});
        l.taken = false;
        l.saved = "";
      }
    }
    for (auto& t : todo) {
      String out;
      int code = hue.v1("PUT", "/lights/" + t.first + "/state", t.second, out);
      if (code == 200 && !v1Error(out)) LOG("Lampada %s riportata com'era prima.", t.first.c_str());
      else LOG("Non riesco a riportare la lampada %s com'era (HTTP %d).", t.first.c_str(), code);
    }
  }

  void stopStream(const String& scene, const IdList& allLights) {
    bool was = dtls.active;
    if (was) closeStream();
    session = false;
    setMembersSafe(allLights);   // l'area torna completa, come nell'app Hue
    restoreScene(scene, was);
    releaseLights(IdList());
  }

  // Le lampade le ha prese l'app Hue o Alexa: ci si ferma senza richiamare scene né ripristinare
  void stopQuiet(const IdList& keepOff) {
    bool was = dtls.active;
    if (was) closeStream();
    session = false;
    {
      Guard g;
      for (auto& l : gLights) { l.taken = false; l.saved = ""; }
    }
    if (!keepOff.empty()) {
      if (was) delay(1500);   // il bridge rimette le lampade com'erano prima dello streaming...
      for (auto& id : keepOff) {   // ...ma erano state spente: restano spente
        String out;
        hue.v1("PUT", "/lights/" + id + "/state", "{\"on\":false}", out);
      }
    }
  }

  HueApi hue;
  DtlsStream dtls;
  IdList members, failed;
  bool hasFailed = false;
  bool session = false;
  String identity;
  uint32_t retryAt = 0, fullRetryAt = 0, closedAt = 0;
};

static Engine gEngine;

// =====================================================================
// TV (core 0): accensione e colori Ambilight
// =====================================================================
static void tvTask(void*) {
  TvApi tv;
  tv.init();
  bool seenConfig = false;
  uint32_t nextCheck = 0, darkSince = 0, offSince = 0;

  for (;;) {
    uint32_t now = millis();
    if ((int32_t)(now - nextCheck) >= 0) {
      nextCheck = now + POWER_CHECK_MS;
      int cfg = tv.config();
      if (cfg == 1 && !seenConfig) { seenConfig = true; LOG("TV accesa (configurazione Ambilight presente)."); }
      Guard g;
      gTvReachable = cfg != -1;
      if (cfg == -1) { gTvOn = 0; gLayer.valid = false; }
      else if (seenConfig) { gTvOn = cfg; if (!cfg) gLayer.valid = false; }
    }

    bool reachable, on;
    { Guard g; reachable = gTvReachable; on = gTvOn == 1; }
    bool fast = false;
    if (reachable && !(seenConfig && !on)) {
      String s;
      Layer L;
      int code = tv.get(String("/6/ambilight/") + TV_ENDPOINT, s);
      if (code == 200 && parseLayer(s, L)) {
        Guard g;
        gLayer = L;
        for (int k = 0; k < 3; k++) if (L.n[k]) gCounts[k] = L.n[k];
        if (!seenConfig) {   // riserva: decidiamo dai colori
          if (layerMax(L) == 0) {
            if (!darkSince) darkSince = now ? now : 1;
            if (now - darkSince >= DARK_OFF_MS) gTvOn = 0;
          } else {
            darkSince = 0;
            gTvOn = 1;
          }
        }
        fast = gStreaming;
      } else {
        if (!seenConfig) { Guard g; gTvOn = 0; gLayer.valid = false; }
        delay(2000);
      }
    }

    {   // TV spenta da un minuto: pausa e modalità cinema si azzerano (resta solo una scena
        // cinema richiamata a TV spenta, che vale per CINEMA_ARM_MS)
      Guard g;
      now = millis();
      if (gTvOn == 1) offSince = 0;
      else if (!offSince) offSince = now ? now : 1;
      bool custom = gSession != -1 || gCinema;
      uint32_t hold = gSessionWhileOff ? CINEMA_ARM_MS : 60000;
      if (custom && offSince && now - offSince > 60000 && now - gSessionAt > hold) {
        gSession = -1;
        gCinema = false;
        LOG("TV spenta: si torna al funzionamento automatico.");
      }
    }

    uint32_t period = fast ? 1000 / TV_FPS : 1000;
    uint32_t el = millis() - now;
    delay(el < period ? period - el : 1);
  }
}

// =====================================================================
// Controllo del bridge (core 0): scena cinema, stato delle lampade, elenco scene
// =====================================================================
static void refreshScenes(HueApi& hue) {
  String s;
  if (hue.v1("GET", "/scenes", "", s) != 200) return;
  JsonDocument sf, sd;
  sf["*"]["name"] = true;
  sf["*"]["group"] = true;
  if (deserializeJson(sd, s, DeserializationOption::Filter(sf)) || !sd.is<JsonObject>()) return;
  JsonDocument gf, gd;
  gf["*"]["name"] = true;
  if (hue.v1("GET", "/groups", "", s) == 200) deserializeJson(gd, s, DeserializationOption::Filter(gf));
  std::vector<SceneInfo> list;
  String cinema;
  for (JsonPair kv : sd.as<JsonObject>()) {
    SceneInfo x;
    x.id = kv.key().c_str();
    x.name = kv.value()["name"] | "";
    String grp = kv.value()["group"] | "";
    x.room = grp.length() ? (gd[grp]["name"] | "") : "";
    list.push_back(x);
  }
  std::sort(list.begin(), list.end(), [](const SceneInfo& a, const SceneInfo& b) {
    String ra = a.room, rb = b.room, na = a.name, nb = b.name;
    ra.toLowerCase(); rb.toLowerCase(); na.toLowerCase(); nb.toLowerCase();
    return ra != rb ? ra < rb : na < nb;
  });
  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (auto& x : list) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = x.id;
    o["name"] = x.name;
    o["room"] = x.room;
    String n = x.name;
    n.trim();
    if (!cinema.length() && n.equalsIgnoreCase("cinema")) cinema = x.id;
  }
  String js;
  serializeJson(out, js);
  Guard g;
  gScenesJson = js;
  gScenes = list;
  gCinemaAuto = cinema;
}

// id v1 della scena -> id v2 (serve per leggere quando è stata richiamata)
static String sceneV2Id(HueApi& hue, const String& v1id) {
  String s;
  if (hue.v2("GET", "scene", "", s) != 200) return "";
  JsonDocument f, d;
  f["data"][0]["id"] = true;
  f["data"][0]["id_v1"] = true;
  if (deserializeJson(d, s, DeserializationOption::Filter(f))) return "";
  for (JsonObject sc : d["data"].as<JsonArray>())
    if (String(sc["id_v1"] | "") == "/scenes/" + v1id) return sc["id"].as<String>();
  return "";
}

// Comando v1 che riporta la lampada allo stato letto
static String restoreBody(JsonObject st) {
  if (!(st["on"] | false)) return "{\"on\":false,\"transitiontime\":4}";
  JsonDocument b;
  b["on"] = true;
  b["bri"] = st["bri"] | 254;
  String cm = st["colormode"] | "";
  if (cm == "xy" && st["xy"].is<JsonArray>()) {
    JsonArray xy = b["xy"].to<JsonArray>();
    xy.add(st["xy"][0].as<float>());
    xy.add(st["xy"][1].as<float>());
  } else if (cm == "ct" && st["ct"].is<int>()) {
    b["ct"] = st["ct"].as<int>();
  } else if (cm == "hs" && st["hue"].is<int>()) {
    b["hue"] = st["hue"].as<int>();
    b["sat"] = st["sat"] | 254;
  }
  b["transitiontime"] = 4;
  String out;
  serializeJson(b, out);
  return out;
}

// App Hue o Alexa hanno preso le lampade durante la sincronizzazione
static void takeover(uint32_t epoch, const IdList& off, const char* why) {
  Guard g;
  if (!gStreaming || gStreamEpoch != epoch) return;   // nel frattempo lo streaming l'abbiamo chiuso noi
  if (millis() - gCinemaAt < 15000) {   // è stata appena richiamata la scena cinema: si riparte
    gRestart = true;
    return;
  }
  gSession = 0;
  gCinema = false;
  sessionChanged();
  gQuiet = true;
  gQuietOff = off;
  gNote = String("Lampade ") + why + ": sincronizzazione in pausa fino al prossimo spegnimento della TV "
          "(o finché non la riattivi).";
  LOG("%s", gNote.c_str());
}

static void watchTask(void*) {
  HueApi hue;
  hue.init();
  uint32_t scenesAt = 0, resolveAt = 0, groupAt = 0, seenEpoch = 0;
  bool scenesLoaded = false, baseline = false;
  String sceneV1, sceneV2, lastRecall, lastActive;

  for (;;) {
    uint32_t now = millis();
    if (!scenesLoaded || now - scenesAt > 300000) {
      refreshScenes(hue);
      scenesAt = millis();
      scenesLoaded = true;
    }

    // 1) scena cinema: quando qualcuno la richiama (app, Alexa, interruttore) parte la modalità cinema
    String want;
    { Guard g; want = cinemaSceneId(); }
    if (want != sceneV1) { sceneV1 = want; sceneV2 = ""; resolveAt = 0; }
    if (sceneV1.length() && !sceneV2.length() && (int32_t)(millis() - resolveAt) >= 0) {
      sceneV2 = sceneV2Id(hue, sceneV1);
      baseline = false;
      resolveAt = millis() + 30000;
      if (!sceneV2.length()) LOG("Scena cinema %s non trovata nell'API v2, riprovo.", sceneV1.c_str());
    }
    if (sceneV2.length()) {
      String s;
      int code = hue.v2("GET", "scene/" + sceneV2, "", s);
      JsonDocument f, d;
      f["data"][0]["status"] = true;
      if (code == 404) sceneV2 = "";
      else if (code == 200 && !deserializeJson(d, s, DeserializationOption::Filter(f))) {
        JsonObject st = d["data"][0]["status"];
        String rec = st["last_recall"] | "";
        String act = st["active"] | "";
        bool fired = false;
        if (baseline) {
          if (rec.length()) fired = rec != lastRecall;
          else fired = act.length() && act != "inactive" && lastActive == "inactive";   // bridge senza last_recall
        }
        baseline = true;
        lastRecall = rec;
        lastActive = act;
        if (fired) {
          Guard g;
          gCinema = true;
          gCinemaAt = millis();
          gSession = 1;
          sessionChanged();
          gNote = "";
          LOG("%s", gTvOn == 1 ? "Scena cinema richiamata: modalità cinema attiva."
                               : "Scena cinema richiamata a TV spenta: modalità cinema pronta per quando accendi la TV.");
        }
      }
    }

    // 2) stato delle lampade: com'erano (per poterle rimettere così) e se qualcuno le ha spente
    {
      String s;
      JsonDocument f, d;
      bool fireOff = false;
      uint32_t offEpoch = 0;
      IdList off;
      f["*"]["state"] = true;
      if (hue.v1("GET", "/lights", "", s) == 200 && !deserializeJson(d, s, DeserializationOption::Filter(f))) {
        Guard g;
        bool streaming = gStreaming;
        if (gStreamEpoch != seenEpoch) { seenEpoch = gStreamEpoch; for (auto& l : gLights) l.seenOn = false; }
        bool any = false, allOff = true;
        for (auto& l : gLights) {
          JsonObject st = d[l.id]["state"];
          if (st.isNull()) continue;
          // la lettura appena dopo il richiamo della scena cinema potrebbe essere già cambiata:
          // si tengono le ultime due e, a modalità cinema attiva, non si aggiornano più
          if (!l.taken && !gCinema) { l.snapOld = l.snapNew; l.snapNew = restoreBody(st); }
          if (!streaming || !l.shown) continue;
          any = true;
          if (st["on"] | false) { l.seenOn = true; allOff = false; }
          else if (l.seenOn) off.push_back(l.id);
          else allOff = false;
        }
        if (streaming && any && allOff && !off.empty()) { fireOff = true; offEpoch = gStreamEpoch; }
      }
      if (fireOff) takeover(offEpoch, off, "spente dall'app Hue o da Alexa");
    }

    // 3) il bridge ha chiuso lo streaming da solo (succede quando un'app comanda le lampade)
    uint32_t epoch, since;
    bool streaming;
    { Guard g; streaming = gStreaming; epoch = gStreamEpoch; since = gStreamSince; }
    if (streaming && millis() - since > 5000 && millis() - groupAt > 2000) {
      groupAt = millis();
      String s;
      JsonDocument f, d;
      f["stream"] = true;
      if (hue.v1("GET", String("/groups/") + ENT_GROUP, "", s) == 200 &&
          !deserializeJson(d, s, DeserializationOption::Filter(f)) && d["stream"].is<JsonObject>() &&
          !(d["stream"]["active"] | true)) {
        takeover(epoch, IdList(), "comandate dall'app Hue o da Alexa");
      }
    }

    uint32_t el = millis() - now;
    delay(el < WATCH_MS ? WATCH_MS - el : 1);
  }
}

static void engineTask(void*) { gEngine.run(); }

// =====================================================================
// Interfaccia web
// =====================================================================
static String stateJson() {
  JsonDocument d;
  Guard g;
  JsonObject st = d["status"].to<JsonObject>();
  st["tv"] = gTvOn == 1 ? "on" : (gTvOn == 0 ? "off" : "unknown");
  st["tv_reachable"] = gTvReachable;
  st["streaming"] = gStreaming;
  st["running"] = runWanted();
  st["cinema"] = gCinema;
  st["cinema_scene_name"] = sceneName(cinemaSceneId());
  st["note"] = gNote;
  JsonObject se = d["settings"].to<JsonObject>();
  se["auto_start"] = gSet.autoStart;
  se["smoothing"] = gSet.smoothing;
  se["brightness"] = gSet.brightness;
  se["restore_scene"] = gSet.restoreScene;
  se["cinema_scene"] = gSet.cinemaScene;
  JsonArray ls = d["lights"].to<JsonArray>();
  for (auto& l : gLights) {
    JsonObject o = ls.add<JsonObject>();
    o["id"] = l.id;
    o["name"] = l.name;
    if (l.shown && gStreaming) {
      JsonArray c = o["color"].to<JsonArray>();
      for (int i = 0; i < 3; i++) c.add(l.show[i]);
    } else {
      o["color"] = nullptr;
    }
    o["mode"] = l.mode;
    o["zone"] = l.zone;
    o["gain"] = l.gain;
    o["max_level"] = l.maxLevel;
  }
  JsonObject ed = d["edges"].to<JsonObject>();
  bool live = gTvOn == 1 && gLayer.valid;
  for (int k = 0; k < 3; k++) {
    JsonArray a = ed[SIDE_NAMES[k]].to<JsonArray>();
    int n = live ? gLayer.n[k] : gCounts[k];
    for (int i = 0; i < n; i++) {
      if (live) {
        JsonArray c = a.add<JsonArray>();
        c.add((int)gLayer.p[k][i].r); c.add((int)gLayer.p[k][i].g); c.add((int)gLayer.p[k][i].b);
      } else {
        a.add(nullptr);
      }
    }
  }
  d["scenes"] = serialized(gScenesJson);
  String out;
  serializeJson(d, out);
  return out;
}

static void handleSettings() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain")) || !d.is<JsonObject>()) {
    server.send(400, "application/json", "{\"error\":\"richiesta non valida\"}");
    return;
  }
  {
    Guard g;
    // comandi della sessione (valgono finché la TV resta accesa)
    if (d["running"].is<bool>()) {
      bool on = d["running"].as<bool>();
      gSession = on ? 1 : 0;
      if (!on) gCinema = false;
      sessionChanged();
      gNote = "";
    }
    if (d["cinema"].is<bool>()) {
      gCinema = d["cinema"].as<bool>();
      if (gCinema) gSession = 1;
      sessionChanged();
      gNote = "";
    }
    // impostazioni salvate
    if (d["auto_start"].is<bool>()) gSet.autoStart = d["auto_start"].as<bool>();
    if (d["smoothing"].is<float>()) gSet.smoothing = clampf(d["smoothing"].as<float>(), 0, 0.95f);
    if (d["brightness"].is<float>()) gSet.brightness = clampf(d["brightness"].as<float>(), 0.3f, 3.0f);
    if (d["restore_scene"].is<const char*>()) gSet.restoreScene = d["restore_scene"].as<String>().substring(0, 64);
    if (d["cinema_scene"].is<const char*>()) gSet.cinemaScene = d["cinema_scene"].as<String>().substring(0, 64);
    for (JsonPair kv : d["lights"].as<JsonObject>()) {
      Light* l = findLight(kv.key().c_str());
      if (!l) continue;
      JsonObject o = kv.value();
      if (o["mode"].is<const char*>() && validMode(o["mode"].as<String>())) l->mode = o["mode"].as<String>();
      if (o["zone"].is<const char*>() && validZone(o["zone"].as<String>())) l->zone = o["zone"].as<String>();
      if (o["gain"].is<float>()) l->gain = clampf(o["gain"].as<float>(), 0.1f, 2.0f);
      if (o["max_level"].is<float>()) l->maxLevel = clampf(o["max_level"].as<float>(), 0.05f, 1.0f);
    }
    gDirty = true;
    gDirtyAt = millis();
  }
  server.send(200, "application/json", stateJson());
}

// =====================================================================
// Avvio
// =====================================================================
static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  LOG("Connessione al Wi-Fi \"%s\"...", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - t0 > 20000) {
      LOG("Wi-Fi non raggiungibile, riprovo (controlla nome e password, e che sia a 2,4 GHz).");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      t0 = millis();
    }
  }
  WiFi.setSleep(false);   // niente risparmio energetico: meno ritardi sullo streaming
  LOG("Wi-Fi connesso, IP %s", WiFi.localIP().toString().c_str());
}

void setup() {
  // LED RGB della scheda spento (GPIO48 sulle DevKitC-1 v1.0, GPIO38 sulle v1.1).
  // Il LED rosso di alimentazione è collegato direttamente e non si può spegnere dal programma.
  rgbLedWrite(48, 0, 0, 0);
  rgbLedWrite(38, 0, 0, 0);
  Serial.begin(115200);
  delay(500);
  LOG("AmbiHue per ESP32-S3");
  gLock = xSemaphoreCreateMutex();
  prefs.begin("ambihue", false);

  connectWifi();

  JsonDocument saved;
  loadSaved(saved);
  IdList members;
  HueApi hue;
  hue.init();
  while (!discoverArea(hue, saved, members)) {
    LOG("Nuovo tentativo di leggere l'area tra 10 secondi...");
    delay(10000);
  }
  {
    Guard g;
    String list;
    for (auto& l : gLights) {
      if (list.length()) list += ", ";
      list += l.name + " (" + l.id + ", " + l.mode + ")";
    }
    LOG("Lampade dell'area: %s", list.c_str());
  }
  gEngine.begin(members);

  xTaskCreatePinnedToCore(tvTask, "tv", 16384, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(watchTask, "watch", 20480, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(engineTask, "engine", 20480, NULL, 2, NULL, 1);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/index.html", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/api/state", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", stateJson());
  });
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  LOG("Interfaccia: http://%s.local  oppure  http://%s", HOSTNAME, WiFi.localIP().toString().c_str());
}

void loop() {
  server.handleClient();
  {
    Guard g;
    if (gDirty && millis() - gDirtyAt > 2000) { gDirty = false; saveSettings(); }
  }
  delay(2);
}
