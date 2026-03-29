#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <map>
#include <vector>
#include <ArduinoJson.h>

// ─── CONFIG ────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "esp_test";
const char* WIFI_PASSWORD = "12345678";
const int   SNIFF_CHANNEL = 1;          // Starting channel (1-13)
const int   CHANNEL_HOP_MS = 500;       // Channel hop interval
// ──────────────────────────────────────────────────────────────────────────

WebServer server(80);

// ─── Device record ─────────────────────────────────────────────────────────
struct DeviceInfo {
  int   rssi;
  int   packetCount;
  unsigned long lastSeen;
  int   channel;
};

std::map<uint64_t, DeviceInfo> deviceMap;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

int  currentChannel   = SNIFF_CHANNEL;
bool alertActive      = false;
int  prevDeviceCount  = 0;

// ─── Sniffer callback ──────────────────────────────────────────────────────
void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  int rssi = pkt->rx_ctrl.rssi;

  // Extract source MAC from bytes [10..15]
  uint64_t mac = 0;
  for (int i = 0; i < 6; i++) {
    mac = (mac << 8) | payload[10 + i];
  }
  if (mac == 0) return;

  portENTER_CRITICAL_ISR(&mux);
  auto& d = deviceMap[mac];
  d.rssi = rssi;
  d.packetCount++;
  d.lastSeen = millis();
  d.channel  = currentChannel;
  portEXIT_CRITICAL_ISR(&mux);
}

// ─── Channel hopper ────────────────────────────────────────────────────────
unsigned long lastHop = 0;
void hopChannel() {
  if (millis() - lastHop > CHANNEL_HOP_MS) {
    currentChannel = (currentChannel % 13) + 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastHop = millis();
  }
}

// ─── Stale device pruning (> 10 s) ────────────────────────────────────────
unsigned long lastPrune = 0;
void pruneStale() {
  if (millis() - lastPrune < 2000) return;
  lastPrune = millis();
  unsigned long now = millis();
  portENTER_CRITICAL(&mux);
  for (auto it = deviceMap.begin(); it != deviceMap.end(); ) {
    if (now - it->second.lastSeen > 10000) it = deviceMap.erase(it);
    else ++it;
  }
  portEXIT_CRITICAL(&mux);
}

// ─── Helpers ───────────────────────────────────────────────────────────────
const char* proximityLabel(int rssi) {
  if (rssi >= -60) return "Near";
  if (rssi >= -80) return "Mid";
  return "Far";
}

const char* statusLabel(int rssi) {
  if (rssi >= -60) return "Active";
  if (rssi >= -80) return "Weak";
  return "Far";
}

float averageRSSI() {
  if (deviceMap.empty()) return 0;
  float sum = 0;
  for (auto& kv : deviceMap) sum += kv.second.rssi;
  return sum / deviceMap.size();
}

int totalPacketRate() {
  int total = 0;
  for (auto& kv : deviceMap) total += kv.second.packetCount;
  return total;
}

const char* activityLevel(int rate) {
  if (rate < 20)  return "Low";
  if (rate < 100) return "Medium";
  return "High";
}

String macToString(uint64_t mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
    (uint8_t)((mac >> 40) & 0xFF),
    (uint8_t)((mac >> 32) & 0xFF),
    (uint8_t)((mac >> 24) & 0xFF),
    (uint8_t)((mac >> 16) & 0xFF),
    (uint8_t)((mac >>  8) & 0xFF),
    (uint8_t)( mac        & 0xFF));
  return String(buf);
}

