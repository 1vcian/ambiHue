#!/usr/bin/env python3
"""
AmbiHue - Ambilight verso Philips Hue, con interfaccia web.

Legge i colori Ambilight dalla TV (JointSpace API v6) e li manda in streaming
alle lampade dell'area di intrattenimento Hue. Dal telefono, aprendo l'indirizzo
che compare all'avvio, puoi accendere/spegnere la sincronizzazione e scegliere
quali lampade usare.

Uso:
  python3 ambihue_web.py pair-ent   -> abbinamento con chiave di streaming (premi il pulsante del bridge)
  python3 ambihue_web.py area       -> mostra quali lampade sono nell'area di intrattenimento
  python3 ambihue_web.py            -> avvia (Ctrl+C per fermare)

Requisiti:  pip install requests python-mbedtls
"""
import json
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests
import urllib3
from requests.auth import HTTPDigestAuth

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# ======================= CONFIGURAZIONE =======================
TV_IP = "192.168.1.45"
TV_USER = ""
TV_PASS = ""
ENDPOINT = "processed"       # "processed" o "measured"

BRIDGE_IP = "192.168.1.20"
ENT_USER = ""                # da: python3 ambihue_web.py pair-ent
ENT_KEY = ""                 # da: python3 ambihue_web.py pair-ent
ENT_GROUP = "200"            # area di intrattenimento "Cinema"

WEB_PORT = 8080              # l'interfaccia sarà su http://IP-DEL-MAC:8080

# Zona iniziale di ogni lampada (poi la cambi dall'interfaccia)
DEFAULT_ZONES = {"3": "left:0", "2": "right:2", "1": "top:3"}

# Lampade solo bianche (fuori dall'area di intrattenimento): "ID": "zona".
# Si accendono solo quando il colore della zona è vicino al bianco e ne seguono la luminosità.
WHITE_LIGHTS = {"4": "top:3"}   # Luce Divano

TV_FPS = 10            # letture al secondo dalla TV durante la sincronizzazione
STREAM_FPS = 50        # pacchetti al secondo verso il bridge
POWER_CHECK = 3        # ogni quanti secondi controllare se la TV è accesa
DARK_OFF_SECONDS = 10  # riserva: TV spenta se i colori restano a 0 per tot secondi
AREA_SETTLE = 4        # secondi di attesa dopo aver cambiato le lampade dell'area
WHITE_PERIOD = 0.4     # ogni quanto aggiornare le lampade bianche (comandi normali, più lenti)
WHITE_HOLD = 1.5       # secondi di conferma prima di accendere/spegnere una lampada bianca
# ==============================================================

SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ambihue_settings.json")
DEFAULT_SETTINGS = {
    "enabled": True,
    "smoothing": 0.6,
    "brightness": 1.0,
    "restore_scene": "",
    "lights": {},
    "area": {},   # lampade dell'area di intrattenimento con le loro posizioni originali
}
SIDES = ("left", "top", "right", "bottom")


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def is_error(res):
    return isinstance(res, list) and bool(res) and "error" in res[0]


# ---------------------------------------------------------------- impostazioni
class Settings:
    def __init__(self):
        self.lock = threading.Lock()
        self.data = json.loads(json.dumps(DEFAULT_SETTINGS))
        try:
            with open(SETTINGS_FILE, encoding="utf-8") as f:
                saved = json.load(f)
            self.data.update({k: v for k, v in saved.items() if k in DEFAULT_SETTINGS})
        except (OSError, ValueError):
            pass

    def ensure_lights(self, ids):
        with self.lock:
            for lid in ids:
                light = self.data["lights"].setdefault(
                    lid, {"enabled": True, "zone": DEFAULT_ZONES.get(lid, "all"), "gain": 1.0})
                light.setdefault("max_level", 1.0)
            self._save()

    def ensure_white(self, ids):
        with self.lock:
            for lid in ids:
                light = self.data["lights"].setdefault(
                    lid, {"enabled": True, "zone": WHITE_LIGHTS.get(lid, "top"), "gain": 1.0})
                light.setdefault("tolerance", 0.25)
                light.setdefault("max_level", 1.0)
            self._save()

    def merge_area(self, members):
        """Unisce le lampade trovate ora nell'area con quelle ricordate."""
        with self.lock:
            area = self.data.setdefault("area", {})
            for lid, info in members.items():
                old = area.get(lid, {})
                area[lid] = {**old, **{k: v for k, v in info.items() if v is not None}}
            self._save()
            return json.loads(json.dumps(area))

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(self.data))

    def update(self, patch):
        with self.lock:
            d = self.data
            if "enabled" in patch:
                d["enabled"] = bool(patch["enabled"])
            for key, lo, hi in (("smoothing", 0.0, 0.95), ("brightness", 0.3, 3.0)):
                if key in patch:
                    d[key] = clamp(float(patch[key]), lo, hi)
            if "restore_scene" in patch:
                d["restore_scene"] = str(patch["restore_scene"])[:64]
            for lid, lp in (patch.get("lights") or {}).items():
                light = d["lights"].get(lid)
                if light is None:
                    continue
                if "enabled" in lp:
                    light["enabled"] = bool(lp["enabled"])
                if "zone" in lp and valid_zone(lp["zone"]):
                    light["zone"] = lp["zone"]
                if "gain" in lp:
                    light["gain"] = clamp(float(lp["gain"]), 0.1, 2.0)
                if "max_level" in lp:
                    light["max_level"] = clamp(float(lp["max_level"]), 0.05, 1.0)
                if "tolerance" in lp and "tolerance" in light:
                    light["tolerance"] = clamp(float(lp["tolerance"]), 0.05, 0.6)
            self._save()

    def _save(self):
        tmp = SETTINGS_FILE + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=2)
            os.replace(tmp, SETTINGS_FILE)
        except OSError as e:
            print("Impossibile salvare le impostazioni:", e)


# ---------------------------------------------------------------- colori
def side(layer, name):
    d = (layer or {}).get(name) or {}
    out = []
    for k in sorted(d, key=int):
        p = d[k] if isinstance(d[k], dict) else {}
        out.append({c: p.get(c, 0) for c in ("r", "g", "b")})
    return out


