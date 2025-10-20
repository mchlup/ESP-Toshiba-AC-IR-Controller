#pragma once

#include <vector>

// ====== Web UI / API (vylepšené, bez reloadů) ======
//
// Závislosti z .ino (beze změn):
// - extern WebServer server;
// - extern Preferences prefs;
// - extern bool g_showOnlyUnknown;
// - extern int8_t g_irTxPin;
// - extern const size_t HISTORY_LEN;
// - extern volatile size_t histWrite, histCount;
// - struct IREvent { uint32_t ms,value,address,command; uint8_t bits,flags; decode_type_t proto; int learnedIndex; };
// - extern IREvent history[];
// - extern bool hasLastUnknown; extern IREvent lastUnknown;
// - extern String jsonEscape(const String&);
// - extern const __FlashStringHelper* protoName(decode_type_t);
// - extern bool isEffectivelyUnknown(const IREvent& e);
// - struct LearnedCode { uint32_t value, addr; uint8_t bits, flags; String proto,vendor,function,remote; };
// - extern const LearnedCode* getLearnedByIndex(int idx);
// - extern bool fsAppendLearned(uint32_t value, uint8_t bits, uint32_t addr, uint32_t flags,
//                               const String& proto, const String& vendor, const String& function,
//                               const String& remote, const std::vector<uint16_t>* rawOpt, uint8_t rawKhz);
// - extern String fsReadLearnedAsArrayJSON();
// - extern bool fsUpdateLearned(size_t index, const String& proto, const String& vendor, const String& function, const String& remote);
// - extern bool irSendLearned(const LearnedCode &e, uint8_t repeats);
// - extern bool fsDeleteLearned(size_t index);
// - extern bool irSendEvent(const IREvent &ev, uint8_t repeats);
// - extern decode_type_t parseProtoLabel(const String&);
// - extern void initIrSender(int8_t txPin);

