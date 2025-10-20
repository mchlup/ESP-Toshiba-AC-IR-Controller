#pragma once

// ====== Web UI / API ======

inline void handleRoot() {
  String html;
  html.reserve(9000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IR Receiver – ESP32-C3</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "h1{font-size:20px;margin:0 0 12px}"
    ".muted{color:#666}"
    "table{border-collapse:collapse;width:100%;max-width:980px}"
    "th,td{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "a.btn,button.btn{display:inline-block;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa;text-decoration:none;color:#222}"
    ".row{margin:8px 0}"
    "</style></head><body>"
  );
  html += F("<h1>IR Receiver – ESP32-C3</h1>");

  html += F("<div class='muted'>IP: ");
  html += WiFi.localIP().toString();
  html += F(" &nbsp; | &nbsp; RSSI: ");
  html += WiFi.RSSI();
  html += F(" dBm</div>");

  html += F("<div class='row'>");
  html += F("<form method='POST' action='/settings' style='display:inline'>");
  html += F("<label><input type='checkbox' name='only_unk' value='1'");
  if (g_showOnlyUnknown) html += F(" checked");
  html += F("> Zobrazovat jen <b>UNKNOWN</b></label> ");
  html += F(" &nbsp; TX pin: <input type='number' name='tx_pin' min='0' max='19' value='"); html += String(g_irTxPin); html += F("' style='width:70px'>");
  html += F(" <button class='btn' type='submit'>Uložit</button>");
  html += F("</form> &nbsp; ");
  html += F("<a class='btn' href='/learn'>Učit kód</a> ");
  html += F("<a class='btn' href='/learned'>Naučené kódy</a> ");
  html += F("<a class='btn' href='/api/history'>API /history</a> ");
  html += F("<a class='btn' href='/api/learned'>API /learned</a>");
  html += F("</div>");

  html += F("<h2 style='font-size:16px;margin:16px 0 8px'>Posledních 10 kódů</h2>");
  html += F("<table><thead><tr>"
            "<th>#</th><th>čas [ms]</th><th>protokol</th><th>bits</th>"
            "<th>addr</th><th>cmd</th><th>value</th><th>flags</th><th>Akce</th></tr></thead><tbody>");

  size_t shown = 0;
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histWrite + HISTORY_LEN - 1 - i) % HISTORY_LEN;
    const IREvent &e = history[idx];
    const LearnedCode *learned = getLearnedByIndex(e.learnedIndex);
    if (g_showOnlyUnknown && !isEffectivelyUnknown(e)) continue;

    html += F("<tr><td>");
    html += ++shown;
    html += F("</td><td>");
    html += e.ms;
    html += F("</td><td>");
    if (learned && learned->proto.length()) html += learned->proto;
    else html += String(protoName(e.proto));
    html += F("</td><td>");
    html += static_cast<uint32_t>(e.bits);
    html += F("</td><td><code>0x");
    html += String(e.address, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.command, HEX);
    html += F("</code></td><td><code>0x");
    html += String(e.value, HEX);
    html += F("</code></td><td>");
    html += e.flags;
    html += F("</td><td>");

    if (isEffectivelyUnknown(e)) {
      html += F("<button class='btn' onclick=\"openLearn(");
      html += static_cast<uint32_t>(e.value);   html += F(",");
      html += static_cast<uint32_t>(e.bits);    html += F(",");
      html += static_cast<uint32_t>(e.address); html += F(",");
      html += static_cast<uint32_t>(e.flags);   html += F(",");
      html += '\'';
      if (learned && learned->proto.length()) html += learned->proto;
      else                                    html += String(protoName(e.proto));
      html += F("'");
      html += F(")\">Učit</button>");
    } else if (learned) {
      html += F("<span class='muted'>");
      if (learned->function.length()) html += learned->function;
      else html += F("Naučený kód");
      if (learned->vendor.length()) { html += F(" ("); html += learned->vendor; html += F(")"); }
      html += F("</span>");
    } else {
      html += F("<span class='muted'>—</span>");
    }

    html += F("</td></tr>");
  }

  if (shown == 0) {
    html += F("<tr><td colspan='9' class='muted'>Žádné položky k zobrazení…</td></tr>");
  }
  html += F("</tbody></table>");

  html += F("<p class='muted' style='margin-top:12px'>Tip: S volbou „jen UNKNOWN“ snadno odfiltruješ známé protokoly a zaměříš se na učení.</p>");

  // --- Modal a JS (inline) ---
  html += F(
    "<div id='learnModal' style='position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.35)'>"
      "<div style='background:#fff;padding:16px 16px 12px;border-radius:10px;min-width:300px;max-width:90vw'>"
        "<h3 style='margin:0 0 8px;font-size:16px'>Učit kód</h3>"
        "<form id='learnForm'>"
          "<input type='hidden' name='value'><input type='hidden' name='bits'>"
          "<input type='hidden' name='addr'><input type='hidden' name='flags'>"
          "<input type='hidden' name='proto'>"
          "<label>Výrobce:</label><input type='text' name='vendor' placeholder='např. Toshiba' required>"
          "<label>Funkce:</label><input type='text' name='function' placeholder='např. Power, TempUp' required>"
          "<label>Ovladač (volitelné):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
          "<div style='margin-top:10px;display:flex;gap:8px;justify-content:flex-end'>"
            "<button type='button' class='btn' id='cancelBtn'>Zrušit</button>"
            "<button type='submit' class='btn'>Uložit</button>"
          "</div>"
        "</form>"
      "</div>"
    "</div>"
    "<script>"
    "const modal=document.getElementById('learnModal');"
    "const form=document.getElementById('learnForm');"
    "const cancelBtn=document.getElementById('cancelBtn');"
    "function openLearn(value,bits,addr,flags,proto){"
      "form.value.value=value; form.bits.value=bits; form.addr.value=addr; form.flags.value=flags; form.proto.value=proto;"
      "modal.style.display='flex';"
    "}"
    "cancelBtn.onclick=()=>{modal.style.display='none'};"
    "form.onsubmit=async (e)=>{"
      "e.preventDefault();"
      "const fd=new FormData(form);"
      "const params=new URLSearchParams(fd);"
      "try{"
        "const r=await fetch('/api/learn_save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params});"
        "const j=await r.json();"
        "if(j.ok){ alert('Uloženo.'); modal.style.display='none'; location.reload(); }"
        "else{ alert('Uložení selhalo.'); }"
      "}catch(err){ alert('Chyba připojení.'); }"
    "}"
    "</script>"
  );

  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// === /settings (POST) ===
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
  server.sendHeader("Location", "/");
  server.send(302);
}

