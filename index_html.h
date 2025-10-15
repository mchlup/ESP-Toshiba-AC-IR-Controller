#pragma once

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang=\"cs\">
<head>
  <meta charset=\"UTF-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />
  <title>IR ovladač</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 2rem; }
    h1 { margin-bottom: 0.5rem; }
    section { margin-bottom: 2rem; }
    label { display: block; margin-bottom: 0.5rem; }
    input[type=text] { padding: 0.5rem; width: 280px; }
    button { margin-top: 0.5rem; padding: 0.5rem 1rem; cursor: pointer; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border: 1px solid #ddd; padding: 0.5rem; text-align: left; }
    th { background: #f4f4f4; }
    .actions button { margin-right: 0.5rem; }
    #status { font-weight: bold; }
  </style>
</head>
<body>
  <h1>ESP IR kontrolér</h1>
  <section>
    <h2>Přidat / naučit kód</h2>
    <label for=\"deviceName\">Název zařízení / funkce:</label>
    <input id=\"deviceName\" type=\"text\" placeholder=\"TV - zapnutí\" />
    <div>
      <button onclick=\"startLearning()\">Spustit učení</button>
    </div>
    <p id=\"learnHint\"></p>
  </section>

  <section>
    <h2>Uložené kódy</h2>
    <table>
      <thead>
        <tr>
          <th>Název</th>
          <th>Protokol</th>
          <th>Bitů</th>
          <th>Poslední aktualizace</th>
          <th>Akce</th>
        </tr>
      </thead>
      <tbody id=\"codes\"></tbody>
    </table>
  </section>

  <section>
    <h2>Stav</h2>
    <p id=\"status\">Načítám stav...</p>
  </section>

  <script>
    async function fetchJSON(url, options) {
      const response = await fetch(url, Object.assign({ headers: { 'Content-Type': 'application/json' } }, options));
      if (!response.ok) {
        const msg = await response.text();
        throw new Error(msg || ('HTTP ' + response.status));
      }
      return response.json();
    }

    function renderCodes(codes) {
      const tbody = document.getElementById('codes');
      tbody.innerHTML = '';
      codes.forEach(code => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>${code.name}</td>
          <td>${code.protocol}</td>
          <td>${code.bits}</td>
          <td>${new Date(code.updated).toLocaleString()}</td>
          <td class="actions">
            <button data-action="send">Vyslat</button>
            <button data-action="delete">Smazat</button>
          </td>`;
        tr.querySelector('[data-action="send"]').addEventListener('click', () => sendCode(code.name));
        tr.querySelector('[data-action="delete"]').addEventListener('click', () => deleteCode(code.name));
        tbody.appendChild(tr);
      });
    }

    async function refreshCodes() {
      try {
        const codes = await fetchJSON('/api/codes');
        renderCodes(codes.items || []);
      } catch (err) {
        console.error(err);
      }
    }

    async function refreshStatus() {
      try {
        const status = await fetchJSON('/api/status');
        document.getElementById('status').textContent = status.message;
        const hint = document.getElementById('learnHint');
        if (status.learning) {
          hint.textContent = 'Zaměřte ovladač na přijímač a stiskněte požadované tlačítko.';
        } else if (status.lastCaptured) {
          hint.textContent = 'Naposledy naučený kód: ' + status.lastCaptured;
        } else {
          hint.textContent = '';
        }
      } catch (err) {
        console.error(err);
      }
    }

    async function startLearning() {
      const name = document.getElementById('deviceName').value.trim();
      if (!name) {
        alert('Zadejte název zařízení nebo funkce.');
        return;
      }
      try {
        await fetchJSON('/api/learn', { method: 'POST', body: JSON.stringify({ name }) });
        document.getElementById('learnHint').textContent = 'Učení spuštěno, stiskněte tlačítko na ovladači.';
      } catch (err) {
        alert('Chyba při spouštění učení: ' + err.message);
      }
      refreshStatus();
    }

    async function sendCode(name) {
      try {
        await fetchJSON('/api/send', { method: 'POST', body: JSON.stringify({ name }) });
      } catch (err) {
        alert('Chyba při odesílání kódu: ' + err.message);
      }
    }

    async function deleteCode(name) {
      if (!confirm('Opravdu odstranit kód "' + name + '"?')) {
        return;
      }
      try {
        await fetchJSON('/api/codes?name=' + encodeURIComponent(name), { method: 'DELETE' });
        refreshCodes();
      } catch (err) {
        alert('Chyba při mazání kódu: ' + err.message);
      }
    }

    setInterval(refreshStatus, 2000);
    refreshCodes();
    refreshStatus();
  </script>
</body>
</html>
)rawliteral";