// ─── Dashboard HTML (served from PROGMEM) ─────────────────────────────────
// Stored as a raw string – we use a placeholder token for the IP to keep
// the HTML self-contained.  The JS polls /data which is always relative.
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wireless Environment Monitor</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;600;800&display=swap');

  :root{
    --bg:#080c14;
    --panel:#0d1522;
    --border:#1a2a44;
    --accent:#00e5ff;
    --green:#00ff88;
    --yellow:#ffd600;
    --red:#ff3d5a;
    --text:#c8d8f0;
    --dim:#4a6080;
    --radius:12px;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:'Exo 2',sans-serif;
       min-height:100vh;padding:0 0 40px}

  /* scan-line overlay */
  body::before{content:'';position:fixed;inset:0;
    background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,229,255,.018) 2px,rgba(0,229,255,.018) 4px);
    pointer-events:none;z-index:999}

  header{display:flex;align-items:center;justify-content:space-between;
         padding:18px 20px;border-bottom:1px solid var(--border);
         background:linear-gradient(135deg,#0d1a2e,#071020);position:sticky;top:0;z-index:100}
  header h1{font-size:1.1rem;font-weight:800;letter-spacing:.12em;text-transform:uppercase;
             color:var(--accent);text-shadow:0 0 16px rgba(0,229,255,.5)}
  .pulse{width:10px;height:10px;border-radius:50%;background:var(--green);
         box-shadow:0 0 8px var(--green);animation:pulse 1.4s ease-in-out infinite}
  @keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.4;transform:scale(.8)}}

  /* alert banner */
  #alertBanner{display:none;background:linear-gradient(90deg,#3a0010,#1a0008);
               border:1px solid var(--red);color:var(--red);text-align:center;
               padding:10px 16px;font-size:.85rem;letter-spacing:.05em;
               animation:blink 1s step-start infinite}
  @keyframes blink{50%{opacity:.5}}

  /* summary cards */
  .cards{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;padding:16px 14px}
  @media(min-width:600px){.cards{grid-template-columns:repeat(4,1fr)}}

  .card{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
        padding:16px 14px;position:relative;overflow:hidden;
        transition:border-color .3s}
  .card::after{content:'';position:absolute;inset:0;opacity:.04;
               background:linear-gradient(135deg,var(--accent),transparent)}
  .card-label{font-size:.65rem;letter-spacing:.12em;text-transform:uppercase;
               color:var(--dim);margin-bottom:6px}
  .card-value{font-family:'Share Tech Mono',monospace;font-size:1.7rem;
               color:var(--accent);text-shadow:0 0 12px rgba(0,229,255,.4);line-height:1}
  .card-sub{font-size:.7rem;color:var(--dim);margin-top:4px}

  /* device table */
  .section-title{font-size:.7rem;letter-spacing:.15em;text-transform:uppercase;
                 color:var(--dim);padding:8px 16px 4px}
  .table-wrap{overflow-x:auto;padding:0 14px}
  table{width:100%;border-collapse:collapse;font-size:.82rem}
  thead th{text-align:left;padding:8px 10px;font-size:.65rem;letter-spacing:.1em;
            text-transform:uppercase;color:var(--dim);border-bottom:1px solid var(--border)}
  tbody tr{border-bottom:1px solid rgba(26,42,68,.6);transition:background .2s}
  tbody tr:hover{background:rgba(0,229,255,.04)}
  td{padding:9px 10px;font-family:'Share Tech Mono',monospace;vertical-align:middle}
  td:first-child{font-family:'Exo 2',sans-serif;font-size:.78rem;max-width:140px;
                 overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

  /* badge */
  .badge{display:inline-block;padding:2px 9px;border-radius:20px;
         font-size:.7rem;font-weight:600;letter-spacing:.05em}
  .near,.active{background:rgba(0,255,136,.15);color:var(--green);border:1px solid rgba(0,255,136,.3)}
  .mid,.weak  {background:rgba(255,214,0,.12);color:var(--yellow);border:1px solid rgba(255,214,0,.25)}
  .far        {background:rgba(255,61,90,.12);color:var(--red);border:1px solid rgba(255,61,90,.25)}

  /* rssi bar */
  .bar-wrap{width:60px;height:6px;background:var(--border);border-radius:3px;display:inline-block;vertical-align:middle;margin-right:6px}
  .bar-fill{height:100%;border-radius:3px;transition:width .5s}

  #lastUpdate{font-size:.7rem;color:var(--dim);text-align:center;margin-top:16px;
               font-family:'Share Tech Mono',monospace}
  .empty-msg{text-align:center;color:var(--dim);font-size:.85rem;padding:24px 0;
              letter-spacing:.1em}
</style>
</head>
<body>

<header>
  <h1>&#9〉 Wireless Environment Monitor</h1>
  <div class="pulse" id="pulseDot"></div>
</header>

<div id="alertBanner">&#9888; ALERT: Unusual wireless activity detected!</div>

<div class="cards">
  <div class="card">
    <div class="card-label">Total Devices</div>
    <div class="card-value" id="totalDevices">—</div>
    <div class="card-sub">nearby probes</div>
  </div>
  <div class="card">
    <div class="card-label">Avg Signal</div>
    <div class="card-value" id="avgRssi">—</div>
    <div class="card-sub">dBm</div>
  </div>
  <div class="card">
    <div class="card-label">Activity</div>
    <div class="card-value" id="activity" style="font-size:1.2rem;padding-top:4px">—</div>
    <div class="card-sub">packet rate</div>
  </div>
  <div class="card">
    <div class="card-label">Channel</div>
    <div class="card-value" id="channel">—</div>
    <div class="card-sub">2.4 GHz</div>
  </div>
</div>

<div class="section-title">&#x25A0; Device Table</div>
<div class="table-wrap">
  <table>
    <thead>
      <tr>
        <th>Device ID</th>
        <th>RSSI</th>
        <th>Proximity</th>
        <th>Status</th>
      </tr>
    </thead>
    <tbody id="deviceTable">
      <tr><td colspan="4" class="empty-msg">Scanning…</td></tr>
    </tbody>
  </table>
</div>

<div id="lastUpdate">Last update: —</div>

<script>
const REFRESH_MS = 2500;