def valid_zone(zone):
    if zone in ("left", "right", "top", "top-left", "top-right", "all"):
        return True
    if isinstance(zone, str) and ":" in zone:
        name, idx = zone.split(":", 1)
        return name in SIDES and idx.isdigit()
    return False


def zone_color(layer, zone):
    if ":" in zone:
        name, idx = zone.split(":", 1)
        try:
            p = side(layer, name)[int(idx)]
            return (p["r"], p["g"], p["b"])
        except (IndexError, ValueError, KeyError):
            return None
    left, right, top = side(layer, "left"), side(layer, "right"), side(layer, "top")
    n = len(top)
    zones = {"left": left, "right": right, "top": top,
             "top-left": top[: (n + 1) // 2], "top-right": top[n // 2:],
             "all": left + top + right + side(layer, "bottom")}
    pts = zones.get(zone) or zones["all"]
    if not pts:
        return None
    return tuple(sum(p[c] for p in pts) / len(pts) for c in ("r", "g", "b"))


def layer_max(layer):
    return max((p[c] for name in SIDES for p in side(layer, name) for c in ("r", "g", "b")),
               default=0)


# ---------------------------------------------------------------- TV
class TV(threading.Thread):
    """Controlla se la TV è accesa e legge i colori Ambilight."""

    def __init__(self, want_fast):
        super().__init__(daemon=True)
        self.want_fast = want_fast
        self.s = requests.Session()
        self.s.auth = HTTPDigestAuth(TV_USER, TV_PASS)
        self.s.verify = False
        self.base = f"https://{TV_IP}:1926/6/ambilight"
        self.on = None
        self.layer = None
        self.counts = {"left": 3, "top": 7, "right": 3}
        self.reachable = True
        self.seen_config = False
        self.dark_since = None

    def _config(self):
        try:
            r = self.s.get(f"{self.base}/currentconfiguration", timeout=2)
        except requests.RequestException:
            return None
        if r.status_code != 200 or not r.text.strip():
            return False
        try:
            return bool(r.json())
        except ValueError:
            return False

    def _off(self):
        self.on = False
        self.layer = None

    def run(self):
        next_check = 0
        while True:
            now = time.time()
            if now >= next_check:
                next_check = now + POWER_CHECK
                cfg = self._config()
                self.reachable = cfg is not None
                if cfg:
                    self.seen_config = True
                if cfg is None:
                    self._off()
                elif self.seen_config:
                    if cfg:
                        self.on = True
                    else:
                        self._off()

            if not self.reachable or (self.seen_config and not self.on):
                time.sleep(1)
                continue

            try:
                r = self.s.get(f"{self.base}/{ENDPOINT}", timeout=2)
                r.raise_for_status()
                layer = r.json()["layer1"]
            except (requests.RequestException, KeyError, ValueError):
                if not self.seen_config:
                    self._off()
                time.sleep(2)
                continue

            self.layer = layer
            counts = {k: len(side(layer, k)) for k in ("left", "top", "right")}
            if any(counts.values()):
                self.counts = counts
            if not self.seen_config:  # riserva: decidiamo dai colori
                if layer_max(layer) == 0:
                    self.dark_since = self.dark_since or now
                    if now - self.dark_since >= DARK_OFF_SECONDS:
                        self.on = False
                else:
                    self.dark_since = None
                    self.on = True

            period = 1 / TV_FPS if self.want_fast() else 1.0
            time.sleep(max(0, period - (time.time() - now)))


# ---------------------------------------------------------------- Hue
class Bridge:
    def __init__(self):
        self.s = requests.Session()
        self.s.verify = False
        self.base = f"https://{BRIDGE_IP}/api/{ENT_USER}"

    def get(self, path):
        return self.s.get(self.base + path, timeout=5).json()

    def put(self, path, body):
        return self.s.put(self.base + path, json=body, timeout=5).json()

    def app_id(self):
        try:
            r = self.s.get(f"https://{BRIDGE_IP}/auth/v1",
                           headers={"hue-application-key": ENT_USER}, timeout=5)
            return r.headers.get("hue-application-id")
        except requests.RequestException:
            return None

    def set_stream(self, active):
        return not is_error(self.put(f"/groups/{ENT_GROUP}", {"stream": {"active": active}}))

    def v2(self, method, path, body=None):
        r = self.s.request(method, f"https://{BRIDGE_IP}/clip/v2/resource/{path}",
                           headers={"hue-application-key": ENT_USER}, json=body, timeout=5)
        data = r.json()
        errors = data.get("errors") or []
        if r.status_code >= 400 or errors:
            raise RuntimeError("; ".join(e.get("description", "") for e in errors) or f"HTTP {r.status_code}")
        return data.get("data", [])

    def _group(self):
        group = self.get(f"/groups/{ENT_GROUP}")
        if is_error(group):
            raise RuntimeError(group[0]["error"].get("description"))
        if group.get("type") != "Entertainment":
            raise RuntimeError(f"il gruppo {ENT_GROUP} non è un'area di intrattenimento")
        return group

    def _v2_config(self):
        for c in self.v2("GET", "entertainment_configuration"):
            if c.get("id_v1") == f"/groups/{ENT_GROUP}":
                return c
        raise RuntimeError("area non trovata nell'API v2")

    def area_info(self):
        """Lampade attualmente nell'area, con nome e posizione."""
        group = self._group()
        lights = self.get("/lights")
        locs = group.get("locations") or {}
        info = {lid: {"name": lights.get(lid, {}).get("name", f"Lampada {lid}"), "loc": locs.get(lid)}
                for lid in group["lights"]}
        try:  # posizioni anche nel formato v2, servono se il bridge rifiuta le modifiche v1
            by_rid = {e["id"]: e.get("id_v1", "").rsplit("/", 1)[-1] for e in self.v2("GET", "entertainment")}
            for sl in self._v2_config()["locations"]["service_locations"]:
                lid = by_rid.get(sl["service"]["rid"])
                if lid in info:
                    info[lid]["v2"] = {"rid": sl["service"]["rid"],
                                       "positions": sl.get("positions") or [sl.get("position")]}
        except (RuntimeError, requests.RequestException, KeyError, ValueError, TypeError):
            pass
        return info, sorted(group["lights"])

    def v2_members(self):
        """Lampade che l'area usa davvero per lo streaming (canali v2)."""
        by_rid = {e["id"]: e.get("id_v1", "").rsplit("/", 1)[-1] for e in self.v2("GET", "entertainment")}
        conf = self._v2_config()
        return sorted({by_rid.get(m["service"]["rid"]) for ch in conf.get("channels", [])
                       for m in ch.get("members", [])} - {None})

    def _members_ok(self, lids):
        time.sleep(0.5)
        if sorted(self._group()["lights"]) != sorted(lids):
            return False
        try:
            return self.v2_members() == sorted(lids)
        except (RuntimeError, requests.RequestException, KeyError, ValueError):
            return True   # API v2 non disponibile: ci fidiamo della v1

    def set_members(self, lids, area):
        """Imposta quali lampade fanno parte dell'area (solo a streaming spento)."""
        errors = []
        if all(area[l].get("v2") for l in lids):
            try:
                locs = [{"service": {"rid": area[l]["v2"]["rid"], "rtype": "entertainment"},
                         "positions": area[l]["v2"]["positions"]} for l in lids]
                conf = self._v2_config()
                self.v2("PUT", f"entertainment_configuration/{conf['id']}",
                        {"locations": {"service_locations": locs}})
                if self._members_ok(lids):
                    return
                errors.append("v2: area non aggiornata")
            except (RuntimeError, requests.RequestException, KeyError, ValueError) as e:
                errors.append(f"v2: {e}")
        try:
            body = {"lights": lids, "locations": {l: area[l]["loc"] for l in lids if area[l].get("loc")}}
            res = self.put(f"/groups/{ENT_GROUP}", body)
            if is_error(res):
                errors.append("v1: " + res[0]["error"].get("description", ""))
            elif self._members_ok(lids):
                return
            else:
                errors.append("v1: area non aggiornata")
        except requests.RequestException as e:
            errors.append(f"v1: {e}")
        raise RuntimeError("; ".join(errors))

    def scenes(self):
        scenes, groups = self.get("/scenes"), self.get("/groups")
        if is_error(scenes):
            return []
        groups = groups if isinstance(groups, dict) else {}
        out = []
        for sid, info in scenes.items():
            room = groups.get(info.get("group", ""), {}).get("name", "")
            out.append({"id": sid, "name": info.get("name", ""), "room": room})
        return sorted(out, key=lambda x: (x["room"].lower(), x["name"].lower()))

    def restore(self, scene_id):
        """Richiama la scena; restituisce il suo nome o solleva RuntimeError."""
        info = self.get(f"/scenes/{scene_id}")
        if is_error(info) or not isinstance(info, dict):
            raise RuntimeError("scena non trovata sul bridge, sceglila di nuovo nell'interfaccia")
        res = self.put(f"/groups/{info.get('group', '0')}/action", {"scene": scene_id})
        if is_error(res):
            raise RuntimeError(res[0]["error"].get("description", "errore del bridge"))
        return info.get("name", scene_id)


HEADER = b"HueStream" + bytes([1, 0, 0, 0, 0, 0, 0])  # v1.0, seq, riservati, RGB, riservato


class Stream:
    def __init__(self, bridge):
        self.bridge = bridge
        self.sock = None
        self.identity = None

    @property
    def active(self):
        return self.sock is not None

    def _connect(self, identity):
        from mbedtls import tls
        conf = tls.DTLSConfiguration(
            pre_shared_key=(identity, bytes.fromhex(ENT_KEY)),
            ciphers=["TLS-PSK-WITH-AES-128-GCM-SHA256"],
            lowest_supported_version=tls.DTLSVersion.DTLSv1_2,
            highest_supported_version=tls.DTLSVersion.DTLSv1_2,
            validate_certificates=False,
        )
        raw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        raw.settimeout(1.0)
        sock = tls.ClientContext(conf).wrap_socket(raw, None)
        sock.connect((BRIDGE_IP, 2100))
        deadline = time.time() + 8
        while True:
            try:
                sock.do_handshake()
                return sock
            except (tls.WantReadError, tls.WantWriteError, TimeoutError, socket.timeout):
                if time.time() > deadline:
                    sock.close()
                    raise TimeoutError("il bridge non ha completato la connessione cifrata")
                time.sleep(0.02)

    def open(self):
        candidates = [self.identity] if self.identity else []
        for c in (self.bridge.app_id(), ENT_USER):
            if c and c not in candidates:
                candidates.append(c)
        for identity in candidates:
            if not self.bridge.set_stream(True):
                raise RuntimeError("il bridge ha rifiutato l'attivazione dell'area")
            try:
                self.sock = self._connect(identity)
                self.identity = identity
                return
            except (TimeoutError, OSError):
                self.bridge.set_stream(False)
                time.sleep(1)
        raise RuntimeError("connessione cifrata non riuscita")

    def send(self, colors):
        msg = bytearray(HEADER)
        for lid, (r, g, b) in colors.items():
            msg += struct.pack(">BHHHH", 0, int(lid),
                               *(int(clamp(v, 0, 255) / 255 * 65535) for v in (r, g, b)))
        self.sock.send(bytes(msg))

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:  # noqa: BLE001
                pass
            self.sock = None
        try:
            self.bridge.set_stream(False)
        except requests.RequestException:
            pass


# ---------------------------------------------------------------- motore
class Engine(threading.Thread):
    def __init__(self, settings, bridge, area, members, white=None):
        super().__init__(daemon=True)
        self.settings = settings
        self.bridge = bridge
        self.area = area
        self.names = {lid: a.get("name", f"Lampada {lid}") for lid, a in area.items()}
        self.white = white or {}                # lampade solo bianche: id -> nome
        self.wstate = {lid: {"on": None, "bri": 0, "since": None, "next": 0} for lid in self.white}
        self.members = sorted(members)          # lampade ora nell'area di intrattenimento
        self.failed_members = None              # combinazione che il bridge ha rifiutato
        self.full_retry_at = 0
        self.session = False   # True da quando parte la sincronizzazione finché non viene fermata
        self.stream = Stream(bridge)
        self.tv = TV(want_fast=lambda: self.stream.active)
        self.cur = {lid: [0.0, 0.0, 0.0] for lid in self.names}
        self.colors = {}
        self.note = ""
        self.retry_at = 0
        self.scene_list = []
        self.scenes_at = 0
        self.stop_evt = threading.Event()

    def _restore(self, scene_id, after_stream=False):
        if not scene_id:
            print("Nessuna scena da richiamare: sceglila in Altre impostazioni.")
            return
        if after_stream:
            time.sleep(1.5)   # il bridge prima rimette le lampade com'erano prima dello streaming
        try:
            name = self.bridge.restore(scene_id)
            if after_stream:
                time.sleep(2.5)
                self.bridge.restore(scene_id)   # seconda volta, nel caso il bridge l'abbia sovrascritta
            print(f"Scena \"{name}\" richiamata.")
        except (RuntimeError, requests.RequestException) as e:
            self.note = f"Impossibile richiamare la scena: {e}"
            print(self.note)

    def _set_members(self, lids):
        lids = sorted(lids)
        if lids == self.members:
            return True
        try:
            self.bridge.set_members(lids, self.area)
            self.members, self.failed_members = lids, None
            self.retry_at = time.time() + AREA_SETTLE   # il bridge deve preparare le lampade
            print("Area aggiornata:", ", ".join(self.names[l] for l in lids))
            return True
        except (RuntimeError, requests.RequestException, KeyError) as e:
            self.failed_members = lids
            self.note = f"Non riesco a liberare le lampade escluse: {e}"
            return False

    def _white_step(self, s, now):
        """Lampade bianche: accese solo se la zona è quasi bianca, con la sua luminosità."""
        layer = self.tv.layer
        for lid in self.white:
            light, st = s["lights"][lid], self.wstate[lid]
            if not light["enabled"]:
                st.update(on=None, since=None)   # esclusa: la lasciamo libera
                continue
            if now < st["next"]:
                continue
            st["next"] = now + WHITE_PERIOD
            c = zone_color(layer, light["zone"]) if layer else None
            if c is None:
                continue
            m, mn = max(c), min(c)
            sat = (m - mn) / m if m > 0 else 0.0
            level = min(1.0, m * light["gain"] * s["brightness"] / 255)
            tol = light["tolerance"]
            if st["on"]:   # isteresi: più facile restare accesa che accendersi
                want_on = sat <= tol + 0.08 and level >= 0.02
            else:
                want_on = sat <= tol and level >= 0.04
            if want_on != bool(st["on"]) and st["on"] is not None:
                st["since"] = st["since"] or now
                if now - st["since"] < WHITE_HOLD:
                    continue   # in attesa di conferma: la lampada resta com'è
                st["since"] = None
            else:
                st["since"] = None
            body = None
            if want_on:
                bri = int(clamp(1 + 253 * min(level, light.get("max_level", 1.0)), 1, 254))
                if not st["on"] or abs(bri - st["bri"]) >= 6:
                    body = {"on": True, "bri": bri, "transitiontime": 4}
            elif st["on"] is not False:
                body = {"on": False, "transitiontime": 4}
            if body:
                try:
                    res = self.bridge.put(f"/lights/{lid}/state", body)
                    if not is_error(res):
                        st["on"], st["bri"] = want_on, body.get("bri", 0)
                except requests.RequestException:
                    pass

    def _stop_stream(self, scene_id):
        was_streaming = self.stream.active
        self.stream.close()
        self.colors = {}
        for st in self.wstate.values():
            st.update(on=None, bri=0, since=None)
        self.session = False
        self._set_members(self.names)       # l'area torna completa, come nell'app Hue
        self._restore(scene_id, after_stream=was_streaming)

    def run(self):
        self.tv.start()
        while not self.stop_evt.is_set():
            t0 = time.time()
            s = self.settings.snapshot()

            if t0 - self.scenes_at > 300:
                self.scenes_at = t0
                try:
                    self.scene_list = self.bridge.scenes()
                except requests.RequestException:
                    pass

            desired = sorted(lid for lid in self.names if s["lights"].get(lid, {}).get("enabled"))
            white_on = [lid for lid in self.white if s["lights"][lid]["enabled"]]
            base = bool(s["enabled"] and self.tv.on)
            want_any = base and bool(desired or white_on)   # qualcosa da sincronizzare
            want = base and bool(desired)                   # serve lo streaming

            # lampade incluse/escluse cambiate: si ferma un attimo lo streaming e si rifà l'area
            if (want and self.stream.active and desired != self.members
                    and desired != self.failed_members):
                self.stream.close()
                self.colors = {}

            if want and not self.stream.active and t0 >= self.retry_at:
                if desired != self.members and desired != self.failed_members:
                    self._set_members(desired)
                    time.sleep(0.3)
                    continue   # si riparte dopo AREA_SETTLE secondi
                try:
                    self.stream.open()
                    if self.members == desired:
                        self.note = ""
                    self.cur = {lid: [0.0, 0.0, 0.0] for lid in self.names}
                    self.session = True
                    print("Sincronizzazione attiva:", ", ".join(self.names[l] for l in self.members))
                except (RuntimeError, requests.RequestException) as e:
                    self.note = f"Bridge: {e}. Nuovo tentativo tra poco."
                    self.retry_at = t0 + 10
            elif not want_any and (self.stream.active or self.session):
                print("Sincronizzazione in pausa.")
                self._stop_stream(s["restore_scene"])
            elif not want and self.stream.active:   # restano solo lampade bianche
                self.stream.close()
                self.colors = {}
                self._set_members(self.names)
            elif (not want and not self.stream.active and self.members != sorted(self.names)
                  and t0 >= self.full_retry_at):
                if not self._set_members(self.names):   # area di nuovo completa
                    self.full_retry_at = t0 + 30

            if want_any and white_on:
                self.session = True
                self._white_step(s, t0)

            if self.stream.active:
                layer = self.tv.layer
                k = s["smoothing"]
                for lid in self.members:
                    light = s["lights"].get(lid, {})
                    if light.get("enabled"):
                        c = zone_color(layer, light["zone"]) if layer else None
                        if c is None:
                            continue
                        g = light["gain"] * s["brightness"]
                        target = [v * g for v in c]
                        cap = 255 * light.get("max_level", 1.0)
                        top = max(target)
                        if top > cap:   # tetto: stessa tinta, meno luce
                            target = [v * cap / top for v in target]
                    else:  # solo se il bridge ha rifiutato di toglierla dall'area
                        target = [255 * 0.4, 180 * 0.4, 110 * 0.4]
                    self.cur[lid] = [self.cur[lid][i] * k + target[i] * (1 - k) for i in range(3)]
                try:
                    self.stream.send({lid: self.cur[lid] for lid in self.members})
                    self.colors = {lid: [round(clamp(v, 0, 255)) for v in self.cur[lid]]
                                   for lid in self.members if s["lights"][lid]["enabled"]}
                except Exception as e:  # noqa: BLE001
                    self.note = f"Streaming interrotto ({type(e).__name__}), riprovo."
                    self.stream.close()
                    self.colors = {}
                    self.retry_at = t0 + 5
                period = 1 / STREAM_FPS
            else:
                period = 0.1 if (want_any and white_on) else 0.3
            time.sleep(max(0, period - (time.time() - t0)))

        if self.stream.active or self.session:
            self._stop_stream(self.settings.snapshot()["restore_scene"])
        else:
            self._set_members(self.names)

    def _white_color(self, lid):
        st = self.wstate[lid]
        if not st["on"]:
            return None
        f = st["bri"] / 254
        return [round(255 * f), round(190 * f), round(120 * f)]

    def state(self):
        s = self.settings.snapshot()
        tv = self.tv
        layer = tv.layer if tv.on else None
        edges = {k: ([[p["r"], p["g"], p["b"]] for p in side(layer, k)] if layer
                     else [None] * tv.counts.get(k, 0)) for k in ("left", "top", "right")}
        return {
            "status": {
                "tv": "on" if tv.on else ("off" if tv.on is False else "unknown"),
                "tv_reachable": tv.reachable,
                "streaming": self.stream.active,
                "note": self.note,
            },
            "settings": {k: s[k] for k in ("enabled", "smoothing", "brightness", "restore_scene")},
            "lights": [dict(id=lid, name=name, kind="color", color=self.colors.get(lid),
                            **s["lights"][lid]) for lid, name in self.names.items()]
                      + [dict(id=lid, name=name, kind="white", color=self._white_color(lid),
                              **s["lights"][lid]) for lid, name in self.white.items()],
            "edges": edges,
            "scenes": self.scene_list,
        }


# ---------------------------------------------------------------- web
PAGE = r"""<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<title>Luci TV</title>
<style>
:root{--bg:#e6e8ec;--surface:#f7f8fa;--line:#d0d3da;--text:#1c1f27;--muted:#5a5f6d;--on:#9a6514;--knob:#ffffff;--screen:#23262e;--off:#c3c6ce;color-scheme:light}
@media (prefers-color-scheme:dark){:root{--bg:#14161c;--surface:#1c1f27;--line:#2b2f3a;--text:#ece7dd;--muted:#8c90a0;--on:#f0cf96;--knob:#14161c;--screen:#07080b;--off:#363a46;color-scheme:dark}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 "Avenir Next","Segoe UI",system-ui,-apple-system,sans-serif;padding-top:env(safe-area-inset-top,0px);padding-bottom:env(safe-area-inset-bottom,0px)}
main{max-width:520px;margin:0 auto;padding:20px 18px 48px}
header{display:flex;justify-content:space-between;align-items:baseline;gap:12px}
h1{font-size:1.45rem;font-weight:600;letter-spacing:-.01em;margin:0}
h2{font-size:1rem;font-weight:600;margin:0 0 4px}
#status{color:var(--muted);font-size:.9rem;text-align:right}
.stage{position:relative;aspect-ratio:100/80;margin:10px 0 4px}
.tv{position:absolute;left:18%;width:64%;top:22%;aspect-ratio:16/9;background:var(--screen);border-radius:5px;box-shadow:0 0 0 1px var(--line)}
.led{position:absolute;width:8px;height:8px;margin:-4px 0 0 -4px;border-radius:50%;background:var(--off);transition:background-color .3s,box-shadow .3s}
.lamp{position:absolute;transform:translate(-50%,-50%);display:flex;flex-direction:column;align-items:center;gap:3px;font-size:.72rem;color:var(--muted);white-space:nowrap;transition:left .4s,top .4s}
.lamp.up{flex-direction:column-reverse}
.lamp.excluded .bulb{opacity:.45}
.bulb{width:20px;height:20px;border-radius:50%;background:var(--off);border:2px solid var(--bg);transition:background-color .3s,box-shadow .3s}
.master{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:16px 0;border-top:1px solid var(--line);border-bottom:1px solid var(--line);margin-bottom:20px}
.master .title{font-size:1.15rem;font-weight:600}
.hint{color:var(--muted);font-size:.85rem;margin:2px 0 0}
.note{color:var(--on);font-size:.85rem;margin:2px 0 0}
.switch{flex:none;width:52px;height:30px;border-radius:15px;border:0;padding:0;background:var(--off);position:relative;cursor:pointer;transition:background-color .2s}
.switch::after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:var(--knob);box-shadow:0 1px 2px rgba(0,0,0,.25);transition:transform .2s}
.switch[aria-checked=true]{background:var(--on)}
.switch[aria-checked=true]::after{transform:translateX(22px)}
.switch.big{width:64px;height:36px;border-radius:18px}
.switch.big::after{width:30px;height:30px}
.switch.big[aria-checked=true]::after{transform:translateX(28px)}
.row{border-bottom:1px solid var(--line)}
.row-main{display:flex;align-items:center;gap:12px;padding:12px 0}
.dot{flex:none;width:14px;height:14px;border-radius:50%;background:var(--off);transition:background-color .3s,box-shadow .3s}
.name{flex:1;min-width:0;text-align:left;background:none;border:0;color:inherit;font:inherit;padding:4px 0;cursor:pointer}
.name small{display:block;color:var(--muted);font-size:.8rem}
.row-more{padding:0 0 14px 26px;display:grid;gap:12px}
.row-more[hidden]{display:none}
label{display:grid;gap:4px;font-size:.88rem;color:var(--muted)}
.lab{display:flex;justify-content:space-between}
.lab output{color:var(--text)}
select{font:inherit;color:var(--text);background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:8px 10px;width:100%}
input[type=range]{width:100%;accent-color:var(--on)}
details.more{margin-top:24px}
details.more summary{cursor:pointer;font-weight:600;padding:8px 0}
details.more .body{display:grid;gap:16px;padding-top:8px}
button:focus-visible,select:focus-visible,input:focus-visible,summary:focus-visible{outline:2px solid var(--on);outline-offset:3px}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style>
</head>
<body>
<main>
  <header><h1>Luci TV</h1><div id="status">Collegamento…</div></header>

  <div class="stage" aria-hidden="true"><div class="tv" id="tv"></div></div>

  <section class="master">
    <div>
      <div class="title">Sincronizzazione</div>
      <p class="hint" id="masterHint"></p>
      <p class="note" id="note"></p>
    </div>
    <button class="switch big" id="master" role="switch" aria-checked="false" aria-label="Sincronizzazione"></button>
  </section>

  <section>
    <h2>Lampade</h2>
    <p class="hint">Tocca un nome per scegliere da quale punto prende il colore.</p>
    <div id="lights"></div>
    <p class="hint">Le lampade escluse restano libere: puoi usarle con l'app Hue o con Alexa. Includere o escludere una lampada mette in pausa le luci per un paio di secondi.</p>
  </section>

  <details class="more">
    <summary>Altre impostazioni</summary>
    <div class="body">
      <label><span class="lab">Luminosità generale <output id="briOut"></output></span>
        <input type="range" id="brightness" min="0.3" max="3" step="0.1"></label>
      <label><span class="lab">Morbidezza dei cambi <output id="smOut"></output></span>
        <input type="range" id="smoothing" min="0" max="0.95" step="0.05"></label>
      <label>Scena da accendere quando la sincronizzazione si ferma
        <select id="scene"></select></label>
    </div>
  </details>
</main>
<script>
const $ = s => document.querySelector(s);
let st = null, builtFor = "";

function display(c){
  if(!c) return null;
  const m = Math.max(c[0], c[1], c[2]);
  if(m < 3) return null;
  const f = (0.35 + 0.65 * m / 255) * 255 / m;
  return `rgb(${c.map(v => Math.round(v * f)).join(",")})`;
}
function paintLight(el, c, size){
  const col = display(c);
  el.style.backgroundColor = col || "";
  el.style.boxShadow = col ? `0 0 ${size}px ${size / 3}px ${col}` : "none";
}

function zoneOptions(n){
  const opts = [["left","Lato sinistro"],["right","Lato destro"],["top","Lato superiore"],
                ["top-left","Metà superiore sinistra"],["top-right","Metà superiore destra"],["all","Tutto lo schermo"]];
  const pts = [];
  for(let i=0;i<n.left;i++) pts.push([`left:${i}`, `Sinistra, punto ${i+1}` + (i===0?" (in basso)":i===n.left-1?" (in alto)":"")]);
  for(let i=0;i<n.top;i++) pts.push([`top:${i}`, `In alto, punto ${i+1}` + (i===Math.floor(n.top/2)?" (centro)":"")]);
  for(let i=0;i<n.right;i++) pts.push([`right:${i}`, `Destra, punto ${i+1}` + (i===0?" (in alto)":i===n.right-1?" (in basso)":"")]);
  return {opts, pts};
}
function zoneLabel(z, n){
  const {opts, pts} = zoneOptions(n);
  const f = opts.concat(pts).find(o => o[0] === z);
  return f ? f[1] : z;
}
function counts(){ return {left: st.edges.left.length, top: st.edges.top.length, right: st.edges.right.length}; }

function lampPos(z, n){
  const m = z.match(/^(left|top|right):(\d+)$/);
  if(m){
    const i = +m[2];
    if(m[1] === "left") return [-20, 100 - (i + .5) / n.left * 100];
    if(m[1] === "right") return [120, (i + .5) / n.right * 100];
    return [(i + .5) / n.top * 100, -36];
  }
  return ({left:[-20,50], right:[120,50], top:[50,-36], "top-left":[25,-36], "top-right":[75,-36]})[z] || [50,140];
}

async function save(patch){
  try{
    const r = await fetch("/api/settings", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(patch)});
    if(r.ok){ st = await r.json(); syncControls(); paint(); }
  }catch(e){ $("#status").textContent = "Server non raggiungibile"; }
}

function build(){
  const n = counts(), tv = $("#tv");
  tv.innerHTML = "";
  const led = (x, y, side, i) => { const d = document.createElement("div"); d.className = "led"; d.dataset.k = side + i;
    d.style.left = x + "%"; d.style.top = y + "%"; tv.appendChild(d); };
  for(let i=0;i<n.left;i++) led(-5, 100 - (i + .5) / n.left * 100, "left", i);
  for(let i=0;i<n.top;i++) led((i + .5) / n.top * 100, -8, "top", i);
  for(let i=0;i<n.right;i++) led(105, (i + .5) / n.right * 100, "right", i);
  for(const l of st.lights){
    const d = document.createElement("div"); d.className = "lamp"; d.id = "lamp" + l.id;
    d.innerHTML = `<div class="bulb"></div><span></span>`; d.querySelector("span").textContent = l.name;
    tv.appendChild(d);
  }

  const list = $("#lights"); list.innerHTML = "";
  const {opts, pts} = zoneOptions(n);
  for(const l of st.lights){
    const row = document.createElement("div"); row.className = "row"; row.id = "row" + l.id;
    row.innerHTML = `
      <div class="row-main">
        <span class="dot"></span>
        <button class="name" aria-expanded="false"><span></span><small></small></button>
        <button class="switch" role="switch" aria-checked="false"></button>
      </div>
      <div class="row-more" hidden>
        <label>Prende il colore da <select></select></label>
        <label><span class="lab">Intensità <output></output></span>
          <input type="range" class="gain" min="0.1" max="2" step="0.05"></label>
        <label><span class="lab">Luminosità massima <output class="maxOut"></output></span>
          <input type="range" class="maxl" min="0.05" max="1" step="0.05"></label>
        ${l.kind === "white" ? `<label><span class="lab">Si accende con <output class="tolOut"></output></span>
          <input type="range" class="tol" min="0.05" max="0.6" step="0.05"></label>` : ""}
      </div>`;
    row.querySelector(".name span").textContent = l.name;
    row.querySelector(".switch").setAttribute("aria-label", "Usa " + l.name);
    const sel = row.querySelector("select");
    const g1 = document.createElement("optgroup"); g1.label = "Zone";
    const g2 = document.createElement("optgroup"); g2.label = "Punti singoli";
    for(const [v, t] of opts) g1.appendChild(new Option(t, v));
    for(const [v, t] of pts) g2.appendChild(new Option(t, v));
    sel.append(g1, g2);
    const more = row.querySelector(".row-more"), nameBtn = row.querySelector(".name");
    nameBtn.onclick = () => { more.hidden = !more.hidden; nameBtn.setAttribute("aria-expanded", String(!more.hidden)); };
    row.querySelector(".switch").onclick = () => save({lights: {[l.id]: {enabled: !cur(l.id).enabled}}});
    sel.onchange = () => save({lights: {[l.id]: {zone: sel.value}}});
    const rng = row.querySelector(".gain"), out = row.querySelector("output");
    rng.oninput = () => out.textContent = Math.round(rng.value * 100) + "%";
    rng.onchange = () => save({lights: {[l.id]: {gain: +rng.value}}});
    const mx = row.querySelector(".maxl"), mxOut = row.querySelector(".maxOut");
    mx.oninput = () => mxOut.textContent = Math.round(mx.value * 100) + "%";
    mx.onchange = () => save({lights: {[l.id]: {max_level: +mx.value}}});
    const tol = row.querySelector(".tol");
    if(tol){
      tol.oninput = () => row.querySelector(".tolOut").textContent = tolText(+tol.value);
      tol.onchange = () => save({lights: {[l.id]: {tolerance: +tol.value}}});
    }
    list.appendChild(row);
  }

  const sc = $("#scene"); sc.innerHTML = ""; sc.appendChild(new Option("Nessuna (le luci restano come sono)", ""));
  for(const s of st.scenes) sc.appendChild(new Option(s.room ? `${s.name} (${s.room})` : s.name, s.id));
  builtFor = st.lights.map(l => l.id).join(",") + "|" + n.left + n.top + n.right + "|" + st.scenes.length;
  syncControls();
}
function cur(id){ return st.lights.find(l => l.id === id); }

function setIfIdle(el, v){ if(document.activeElement !== el) el.value = v; }
function syncControls(){
  const s = st.settings, n = counts();
  $("#master").setAttribute("aria-checked", String(s.enabled));
  for(const l of st.lights){
    const row = $("#row" + l.id); if(!row) continue;
    row.querySelector(".switch").setAttribute("aria-checked", String(l.enabled));
    row.querySelector("small").textContent = !l.enabled ? "Esclusa, libera per app e Alexa"
      : (l.kind === "white" ? "Solo con i bianchi, " + zoneLabel(l.zone, n).toLowerCase() : zoneLabel(l.zone, n));
    setIfIdle(row.querySelector("select"), l.zone);
    const rng = row.querySelector(".gain"); setIfIdle(rng, l.gain);
    row.querySelector("output").textContent = Math.round(l.gain * 100) + "%";
    const mx = row.querySelector(".maxl"), mxv = l.max_level ?? 1; setIfIdle(mx, mxv);
    row.querySelector(".maxOut").textContent = Math.round(mx.value * 100) + "%";
    const tol = row.querySelector(".tol");
    if(tol){ setIfIdle(tol, l.tolerance); row.querySelector(".tolOut").textContent = tolText(+tol.value); }
  }
  setIfIdle($("#brightness"), s.brightness); briText();
  setIfIdle($("#smoothing"), s.smoothing); smText();
  setIfIdle($("#scene"), s.restore_scene);
}
function tolText(v){ return v <= 0.15 ? "solo bianco puro" : v <= 0.35 ? "bianchi e toni chiari" : "anche colori tenui"; }
function briText(){ $("#briOut").textContent = Math.round($("#brightness").value * 100) + "%"; }
function smText(){ const v = +$("#smoothing").value; $("#smOut").textContent = v < .3 ? "reattivo" : v < .7 ? "equilibrato" : "morbido"; }

function paint(){
  const s = st.status, n = counts();
  let text, hint;
  if(s.tv === "off") { text = s.tv_reachable ? "TV in standby" : "TV non raggiungibile"; }
  else if(s.streaming) { text = "Luci sincronizzate"; }
  else if(!st.settings.enabled) { text = "Sincronizzazione spenta"; }
  else if(!st.lights.some(l => l.enabled)) { text = "Nessuna lampada attiva"; }
  else { text = "In avvio"; }
  if(!st.settings.enabled) hint = "Le lampade sono libere di essere usate dall'app Hue.";
  else if(s.tv === "off") hint = "Riparte da sola quando accendi la TV.";
  else if(s.streaming) hint = "Le lampade seguono l'Ambilight.";
  else hint = "";
  $("#status").textContent = text;
  $("#masterHint").textContent = hint;
  $("#note").textContent = s.note || "";

  for(const side of ["left","top","right"])
    st.edges[side].forEach((c, i) => { const d = document.querySelector(`.led[data-k="${side}${i}"]`); if(d) paintLight(d, c, 10); });
  const pos = {}, groups = {};
  for(const l of st.lights){
    pos[l.id] = l.enabled ? lampPos(l.zone, n) : [50, 140];
    (groups[pos[l.id].join()] = groups[pos[l.id].join()] || []).push(l.id);
  }
  for(const ids of Object.values(groups)){   // lampade nello stesso punto: affiancate
    ids.forEach((id, i) => {
      const d = i - (ids.length - 1) / 2, p = pos[id];
      pos[id] = (p[0] < 0 || p[0] > 100) ? [p[0], p[1] + d * 34] : [p[0] + d * 36, p[1]];
    });
  }
  for(const l of st.lights){
    const lamp = $("#lamp" + l.id), row = $("#row" + l.id);
    if(lamp){
      const [x, y] = pos[l.id];
      lamp.style.left = x + "%"; lamp.style.top = y + "%";
      lamp.classList.toggle("up", l.enabled && /^top|^top-/.test(l.zone));
      lamp.classList.toggle("excluded", !l.enabled);
      paintLight(lamp.querySelector(".bulb"), l.color, 14);
    }
    if(row) paintLight(row.querySelector(".dot"), l.color, 6);
  }
}

async function load(){
  try{
    const r = await fetch("/api/state"); st = await r.json();
    const key = st.lights.map(l => l.id).join(",") + "|" + counts().left + counts().top + counts().right + "|" + st.scenes.length;
    if(key !== builtFor) build(); else syncControls();
    paint();
  }catch(e){ $("#status").textContent = "Server non raggiungibile"; }
}

$("#master").onclick = () => save({enabled: !st.settings.enabled});
$("#brightness").oninput = briText; $("#brightness").onchange = e => save({brightness: +e.target.value});
$("#smoothing").oninput = smText;  $("#smoothing").onchange = e => save({smoothing: +e.target.value});
$("#scene").onchange = e => save({restore_scene: e.target.value});
load(); setInterval(load, 800);
</script>
</body>
</html>
"""


def make_handler(engine, settings):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="application/json"):
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype + "; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self._send(200, PAGE, "text/html")
            elif self.path == "/api/state":
                self._send(200, json.dumps(engine.state()))
            else:
                self._send(404, '{"error":"not found"}')

        def do_POST(self):
            if self.path != "/api/settings":
                self._send(404, '{"error":"not found"}')
                return
            try:
                length = min(int(self.headers.get("Content-Length", 0)), 65536)
                patch = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(patch, dict):
                    raise ValueError
                settings.update(patch)
            except (ValueError, TypeError, AttributeError):
                self._send(400, '{"error":"richiesta non valida"}')
                return
            self._send(200, json.dumps(engine.state()))

        def log_message(self, *args):
            pass

    return Handler


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((BRIDGE_IP, 80))
        return s.getsockname()[0]
    except OSError:
        return "localhost"
    finally:
        s.close()