inline void handleRoot() {
  String html;
  html.reserve(12000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IR Receiver – ESP32-C3</title>"
    "<style>"
    ":root{--bg:#fff;--muted:#666;--line:#e5e5e5;--card:#fafafa;--btn:#f6f6f6;--ok:#15a34a;--err:#dc2626}"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px;background:var(--bg)}"
    "h1{font-size:20px;margin:0 0 12px}"
    ".muted{color:var(--muted)}"
    ".row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}"
    ".btn{display:inline-flex;align-items:center;gap:6px;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:var(--btn);text-decoration:none;color:#222;cursor:pointer}"
    ".btn[disabled]{opacity:.5;cursor:not-allowed}"
    "input[type=number]{width:84px;padding:4px 6px}"
    "table{border-collapse:collapse;width:100%;max-width:1100px;margin-top:8px}"
    "th,td{border:1px solid var(--line);padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5;position:sticky;top:0}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    ".diag-grid{display:flex;flex-wrap:wrap;gap:12px;margin:12px 0}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px;flex:1;min-width:260px}"
    ".card h3{margin:0 0 8px;font-size:16px}"
    ".kv{display:grid;grid-template-columns:max-content 1fr;gap:4px 12px;font-size:13px}"
    ".kv .label{color:var(--muted)}"
    ".mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    ".status-ok{color:var(--ok);font-weight:600}"
    ".status-err{color:var(--err);font-weight:600}"
    "#toast{position:fixed;right:12px;bottom:12px;display:none;padding:10px 12px;border-radius:8px;color:#fff;font-weight:500}"
    "#toast.ok{background:var(--ok)}#toast.err{background:var(--err)}"
    "#learnModal{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.35)}"
    "#learnModal .card{background:#fff;padding:16px 16px 12px;border-radius:10px;min-width:300px;max-width:90vw}"
    "#learnModal label{display:block;margin:6px 0 2px}"
    "#learnModal input[type=text]{width:100%;max-width:420px;padding:6px 8px}"
    "</style></head><body>"
  );

  html += F("<h1>IR Receiver – ESP32-C3</h1><div class='muted' id='hdr'></div>");

  // Ovládací řádek (AJAX /settings)
  html += F(
    "<div class='row'>"
      "<label><input id='onlyUnk' type='checkbox'> Jen <b>UNKNOWN</b></label>"
      "<span style='margin-left:12px'>TX pin: <input id='txPin' type='number' min='0' max='19'></span>"
      "<button id='saveBtn' class='btn'>Uložit</button>"
      "<a class='btn' href='/learn'>Učit kód</a>"
      "<a class='btn' href='/learned'>Naučené kódy</a>"
      "<a class='btn' href='/api/history'>API /history</a>"
      "<a class='btn' href='/api/learned'>API /learned</a>"
    "</div>"
  );

  html += F(
    "<div class='diag-grid'>"
      "<div class='card'>"
        "<h3>Diagnostika příjmu</h3>"
        "<div class='kv'>"
          "<span class='label'>Stav:</span><span id='rawState' class='muted'>Čekám na signál…</span>"
          "<span class='label'>Zdroj:</span><span id='rawSource' class='mono'>–</span>"
          "<span class='label'>Délka:</span><span id='rawLen'>0 pulzů</span>"
          "<span class='label'>Frekvence:</span><span id='rawFreq'>0 kHz</span>"
          "<span class='label'>Stáří:</span><span id='rawAge'>–</span>"
        "</div>"
        "<div class='kv' style='margin-top:8px'>"
          "<span class='label'>Ukázka:</span><span id='rawPreview' class='mono muted'>—</span>"
        "</div>"
        "<div class='row' style='margin-top:10px'>"
          "<button id='rawSendBtn' class='btn' disabled>Odeslat RAW</button>"
          "<span class='muted'>repeat <input id='rawRepeat' type='number' min='0' max='3' value='0' style='width:60px'></span>"
          "<a id='rawDownload' class='btn' href='/api/raw_dump' target='_blank'>Stáhnout JSON</a>"
        "</div>"
      "</div>"
      "<div class='card'>"
        "<h3>Diagnostika odesílání</h3>"
        "<div class='kv'>"
          "<span class='label'>Poslední stav:</span><span id='sendState' class='muted'>Bez záznamu</span>"
          "<span class='label'>Metoda:</span><span id='sendMethod' class='mono'>–</span>"
          "<span class='label'>Protokol:</span><span id='sendProto' class='mono'>–</span>"
          "<span class='label'>Pulzy:</span><span id='sendPulses'>0</span>"
          "<span class='label'>Frekvence:</span><span id='sendFreq'>–</span>"
          "<span class='label'>Stáří:</span><span id='sendAge'>–</span>"
        "</div>"
      "</div>"
    "</div>"
  );

  // Tabulka
  html += F(
    "<h2 style='font-size:16px;margin:16px 0 8px'>Posledních 10 kódů</h2>"
    "<table><thead><tr>"
    "<th>#</th><th>čas [ms]</th><th>protokol</th><th>bits</th>"
    "<th>addr</th><th>cmd</th><th>value</th><th>flags</th><th>Akce</th>"
    "</tr></thead><tbody id='tb'></tbody></table>"
    "<p class='muted' style='margin-top:12px'>Tip: S volbou „jen UNKNOWN“ snadno odfiltruješ známé protokoly a zaměříš se na učení.</p>"
  );

  // Modal „Učit“ + skripty
  html += F(
    "<div id='learnModal'><div class='card'>"
      "<h3 style='margin:0 0 8px;font-size:16px'>Učit kód</h3>"
      "<form id='learnForm'>"
        "<input type='hidden' name='value'><input type='hidden' name='bits'>"
        "<input type='hidden' name='addr'><input type='hidden' name='flags'>"
        "<input type='hidden' name='proto'>"
        "<label>Výrobce:</label><input type='text' name='vendor' placeholder='např. Toshiba' required>"
        "<label>Funkce:</label><input type='text' name='function' placeholder='např. Power, TempUp' required>"
        "<label>Ovladač (volit.):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
        "<div style='margin-top:10px;display:flex;gap:8px;justify-content:flex-end'>"
          "<button type='button' class='btn' id='cancelBtn'>Zrušit</button>"
          "<button type='submit' class='btn' id='saveLearn'>Uložit</button>"
        "</div>"
      "</form>"
    "</div></div>"
    "<div id='toast'></div>"
    "<script>"
    "const hdr=document.getElementById('hdr');"
    "const tb=document.getElementById('tb');"
    "const toast=document.getElementById('toast');"
    "const onlyUnk=document.getElementById('onlyUnk');"
    "const txPin=document.getElementById('txPin');"
    "const saveBtn=document.getElementById('saveBtn');"
    "const modal=document.getElementById('learnModal');"
    "const form=document.getElementById('learnForm');"
    "const cancelBtn=document.getElementById('cancelBtn');"
    "const rawState=document.getElementById('rawState');"
    "const rawSource=document.getElementById('rawSource');"
    "const rawLen=document.getElementById('rawLen');"
    "const rawFreq=document.getElementById('rawFreq');"
    "const rawAge=document.getElementById('rawAge');"
    "const rawPreview=document.getElementById('rawPreview');"
    "const rawSendBtn=document.getElementById('rawSendBtn');"
    "const rawRepeat=document.getElementById('rawRepeat');"
    "const sendState=document.getElementById('sendState');"
    "const sendMethod=document.getElementById('sendMethod');"
    "const sendProto=document.getElementById('sendProto');"
    "const sendPulses=document.getElementById('sendPulses');"
    "const sendFreq=document.getElementById('sendFreq');"
    "const sendAge=document.getElementById('sendAge');"
    "let state={onlyUnknown:false,tx:0};"
    "function showToast(msg,ok=true){toast.textContent=msg;toast.className=ok?'ok':'err';toast.style.display='block';setTimeout(()=>toast.style.display='none',2000)}"
    "function toHex(n){return '0x'+(Number(n)>>>0).toString(16).toUpperCase()}"
    "function fmtAge(ms){if(!ms||ms<0)return '–';if(ms<1000)return ms+' ms';if(ms<60000)return (ms/1000).toFixed(1)+' s';return (ms/60000).toFixed(1)+' min'}"
    "function openLearn(v,b,a,f,p){form.value.value=v;form.bits.value=b;form.addr.value=a;form.flags.value=f;form.proto.value=p;modal.style.display='flex'}"
    "cancelBtn.onclick=()=>{modal.style.display='none'};"
    "modal.addEventListener('click',e=>{if(e.target===modal)modal.style.display='none'});"
    "rawSendBtn.onclick=async()=>{rawSendBtn.disabled=true;let rep=parseInt(rawRepeat.value||'0',10);if(isNaN(rep))rep=0;rep=Math.max(0,Math.min(3,rep));try{const r=await fetch('/api/raw_send?repeat='+rep);const j=await r.json();if(j.ok){showToast('RAW odeslán.');}else{showToast(j.err||'Odeslání RAW selhalo',false);}}catch(err){showToast('Chyba odeslání RAW',false);}rawSendBtn.disabled=false;loadDiag();};"

    // Uložení learned
    "form.onsubmit=async e=>{e.preventDefault();"
      "document.getElementById('saveLearn').disabled=true;"
      "const fd=new FormData(form);"
      "const body=new URLSearchParams(fd);"
      "try{const r=await fetch('/api/learn_save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
           "const j=await r.json();"
           "if(j.ok){showToast('Uloženo.');modal.style.display='none';loadHistory(); loadDiag();}"
           "else{showToast(j.err||'Uložení selhalo',false)}"
      "}catch(err){showToast('Chyba připojení',false)}"
      "document.getElementById('saveLearn').disabled=false;"
    "};"

    // Načtení historie
    "async function loadHistory(){"
      "try{const r=await fetch('/api/history'); const j=await r.json();"
          "hdr.textContent='IP: '+j.ip+'  |  RSSI: '+j.rssi+' dBm';"
          "onlyUnk.checked = !!j.only_unknown;"
          "tb.innerHTML='';"
          "let shown=0;"
          "j.history.forEach((e,idx)=>{"
            "if(onlyUnk.checked && !e.proto.includes('UNKNOWN') && !(!e.learned && e.proto==='UNKNOWN')) return;"
            "shown++;"
            "const tr=document.createElement('tr');"
            "function td(t){const x=document.createElement('td');x.textContent=t;tr.appendChild(x)}"
            "td(shown); td(e.ms); td(e.learned_proto||e.proto); td(e.bits);"
            "td(toHex(e.addr)); td(toHex(e.cmd)); td(toHex(e.value)); td(e.flags);"
            "const act=document.createElement('td');"
            "const sendBtn=document.createElement('button');sendBtn.className='btn';sendBtn.textContent='Odeslat';"
            "sendBtn.onclick=async()=>{sendBtn.disabled=true;try{const r=await fetch('/api/history_send?ms='+e.ms);const j=await r.json();if(j.ok){showToast('Odesláno.');}else{showToast(j.err||'Odeslání selhalo',false);}}catch(err){showToast('Chyba odeslání',false);}sendBtn.disabled=false;loadDiag();};"
            "act.appendChild(sendBtn);"
            "if(e.proto.includes('UNKNOWN')||(!e.learned&&e.learned_proto==='')){"
              "const b=document.createElement('button');b.className='btn';b.textContent='Učit';b.style.marginLeft='6px';"
              "b.onclick=()=>openLearn(e.value,e.bits,e.addr,e.flags,(e.learned_proto||e.proto));"
              "act.appendChild(b);"
            "}else{"
              "const span=document.createElement('span');span.className='muted';span.style.marginLeft='6px';"
              "span.textContent = (e.learned_function||'Naučený kód') + (e.learned_vendor?(' ('+e.learned_vendor+')'):'');"
              "act.appendChild(span);"
            "}"
            "tr.appendChild(act); tb.appendChild(tr);"
          "});"
          "if(shown===0){const tr=document.createElement('tr');const td=document.createElement('td');td.colSpan=9;td.className='muted';td.textContent='Žádné položky k zobrazení…';tr.appendChild(td);tb.appendChild(tr)}"
      "}catch(err){/* noop */}"
    "}"

    "async function loadDiag(){"
      "try{const r=await fetch('/api/diag');const j=await r.json();"
          "rawState.textContent=j.raw.valid?'Zachyceno':'Čekám na signál…';"
          "rawState.className=j.raw.valid?'status-ok':'muted';"
          "rawSource.textContent=j.raw.source||'–';"
          "rawLen.textContent=j.raw.valid?(j.raw.len+' pulzů'):'0 pulzů';"
          "rawFreq.textContent=j.raw.valid?((j.raw.freq||0)+' kHz'):'–';"
          "rawAge.textContent=j.raw.valid?fmtAge(j.raw.age_ms||0):'–';"
          "if(j.raw.preview&&j.raw.preview.length){rawPreview.textContent=j.raw.preview.join(', ')+(j.raw.preview_truncated?', …':'');rawPreview.className='mono';}else{rawPreview.textContent='—';rawPreview.className='mono muted';}"
          "rawSendBtn.disabled=!j.raw.valid;"
          "sendState.textContent=j.send.valid?(j.send.ok?'OK':'Chyba'):'Bez záznamu';"
          "sendState.className=j.send.valid?(j.send.ok?'status-ok':'status-err'):'muted';"
          "sendMethod.textContent=j.send.method||'–';"
          "sendProto.textContent=j.send.proto||'–';"
          "sendPulses.textContent=j.send.valid?(j.send.pulses+' pulzů'):'–';"
          "sendFreq.textContent=j.send.valid&&(j.send.freq)?j.send.freq+' kHz':'–';"
          "sendAge.textContent=j.send.valid?fmtAge(j.send.age_ms||0):'–';"
      "}catch(err){/* noop */}"
    "}"

    // Uložení nastavení bez reloadu
    "saveBtn.onclick=async()=>{"
      "saveBtn.disabled=true;"
      "try{const p=new URLSearchParams();"
          "p.set('only_unk',onlyUnk.checked?'1':'0');"
          "if(txPin.value!=='') p.set('tx_pin',txPin.value);"
          "const r=await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});"
          "if(r.status===302||r.ok){showToast('Nastavení uloženo'); loadHistory(); loadDiag();}"
          "else showToast('Uložení nastavení selhalo',false);"
      "}catch(e){showToast('Chyba připojení',false)}"
      "saveBtn.disabled=false;"
    "};"

    // Init – načti historii a z API /history nahraj current TX pin/flag (přijdou nepřímo: only_unknown už je tam)
    "const refresh=()=>{loadHistory(); loadDiag();};"
    "document.addEventListener('DOMContentLoaded',()=>{refresh(); setInterval(refresh,2000);});"
  );

  // Předvyplň aktuální hodnoty (serverem vložené)
  html += F("</script>");

  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// === /settings (POST) – zachováno, nyní voláno AJAXem ===
