#pragma once
// Interfaccia web
static const char PAGE[] PROGMEM = R"rawliteral(<!doctype html>
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
.toggles{border-top:1px solid var(--line);border-bottom:1px solid var(--line);margin-bottom:20px}
.toggle{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px 0}
.toggle+.toggle{border-top:1px solid var(--line)}
.toggle .title{font-size:1.1rem;font-weight:600}
.bulb{width:20px;height:20px;border-radius:50%;background:var(--off);border:2px solid var(--bg);transition:background-color .3s,box-shadow .3s}
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

  <section class="toggles">
    <div class="toggle">
      <div>
        <div class="title">Luci sincronizzate</div>
        <p class="hint" id="masterHint"></p>
        <p class="note" id="note"></p>
      </div>
      <button class="switch big" id="master" role="switch" aria-checked="false" aria-label="Luci sincronizzate"></button>
    </div>
    <div class="toggle">
      <div>
        <div class="title">Modalità cinema</div>
        <p class="hint" id="cinemaHint"></p>
      </div>
      <button class="switch big" id="cinema" role="switch" aria-checked="false" aria-label="Modalità cinema"></button>
    </div>
  </section>

  <section>
    <h2>Lampade</h2>
    <p class="hint">Tocca un nome per scegliere quando si collega e da quale punto prende il colore.</p>
    <div id="lights"></div>
    <p class="hint">Le lampade non collegate restano libere: puoi usarle con l'app Hue o con Alexa. Cambiare le lampade collegate mette in pausa le luci per un paio di secondi.</p>
  </section>

  <details class="more">
    <summary>Altre impostazioni</summary>
    <div class="body">
      <div class="toggle">
        <div>
          <div>Parti da sola quando accendi la TV</div>
          <p class="hint">Se è spento, le luci si collegano solo da qui o con la scena cinema.</p>
        </div>
        <button class="switch" id="auto" role="switch" aria-checked="false" aria-label="Parti da sola quando accendi la TV"></button>
      </div>
      <label><span class="lab">Luminosità generale <output id="briOut"></output></span>
        <input type="range" id="brightness" min="0.3" max="3" step="0.1"></label>
      <label><span class="lab">Morbidezza dei cambi <output id="smOut"></output></span>
        <input type="range" id="smoothing" min="0" max="0.95" step="0.05"></label>
      <label>Scena per le lampade «sempre» quando la sincronizzazione si ferma
        <select id="scene"></select></label>
      <label>Scena che attiva la modalità cinema (es. «Alexa, attiva Cinema»)
        <select id="cinemaScene"></select></label>
      <p class="hint">Le lampade «solo cinema» non vanno alla scena: tornano esattamente com'erano prima.</p>
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
const MODES = [["auto","Sempre con la TV"],["cinema","Solo in modalità cinema"],["off","Mai"]];
function modeLabel(m){ return (MODES.find(x => x[0] === m) || MODES[0])[1]; }
function active(l){ return l.mode === "auto" || (l.mode === "cinema" && st.status.cinema); }
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
      </div>
      <div class="row-more" hidden>
        <label>Si collega <select class="mode"></select></label>
        <label>Prende il colore da <select class="zone"></select></label>
        <label><span class="lab">Intensità <output></output></span>
          <input type="range" class="gain" min="0.1" max="2" step="0.05"></label>
        <label><span class="lab">Luminosità massima <output class="maxOut"></output></span>
          <input type="range" class="maxl" min="0.05" max="1" step="0.05"></label>
      </div>`;
    row.querySelector(".name span").textContent = l.name;
    const md = row.querySelector(".mode");
    for(const [v, t] of MODES) md.appendChild(new Option(t, v));
    md.onchange = () => save({lights: {[l.id]: {mode: md.value}}});
    const sel = row.querySelector(".zone");
    const g1 = document.createElement("optgroup"); g1.label = "Zone";
    const g2 = document.createElement("optgroup"); g2.label = "Punti singoli";
    for(const [v, t] of opts) g1.appendChild(new Option(t, v));
    for(const [v, t] of pts) g2.appendChild(new Option(t, v));
    sel.append(g1, g2);
    const more = row.querySelector(".row-more"), nameBtn = row.querySelector(".name");
    nameBtn.onclick = () => { more.hidden = !more.hidden; nameBtn.setAttribute("aria-expanded", String(!more.hidden)); };
    sel.onchange = () => save({lights: {[l.id]: {zone: sel.value}}});
    const rng = row.querySelector(".gain"), out = row.querySelector("output");
    rng.oninput = () => out.textContent = Math.round(rng.value * 100) + "%";
    rng.onchange = () => save({lights: {[l.id]: {gain: +rng.value}}});
    const mx = row.querySelector(".maxl"), mxOut = row.querySelector(".maxOut");
    mx.oninput = () => mxOut.textContent = Math.round(mx.value * 100) + "%";
    mx.onchange = () => save({lights: {[l.id]: {max_level: +mx.value}}});
    list.appendChild(row);
  }

  const sc = $("#scene"); sc.innerHTML = ""; sc.appendChild(new Option("Nessuna (le luci restano come sono)", ""));
  const cs = $("#cinemaScene"); cs.innerHTML = "";
  cs.appendChild(new Option("Automatica: la scena chiamata «Cinema»", "")); cs.appendChild(new Option("Nessuna", "-"));
  for(const s of st.scenes){
    const t = s.room ? `${s.name} (${s.room})` : s.name;
    sc.appendChild(new Option(t, s.id)); cs.appendChild(new Option(t, s.id));
  }
  builtFor = st.lights.map(l => l.id).join(",") + "|" + n.left + n.top + n.right + "|" + st.scenes.length;
  syncControls();
}

function setIfIdle(el, v){ if(document.activeElement !== el) el.value = v; }
function syncControls(){
  const s = st.settings, n = counts();
  $("#master").setAttribute("aria-checked", String(st.status.running));
  $("#cinema").setAttribute("aria-checked", String(st.status.cinema));
  $("#auto").setAttribute("aria-checked", String(s.auto_start));
  for(const l of st.lights){
    const row = $("#row" + l.id); if(!row) continue;
    row.querySelector("small").textContent = l.mode === "off" ? "Mai collegata, libera per app e Alexa"
      : modeLabel(l.mode) + " · " + zoneLabel(l.zone, n);
    setIfIdle(row.querySelector(".mode"), l.mode);
    setIfIdle(row.querySelector(".zone"), l.zone);
    const rng = row.querySelector(".gain"); setIfIdle(rng, l.gain);
    row.querySelector("output").textContent = Math.round(l.gain * 100) + "%";
    const mx = row.querySelector(".maxl"), mxv = l.max_level ?? 1; setIfIdle(mx, mxv);
    row.querySelector(".maxOut").textContent = Math.round(mx.value * 100) + "%";
  }
  setIfIdle($("#brightness"), s.brightness); briText();
  setIfIdle($("#smoothing"), s.smoothing); smText();
  setIfIdle($("#scene"), s.restore_scene);
  setIfIdle($("#cinemaScene"), s.cinema_scene);
}
function briText(){ $("#briOut").textContent = Math.round($("#brightness").value * 100) + "%"; }
function smText(){ const v = +$("#smoothing").value; $("#smOut").textContent = v < .3 ? "reattivo" : v < .7 ? "equilibrato" : "morbido"; }

function paint(){
  const s = st.status, n = counts();
  let text, hint;
  if(s.tv === "off") { text = s.tv_reachable ? "TV in standby" : "TV non raggiungibile"; }
  else if(s.streaming) { text = s.cinema ? "Cinema" : "Luci sincronizzate"; }
  else if(!s.running) { text = "In pausa"; }
  else if(!st.lights.some(active)) { text = "Nessuna lampada collegata"; }
  else { text = "In avvio"; }
  const auto = st.settings.auto_start;
  if(s.tv === "off") hint = s.running ? "Si collegano da sole quando accendi la TV." : "Avvio automatico spento: accendi da qui o con la scena cinema.";
  else if(!s.running) hint = auto ? "Le lampade sono libere. Alla prossima accensione della TV ripartono da sole." : "Le lampade sono libere.";
  else if(s.streaming) hint = "Le lampade seguono l'Ambilight. Spegnendo qui restano libere fino al prossimo spegnimento della TV.";
  else hint = "";
  $("#status").textContent = text;
  $("#masterHint").textContent = hint;
  $("#note").textContent = s.note || "";
  const cl = st.lights.filter(l => l.mode === "cinema").map(l => l.name);
  const scn = s.cinema_scene_name ? `Si attiva anche richiamando la scena «${s.cinema_scene_name}», ad esempio con Alexa.` : "Nessuna scena collegata (sceglila in Altre impostazioni).";
  $("#cinemaHint").textContent = (cl.length ? `Collega anche ${cl.join(", ")}. ` : "Nessuna lampada è impostata «solo cinema». ")
    + (s.cinema && s.tv === "off" ? "Pronta per quando accendi la TV. " : "") + scn;

  for(const side of ["left","top","right"])
    st.edges[side].forEach((c, i) => { const d = document.querySelector(`.led[data-k="${side}${i}"]`); if(d) paintLight(d, c, 10); });
  const pos = {}, groups = {};
  for(const l of st.lights){
    pos[l.id] = active(l) ? lampPos(l.zone, n) : [50, 140];
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
      lamp.classList.toggle("up", active(l) && /^top/.test(l.zone));
      lamp.classList.toggle("excluded", !active(l));
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

$("#master").onclick = () => save({running: !st.status.running});
$("#cinema").onclick = () => save({cinema: !st.status.cinema});
$("#auto").onclick = () => save({auto_start: !st.settings.auto_start});
$("#cinemaScene").onchange = e => save({cinema_scene: e.target.value});
$("#brightness").oninput = briText; $("#brightness").onchange = e => save({brightness: +e.target.value});
$("#smoothing").oninput = smText;  $("#smoothing").onchange = e => save({smoothing: +e.target.value});
$("#scene").onchange = e => save({restore_scene: e.target.value});
load(); setInterval(load, 800);
</script>
</body>
</html>
)rawliteral";
