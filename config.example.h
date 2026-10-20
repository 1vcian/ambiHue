#pragma once
// ======================= CONFIGURAZIONE =======================
// Copia questo file come config.h e compila i tuoi dati.

// --- Wi-Fi (solo rete a 2,4 GHz: l'ESP32 non vede le reti a 5 GHz)
#define WIFI_SSID "NOME-DELLA-TUA-RETE"
#define WIFI_PASS "PASSWORD-DEL-WIFI"
#define HOSTNAME  "ambihue"          // interfaccia su http://ambihue.local

// --- TV
#define TV_IP       "192.168.1.45"
#define TV_USER     ""                 // da abbinamento con la TV (JointSpace)
#define TV_PASS     ""
#define TV_ENDPOINT "processed"      // "processed" o "measured"

// --- Hue Bridge
#define BRIDGE_IP "192.168.1.20"
#define ENT_USER  ""                 // da: python3 ambihue_web.py pair-ent
#define ENT_KEY   ""
#define ENT_GROUP "200"              // area di intrattenimento "Cinema"

// Scena richiamata sulle lampade "sempre" quando la sincronizzazione si ferma
// (poi la cambi dall'interfaccia)
#define DEFAULT_RESTORE_SCENE ""

// Scena che attiva la modalità cinema quando viene richiamata (es. "Alexa, attiva Cinema").
// "" = cerca da sola una scena chiamata "Cinema"; "-" = nessuna. Si cambia dall'interfaccia.
#define DEFAULT_CINEMA_SCENE ""

// Lampade dell'area di intrattenimento: { "ID", "zona", "quando", luminosità massima }
//   quando: "auto"   = sempre quando la TV è accesa; a fine sincronizzazione va alla scena
//           "cinema" = solo in modalità cinema; a fine sincronizzazione torna com'era prima
//           "off"    = mai
// Le lampade che non sono nell'area (es. la luce del divano) non vengono mai toccate.
struct LightDefault { const char* id; const char* zone; const char* mode; float maxLevel; };
static const LightDefault DEFAULT_LIGHTS[] = {
  {"3", "left:0",  "auto",   1.0f},  // faretto
  {"2", "right:2", "auto",   1.0f},  // lampada
  {"1", "top:3",   "cinema", 0.5f},  // luce tavolo: solo in modalità cinema, al massimo 50%
};

// --- Tempi
#define TV_FPS          10     // letture al secondo dalla TV durante la sincronizzazione
#define STREAM_FPS      50     // pacchetti al secondo verso il bridge
#define POWER_CHECK_MS  3000   // ogni quanto controllare se la TV è accesa
#define TV_OFF_CONFIRM_MS 15000 // la TV è spenta solo se non risponde per così tanto
#define DARK_OFF_MS     10000  // riserva: TV spenta se i colori restano a 0 così a lungo
#define AREA_SETTLE_MS  4000   // attesa dopo aver cambiato le lampade dell'area
#define WATCH_MS        1000   // ogni quanto controllare scena cinema e stato delle lampade
#define CINEMA_ARM_MS   600000 // scena cinema richiamata a TV spenta: vale se la accendi entro 10 minuti
// ==============================================================