inline void handleSettingsPost() {
  bool only = (server.hasArg("only_unk") && server.arg("only_unk") == "1");
  g_showOnlyUnknown = only;
  prefs.putBool("only_unk", g_showOnlyUnknown);

  if (server.hasArg("tx_pin") && server.arg("tx_pin").length() > 0) {
    int np = strtol(server.arg("tx_pin").c_str(), nullptr, 10);
    if (np >= 0 && np <= 19) {
      if (np != g_irTxPin) {
        g_irTxPin = (int8_t)np;
        prefs.putInt("tx_pin", g_irTxPin);
        initIrSender(g_irTxPin);
      }
    }
  }

  // Pro kompatibilitu se stávajícím kódem necháme 302 (AJAX to zvládne)
  server.sendHeader("Location", "/");
  server.send(302);
}

// === /api/history (GET) – beze změn ve struktuře ===
inline void handleJsonHistory() {
  String out; out.reserve(3072);
  out += F("{\"ip\":\""); out += WiFi.localIP().toString();
  out += F("\",\"rssi\":"); out += WiFi.RSSI();
  out += F(",\"only_unknown\":"); out += g_showOnlyUnknown ? "true" : "false";
  out += F(",\"history\":[");
  bool first = true;
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
    if (g_showOnlyUnknown && !isEffectivelyUnknown(e)) continue;
    if (!first) out += ',';
    out += F("{\"ms\":"); out += e.ms;
    out += F(",\"proto\":\"");
    String protoStr = learned && learned->proto.length() ? learned->proto : String(protoName(e.proto));
    out += jsonEscape(protoStr);
    out += F("\",\"bits\":"); out += static_cast<uint32_t>(e.bits);
    out += F(",\"addr\":");   out += e.address;
    out += F(",\"cmd\":");    out += e.command;
    out += F(",\"value\":");  out += e.value;
    out += F(",\"flags\":");  out += e.flags;
    out += F(",\"learned\":"); out += learned ? "true" : "false";
    out += F(",\"learned_proto\":\""); if (learned && learned->proto.length()) out += jsonEscape(learned->proto);
    out += F("\",\"learned_vendor\":\""); if (learned && learned->vendor.length()) out += jsonEscape(learned->vendor);
    out += F("\",\"learned_function\":\""); if (learned && learned->function.length()) out += jsonEscape(learned->function);
    out += F("\",\"learned_remote\":\""); if (learned && learned->remote.length()) out += jsonEscape(learned->remote);
    out += F("\"}");
    first = false;
  }
  out += F("]}");
  server.send(200, "application/json", out);
}