def pair_ent():
    input("Premi il pulsante rotondo sul Hue Bridge, poi premi Invio qui (hai 30 secondi)...")
    r = requests.post(f"https://{BRIDGE_IP}/api",
                      json={"devicetype": "ambihue#web", "generateclientkey": True},
                      verify=False, timeout=5)
    res = r.json()[0]
    if "success" in res:
        print("Fatto! Copia questi valori nella CONFIGURAZIONE:")
        print(f'ENT_USER = "{res["success"]["username"]}"')
        print(f'ENT_KEY = "{res["success"]["clientkey"]}"')
    else:
        print("Errore:", res.get("error", {}).get("description", res))


def show_area():
    b = Bridge()
    print("Area (API v1):", b._group()["lights"])
    try:
        print("Area (API v2, lampade che ricevono lo streaming):", b.v2_members())
    except (RuntimeError, requests.RequestException, KeyError, ValueError) as e:
        print("API v2 non disponibile:", e)


def main():
    if not (TV_USER and TV_PASS and ENT_USER and ENT_KEY):
        print("Compila TV_USER, TV_PASS, ENT_USER ed ENT_KEY nella CONFIGURAZIONE.")
        return
    try:
        bytes.fromhex(ENT_KEY)
    except ValueError:
        print("ENT_KEY non valida: deve essere la stringa data da pair-ent.")
        return

    bridge = Bridge()
    try:
        found, members = bridge.area_info()
    except (RuntimeError, requests.RequestException) as e:
        print(f"Impossibile leggere l'area di intrattenimento {ENT_GROUP}: {e}")
        return

    settings = Settings()
    area = settings.merge_area(found)   # ricorda anche le lampade tolte in una sessione precedente
    settings.ensure_lights(area)
    print("Lampade dell'area:", ", ".join(f"{a.get('name')} ({i})" for i, a in area.items()))

    white = {}
    if WHITE_LIGHTS:
        try:
            all_lights = bridge.get("/lights")
        except requests.RequestException:
            all_lights = {}
        for lid in WHITE_LIGHTS:
            if lid in area:
                print(f"La lampada {lid} è già nell'area di intrattenimento: la tratto come lampada a colori.")
            elif lid in all_lights:
                white[lid] = all_lights[lid].get("name", f"Lampada {lid}")
            else:
                print(f"Lampada bianca {lid} non trovata sul bridge: ignorata.")
        settings.ensure_white(white)
        if white:
            print("Lampade bianche:", ", ".join(f"{n} ({i})" for i, n in white.items()))
    engine = Engine(settings, bridge, area, members, white)
    engine.start()

    server = ThreadingHTTPServer(("0.0.0.0", WEB_PORT), make_handler(engine, settings))
    print(f"Interfaccia: http://{local_ip()}:{WEB_PORT}  (Ctrl+C per fermare)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nChiusura...")
    finally:
        server.server_close()
        engine.stop_evt.set()
        engine.join(timeout=10)
        print("Fermato.")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "pair-ent":
        pair_ent()
    elif cmd == "area":
        show_area()
    else:
        main()