// === /api/history ===
inline void handleJsonHistory() {
  String out; out.reserve(2048);
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

// === /learn (GET) a /learn_save (POST) – stránka pro poslední UNKNOWN ===
inline void handleLearnPage() {
  String html; html.reserve(4000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Učení kódu (UNKNOWN)</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "label{display:block;margin:6px 0 2px}"
    "input[type=text]{width:100%;max-width:420px;padding:6px 8px}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "button{padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa}"
    ".muted{color:#666}"
    "</style></head><body>"
    "<h1>Učení kódu (UNKNOWN)</h1>"
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

inline void handleLearnSave() {
  if (!hasLastUnknown) { server.sendHeader("Location", "/learn"); server.send(302); return; }

  String vendor      = server.hasArg("vendor") ? server.arg("vendor") : "";
  String protoLabel  = server.hasArg("proto_label") ? server.arg("proto_label") : "";
  String remoteLabel = server.hasArg("remote_label") ? server.arg("remote_label") : "";

  vendor.trim(); protoLabel.trim(); remoteLabel.trim();

  bool ok = (vendor.length() && protoLabel.length() && remoteLabel.length()) &&
          fsAppendLearned(lastUnknown.value,
                          lastUnknown.bits,
                          lastUnknown.address,
                          lastUnknown.flags,
                          String("UNKNOWN"),
                          vendor,
                          protoLabel,
                          remoteLabel);

  String html; html.reserve(1200);
  html += F("<!doctype html><html><meta charset='utf-8'><title>Uloženo</title><body>");
  html += ok ? F("<p>✅ Kód uložen.</p>") : F("<p>❌ Uložení selhalo.</p>");
  html += F("<p><a href='/learn'>← Zpět na učení</a> &nbsp; <a href='/learned'>Naučené kódy</a> &nbsp; <a href='/'>Domů</a></p>");
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// === /learned (GET) – tabulka naučených + JS editor + ODESLAT ===
inline void handleLearnedList() {
  String data = fsReadLearnedAsArrayJSON();
  String html; html.reserve(8000);
  html += F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Naučené kódy</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:16px}"
    "table{border-collapse:collapse;width:100%;max-width:980px}"
    "th,td{border:1px solid #ddd;padding:6px 8px;font-size:14px;text-align:left}"
    "th{background:#f5f5f5}"
    "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
    "a.btn,button.btn{display:inline-block;padding:6px 10px;border-radius:8px;border:1px solid #bbb;background:#fafafa;text-decoration:none;color:#222}"
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
        "<label>Ovladač (volitelné):</label><input type='text' name='remote_label' placeholder='např. Klima Obývák'>"
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
    "function openEdit(idx,obj){"
      "idxInput.value=idx;"
      "form.proto.value=obj.proto||'UNKNOWN';"
      "form.vendor.value=obj.vendor||'';"
      "form.function.value=obj.function||'';"
      "form.remote_label.value=obj.remote_label||'';"
      "modal.style.display='flex';"
    "}"
    "cancelBtn.onclick=()=>{modal.style.display='none';};"
    "modal.addEventListener('click',e=>{if(e.target===modal){modal.style.display='none';}});"
    "form.onsubmit=async(e)=>{"
      "e.preventDefault();"
      "const fd=new FormData(form);"
      "const params=new URLSearchParams(fd);"
      "try{"
        "const r=await fetch('/api/learn_update',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params});"
        "const j=await r.json();"
        "if(j.ok){alert('Uloženo.');modal.style.display='none';location.reload();}"
        "else{alert('Uložení selhalo.');}"
      "}catch(err){alert('Chyba připojení.');}"
    "};"
    "data.forEach((o,i)=>{"
      "const tr=document.createElement('tr');"
      "function addCell(text){const td=document.createElement('td');td.textContent=text;tr.appendChild(td);}"
      "addCell(i+1);"
      "addCell(o.vendor||'');"
      "addCell(o.proto||'UNKNOWN');"
      "addCell(o.function||'');"
      "addCell(o.remote_label||'');"
      "addCell(o.bits||0);"
      "addCell(toHex(o.addr||0));"
      "addCell(toHex(o.value||0));"
      "addCell(o.flags||0);"
      "const actionTd=document.createElement('td');"
      "const btn=document.createElement('button');"
      "btn.type='button';"
      "btn.textContent='Upravit';"
      "btn.className='btn';"
      "btn.onclick=()=>openEdit(i,o);"
      "actionTd.appendChild(btn);"
      "const sbtn=document.createElement('button');"
      "sbtn.type='button'; sbtn.textContent='Odeslat'; sbtn.className='btn'; sbtn.style.marginLeft='6px';"
      "sbtn.onclick=async function(){"
        "try{"
          "const r=await fetch('/api/send?index='+i);"
          "const j=await r.json();"
          "if(!j.ok) alert('Odeslání selhalo: '+(j.err||r.status));"
        "}catch(e){ alert('Chyba odeslání: '+e); }"
      "};"
      "actionTd.appendChild(sbtn);"
      "tr.appendChild(actionTd);"
      "tb.appendChild(tr);"
    "});"
    "</script></body></html>"
  );
  server.send(200, "text/html; charset=utf-8", html);
}

// === /api/learned (GET) ===
inline void handleApiLearned() {
  server.send(200, "application/json", fsReadLearnedAsArrayJSON());
}

// === /api/learn_save (POST) – přímé uložení z hlavní stránky ===
inline void handleApiLearnSave() {
  auto need = [&](const char* k){ return server.hasArg(k) && server.arg(k).length() > 0; };
  if (!(need("value") && need("bits") && need("addr") && need("flags") && need("proto") && need("vendor") && need("function"))) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }

  uint32_t value = (uint32_t) strtoul(server.arg("value").c_str(),  nullptr, 10);
  uint8_t  bits  = (uint8_t)  strtoul(server.arg("bits").c_str(),   nullptr, 10);
  uint32_t addr  = (uint32_t) strtoul(server.arg("addr").c_str(),   nullptr, 10);
  uint32_t flags = (uint32_t) strtoul(server.arg("flags").c_str(),  nullptr, 10);

  String proto = server.arg("proto"); proto.trim();
  decode_type_t p = parseProtoLabel(proto);
  if (p == UNKNOWN) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"unsupported proto label\"}");
    return;
  }
  String vendor  = server.arg("vendor");       vendor.trim();
  String func    = server.arg("function");     func.trim();
  String remote  = server.hasArg("remote_label") ? server.arg("remote_label") : "";
  remote.trim();

  bool ok = fsAppendLearned(value, bits, addr, flags, proto, vendor, func, remote);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// === /api/learn_update (POST) – update metadat položky ===
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
  String remote = server.hasArg("remote_label") ? server.arg("remote_label") : "";
  remote.trim();

  bool ok = fsUpdateLearned(index, proto, vendor, func, remote);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// === /api/send (GET) – odeslání vybraného naučeného kódu ===
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

  const LearnedCode* e = getLearnedByIndex(idx);
  if (!e) {
    server.send(404, "application/json", "{\"ok\":false,\"err\":\"index out of range\"}");
    return;
  }

  const bool ok = irSendLearned(*e, reps);
  if (ok) { server.send(200, "application/json", "{\"ok\":true}"); return; }

  String why = "unsupported protocol";
  if (parseProtoLabel(e->proto) == UNKNOWN) why = "proto UNKNOWN or not mapped";
  String resp = String("{\"ok\":false,\"err\":\"") + why + "\"}";
  server.send(501, "application/json", resp);
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
  server.on("/api/send", handleApiSend);

  server.begin();
  Serial.println(F("[NET] WebServer běží na portu 80"));
}

inline void serviceClient() {
  server.handleClient();
  delay(1);
}