// === /learn (GET) – informativní stránka pro poslední UNKNOWN ===
inline void handleLearnPage() {
  String html; html.reserve(4000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Učení kódu (UNKNOWN)</title>"
    "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "label{display:block;margin:6px 0 2px}"
    "input[type=text]{width:100%;max-width:420px;padding:6px 8px}"
    ".muted{color:#666}</style></head><body><h1>Učení kódu (UNKNOWN)</h1>"
  );
  if (!hasLastUnknown) {
    html += F("<p class='muted'>Zatím nebyl zachycen žádný validní kód typu <b>UNKNOWN</b>. "
              "Vrať se na <a href='/'>hlavní stránku</a> a zkus odeslat IR z ovladače.</p></body></html>");
    server.send(200, "text/html; charset=utf-8", html);
    return;
  }
  html += F("<p>Poslední UNKNOWN zachycený kód:</p><ul>");
  html += F("<li>bits: ");   html += static_cast<uint32_t>(lastUnknown.bits); html += F("</li>");
  html += F("<li>addr: <code>0x"); html += String(lastUnknown.address, HEX); html += F("</code></li>");
  html += F("<li>cmd:  <code>0x"); html += String(lastUnknown.command, HEX); html += F("</code></li>");
  html += F("<li>value:<code>0x"); html += String(lastUnknown.value, HEX);   html += F("</code></li>");
  html += F("<li>flags: ");  html += lastUnknown.flags;                  html += F("</li></ul>");

  html += F(
    "<form method='POST' action='/learn_save'>"
    "<label>Výrobce zařízení (vendor):</label>"
    "<input type='text' name='vendor' placeholder='např. Toshiba' required>"
    "<label>Označení protokolu (label):</label>"
    "<input type='text' name='proto_label' placeholder='např. Toshiba-IR-RAW' required>"
    "<label>Označení ovladače / zařízení:</label>"
    "<input type='text' name='remote_label' placeholder='např. Klima Obývák' required>"
    "<div style='margin-top:10px'><button type='submit'>Uložit do naučených</button></div>"
    "</form>"
    "<p class='muted' style='margin-top:10px'>Pozn.: ukládá se aktuálně poslední zachycený UNKNOWN kód.</p>"
    "<p><a href='/'>← Zpět</a> &nbsp; <a href='/learned'>Naučené kódy</a></p>"
    "</body></html>"
  );
  server.send(200, "text/html; charset=utf-8", html);
}