function rssiColor(rssi){
  if(rssi>=-60) return 'var(--green)';
  if(rssi>=-80) return 'var(--yellow)';
  return 'var(--red)';
}
function rssiPct(rssi){
  // map -100..-30 → 0..100
  return Math.max(0,Math.min(100,((rssi+100)/70)*100)).toFixed(0);
}
function proxClass(p){ return p.toLowerCase(); }
function statusClass(s){ return s.toLowerCase(); }

async function fetchData(){
  try{
    const res = await fetch('/data');
    if(!res.ok) return;
    const d = await res.json();

    document.getElementById('totalDevices').textContent = d.totalDevices ?? '0';
    document.getElementById('avgRssi').textContent      = d.avgRssi ?? '—';
    document.getElementById('channel').textContent      = d.channel ?? '—';

    const actEl = document.getElementById('activity');
    actEl.textContent = d.activity ?? '—';
    actEl.style.color = d.activity==='High' ? 'var(--red)'
                      : d.activity==='Medium' ? 'var(--yellow)'
                      : 'var(--green)';

    // alert banner
    const banner = document.getElementById('alertBanner');
    banner.style.display = d.alert ? 'block' : 'none';

    // device table
    const tbody = document.getElementById('deviceTable');
    const devices = d.devices || [];
    if(!devices.length){
      tbody.innerHTML='<tr><td colspan="4" class="empty-msg">No devices detected</td></tr>';
    } else {
      tbody.innerHTML = devices.map(dev=>{
        const pct = rssiPct(dev.rssi);
        const col = rssiColor(dev.rssi);
        const pc  = proxClass(dev.proximity);
        const sc  = statusClass(dev.status);
        return `<tr>
          <td title="${dev.mac}">${dev.mac}</td>
          <td>
            <span class="bar-wrap"><span class="bar-fill" style="width:${pct}%;background:${col}"></span></span>
            <span style="color:${col}">${dev.rssi}</span>
          </td>
          <td><span class="badge ${pc}">${dev.proximity}</span></td>
          <td><span class="badge ${sc}">${dev.status}</span></td>
        </tr>`;
      }).join('');
    }

    const now = new Date();
    document.getElementById('lastUpdate').textContent =
      'Last update: '+now.toLocaleTimeString();

  } catch(e){ console.warn('Fetch error',e); }
}

fetchData();
setInterval(fetchData, REFRESH_MS);
</script>
</body>
</html>
)rawliteral";

// ─── /data endpoint ────────────────────────────────────────────────────────
void handleData() {
  portENTER_CRITICAL(&mux);
  std::vector<std::pair<uint64_t, DeviceInfo>> snap(deviceMap.begin(), deviceMap.end());
  portEXIT_CRITICAL(&mux);

  int  count     = snap.size();
  int  rate      = 0;
  float rssiSum  = 0;

  for (auto& kv : snap) {
    rssiSum += kv.second.rssi;
    rate    += kv.second.packetCount;
  }

  float avgRssi = count ? rssiSum / count : 0;

  // Alert: sudden spike > 5 new devices OR very high activity
  bool alert = (count - prevDeviceCount > 5) || (rate > 200);
  prevDeviceCount = count;

  StaticJsonDocument<4096> doc;
  doc["totalDevices"] = count;
  doc["avgRssi"]      = (int)avgRssi;
  doc["channel"]      = currentChannel;
  doc["activity"]     = activityLevel(rate);
  doc["alert"]        = alert;

  JsonArray devArr = doc.createNestedArray("devices");
  // Sort by RSSI descending
  std::sort(snap.begin(), snap.end(),
  [](const std::pair<uint64_t, DeviceInfo>& a,
     const std::pair<uint64_t, DeviceInfo>& b) {
    return a.second.rssi > b.second.rssi;
  });

  for (auto& kv : snap) {
    JsonObject obj = devArr.createNestedObject();
    obj["mac"]      = macToString(kv.first);
    obj["rssi"]     = kv.second.rssi;
    obj["proximity"]= proximityLabel(kv.second.rssi);
    obj["status"]   = statusLabel(kv.second.rssi);
  }

  String json;
  serializeJson(doc, json);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ─── setup ─────────────────────────────────────────────────────────────────
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);

  // Connect to AP (needed to host the web server)
  WiFi.mode(WIFI_STA);
WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

Serial.print("Connecting WiFi");

int tries = 0;
while (WiFi.status() != WL_CONNECTED && tries < 20) {
  delay(500);
  Serial.print('.');
  Serial.print(" status=");
  Serial.println(WiFi.status());
  tries++;
}

if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\nConnected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
} else {
  Serial.println("\n❌ FAILED TO CONNECT");
}
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // Switch to promiscuous mode for sniffing while keeping AP mode
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  esp_wifi_set_channel(SNIFF_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Web server routes
  server.on("/", [](){
    server.send_P(200, "text/html", DASHBOARD_HTML);
  });
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP server started");
}

// ─── loop ──────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  hopChannel();
  pruneStale();
}