// === /learn_save (POST) – stránka „Učit“ (UNKNOWN) – nová verze ===
inline void handleLearnSave() {
  if (!hasLastUnknown) { server.sendHeader("Location", "/learn"); server.send(302); return; }

  String vendor      = server.hasArg("vendor") ? server.arg("vendor") : "";
  String protoLabel  = server.hasArg("proto_label") ? server.arg("proto_label") : "";
  String remoteLabel = server.hasArg("remote_label") ? server.arg("remote_label") : "";

  vendor.trim(); protoLabel.trim(); remoteLabel.trim();
  if (!vendor.length() || !protoLabel.length() || !remoteLabel.length()) {
    server.send(400, "text/plain", "Missing fields");
    return;
  }

  // Ukládáme vždy jako "UNKNOWN" (z pohledu dekodéru); UI label si neseš v protoLabel parametru – ten už máš ve storage jako string 'proto'
  bool ok = fsAppendLearned(lastUnknown.value, lastUnknown.bits, lastUnknown.address, lastUnknown.flags,
                            String("UNKNOWN"), vendor, protoLabel, remoteLabel);

  String html;
  html += F("<!doctype html><html><meta charset='utf-8'><title>Uloženo</title><body>");
  html += ok ? F("<p>✅ Kód uložen (včetně RAW, je-li k dispozici).</p>") : F("<p>❌ Uložení selhalo.</p>");
  html += F("<p><a href='/learn'>← Zpět na učení</a> &nbsp; <a href='/learned'>Naučené kódy</a> &nbsp; <a href='/'>Domů</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// === /learned (GET) – tabulka naučených + inline editor + ODESLAT (repeat) ===
inline void handleLearnedList() {
  String data = fsReadLearnedAsArrayJSON();
  String html; html.reserve(9000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Naučené kódy</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "table{border-collapse:collapse;width:100%;max-width:1100px}"
    "th,td{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    ".btn{display:inline-block;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa;text-decoration:none;color:#222}"
    "#editModal{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.35)}"
    "#editModal .card{background:#fff;padding:16px 16px 12px;border-radius:10px;min-width:320px;max-width:90vw}"
    "#editModal label{display:block;margin:6px 0 2px;font-size:14px}"
    "#editModal input[type=text]{width:100%;padding:6px 8px;font-size:14px}"
    "</style></head><body>"
    "<h1>Naučené kódy</h1>"
    "<table><thead><tr>"
    "<th>#</th><th>vendor</th><th>proto</th><th>function</th><th>remote_label</th>"
    "<th>bits</th><th>addr</th><th>value</th><th>flags</th><th>Akce</th>"
    "</tr></thead><tbody id='tb'></tbody></table>"
    "<p><a href='/'>← Domů</a></p>"
    "<div id='editModal'><div class='card'>"
      "<h3 style='margin:0 0 8px;font-size:16px'>Upravit kód</h3>"
      "<form id='editForm'>"
        "<input type='hidden' name='index'>"
        "<label>Protokol:</label><input type='text' name='proto' placeholder='např. NEC' required>"
        "<label>Výrobce:</label><input type='text' name='vendor' placeholder='např. Toshiba' required>"
        "<label>Funkce:</label><input type='text' name='function' placeholder='např. Power, TempUp' required>"
        "<label>Ovladač (volit.):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
        "<div style='margin-top:10px;display:flex;gap:8px;justify-content:flex-end'>"
          "<button type='button' class='btn' id='editCancel'>Zrušit</button>"
          "<button type='submit' class='btn'>Uložit</button>"
        "</div>"
      "</form>"
    "</div></div>"
    "<script>const data="
  );
  html += data;
  html += F(";"
    "const tb=document.getElementById('tb');"
    "const modal=document.getElementById('editModal');"
    "const form=document.getElementById('editForm');"
    "const cancelBtn=document.getElementById('editCancel');"
    "const idxInput=form.querySelector('input[name=index]');"
    "function toHex(num){return '0x'+((num>>>0).toString(16).toUpperCase());}"
    "function openEdit(idx,obj){idxInput.value=idx;form.proto.value=obj.proto||'UNKNOWN';form.vendor.value=obj.vendor||'';form.function.value=obj.function||'';form.remote_label.value=obj.remote_label||'';modal.style.display='flex'}"
    "cancelBtn.onclick=()=>{modal.style.display='none'};"
    "modal.addEventListener('click',e=>{if(e.target===modal){modal.style.display='none'}});"
    "form.onsubmit=async(e)=>{e.preventDefault();const fd=new FormData(form);const params=new URLSearchParams(fd);try{const r=await fetch('/api/learn_update',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params});const j=await r.json();if(j.ok){alert('Uloženo.');modal.style.display='none';location.reload();}else{alert('Uložení selhalo.');}}catch(err){alert('Chyba připojení.');}};"
    "data.forEach((o,i)=>{const tr=document.createElement('tr');"
      "function cell(t){const td=document.createElement('td');td.textContent=t;tr.appendChild(td)}"
      "cell(i+1); cell(o.vendor||''); cell(o.proto||'UNKNOWN'); cell(o.function||''); cell(o.remote_label||'');"
      "cell(o.bits||0); cell(toHex(o.addr||0)); cell(toHex(o.value||0)); cell(o.flags||0);"
      "const act=document.createElement('td');"
      "const edit=document.createElement('button'); edit.className='btn'; edit.textContent='Upravit'; edit.onclick=()=>openEdit(i,o); act.appendChild(edit);"
      // Odeslat s repeat volbou
      "const sbtn=document.createElement('button'); sbtn.className='btn'; sbtn.textContent='Odeslat'; sbtn.style.marginLeft='6px';"
      "const rep=document.createElement('input'); rep.type='number'; rep.min=0; rep.max=3; rep.value=0; rep.title='repeat'; rep.style.width='56px'; rep.style.marginLeft='6px';"
      "sbtn.onclick=async()=>{try{const r=await fetch('/api/send?index='+i+'&repeat='+rep.value); const j=await r.json(); if(!j.ok) alert('Odeslání selhalo: '+(j.err||'error'));}catch(e){alert('Chyba odeslání: '+e);}};"
      "act.appendChild(sbtn); act.appendChild(rep);"
      "const del=document.createElement('button'); del.className='btn'; del.textContent='Smazat'; del.style.marginLeft='6px';"
      "del.onclick=async()=>{if(!confirm('Smazat tento kód?')) return; const params=new URLSearchParams(); params.set('index',i); try{const r=await fetch('/api/learn_delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params}); const j=await r.json(); if(j.ok){alert('Smazáno.'); location.reload();}else{alert('Smazání selhalo.');}}catch(err){alert('Chyba smazání.');}};"
      "act.appendChild(del); tr.appendChild(act); tb.appendChild(tr);"
    "});"
    "</script></body></html>"
  );
  server.send(200, "text/html; charset=utf-8", html);
}

// === /api/learned (GET) – JSON list ===
inline void handleApiLearned() {
  server.send(200, "application/json", fsReadLearnedAsArrayJSON());
}

// === /api/learn_save (POST) – povolí i UNKNOWN, nic neblokuje
inline void handleApiLearnSave() {
  auto need = [&](const char* k){ return server.hasArg(k) && server.arg(k).length() > 0; };
  if (!(need("value") && need("bits") && need("addr") && need("flags")
        && need("proto") && need("vendor") && need("function"))) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }

  uint32_t value = (uint32_t) strtoul(server.arg("value").c_str(),  nullptr, 10);
  uint8_t  bits  = (uint8_t)  strtoul(server.arg("bits").c_str(),   nullptr, 10);
  uint32_t addr  = (uint32_t) strtoul(server.arg("addr").c_str(),   nullptr, 10);
  uint32_t flags = (uint32_t) strtoul(server.arg("flags").c_str(),  nullptr, 10);

  String proto  = server.arg("proto");   proto.trim();   // může být "UNKNOWN"
  String vendor = server.arg("vendor");  vendor.trim();
  String func   = server.arg("function");func.trim();
  String remote = server.hasArg("remote_label") ? server.arg("remote_label") : "";
  remote.trim();

  std::vector<uint16_t> rawFromRequest;
  const std::vector<uint16_t>* rawPtr = nullptr;
  uint8_t rawFreq = 38;

  if (server.hasArg("raw") && server.arg("raw").length() > 0) {
    if (!parseRawDurationsArg(server.arg("raw"), rawFromRequest)) {
      server.send(400, "application/json", "{\"ok\":false,\"err\":\"invalid raw list\"}");
      return;
    }

    auto parseFreq = [&](const String &s) {
      if (!s.length()) return;
      long freq = strtol(s.c_str(), nullptr, 10);
      if (freq < 15) freq = 15;
      if (freq > 80) freq = 80;
      rawFreq = static_cast<uint8_t>(freq);
    };

    if (server.hasArg("freq")) {
      parseFreq(server.arg("freq"));
    } else if (server.hasArg("freq_khz")) {
      parseFreq(server.arg("freq_khz"));
    } else if (server.hasArg("khz")) {
      parseFreq(server.arg("khz"));
    }

    rawPtr = &rawFromRequest;
  }

  bool ok = fsAppendLearned(value, bits, addr, flags, proto, vendor, func, remote, rawPtr, rawFreq);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}


// === /api/learn_update (POST) – update metadat ===
inline void handleApiLearnUpdate() {
  auto need = [&](const char* k){ return server.hasArg(k) && server.arg(k).length() > 0; };
  if (!need("index") || !need("proto") || !need("vendor") || !need("function")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }
  size_t index = static_cast<size_t>(strtoul(server.arg("index").c_str(), nullptr, 10));
  String proto  = server.arg("proto");        proto.trim();
  String vendor = server.arg("vendor");       vendor.trim();
  String func   = server.arg("function");     func.trim();
  String remote = server.hasArg("remote_label") ? server.arg("remote_label") : ""; remote.trim();
  bool ok = fsUpdateLearned(index, proto, vendor, func, remote);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

inline void handleApiLearnDelete() {
  if (!server.hasArg("index")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing index\"}");
    return;
  }
  size_t index = static_cast<size_t>(strtoul(server.arg("index").c_str(), nullptr, 10));
  bool ok = fsDeleteLearned(index);
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// === /api/send (GET) – odeslání naučeného kódu (support repeat) ===
// === /api/send (GET) – odeslání naučeného kódu (nová verze) ===
inline void handleApiSend() {
  if (!server.hasArg("index")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing index\"}");
    return;
  }
  const int idx = strtol(server.arg("index").c_str(), nullptr, 10);
  uint8_t reps = 0;
  if (server.hasArg("repeat")) {
    long r = strtol(server.arg("repeat").c_str(), nullptr, 10);
    if (r < 0) r = 0; if (r > 3) r = 3; reps = (uint8_t)r;
  }

  const bool ok = irSendLearnedByIndex(idx, reps);
  if (ok) { server.send(200, "application/json", "{\"ok\":true}"); return; }

  server.send(501, "application/json", "{\"ok\":false,\"err\":\"no mapped proto and no RAW\"}");
}

inline void handleApiHistorySend() {
  if (!server.hasArg("ms")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing ms\"}");
    return;
  }
  uint32_t targetMs = static_cast<uint32_t>(strtoul(server.arg("ms").c_str(), nullptr, 10));
  uint8_t repeats = 0;
  if (server.hasArg("repeat")) {
    long r = strtol(server.arg("repeat").c_str(), nullptr, 10);
    if (r < 0) r = 0;
    if (r > 3) r = 3;
    repeats = static_cast<uint8_t>(r);
  }

  const IREvent* match = nullptr;
  for (size_t i = 0; i < histCount; ++i) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &ev = history[idx];
    if (ev.ms == targetMs) {
      match = &ev;
      break;
    }
  }

  if (!match) {
    server.send(404, "application/json", "{\"ok\":false,\"err\":\"not found\"}");
    return;
  }

  bool ok = irSendEvent(*match, repeats);
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"send failed\"}");
}

inline void handleApiDiag() {
  server.send(200, "application/json", buildDiagnosticsJson());
}

inline void handleApiRawSend() {
  uint8_t repeats = 0;
  if (server.hasArg("repeat")) {
    long r = strtol(server.arg("repeat").c_str(), nullptr, 10);
    if (r < 0) r = 0;
    if (r > 3) r = 3;
    repeats = static_cast<uint8_t>(r);
  }
  bool ok = irSendLastRaw(repeats);
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"no raw\"}");
}

inline void handleApiRawDump() {
  server.send(200, "application/json", buildRawDumpJson());
}


// ====== Router a běh webu ======
inline void startWebServer() {
  server.on("/", handleRoot);
  server.on("/settings", HTTP_POST, handleSettingsPost);

  server.on("/learn", handleLearnPage);
  server.on("/learn_save", HTTP_POST, handleLearnSave);
  server.on("/learned", handleLearnedList);

  server.on("/api/history", handleJsonHistory);
  server.on("/api/learned", handleApiLearned);
  server.on("/api/learn_save", HTTP_POST, handleApiLearnSave);
  server.on("/api/learn_update", HTTP_POST, handleApiLearnUpdate);
  server.on("/api/learn_delete", HTTP_POST, handleApiLearnDelete);
  server.on("/api/send", handleApiSend);
  server.on("/api/history_send", handleApiHistorySend);
  server.on("/api/diag", handleApiDiag);
  server.on("/api/raw_send", handleApiRawSend);
  server.on("/api/raw_dump", handleApiRawDump);

  server.begin();
  Serial.println(F("[NET] WebServer běží na portu 80"));
}

inline void serviceClient() {
  server.handleClient();
  delay(1);
}