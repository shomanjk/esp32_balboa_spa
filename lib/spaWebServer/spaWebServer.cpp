#include "spaWebServer.h"

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoLog.h>
#include <webLogBuffer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <base64.hpp>
#include "FS.h"
#include <LittleFS.h>
#ifdef spaEpaper
#include <epd47.h>
#endif

#define FORMAT_LITTLEFS_IF_FAILED true

// Internal libraries

#include <tinyxml2.h>
#include <cmath>
#include <ctime>
#include <spaMessage.h>
#include <spaUtilities.h>
#include <restartReason.h>
#include <rs485.h>
#include "../../src/main.h"

// Local functions

void handleConfig(AsyncWebServerRequest *request);
void handleStatus(AsyncWebServerRequest *request);
void handleState(AsyncWebServerRequest *request);
void handleVersion(AsyncWebServerRequest *request);
void handleWifi(AsyncWebServerRequest *request);
void handleStatusControlsApi(AsyncWebServerRequest *request);
void handleDiagToggleApi(AsyncWebServerRequest *request);
void handleDiagToggleSequenceApi(AsyncWebServerRequest *request);
void handleDiagLight1NextCtsApi(AsyncWebServerRequest *request);
void handleDiagLight1NextCtsWindowApi(AsyncWebServerRequest *request);
void handleRs485(AsyncWebServerRequest *request);
void handleRs485Raw(AsyncWebServerRequest *request);
void handleRs485History(AsyncWebServerRequest *request);
void handleSlash(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleData(AsyncWebServerRequest *request);
void handleLoginData(AsyncWebServerRequest *request);
void handleOptionsData(AsyncWebServerRequest *request);
void handleOptionsLoginData(AsyncWebServerRequest *request);
void handleepdpanel(AsyncWebServerRequest *request);
void handleLogsApi(AsyncWebServerRequest *request);
void handleLogsPage(AsyncWebServerRequest *request);
void handleLogsConfigGet(AsyncWebServerRequest *request);
void handleLogsConfigPost(AsyncWebServerRequest *request);
String parseBody(String body);
void listDir(fs::FS &fs, const char *dirname, uint8_t levels);
String listDirToString(fs::FS &fs, const char *dirname, uint8_t levels);

static const char *wifiStatusName(wl_status_t s)
{
  switch (s)
  {
  case WL_IDLE_STATUS:
    return "Idle";
  case WL_NO_SSID_AVAIL:
    return "No SSID";
  case WL_SCAN_COMPLETED:
    return "Scan completed";
  case WL_CONNECTED:
    return "Connected";
  case WL_CONNECT_FAILED:
    return "Connect failed";
  case WL_CONNECTION_LOST:
    return "Connection lost";
  case WL_DISCONNECTED:
    return "Disconnected";
  default:
    return "Unknown";
  }
}

static String rs485HealthLabel(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "Healthy";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "Bytes seen, no valid frames";
  }
  return "No UART bytes";
}

static String rs485HealthColor(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "#04AA6D";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "#ef6c00";
  }
  return "#c62828";
}

static String rs485ModeHint(bool inverted)
{
  return inverted
             ? "Inverted RX/TX mode is active (virtual A/B swap)."
             : "Normal RX/TX mode is active.";
}

static String rs485HealthHint(const String &health)
{
  if (health == "VALID_FRAMES_OK")
  {
    return "Balboa frames are decoding correctly.";
  }
  if (health == "UART_BYTES_NO_VALID_FRAMES")
  {
    return "Signal is present, but framing/quality is not yet valid.";
  }
  return "No bus activity detected at UART RX yet.";
}

static void appendWifiStateSection(String &html)
{
  wl_status_t st = WiFi.status();
  bool ok = (st == WL_CONNECTED);

  html += "</ul></section><section class='panel'><h1>WiFi</h1><ul>";
  html += "<li><b>Status: </b><span id=\"wf-st\">";
  html += wifiStatusName(st);
  html += " (";
  html += String(static_cast<int>(st));
  html += ")</span></li>";

  html += "<li><b>SSID: </b><span id=\"wf-ssid\">";
  html += ok ? WiFi.SSID() : String("—");
  html += "</span></li>";

  html += "<li><b>Hostname: </b><span id=\"wf-host\">";
  html += WiFi.getHostname() ? String(WiFi.getHostname()) : String("—");
  html += "</span></li>";

  html += "<li><b>IP: </b><span id=\"wf-ip\">";
  html += ok ? WiFi.localIP().toString() : String("—");
  html += "</span></li>";

  html += "<li><b>Gateway: </b><span id=\"wf-gw\">";
  html += ok ? WiFi.gatewayIP().toString() : String("—");
  html += "</span></li>";

  html += "<li><b>Subnet: </b><span id=\"wf-sn\">";
  html += ok ? WiFi.subnetMask().toString() : String("—");
  html += "</span></li>";

  html += "<li><b>DNS: </b><span id=\"wf-dns\">";
  html += ok ? WiFi.dnsIP(0).toString() : String("—");
  html += "</span></li>";

  html += "<li><b>Channel: </b><span id=\"wf-ch\">";
  html += ok ? String(WiFi.channel()) : String("—");
  html += "</span></li>";

  html += "<li><b>MAC: </b><span id=\"wf-mac\">";
  html += WiFi.macAddress();
  html += "</span></li>";

  html += "<li><b>Signal (RSSI): </b><span id=\"wf-rssi\" style=\"font-weight:600\">";
  html += ok ? String(WiFi.RSSI()) + " dBm" : String("—");
  html += "</span> <span id=\"wf-quality\" style=\"font-weight:600\"></span></li>";
  html += "<li><b>5 min Avg RSSI: </b><span id=\"wf-avg\">—</span></li>";
  html += "</ul>";

  html += "<p class='chart-title'><b>RSSI over time</b> (5s samples, ~5 min window)</p>";
  html += "<div class='chart-wrap'><canvas id=\"wifiRssiChart\" height=\"160\"></canvas></div>";
  html += "<script>";
  html += "(function(){var pollMs=5000,maxPts=60,warnRssi=-75,badRssi=-80;var c=document.getElementById('wifiRssiChart');";
  html += "if(!c)return;var x=c.getContext('2d'),d=[];";
  html += "function set(t,v){var e=document.getElementById(t);if(e)e.textContent=v;}";
  html += "function colorOf(v){if(v<=badRssi)return '#c62828';if(v<=warnRssi)return '#ef6c00';return '#04AA6D';}";
  html += "function qualityOf(v){if(v<=badRssi)return 'Weak';if(v<=warnRssi)return 'Fair';if(v<=-67)return 'Good';return 'Excellent';}";
  html += "function yOf(v,lo,hi,h){return h-8-(v-lo)/(hi-lo)*(h-16);}";
  html += "function resizeCanvas(){var p=c.parentElement;var cssW=p?Math.max(280,p.clientWidth-2):320;var cssH=160;var dpr=window.devicePixelRatio||1;c.width=Math.round(cssW*dpr);c.height=Math.round(cssH*dpr);c.style.width=cssW+'px';c.style.height=cssH+'px';x.setTransform(1,0,0,1,0,0);x.scale(dpr,dpr);draw();}";
  html += "function draw(){var w=parseFloat(c.style.width)||320,h=parseFloat(c.style.height)||160;x.fillStyle='#fff';x.fillRect(0,0,w,h);";
  html += "x.strokeStyle='#ccc';x.strokeRect(0.5,0.5,w-1,h-1);x.fillStyle='#333';x.font='12px sans-serif';";
  html += "if(d.length<1){x.fillText('Collecting samples…',10,80);return;}";
  html += "var lo=-100,hi=-30,i,m;";
  html += "for(i=0;i<d.length;i++){m=d[i];if(m<lo)lo=m;if(m>hi)hi=m;}";
  html += "if(warnRssi<lo)lo=warnRssi-2;if(warnRssi>hi)hi=warnRssi+2;if(badRssi<lo)lo=badRssi-2;if(badRssi>hi)hi=badRssi+2;";
  html += "if(hi-lo<8){lo-=4;hi+=4;}";
  html += "x.fillText(lo+' dBm',4,h-4);x.fillText(hi+' dBm',4,14);";
  html += "x.strokeStyle='rgba(239,108,0,0.65)';x.setLineDash([4,4]);x.beginPath();x.moveTo(10,yOf(warnRssi,lo,hi,h));x.lineTo(w-10,yOf(warnRssi,lo,hi,h));x.stroke();";
  html += "x.strokeStyle='rgba(198,40,40,0.75)';x.beginPath();x.moveTo(10,yOf(badRssi,lo,hi,h));x.lineTo(w-10,yOf(badRssi,lo,hi,h));x.stroke();x.setLineDash([]);";
  html += "x.strokeStyle='#04AA6D';x.lineWidth=1.5;x.beginPath();";
  html += "for(i=0;i<d.length;i++){var slot=maxPts-d.length+i;var px=10+slot*(w-20)/Math.max(1,maxPts-1);";
  html += "var py=yOf(d[i],lo,hi,h);if(i===0)x.moveTo(px,py);else x.lineTo(px,py);}";
  html += "x.stroke();}";
  html += "function poll(){fetch('/api/wifi').then(function(r){return r.json();}).then(function(j){";
  html += "set('wf-st',j.statusName+' ('+j.status+')');";
  html += "set('wf-ssid',j.connected?j.ssid:'—');set('wf-host',j.hostname||'—');";
  html += "set('wf-ip',j.connected?j.ip:'—');set('wf-gw',j.connected?j.gateway:'—');";
  html += "set('wf-sn',j.connected?j.subnet:'—');set('wf-dns',j.connected?j.dns:'—');";
  html += "set('wf-ch',j.connected?String(j.channel):'—');set('wf-mac',j.mac||'—');";
  html += "if(j.connected&&typeof j.rssi==='number'){set('wf-rssi',j.rssi+' dBm');set('wf-quality','('+qualityOf(j.rssi)+')');";
  html += "var rc=document.getElementById('wf-rssi'),qc=document.getElementById('wf-quality');if(rc)rc.style.color=colorOf(j.rssi);if(qc)qc.style.color=colorOf(j.rssi);";
  html += "d.push(j.rssi);if(d.length>maxPts)d.shift();var sum=0;for(var k=0;k<d.length;k++)sum+=d[k];set('wf-avg',(sum/d.length).toFixed(1)+' dBm');draw();}";
  html += "else{set('wf-rssi','—');set('wf-quality','');set('wf-avg','—');var rc=document.getElementById('wf-rssi');if(rc)rc.style.color='';d=[];draw();}}).catch(function(){});}";
  html += "window.addEventListener('resize',resizeCanvas);window.addEventListener('orientationchange',resizeCanvas);resizeCanvas();poll();setInterval(poll,pollMs);})();";
  html += "</script>";

  html += "</section><section class='panel'><h1>Spa Status</h1><ul>";
}

AsyncWebServer server(80);
static AsyncWebSocket wsLog("/api/logs/ws");
static uint32_t wsLogBroadcastSeq = 0;
bool serverSetup = false;

static void onWsLogEvent(AsyncWebSocket *wsServer, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  (void)arg;
  (void)data;
  (void)len;
  if (type == WS_EVT_CONNECT)
  {
    String hist;
    webLogBufferBuildJsonFull(hist);
    client->text(hist);
    wsLogBroadcastSeq = webLogBufferNewestSeq();
  }
  else if (type == WS_EVT_DISCONNECT && wsServer->count() == 0)
  {
    wsLogBroadcastSeq = webLogBufferNewestSeq();
  }
}

static void spaWebServerLogPoll()
{
  wsLog.cleanupClients();
  if (wsLog.count() == 0)
  {
    return;
  }
  const uint32_t newest = webLogBufferNewestSeq();
  if (newest <= wsLogBroadcastSeq)
  {
    return;
  }
  String delta;
  webLogBufferAppendJsonDelta(wsLogBroadcastSeq, newest, delta);
  if (delta.length() > 0)
  {
    wsLog.textAll(delta);
  }
  wsLogBroadcastSeq = newest;
}

void spaWebServerSetup()
{
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED))
  {
    Log.error("[Web]: Error LittleFS Mount Failed");
  }
  else
  {
    Log.notice("[Web]: LittleFS Mounted" CR);
    listDir(LittleFS, "/", 3);
  }
  Log.notice("[Web]: Web App config" CR);
  File envFile = LittleFS.open("/.env", "r");
  if (envFile)
  {
    // Log.notice("[Web]: .env file found" CR);
    while (envFile.available())
    {
      String line = envFile.readStringUntil('\n');
      Log.notice("[Web]: /.env - %s" CR, line.c_str());
    }
    envFile.close();
  }
  else
  {
    Log.error("[Web]: .env file not found" CR);
  }
  // put your setup code here, to run once:
  Log.verbose(F("[Web]: spaWebServerSetup()" CR));
}

void spaWebServerLoop()
{
  if (!serverSetup)
  {
    server.on("/", HTTP_GET, handleState);
    server.on("/state", HTTP_GET, handleState);
    server.on("/config", HTTP_GET, handleConfig);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/api/version", HTTP_GET, handleVersion);
    server.on("/api/wifi", HTTP_GET, handleWifi);
    server.on("/api/status/controls", HTTP_GET, handleStatusControlsApi);
    server.on("/api/diag/toggle", HTTP_GET, handleDiagToggleApi);
    server.on("/api/diag/toggle_sequence", HTTP_GET, handleDiagToggleSequenceApi);
    server.on("/api/diag/light1_next_cts", HTTP_GET, handleDiagLight1NextCtsApi);
    server.on("/api/diag/light1_next_cts_window", HTTP_GET, handleDiagLight1NextCtsWindowApi);
    server.on("/api/rs485/raw", HTTP_GET, handleRs485Raw);
    server.on("/api/rs485/history", HTTP_GET, handleRs485History);
    server.on("/api/rs485", HTTP_GET, handleRs485);
    server.on("/api/logs", HTTP_GET, handleLogsApi);
    server.on("/api/logs/config", HTTP_GET, handleLogsConfigGet);
    server.on("/api/logs/config", HTTP_POST, handleLogsConfigPost, NULL, handleBody);
    server.on("/logs", HTTP_GET, handleLogsPage);
    wsLog.onEvent(onWsLogEvent);
    server.addHandler(&wsLog);
#ifdef spaEpaper
    server.on("/panel.jpg", HTTP_GET, handleepdpanel);
#endif
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      Log.notice(F("[Web]: Restart requested by %p" CR), request->client()->remoteIP());
      AsyncWebServerResponse *response = request->beginResponse(302);
      response->addHeader("Location", "/");
      request->send(response);
      delay(1000);
      ESP.restart(); });

    // Balboa cloud emulation

    server.on("/devices/sci", HTTP_OPTIONS, handleOptionsData);
    server.on("/devices/sci", HTTP_POST, handleData, NULL, handleBody);

    server.on("/users/login", HTTP_OPTIONS, handleOptionsLoginData);
    server.on("/users/login", HTTP_POST, handleLoginData, NULL, handleBody);

    server.onNotFound(handleNotFound);

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");

    server.begin();
    serverSetup = true;
    Log.notice(F("[Web]: Web server started at http://%p/" CR), WiFi.localIP());
  }
  spaWebServerLogPoll();
}

#ifdef spaEpaper
void handleepdpanel(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: Request %s received from %p - size %d" CR, request->url().c_str(), request->client()->remoteIP(), jpegSize);
  if (captureToJPEG() > 0)
  {
    // Send the BMP image as a response
    AsyncWebServerResponse *response = request->beginResponse_P(200, "image/jpeg", jpegBuffer, jpegSize);
    response->addHeader("Content-Disposition", "inline; filename=\"framebuffer.jpeg\"");
    request->send(response);
  }
  else
  {
    request->send(404, "text/plain", "Image not available");
  }
}
#endif

#define style String("<style>:root{--bg:#f4f7f8;--panel:#fff;--text:#1f2933;--muted:#5f6c7b;--brand:#037e52;--brandActive:#4b5563;--border:#d4dbe1;--focus:#0f4a87;--space-1:6px;--space-2:10px;--space-3:14px;--space-4:20px;}*{box-sizing:border-box;}body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text);line-height:1.5;}html,body{max-width:100%;overflow-x:hidden;}img,canvas{display:block;max-width:100%;height:auto;}.skip-link{position:absolute;left:10px;top:-48px;z-index:999;background:#0f4a87;color:#fff;padding:10px 12px;border-radius:6px;text-decoration:none;}.skip-link:focus{top:10px;outline:3px solid #fff;outline-offset:2px;}.page{max-width:980px;margin:0 auto;padding:var(--space-3);}h1{color:#0f4a87;font-size:1.05rem;margin:0 0 var(--space-2) 0;line-height:1.3;}.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:var(--space-3);margin-bottom:var(--space-3);box-shadow:0 1px 2px rgba(0,0,0,.04);}ul{list-style:none;margin:0;padding:0;}li{padding:var(--space-1) 0;border-bottom:1px dashed #e5eaef;overflow-wrap:anywhere;word-break:break-word;}li:last-child{border-bottom:none;}.spacer{height:8px;border-bottom:none;padding:0;}.top-nav{display:flex;flex-wrap:wrap;gap:var(--space-1);margin-bottom:var(--space-3);}button{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;flex:1 1 170px;font-weight:600;transition:background-color .15s ease,transform .15s ease;}.active{background-color:var(--brandActive);color:#fff;}@media (hover:hover){button:hover{background-color:var(--brandActive);}}button:focus-visible{outline:3px solid var(--focus);outline-offset:2px;}button:active{transform:translateY(1px);}.panel-image{width:100%;max-width:600px;margin:0 auto var(--space-3) auto;border-radius:8px;}.chart-title{margin:12px 0 6px 0;color:var(--muted);}.chart-wrap{width:100%;max-width:100%;overflow:hidden;border:1px solid #ccc;background:#fff;border-radius:6px;}#wf-rssi,#wf-quality{font-weight:700;}@media (max-width:640px){.page{padding:var(--space-2);}button{flex:1 1 100%;width:100%;}.panel{padding:var(--space-2);}h1{font-size:1rem;}}@media (prefers-reduced-motion:reduce){button{transition:none;}}</style>")

#define icon String("<link rel='icon' href='/assets/style/hottubbing.webp' type='image/x-icon' />")

#define head String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Spa Web Server State</title>") + icon + style + String("</head>")

#define webMenuStatus String("<nav aria-label='Portal navigation'><form class='top-nav'><button class='active' formaction='/status'>SPA Status</button><button formaction='/config'>SPA Config</button><button formaction='/state'>ESP State</button><button formaction='/logs'>Logs</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define webMenuConfig String("<nav aria-label='Portal navigation'><form class='top-nav'><button formaction='/status'>SPA Status</button><button class='active' formaction='/config'>SPA Config</button><button formaction='/state'>ESP State</button><button formaction='/logs'>Logs</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define webMenuState String("<nav aria-label='Portal navigation'><form class='top-nav'><button formaction='/status'>SPA Status</button><button formaction='/config'>SPA Config</button><button class='active' formaction='/state'>ESP State</button><button formaction='/logs'>Logs</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define webMenuLogs String("<nav aria-label='Portal navigation'><form class='top-nav'><button formaction='/status'>SPA Status</button><button formaction='/config'>SPA Config</button><button formaction='/state'>ESP State</button><button class='active' formaction='/logs'>Logs</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define headLogs String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Spa Logs</title>") + icon + style + String("<style>.log-pre{min-height:260px;max-height:70vh;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:12px;border-radius:8px;white-space:pre-wrap;word-break:break-word;margin:0;border:1px solid var(--border)}.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:12px}.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px}.log-controls label{font-size:14px;color:var(--muted)}.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px}</style></head>")

#ifdef spaEpaper
#define ePaper String("<img class='panel-image' src='panel.jpg' alt='Spa Panel'>")
#else
#define ePaper String("")
#endif

/** Local wall time for /status; invalid or epoch 0 → "Time not synced". */
static String statusFormatEpochLocalHuman(time_t t)
{
  if (t <= 0)
  {
    return String("Time not synced");
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (!p)
  {
    return String("Time not synced");
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", p) == 0)
  {
    return String("Time not synced");
  }
  return String(buf);
}

/** Primary local date/time + optional collapsible raw Unix epoch (seconds). */
static String statusLastUpdateDisplayHtml(unsigned long epoch)
{
  time_t t = static_cast<time_t>(epoch);
  String primary = statusFormatEpochLocalHuman(t);
  if (epoch == 0UL)
  {
    return primary;
  }
  String out = primary;
  out += "<details class=\"history-raw\"><summary>Raw epoch (Unix s)</summary>";
  out += formatNumberWithCommas(epoch);
  out += "</details>";
  return out;
}

static String webWallClockDisplayHtml(time_t t)
{
  if (t <= 0)
  {
    return statusLastUpdateDisplayHtml(0UL);
  }
  return statusLastUpdateDisplayHtml(static_cast<unsigned long>(t));
}

static void appendStatusKvRow(String &html, const char *label, const String &value)
{
  html += "<div class=\"kv-row\"><dt>";
  html += label;
  html += "</dt><dd>";
  html += value;
  html += "</dd></div>";
}

static bool statusSpaConfigReady()
{
  return spaConfigurationData.lastUpdate != 0;
}

/** Configuration 0x2E: pump two-bit 0 = None (not installed). */
static bool statusPumpConfiguredAbsent(unsigned pumpId)
{
  if (!statusSpaConfigReady() || pumpId < 1 || pumpId > 6)
  {
    return false;
  }
  const uint8_t *p = &spaConfigurationData.pump1;
  return p[pumpId - 1] == 0;
}

static uint8_t statusPumpConfigSpeed(unsigned pumpId)
{
  if (!statusSpaConfigReady() || pumpId < 1 || pumpId > 6)
  {
    return 0;
  }
  const uint8_t *p = &spaConfigurationData.pump1;
  return p[pumpId - 1];
}

static uint8_t statusPumpRawState(unsigned pumpId)
{
  if (pumpId < 1 || pumpId > 6)
  {
    return 0;
  }
  const uint8_t *p = &spaStatusData.pump1;
  return p[pumpId - 1];
}

static bool statusPumpIsOn(unsigned pumpId)
{
  return statusPumpRawState(pumpId) > 0;
}

static String statusPumpDisplayState(unsigned pumpId)
{
  const uint8_t cfg = statusPumpConfigSpeed(pumpId);
  if (cfg == 1)
  {
    return getMapDescription(statusPumpIsOn(pumpId) ? 1 : 0, onOffMap);
  }
  return getMapDescription(statusPumpRawState(pumpId), pumpMap);
}

static void fillPumpDiagSnapshot(JsonObject obj)
{
  obj["statusLastUpdate"] = spaStatusData.lastUpdate;
  obj["pump1"] = spaStatusData.pump1;
  obj["pump2"] = spaStatusData.pump2;
  obj["pump3"] = spaStatusData.pump3;
  obj["pump4"] = spaStatusData.pump4;
  obj["pump5"] = spaStatusData.pump5;
  obj["pump6"] = spaStatusData.pump6;
  obj["pump1On"] = statusPumpIsOn(1);
  obj["pump2On"] = statusPumpIsOn(2);
  obj["pump3On"] = statusPumpIsOn(3);
  obj["pump4On"] = statusPumpIsOn(4);
  obj["pump5On"] = statusPumpIsOn(5);
  obj["pump6On"] = statusPumpIsOn(6);
  obj["light1"] = spaStatusData.light1 ? 1 : 0;
  obj["setTemp"] = spaStatusData.setTemp;
  obj["heatingState"] = spaStatusData.heatingState;
  if (spaStatusData.rawDataLength > 20)
  {
    JsonObject statusBytes = obj.createNestedObject("statusBytes");
    statusBytes["hf"] = spaStatusData.rawData[10];
    statusBytes["pp"] = spaStatusData.rawData[11];
    statusBytes["lf"] = spaStatusData.rawData[14];
    statusBytes["stRaw"] = spaStatusData.rawData[20];
  }
}

/** Configuration byte 2: 0 = None, 1 = Present. */
static bool statusLightConfiguredAbsent(unsigned lightId)
{
  if (!statusSpaConfigReady() || lightId < 1 || lightId > 2)
  {
    return false;
  }
  return lightId == 1 ? (spaConfigurationData.light1 == 0) : (spaConfigurationData.light2 == 0);
}

/** Two-bit 0 in config payload = not fitted (same convention as pumps). */
static bool statusCircConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.circulationPump;
}

static bool statusBlowerConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.blower;
}

static bool statusMisterConfiguredAbsent()
{
  return statusSpaConfigReady() && !spaConfigurationData.mister;
}

static int toggleCountForButtonRequest(uint8_t itemCode, bool requestHasState, bool desiredOn)
{
  // Lights are binary; one toggle transitions state.
  if (itemCode == 17 || itemCode == 18)
  {
    if (!requestHasState)
    {
      return 1;
    }
    const bool isOn = (itemCode == 17 ? spaStatusData.light1 : spaStatusData.light2);
    return (isOn == desiredOn) ? 0 : 1;
  }

  // Pumps can be two-speed: Off(0)->Low(1)->High(2)->Off(0).
  if (itemCode >= 4 && itemCode <= 9)
  {
    if (!requestHasState)
    {
      return 1;
    }

    const uint8_t pumpId = (itemCode - 3);
    const uint8_t pumpStatus[] = {spaStatusData.pump1, spaStatusData.pump2, spaStatusData.pump3, spaStatusData.pump4, spaStatusData.pump5, spaStatusData.pump6};
    const uint8_t pumpConfig[] = {spaConfigurationData.pump1, spaConfigurationData.pump2, spaConfigurationData.pump3, spaConfigurationData.pump4, spaConfigurationData.pump5, spaConfigurationData.pump6};

    uint8_t state = pumpStatus[pumpId - 1];
    uint8_t speedConfig = pumpConfig[pumpId - 1];
    if (speedConfig <= 1)
    {
      bool isOn = state > 0;
      return (isOn == desiredOn) ? 0 : 1;
    }

    if (desiredOn)
    {
      return (state == 0) ? 1 : 0;
    }
    // desired Off: from Low->Off = 2 toggles (via High), from High->Off = 1 toggle.
    if (state == 0)
    {
      return 0;
    }
    if (state == 1)
    {
      return 2;
    }
    return 1;
  }

  // Other items (blower/aux/range/mode) remain single-toggle for now.
  return 1;
}

static void appendStatusEquipCell(String &html, const char *label, const String &value, bool configuredAbsent)
{
  if (configuredAbsent)
  {
    html += "<div class=\"equip-cell equip-absent\" title=\"Not installed (spa configuration)\"><div class=\"equip-label\">";
  }
  else
  {
    html += "<div class=\"equip-cell\"><div class=\"equip-label\">";
  }
  html += label;
  html += "</div><div class=\"equip-val\">";
  html += value;
  html += "</div></div>";
}

static void appendStatusControlCell(String &html, const char *label, const String &value, bool configuredAbsent, int buttonCode, const char *desiredState)
{
  if (configuredAbsent)
  {
    html += "<div class=\"equip-cell equip-absent\" title=\"Not installed (spa configuration)\">";
  }
  else
  {
    html += "<div class=\"equip-cell\">";
  }
  html += "<div class=\"equip-label\">";
  html += label;
  html += "</div><div class=\"equip-val\">";
  html += value;
  html += "</div>";

  if (!configuredAbsent && buttonCode > 0 && desiredState != nullptr)
  {
    html += "<div class=\"equip-actions\"><button class=\"equip-btn\" type=\"button\" data-button=\"";
    html += String(buttonCode);
    html += "\" data-state=\"";
    html += desiredState;
    html += "\" onclick=\"statusSendButton(this)\">Turn ";
    html += String(desiredState).equalsIgnoreCase("on") ? "On" : "Off";
    html += "</button></div>";
  }
  html += "</div>";
}

/** Spa status `tempScale`: 0 = Fahrenheit (1°F steps), 1 = Celsius (0.5°C steps). */
static bool statusSpaTempReady()
{
  return spaStatusData.lastUpdate != 0;
}

static String statusTempDegreeSuffixStr()
{
  if (!statusSpaTempReady())
  {
    return String("");
  }
  return spaStatusData.tempScale ? (String("\xc2\xb0") + "C") : (String("\xc2\xb0") + "F");
}

static String statusFormatTempValue(float v)
{
  if (!statusSpaTempReady())
  {
    return String("---");
  }
  if (spaStatusData.tempScale)
  {
    return String(v, 1);
  }
  return String(static_cast<long>(lroundf(v)));
}

static String statusFormattedTempWithUnit(float v)
{
  if (!statusSpaTempReady())
  {
    return String("---");
  }
  return statusFormatTempValue(v) + statusTempDegreeSuffixStr();
}

static String statusFormatRuntimeHoursMinutes(unsigned long totalSeconds)
{
  const unsigned long totalMinutes = totalSeconds / 60UL;
  const unsigned long hours = totalMinutes / 60UL;
  const unsigned long minutes = totalMinutes % 60UL;

  String out = formatNumberWithCommas(hours);
  out += "h ";
  if (minutes < 10UL)
  {
    out += "0";
  }
  out += String(minutes);
  out += "m";
  return out;
}

static String statusTempScaleDescription()
{
  if (!statusSpaTempReady())
  {
    return String("---");
  }
  return spaStatusData.tempScale ? (String("Celsius (0.5") + String("\xc2\xb0") + "C steps)")
                                 : (String("Fahrenheit (1") + String("\xc2\xb0") + "F steps)");
}

/** Append JSON array oldest-to-newest (left-to-right on chart); firmware index 0 is newest. */
static void appendStatusJsonFloatArrayOldestFirst(String &html, const float *arr, int n)
{
  for (int i = n - 1; i >= 0; i--)
  {
    if (i != n - 1)
    {
      html += ",";
    }
    html += String(arr[i], 4);
  }
}

static void appendStatusHistoriesSection(String &html)
{
  html += "<div class=\"history-block\"><h3>Temperature history</h3>";
  html += "<p class=\"chart-caption\">Samples left (older) to right (newer). Raw list index 0 is newest.</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusTempHistChart\" height=\"140\" aria-label=\"Temperature history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw temperature values</summary><pre>";
  html += historyToString(spaStatusData.temperatureHistory);
  html += "</pre></details></div>";

  html += "<div class=\"history-block\"><h3>Heater on-time history</h3>";
  html += "<p class=\"chart-caption\">Seconds per day (raw); chart uses minutes per day (div 60).</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusHeatHistChart\" height=\"140\" aria-label=\"Heater history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw heat history (seconds per day)</summary><pre>";
  html += historyToString(spaStatusData.heatOn->history());
  html += "</pre></details></div>";

  html += "<div class=\"history-block\"><h3>Filter on-time history</h3>";
  html += "<p class=\"chart-caption\">Seconds per day (raw); chart uses hours per day (div 3600).</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusFilterHistChart\" height=\"140\" aria-label=\"Filter history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw filter history (seconds per day)</summary><pre>";
  html += historyToString(spaStatusData.filterOn->history());
  html += "</pre></details></div>";

  html += "<script>";
  html += "const STATUS_TEMP_IS_C=";
  html += (statusSpaTempReady() && spaStatusData.tempScale) ? "1" : "0";
  html += ",STATUS_TEMP_HIST=[";
  appendStatusJsonFloatArrayOldestFirst(html, spaStatusData.temperatureHistory, GRAPH_MAX_READINGS);
  html += "],STATUS_HEAT_SEC=[";
  appendStatusJsonFloatArrayOldestFirst(html, spaStatusData.heatOn->history(), GRAPH_MAX_READINGS);
  html += "],STATUS_FILTER_SEC=[";
  appendStatusJsonFloatArrayOldestFirst(html, spaStatusData.filterOn->history(), GRAPH_MAX_READINGS);
  html += "];";
  html += "function statusScaleHeat(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/60);return b;}";
  html += "function statusScaleFilter(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/3600);return b;}";
  html += "function statusDrawLineChart(id,data,isTemp){";
  html += "var c=document.getElementById(id);if(!c||!data||data.length<1)return;";
  html += "var ctx=c.getContext('2d');if(!ctx)return;";
  html += "function fmtY(v){if(isTemp)return STATUS_TEMP_IS_C?Number(v).toFixed(1):String(Math.round(Number(v)));return Number(v).toFixed(2);}";
  html += "function draw(W,H){";
  html += "ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);ctx.strokeStyle='#ccc';ctx.strokeRect(0.5,0.5,W-1,H-1);";
  html += "var lo=Infinity,hi=-Infinity,i,v;";
  html += "for(i=0;i<data.length;i++){v=Number(data[i]);if(!isFinite(v))continue;if(v<lo)lo=v;if(v>hi)hi=v;}";
  html += "if(!isFinite(lo)||!isFinite(hi)){ctx.fillStyle='#333';ctx.font='12px sans-serif';ctx.fillText('No data',10,H/2);return;}";
  html += "if(hi-lo<1e-6){lo-=0.5;hi+=0.5;}";
  html += "var pad=30,pw=W-2*pad,ph=H-22;";
  html += "function yOf(val){return 14+ph-(val-lo)/(hi-lo)*ph;}";
  html += "ctx.fillStyle='#333';ctx.font='11px sans-serif';ctx.fillText(fmtY(lo),4,H-6);ctx.fillText(fmtY(hi),4,12);";
  html += "ctx.strokeStyle='#037e52';ctx.lineWidth=1.5;ctx.beginPath();";
  html += "for(i=0;i<data.length;i++){var px=pad+i*pw/Math.max(1,data.length-1);var py=yOf(Number(data[i]));if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py);}";
  html += "ctx.stroke();}";
  html += "function render(){var p=c.parentElement;var W=p?Math.max(260,p.clientWidth-2):280;var H=parseInt(c.getAttribute('height')||'140',10)||140;";
  html += "W=Math.min(1000,W);H=Math.min(260,H);";
  html += "var dpr=Math.min(2,Math.max(1,window.devicePixelRatio||1));";
  html += "c.width=Math.round(W*dpr);c.height=Math.round(H*dpr);c.style.width=W+'px';c.style.height=H+'px';";
  html += "ctx.setTransform(1,0,0,1,0,0);ctx.scale(dpr,dpr);draw(W,H);}";
  html += "var raf=0;function scheduleRender(){if(raf)return;raf=window.requestAnimationFrame(function(){raf=0;render();});}";
  html += "window.addEventListener('resize',scheduleRender,{passive:true});window.addEventListener('orientationchange',scheduleRender,{passive:true});";
  html += "c.addEventListener('webglcontextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});";
  html += "c.addEventListener('contextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});";
  html += "c.addEventListener('contextrestored',scheduleRender,{passive:true});";
  html += "scheduleRender();}";
  html += "statusDrawLineChart('statusTempHistChart',STATUS_TEMP_HIST,true);";
  html += "statusDrawLineChart('statusHeatHistChart',statusScaleHeat(STATUS_HEAT_SEC),false);";
  html += "statusDrawLineChart('statusFilterHistChart',statusScaleFilter(STATUS_FILTER_SEC),false);";
  html += "</script>";
}

void handleStatus(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());
  const char *statusStyle =
      "<style>"
      ".status-page-title{color:#0f4a87;font-size:1.1rem;margin:0 0 var(--space-3) 0;line-height:1.3;}"
      ".status-layout{display:grid;grid-template-columns:1fr;gap:var(--space-3);}"
      "@media (min-width:720px){.status-layout{grid-template-columns:1fr 1fr;}}"
      ".status-layout .panel{margin-bottom:0;}"
      ".status-span-full{grid-column:1/-1;}"
      ".status-layout h2{color:#0f4a87;font-size:0.95rem;margin:0 0 var(--space-2) 0;font-weight:700;line-height:1.3;}"
      "dl.kv{margin:0;padding:0;}"
      "dl.kv .kv-row{display:grid;grid-template-columns:minmax(110px,42%) 1fr;gap:6px 12px;padding:var(--space-1) 0;"
      "border-bottom:1px dashed #e5eaef;align-items:start;}"
      "dl.kv .kv-row:last-child{border-bottom:none;}"
      "dl.kv dt{margin:0;font-weight:600;color:var(--muted);font-size:0.92rem;}"
      "dl.kv dd{margin:0;overflow-wrap:anywhere;word-break:break-word;}"
      ".equip-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin-top:var(--space-2);}"
      ".equip-cell{border:1px solid var(--border);border-radius:8px;padding:8px 10px;background:#fafbfc;}"
      ".equip-cell.equip-absent{opacity:0.58;background:#eef1f4;color:var(--muted);border-color:#dde2e8;}"
      ".equip-cell.equip-absent .equip-label{color:#7a8794;}"
      ".equip-cell.equip-absent .equip-val{font-weight:500;color:#5f6c7b;}"
      ".equip-label{font-size:0.82rem;color:var(--muted);font-weight:600;}"
      ".equip-val{font-weight:600;margin-top:2px;line-height:1.35;}"
      ".equip-actions{margin-top:8px;}"
      ".equip-btn{background:#0f4a87;color:#fff;border:none;border-radius:6px;padding:6px 10px;cursor:pointer;font-size:.84rem;}"
      ".equip-btn:disabled{opacity:.55;cursor:not-allowed;}"
      ".status-control-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px;}"
      ".status-control-row input{border:1px solid var(--border);border-radius:6px;padding:6px 8px;min-width:90px;}"
      ".status-control-result{margin-top:8px;font-size:.84rem;color:var(--muted);}"
      ".history-block{margin-top:var(--space-2);}"
      ".history-block h3{margin:0 0 6px 0;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      ".history-block pre{margin:0;padding:10px;background:#fafbfc;border:1px solid var(--border);border-radius:8px;"
      "font-size:0.8rem;line-height:1.45;overflow-x:auto;white-space:pre-wrap;word-break:break-word;font-family:ui-monospace,Courier,monospace;}"
      ".chart-caption{font-size:0.82rem;color:var(--muted);margin:0 0 6px 0;line-height:1.35;}"
      ".history-raw{margin-top:8px;}details.history-raw summary{cursor:pointer;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      "</style>";

  String html = "<html>" + head + String(statusStyle) +
                "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuStatus +
                "<main id='mainContent'>" + ePaper + "<h1 class=\"status-page-title\">Spa Status</h1><div class=\"status-layout\">";

  html += "<section class=\"panel\"><h2>Data sync</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "lastUpdate", statusLastUpdateDisplayHtml(spaStatusData.lastUpdate));
  appendStatusKvRow(html, "magicNumber", String(spaStatusData.magicNumber));
  html += "</dl>";
  html += "<p class=\"chart-caption\">magicNumber is a firmware RAM struct validity marker (expected 0x12345678 after init), not a spa model ID.</p>";
  html += "</section>";

  html += "<section class=\"panel\"><h2>Device memory</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Free Heap", formatNumberWithCommas(ESP.getFreeHeap()));
  appendStatusKvRow(html, "Free PSRAM", formatNumberWithCommas(ESP.getFreePsram()));
  appendStatusKvRow(html, "Free Stack", formatNumberWithCommas(uxTaskGetStackHighWaterMark(NULL)));
  html += "</dl></section>";

  html += "<section class=\"panel\"><h2>Temperatures</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Current Temp", statusFormattedTempWithUnit(spaStatusData.currentTemp));
  appendStatusKvRow(html, "Set Temp", statusFormattedTempWithUnit(spaStatusData.setTemp));
  appendStatusKvRow(html, "High Set Temp", statusFormattedTempWithUnit(spaStatusData.highSetTemp));
  appendStatusKvRow(html, "Low Set Temp", statusFormattedTempWithUnit(spaStatusData.lowSetTemp));
  appendStatusKvRow(html, "Temp Range", getMapDescription(spaStatusData.tempRange, tempRangeMap));
  appendStatusKvRow(html, "Temp Scale", statusTempScaleDescription());
  html += "</dl>";
  html += "<div class=\"status-control-row\"><label for=\"statusSetTempInput\" class=\"equip-label\">Set Temp</label>";
  html += "<input id=\"statusSetTempInput\" type=\"number\" step=\"";
  html += (spaStatusData.tempScale ? "0.5" : "1");
  html += "\" value=\"";
  html += String(spaStatusData.setTemp, spaStatusData.tempScale ? 1 : 0);
  html += "\" />";
  html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSendSetTemp()\">Send</button></div>";
  html += "<div id=\"statusSetTempResult\" class=\"status-control-result\"></div>";
  html += "</section>";

  html += "<section class=\"panel\"><h2>Spa and heating</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Spa State", getMapDescription(spaStatusData.spaState, spaStateMap));
  appendStatusKvRow(html, "Init Mode", getMapDescription(spaStatusData.initMode, initModeMap));
  appendStatusKvRow(html, "Heating Mode", getMapDescription(spaStatusData.heatingMode, heatingModeMap));
  appendStatusKvRow(html, "Heating State", getMapDescription(spaStatusData.heatingState, heatingStateMap));
  appendStatusKvRow(html, "Needs Heat", String(spaStatusData.needsHeat));
  html += "</dl></section>";

  html += "<section class=\"panel\"><h2>Time and filtration</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Time", String(spaStatusData.time));
  appendStatusKvRow(html, "Clock Mode", String(spaStatusData.clockMode));
  appendStatusKvRow(html, "Filter Mode", getMapDescription(spaStatusData.filterMode, filterModeMap));
  html += "</dl></section>";

  html += "<section class=\"panel status-span-full\"><h2>Equipment</h2><div class=\"equip-grid\">";
  appendStatusControlCell(html, "Pump 1", statusPumpDisplayState(1), statusPumpConfiguredAbsent(1), 4, statusPumpIsOn(1) ? "off" : "on");
  appendStatusControlCell(html, "Pump 2", statusPumpDisplayState(2), statusPumpConfiguredAbsent(2), 5, statusPumpIsOn(2) ? "off" : "on");
  appendStatusControlCell(html, "Pump 3", statusPumpDisplayState(3), statusPumpConfiguredAbsent(3), 6, statusPumpIsOn(3) ? "off" : "on");
  appendStatusControlCell(html, "Pump 4", statusPumpDisplayState(4), statusPumpConfiguredAbsent(4), 7, statusPumpIsOn(4) ? "off" : "on");
  appendStatusControlCell(html, "Pump 5", statusPumpDisplayState(5), statusPumpConfiguredAbsent(5), 8, statusPumpIsOn(5) ? "off" : "on");
  appendStatusControlCell(html, "Pump 6", statusPumpDisplayState(6), statusPumpConfiguredAbsent(6), 9, statusPumpIsOn(6) ? "off" : "on");
  appendStatusControlCell(html, "Circulation Pump", getMapDescription(spaStatusData.circ, onOffMap), statusCircConfiguredAbsent(), 0, nullptr);
  appendStatusControlCell(html, "Blower", getMapDescription(spaStatusData.blower, onOffMap), statusBlowerConfiguredAbsent(), 12, spaStatusData.blower == 0 ? "on" : "off");
  appendStatusControlCell(html, "Light 1", getMapDescription(spaStatusData.light1, onOffMap), statusLightConfiguredAbsent(1), 17, spaStatusData.light1 ? "off" : "on");
  appendStatusControlCell(html, "Light 2", getMapDescription(spaStatusData.light2, onOffMap), statusLightConfiguredAbsent(2), 18, spaStatusData.light2 ? "off" : "on");
  appendStatusControlCell(html, "Mister", getMapDescription(spaStatusData.mister, onOffMap), statusMisterConfiguredAbsent(), 14, spaStatusData.mister ? "off" : "on");
  html += "</div><div id=\"statusButtonResult\" class=\"status-control-result\"></div></section>";

  html += "<section class=\"panel\"><h2>Panel and flags</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Panel Locked", getMapDescription(spaStatusData.panelLocked, lockedMap));
  appendStatusKvRow(html, "Settings Lock", getMapDescription(spaStatusData.settingsLock, lockedMap));
  appendStatusKvRow(html, "M8 Cycle Time", String(spaStatusData.m8CycleTime));
  appendStatusKvRow(html, "Notification", String(spaStatusData.notification));
  appendStatusKvRow(html, "Flags 19", String(spaStatusData.flags19));
  html += "</dl></section>";

  html += "<section class=\"panel\"><h2>Run times</h2><dl class=\"kv\">";
  appendStatusKvRow(html, "Heater On Time Today", statusFormatRuntimeHoursMinutes(spaStatusData.heaterOnTimeToday));
  appendStatusKvRow(html, "Heater On Time Yesterday", statusFormatRuntimeHoursMinutes(spaStatusData.heaterOnTimeYesterday));
  appendStatusKvRow(html, "Filter On Time Today", statusFormatRuntimeHoursMinutes(spaStatusData.filterOnTimeToday));
  appendStatusKvRow(html, "Filter On Time Yesterday", statusFormatRuntimeHoursMinutes(spaStatusData.filterOnTimeYesterday));
  html += "</dl></section>";

  html += "<section class=\"panel status-span-full\"><h2>Histories</h2>";
  appendStatusHistoriesSection(html);
  html += "</section>";

  html += "<script>"
          "async function statusSendSci(payload){"
          "const body='<sci_request version=\"1.0\"><data_service><targets><device id=\"00 11 22 33 44 55 66 77\"/></targets><requests>'+payload+'</requests></data_service></sci_request>';"
          "const r=await fetch('/devices/sci',{method:'POST',headers:{'Content-Type':'application/xml'},body});"
          "return await r.text();"
          "}"
          "async function statusFetchControls(){const r=await fetch('/api/status/controls');return await r.json();}"
          "function statusButtonMatch(snap,code,desired){var on=(desired||'on').toLowerCase()==='on';"
          "if(code===17)return (snap.light1>0)===on;"
          "if(code===18)return (snap.light2>0)===on;"
          "if(code===4)return on?!!snap.pump1On:!snap.pump1On;"
          "if(code===5)return on?!!snap.pump2On:!snap.pump2On;"
          "if(code===6)return on?!!snap.pump3On:!snap.pump3On;"
          "if(code===7)return on?!!snap.pump4On:!snap.pump4On;"
          "if(code===8)return on?!!snap.pump5On:!snap.pump5On;"
          "if(code===9)return on?!!snap.pump6On:!snap.pump6On;"
          "if(code===12)return on?(snap.blower>0):(snap.blower===0);"
          "if(code===14)return (snap.mister>0)===on;"
          "return false;}"
          "async function statusWaitForButtonState(code,desired){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(statusButtonMatch(snap,code,desired))return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForSetTemp(target){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(Math.abs(Number(snap.setTemp)-Number(target))<0.26)return true;}catch(e){}}"
          "return false;}"
          "function statusSetResult(id,text){var el=document.getElementById(id);if(el)el.textContent=text;}"
          "async function statusSendButton(btn){"
          "try{btn.disabled=true;const c=btn.getAttribute('data-button');const s=btn.getAttribute('data-state')||'on';"
          "const xml='<device_request target_name=\"Button\">'+c+':'+s+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusButtonResult','Button command response: '+out);return;}"
          "statusSetResult('statusButtonResult','Button command accepted; waiting for spa status update...');"
          "const changed=await statusWaitForButtonState(Number(c),s);"
          "if(changed){statusSetResult('statusButtonResult','Button command accepted and state changed.');setTimeout(function(){location.reload();},500);}else{statusSetResult('statusButtonResult','Button command accepted, but state did not change yet.');}"
          "}catch(e){statusSetResult('statusButtonResult','Button command failed: '+e);}finally{btn.disabled=false;}"
          "}"
          "async function statusSendSetTemp(){"
          "const input=document.getElementById('statusSetTempInput');if(!input)return;const v=input.value;"
          "try{const xml='<device_request target_name=\"SetTemp\">'+v+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSetTempResult','SetTemp response: '+out);return;}"
          "statusSetResult('statusSetTempResult','SetTemp accepted; waiting for spa status update...');"
          "const changed=await statusWaitForSetTemp(v);"
          "if(changed){statusSetResult('statusSetTempResult','SetTemp accepted and state changed.');setTimeout(function(){location.reload();},500);}else{statusSetResult('statusSetTempResult','SetTemp accepted, but setpoint did not change yet.');}"
          "}catch(e){statusSetResult('statusSetTempResult','SetTemp failed: '+e);}"
          "}"
          "</script>";
  html += "</div></main></div></body></html>";
  request->send(200, "text/html", html);
  Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
}

void handleConfig(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());

  String html = "<html>" + head + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuConfig + "<main id='mainContent'>" + ePaper + "<section class='panel'><h1>Spa Configuration</h1><ul>";
  if (spaConfigurationData.lastUpdate == 0)
  {
    html += "<li><b>Spa Configuration not available</b></li>";
  }
  else
  {
    html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastUpdate) + "</li>";
    html += "<li><b>magicNumber: </b>" + String(spaConfigurationData.magicNumber) + "</li>";
    html += "<li><b>Pump 1: </b>" + String(spaConfigurationData.pump1) + "</li>";
    html += "<li><b>Pump 2: </b>" + String(spaConfigurationData.pump2) + "</li>";
    html += "<li><b>Pump 3: </b>" + String(spaConfigurationData.pump3) + "</li>";
    html += "<li><b>Pump 4: </b>" + String(spaConfigurationData.pump4) + "</li>";
    html += "<li><b>Pump 5: </b>" + String(spaConfigurationData.pump5) + "</li>";
    html += "<li><b>Pump 6: </b>" + String(spaConfigurationData.pump6) + "</li>";
    html += "<li><b>Light 1: </b>" + String(spaConfigurationData.light1) + "</li>";
    html += "<li><b>Light 2: </b>" + String(spaConfigurationData.light2) + "</li>";
    html += "<li><b>Blower: </b>" + String(spaConfigurationData.blower) + "</li>";
    html += "<li><b>Circulation Pump: </b>" + String(spaConfigurationData.circulationPump) + "</li>";
    html += "<li><b>Aux 1: </b>" + String(spaConfigurationData.aux1) + "</li>";
    html += "<li><b>Aux 2: </b>" + String(spaConfigurationData.aux2) + "</li>";
    html += "<li><b>Mister: </b>" + String(spaConfigurationData.mister) + "</li>";
    html += "<li><b>temp_scale: </b>" + String(spaConfigurationData.temp_scale) + "</li>";
    html += "</ul></section><section class='panel'><h1>Filter Configuration</h1>";
    html += "<p class=\"chart-caption\" style=\"margin:0 0 10px 0\">Times use the spa panel clock. <b>Filter N Time</b> is the daily start of that filtration cycle. <b>Filter N Duration</b> is how long the pump runs for that cycle each day (circulation through the filter). Many tubs use two staggered cycles; the second may be unused on some packs.</p><ul>";
    html += "<li><b>lastUpdate (filter settings): </b>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastUpdate) + "</li>";
    if (spaFilterSettingsData.lastUpdate != 0)
    {
      html += "<li><b>Filter 2 enabled: </b>" + String(spaFilterSettingsData.filt2Enable ? "yes" : "no") + "</li>";
    }
    html += "<li><b>Filter 1 Time: </b>" + formatAsHourMinute(spaFilterSettingsData.filt1Hour, spaFilterSettingsData.filt1Minute) + "</li>";
    html += "<li><b>Filter 1 Duration: </b>" + formatAsHourMinute(spaFilterSettingsData.filt1DurationHour, spaFilterSettingsData.filt1DurationMinute) + "</li>";
    html += "<li><b>Filter 2 Time: </b>" + formatAsHourMinute(spaFilterSettingsData.filt2Hour, spaFilterSettingsData.filt2Minute) + "</li>";
    html += "<li><b>Filter 2 Duration: </b>" + formatAsHourMinute(spaFilterSettingsData.filt2DurationHour, spaFilterSettingsData.filt2DurationMinute) + "</li>";
    html += "</ul></section><section class='panel'><h1>LittleFS Configuration</h1><ul>";

    if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED))
    {
      html += "<li><b>Error LittleFS Mount Failed </b></li>";
    }
    else
    {
      html += "<li>" + listDirToString(LittleFS, "/", 3) + "</li>";
    }
    // Add more fields as needed
  }
  html += "</ul></section></main></div></body></html>";
  request->send(200, "text/html", html);
  // Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
  Log.verbose("[Web]: handleConfig %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
}

time_t testLastCheckedTime = getTime();

void handleState(AsyncWebServerRequest *request)
{
  // Log.verbose(F("[Web]: handleStatus()" CR));
  String stateEnhancements = "<style>.state-grid{display:grid;grid-template-columns:1fr;gap:14px;}@media (min-width:980px){.state-grid{grid-template-columns:1fr 1fr;}.state-grid .panel{margin-bottom:0;}}.diag-badge{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.88rem;}</style>";
  String html = "<html>" + head + stateEnhancements + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuState + "<main id='mainContent'>" + ePaper + "<div class='state-grid'><section class='panel'><h1>ESP State</h1><ul>";
  html += "<li><b>Free Heap: </b>" + formatNumberWithCommas(ESP.getFreeHeap()) + "</li>";
  html += "<li><b>Free PSRAM: </b>" + formatNumberWithCommas(ESP.getFreePsram()) + "</li>";
  html += "<li><b>Free Stack: </b>" + formatNumberWithCommas(uxTaskGetStackHighWaterMark(NULL)) + "</li>";
  html += "<li><b>Uptime: </b>" + formatNumberWithCommas(millis() / 1000) + " s</li>";
  html += "<li><b>Time: </b>" + webWallClockDisplayHtml(getTime()) + "</li>";
  html += "<li><b>Refresh Time: </b>" + webWallClockDisplayHtml(getTime() + static_cast<time_t>(60 * 60)) + "</li>";
  html += "<li><b>Restart Reason: </b>" + getLastRestartReason() + "</li>";
  html += "<li><b>Firmware Version: </b>" + String(VERSION) + "</li>";
  html += "<li><b>Firmware Build: </b>" + String(BUILD) + "</li>";
  String release = String(__DATE__) + " - " + String(__TIME__);
  html += "<li><b>Release: </b>" + release + "</li>";
  html += "<li><b>Build Definition: </b>" + buildDefinitionString + "</li>";

  html += "<li class='spacer'></li><li><b>getTime(): </b>" + webWallClockDisplayHtml(getTime()) + "</li>";
  html += "<li><b>getHour(testLastCheckedTime): </b>" + formatNumberWithCommas(getHour(testLastCheckedTime)) + "</li>";
  html += "<li><b>getHour(getTime()): </b>" + formatNumberWithCommas(getHour(getTime())) + "</li>";
  html += "<li><b>hasDayChanged(testLastCheckedTime): </b>" + String(hasDayChanged(testLastCheckedTime)) + "</li>";

#ifdef LOCAL_CLIENT
  String rsHealth = String(rs485HealthCode());

  html += "</ul></section><section class='panel'><h1>RS485 Diagnostics</h1><ul>";
  html += "<li><b>Health: </b><span class='diag-badge' style='font-weight:700;color:#fff;background:" + rs485HealthColor(rsHealth) + "'>" + rs485HealthLabel(rsHealth) + "</span></li>";
  html += "<li><b>Hint: </b>" + rs485HealthHint(rsHealth) + "</li>";
  html += "<li><b>Mode: </b><span class='diag-badge' style='font-weight:700;color:#fff;background:" + String(rs485Stats.polarityInverted ? "#0f4a87" : "#4b5563") + "'>" + String(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal") + "</span></li>";
  html += "<li><b>Mode Hint: </b>" + rs485ModeHint(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>Detect Phase: </b>" + String(rs485Stats.polarityLocked ? "2 (locked)" : (rs485Stats.polarityInverted ? "1 (testing inverted_rx_tx)" : "0 (testing normal)")) + "</li>";
  html += "<li><b>Polarity Locked: </b>" + String(rs485Stats.polarityLocked ? "yes" : "no") + "</li>";
  html += "<li class='spacer'></li><li><b>Raw Bytes (today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesToday) + "</li>";
  html += "<li><b>Raw Bytes (normal today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalToday) + "</li>";
  html += "<li><b>Raw Bytes (inverted today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedToday) + "</li>";
  html += "<li><b>Frame Attempts (today): </b>" + formatNumberWithCommas(rs485Stats.framesToday) + "</li>";
  html += "<li><b>0x7E Markers (today): </b>" + formatNumberWithCommas(rs485Stats.frameMarkersToday) + "</li>";
  html += "<li><b>Valid Frames (today): </b>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</li>";
  html += "<li><b>CRC Errors (today): </b>" + formatNumberWithCommas(rs485Stats.crcToday) + "</li>";
  html += "<li><b>Format Errors (today): </b>" + formatNumberWithCommas(rs485Stats.badFormatToday) + "</li>";
  html += "<li><b>Mode Switches (today): </b>" + formatNumberWithCommas(rs485Stats.polaritySwitchesToday) + "</li>";
  html += "<li><b>Max UART Backlog (today): </b>" + formatNumberWithCommas(rs485Stats.maxUartAvailableToday) + "</li>";
  html += "<li class='spacer'></li><li><b>Raw Bytes (yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesYesterday) + "</li>";
  html += "<li><b>Raw Bytes (normal yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalYesterday) + "</li>";
  html += "<li><b>Raw Bytes (inverted yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedYesterday) + "</li>";
  html += "<li><b>Frame Attempts (yesterday): </b>" + formatNumberWithCommas(rs485Stats.framesYesterday) + "</li>";
  html += "<li><b>0x7E Markers (yesterday): </b>" + formatNumberWithCommas(rs485Stats.frameMarkersYesterday) + "</li>";
  html += "<li><b>Valid Frames (yesterday): </b>" + formatNumberWithCommas(rs485Stats.messagesYesterday) + "</li>";
  html += "<li><b>CRC Errors (yesterday): </b>" + formatNumberWithCommas(rs485Stats.crcYesterday) + "</li>";
  html += "<li><b>Format Errors (yesterday): </b>" + formatNumberWithCommas(rs485Stats.badFormatYesterday) + "</li>";
  html += "<li><b>Mode Switches (yesterday): </b>" + formatNumberWithCommas(rs485Stats.polaritySwitchesYesterday) + "</li>";
  html += "<li><b>Max UART Backlog (yesterday): </b>" + formatNumberWithCommas(rs485Stats.maxUartAvailableYesterday) + "</li>";
  html += "<li class='spacer'></li><li><b>Last Byte Millis: </b>" + formatNumberWithCommas(rs485Stats.lastByteMs) + "</li>";
  html += "<li><b>Last Valid Frame Millis: </b>" + formatNumberWithCommas(rs485Stats.lastValidFrameMs) + "</li>";
  html += "<li><b>UART Pins: </b>RX GPIO " + String(rs485RxGpio()) + ", TX GPIO " + String(rs485TxGpio()) + ", " + String(rs485Baud()) + " baud</li>";
  html += "<li><b>AUTO_TX: </b>" + String(rs485AutoTxEnabled() ? "true" : "false") + "</li>";
  html += "<li><b>Polarity Inverted (raw): </b>" + String(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>Health Code (raw): </b>" + rsHealth + "</li>";

  html += "</ul></section><section class='panel'><h1>RS485 Raw Counters</h1><ul>";
  html += "<li class='spacer'></li><li><b>rs485 messagesToday: </b>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</li>";
  html += "<li><b>rs485 rawBytesToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesToday) + "</li>";
  html += "<li><b>rs485 rawBytesNormalToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalToday) + "</li>";
  html += "<li><b>rs485 rawBytesInvertedToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedToday) + "</li>";
  html += "<li><b>rs485 framesToday: </b>" + formatNumberWithCommas(rs485Stats.framesToday) + "</li>";
  html += "<li><b>rs485 frameMarkersToday: </b>" + formatNumberWithCommas(rs485Stats.frameMarkersToday) + "</li>";
  html += "<li><b>rs485 crcToday: </b>" + formatNumberWithCommas(rs485Stats.crcToday) + "</li>";
  html += "<li><b>rs485 messagesYesterday: </b>" + formatNumberWithCommas(rs485Stats.messagesYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesNormalYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesInvertedYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedYesterday) + "</li>";
  html += "<li><b>rs485 framesYesterday: </b>" + formatNumberWithCommas(rs485Stats.framesYesterday) + "</li>";
  html += "<li><b>rs485 frameMarkersYesterday: </b>" + formatNumberWithCommas(rs485Stats.frameMarkersYesterday) + "</li>";
  html += "<li><b>rs485 crcYesterday: </b>" + formatNumberWithCommas(rs485Stats.crcYesterday) + "</li>";
  html += "<li><b>rs485 badFormatToday: </b>" + formatNumberWithCommas(rs485Stats.badFormatToday) + "</li>";
  html += "<li><b>rs485 badFormatYesterday: </b>" + formatNumberWithCommas(rs485Stats.badFormatYesterday) + "</li>";
  html += "<li><b>rs485 polarityInverted: </b>" + String(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>rs485 polarityLocked: </b>" + String(rs485Stats.polarityLocked) + "</li>";
  html += "<li><b>rs485 mode: </b>" + String(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal") + "</li>";
  html += "<li><b>rs485 detectPhase: </b>" + String(rs485Stats.polarityLocked ? "2" : (rs485Stats.polarityInverted ? "1" : "0")) + "</li>";
  html += "<li><b>rs485 lastByteMs: </b>" + formatNumberWithCommas(rs485Stats.lastByteMs) + "</li>";
  html += "<li><b>rs485 lastValidFrameMs: </b>" + formatNumberWithCommas(rs485Stats.lastValidFrameMs) + "</li>";
  html += "<li><b>rs485 raw endpoint: </b>/api/rs485/raw?limit=80</li>";
#endif

  appendWifiStateSection(html);
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaStatusData.lastUpdate) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaStatusData.magicNumber) + "</li>";

  html += "</ul></section><section class='panel'><h1>Configuration Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaConfigurationData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaConfigurationData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaConfigurationData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Preferences Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaPreferencesData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaPreferencesData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaPreferencesData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaPreferencesData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaPreferencesData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Filters Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaFilterSettingsData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaFilterSettingsData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaFilterSettingsData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Information Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaInformationData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaInformationData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaInformationData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaInformationData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaInformationData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Fault Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaFaultLogData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaFaultLogData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaFaultLogData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaFaultLogData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaFaultLogData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>spaSettings0x04Data Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + statusLastUpdateDisplayHtml(spaSettings0x04Data.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + statusLastUpdateDisplayHtml(spaSettings0x04Data.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaSettings0x04Data.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaSettings0x04Data)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaSettings0x04Data)) + "</li>";

  html += "</ul></section></div></main></div></body></html>";

  request->send(200, "text/html", html);
  Log.verbose("[Web]: handleStatus %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());

  // Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
}

void handleLogsApi(AsyncWebServerRequest *request)
{
  uint32_t since = 0;
  if (request->hasParam("since"))
  {
    since = (uint32_t)request->getParam("since")->value().toInt();
  }
  unsigned limit = 120;
  if (request->hasParam("limit"))
  {
    limit = (unsigned)request->getParam("limit")->value().toInt();
  }
  String body;
  webLogBufferBuildJsonSince(since, limit, Log.getLevel(), body);
  request->send(200, "application/json", body);
}

void handleLogsConfigGet(AsyncWebServerRequest *request)
{
  String body;
  webLogBufferBuildJsonLogConfig(Log.getLevel(), body);
  request->send(200, "application/json", body);
}

void handleLogsConfigPost(AsyncWebServerRequest *request)
{
  if (request->_tempObject == nullptr)
  {
    request->send(400, "application/json", "{\"error\":\"no_body\"}");
    return;
  }
  String *bodyPtr = (String *)request->_tempObject;
  String body = *bodyPtr;
  delete bodyPtr;
  request->_tempObject = nullptr;

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    request->send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }
  if (!doc.containsKey("level"))
  {
    request->send(400, "application/json", "{\"error\":\"missing_level\"}");
    return;
  }
  int level = doc["level"].as<int>();
  if (level < LOG_LEVEL_SILENT)
  {
    level = LOG_LEVEL_SILENT;
  }
  if (level > LOG_LEVEL)
  {
    level = LOG_LEVEL;
  }
  Log.setLevel(level);
  String reply = "{\"ok\":true,\"level\":";
  reply += String(level);
  reply += "}";
  request->send(200, "application/json", reply);
}

void handleLogsPage(AsyncWebServerRequest *request)
{
  String html = "<html>" + headLogs + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuLogs + "<main id='mainContent'><section class='panel'><h1>Device logs</h1>";
  html += "<p style='color:var(--muted);font-size:14px;margin-top:0'>Recent lines are buffered on the gateway; include/exclude filters run in the browser. When <code>TELNET_LOG</code> is enabled, <code>nc &lt;host&gt; 23</code> is still the lowest-overhead tail.</p>";
  html += R"HTML(<style>
.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px}
.log-controls label{font-size:14px;color:var(--muted)}
.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px}
.preset-row{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 10px 0}
.preset-row button{flex:0 0 auto;padding:8px 11px;font-size:13px;min-height:36px}
.status-row{display:flex;align-items:center;gap:10px;margin:0 0 10px 0;color:var(--muted);font-size:13px}
.log-view{min-height:260px;max-height:70vh;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:8px;border-radius:8px;border:1px solid var(--border)}
.log-line{display:flex;gap:8px;padding:2px 4px;border-radius:4px;white-space:pre-wrap;word-break:break-word}
.log-seq{color:#93a8c5;min-width:56px}
.log-tag{display:inline-block;padding:0 6px;border-radius:999px;background:#233148;color:#d7e3f4;font-size:11px}
.lvl-e{background:rgba(190,24,36,.2)} .lvl-w{background:rgba(202,138,4,.2)} .lvl-i{background:rgba(2,132,199,.16)} .lvl-v{background:rgba(71,85,105,.2)}
#newBadge{display:none}
</style>)HTML";
  html += "<div class='preset-row'><button type='button' id='pAll'>All</button><button type='button' id='pErr'>Errors only</button><button type='button' id='pRs'>RS485</button><button type='button' id='pBridge'>BridgeDiag</button><button type='button' id='pWifi'>WiFi</button></div>";
  html += "<div class='log-controls'><label>Level <select id='lvl'><option value='0'>SILENT</option><option value='1'>FATAL</option><option value='2'>ERROR</option><option value='3'>WARNING</option><option value='4'>INFO/NOTICE</option><option value='5'>TRACE</option><option value='6'>VERBOSE</option></select></label>";
  html += "<button type='button' id='applyLvl'>Apply level</button>";
  html += "<label>Include <input type='text' id='fInc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label>Exclude <input type='text' id='fExc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label><input type='checkbox' id='pause'/> Pause</label>";
  html += "<label><input type='checkbox' id='hideIdleCts' checked/> Hide idle CTS</label>";
  html += "<label><input type='checkbox' id='showHidden'/> Show hidden</label>";
  html += "<label><input type='checkbox' id='useWs'/> WebSocket tail</label>";
  html += "<label><input type='checkbox' id='autoScroll' checked/> Auto-scroll</label>";
  html += "<button type='button' id='newBadge'>0 new lines</button>";
  html += "<button type='button' id='clr'>Clear view</button><button type='button' id='copyTxt'>Copy</button><button type='button' id='dlTxt'>Download .log</button><button type='button' id='dlJson'>Download .json</button>";
  html += "</div>";
  html += "<div class='status-row'><span id='streamMode'>poll</span><span id='renderCount'>0 lines</span><span id='hiddenCount'>hidden idle CTS: 0</span><span id='connState'></span></div>";
  html += "<div id='logView' class='log-view' aria-live='polite'></div></section></main></div><script>";
  html += R"JS((function(){
var logView=document.getElementById('logView'),since=0,pollMs=900,timer,ws,useWs=false,newBuffered=0;
var fInc=document.getElementById('fInc'),fExc=document.getElementById('fExc'),sel=document.getElementById('lvl');
var pauseEl=document.getElementById('pause'),autoScrollEl=document.getElementById('autoScroll'),newBadge=document.getElementById('newBadge');
var hideIdleCtsEl=document.getElementById('hideIdleCts'),showHiddenEl=document.getElementById('showHidden');
var streamMode=document.getElementById('streamMode'),renderCount=document.getElementById('renderCount'),hiddenCountEl=document.getElementById('hiddenCount'),connState=document.getElementById('connState');
var rendered=[],maxRendered=8000;
var hiddenIdleCts=0;
function getTag(t){var m=t.match(/\[([^\]]+)\]/);return m?m[1]:'';}
function getLevelClass(t){if(/\bE:|\bERROR\b/.test(t))return'lvl-e';if(/\bW:|\bWARNING\b/.test(t))return'lvl-w';if(/\bI:|\bNOTICE\b|\bINFO\b/.test(t))return'lvl-i';if(/\bTRACE\b|\bVERBOSE\b/.test(t))return'lvl-v';return'';}
function esc(s){return s.replace(/[&<>"]/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c];});}
function isIdleCtsLine(t){
var l=t.toLowerCase();
if(l.indexOf('[bridgediag]')<0)return false;
if(l.indexOf('cts')<0)return false;
if(l.indexOf('depth_before=0')<0)return false;
return true;
}
function passes(t){var i=(fInc.value||'').trim();var x=(fExc.value||'').trim();if(i&&t.toLowerCase().indexOf(i.toLowerCase())<0)return false;if(x&&t.toLowerCase().indexOf(x.toLowerCase())>=0)return false;return true;}
function renderLine(rec){var tag=getTag(rec.t),cls=getLevelClass(rec.t);var body=esc(rec.t);if(tag){body=body.replace('['+tag+']','<span class=\"log-tag\">['+esc(tag)+']</span>');}
return '<div class=\"log-line '+cls+'\"><span class=\"log-seq\">#'+rec.s+'</span><span>'+body+'</span></div>';}
function refreshFromRendered(){var out='',n=0,h=0;for(var i=0;i<rendered.length;i++){var line=rendered[i].t;var hiddenByIdleCts=hideIdleCtsEl.checked&&isIdleCtsLine(line);if(hiddenByIdleCts){h++;if(!showHiddenEl.checked)continue;}if(!passes(line))continue;out+=renderLine(rendered[i]);n++;}hiddenIdleCts=h;hiddenCountEl.textContent='hidden idle CTS: '+String(hiddenIdleCts);logView.innerHTML=out;renderCount.textContent=n+' lines';if(autoScrollEl.checked){logView.scrollTop=logView.scrollHeight;}}
function appendLines(arr){if(!arr)return;for(var j=0;j<arr.length;j++){rendered.push({s:arr[j].s,t:arr[j].t});if(rendered.length>maxRendered)rendered.shift();}refreshFromRendered();}
function receiveLines(arr){if(!arr||!arr.length)return;var atBottom=(logView.scrollTop+logView.clientHeight+20)>=logView.scrollHeight;
if(autoScrollEl.checked||atBottom){appendLines(arr);newBuffered=0;newBadge.style.display='none';}
else{newBuffered+=arr.length;newBadge.textContent=String(newBuffered)+' new lines';newBadge.style.display='inline-flex';for(var k=0;k<arr.length;k++){rendered.push({s:arr[k].s,t:arr[k].t});if(rendered.length>maxRendered)rendered.shift();}renderCount.textContent=rendered.length+' lines';}}
function capSel(mx){for(var i=0;i<sel.options.length;i++){var o=sel.options[i];o.disabled=(parseInt(o.value,10)>mx);}if((parseInt(sel.value,10)||0)>mx)sel.value=String(mx);}
function poll(){if(document.hidden)return;fetch('/api/logs?since='+since+'&limit=120').then(function(r){return r.json();}).then(function(j){
connState.textContent='ok';
if(typeof j.compileMaxLevel==='number')capSel(j.compileMaxLevel);
var lines=j.lines||[];
receiveLines(lines);
if(lines.length>0&&typeof lines[lines.length-1].s==='number'){since=lines[lines.length-1].s;}
else if(typeof j.newestSeq==='number'){since=j.newestSeq;}
}).catch(function(){connState.textContent='error';});}
function startPoll(){stopPoll();streamMode.textContent='poll';timer=setInterval(poll,pollMs);poll();}
function stopPoll(){if(timer){clearInterval(timer);timer=null;}}
function connectWs(){streamMode.textContent='ws';var p=location.protocol==='https:'?'wss:':'ws:';ws=new WebSocket(p+'//'+location.host+'/api/logs/ws');connState.textContent='connecting';
ws.onopen=function(){connState.textContent='ws-open';};ws.onmessage=function(ev){try{var o=JSON.parse(ev.data);if(o.lines)receiveLines(o.lines);if(o.d)receiveLines(o.d);}catch(e){}};ws.onclose=function(){connState.textContent='ws-closed';ws=null;};}
function setPreset(inc,exc){fInc.value=inc||'';fExc.value=exc||'';refreshFromRendered();}
function dl(name,content,type){var b=new Blob([content],{type:type});var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=name;document.body.appendChild(a);a.click();setTimeout(function(){URL.revokeObjectURL(a.href);a.remove();},0);}
document.getElementById('pAll').addEventListener('click',function(){setPreset('','');});
document.getElementById('pErr').addEventListener('click',function(){setPreset('E:','');});
document.getElementById('pRs').addEventListener('click',function(){setPreset('[RS485]','');});
document.getElementById('pBridge').addEventListener('click',function(){setPreset('[BridgeDiag]','');});
document.getElementById('pWifi').addEventListener('click',function(){setPreset('[WiFi]','');});
fInc.addEventListener('input',refreshFromRendered);fExc.addEventListener('input',refreshFromRendered);
hideIdleCtsEl.addEventListener('change',refreshFromRendered);
showHiddenEl.addEventListener('change',refreshFromRendered);
newBadge.addEventListener('click',function(){newBuffered=0;newBadge.style.display='none';refreshFromRendered();logView.scrollTop=logView.scrollHeight;});
document.getElementById('pause').addEventListener('change',function(){if(this.checked){stopPoll();if(ws){ws.close();ws=null;}}else if(useWs)connectWs();else startPoll();});
document.getElementById('useWs').addEventListener('change',function(){useWs=this.checked;stopPoll();if(ws){ws.close();ws=null;}if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}});
document.getElementById('clr').addEventListener('click',function(){rendered=[];refreshFromRendered();});
document.getElementById('copyTxt').addEventListener('click',function(){
var txt='';for(var i=0;i<rendered.length;i++){if(passes(rendered[i].t))txt+=rendered[i].t+'\n';}
if(!txt){connState.textContent='nothing to copy';return;}
if(navigator.clipboard&&navigator.clipboard.writeText){
navigator.clipboard.writeText(txt).then(function(){connState.textContent='copied';}).catch(function(){fallbackCopy(txt);});
}else{fallbackCopy(txt);}
});
function fallbackCopy(txt){
var ta=document.createElement('textarea');ta.value=txt;ta.setAttribute('readonly','readonly');
ta.style.position='fixed';ta.style.top='-1000px';document.body.appendChild(ta);ta.focus();ta.select();
try{var ok=document.execCommand('copy');connState.textContent=ok?'copied':'copy failed';}
catch(e){connState.textContent='copy failed';}
document.body.removeChild(ta);
}
document.getElementById('dlTxt').addEventListener('click',function(){var txt='';for(var i=0;i<rendered.length;i++){if(passes(rendered[i].t))txt+=rendered[i].t+'\n';}dl('spa-logs-'+Date.now()+'.log',txt,'text/plain');});
document.getElementById('dlJson').addEventListener('click',function(){var out=[];for(var i=0;i<rendered.length;i++){if(passes(rendered[i].t))out.push(rendered[i]);}dl('spa-logs-'+Date.now()+'.json',JSON.stringify(out,null,2),'application/json');});
document.getElementById('applyLvl').addEventListener('click',function(){var v=parseInt(sel.value,10);fetch('/api/logs/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({level:v})}).then(function(){return fetch('/api/logs/config');}).then(function(r){return r.json();}).then(function(c){if(typeof c.currentLevel==='number')sel.value=String(c.currentLevel);if(typeof c.compileMaxLevel==='number')capSel(c.compileMaxLevel);}).catch(function(){});});
fetch('/api/logs/config').then(function(r){return r.json();}).then(function(c){sel.value=String(c.currentLevel||0);capSel(c.compileMaxLevel||6);}).catch(function(){});
if(!pauseEl.checked)startPoll();
})();)JS";
  html += "</script></body></html>";
  request->send(200, "text/html", html);
  Log.verbose("[Web]: handleLogsPage %p" CR, request->client()->remoteIP());
}

void handleVersion(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(256);
  doc["version"] = VERSION;
  doc["build"] = BUILD;
  doc["hostname"] = WiFi.getHostname();
  doc["ip"] = WiFi.localIP().toString();
  doc["restartReason"] = getLastRestartReason();
  serializeJson(doc, *response);
  request->send(response);
}

void handleWifi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(512);
  wl_status_t st = WiFi.status();
  const bool ok = (st == WL_CONNECTED);
  doc["connected"] = ok;
  doc["status"] = static_cast<int>(st);
  doc["statusName"] = wifiStatusName(st);
  doc["mac"] = WiFi.macAddress();
  if (WiFi.getHostname())
  {
    doc["hostname"] = WiFi.getHostname();
  }
  else
  {
    doc["hostname"] = "";
  }
  if (ok)
  {
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["subnet"] = WiFi.subnetMask().toString();
    doc["dns"] = WiFi.dnsIP(0).toString();
    doc["channel"] = WiFi.channel();
  }
  else
  {
    doc["ssid"] = "";
    doc["ip"] = "";
    doc["gateway"] = "";
    doc["subnet"] = "";
    doc["dns"] = "";
    doc["channel"] = 0;
  }
  serializeJson(doc, *response);
  request->send(response);
}

void handleStatusControlsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(768);
  doc["lastUpdate"] = spaStatusData.lastUpdate;
  doc["tempScaleCelsius"] = spaStatusData.tempScale ? true : false;
  doc["setTemp"] = spaStatusData.setTemp;
  doc["light1"] = spaStatusData.light1 ? 1 : 0;
  doc["light2"] = spaStatusData.light2 ? 1 : 0;
  doc["pump1"] = spaStatusData.pump1;
  doc["pump2"] = spaStatusData.pump2;
  doc["pump3"] = spaStatusData.pump3;
  doc["pump4"] = spaStatusData.pump4;
  doc["pump5"] = spaStatusData.pump5;
  doc["pump6"] = spaStatusData.pump6;
  doc["pump1On"] = statusPumpIsOn(1);
  doc["pump2On"] = statusPumpIsOn(2);
  doc["pump3On"] = statusPumpIsOn(3);
  doc["pump4On"] = statusPumpIsOn(4);
  doc["pump5On"] = statusPumpIsOn(5);
  doc["pump6On"] = statusPumpIsOn(6);
  doc["blower"] = spaStatusData.blower;
  doc["mister"] = spaStatusData.mister ? 1 : 0;
  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagToggleApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(768);

  if (!request->hasParam("item"))
  {
    doc["ok"] = false;
    doc["error"] = "missing item query parameter";
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  const String itemStr = request->getParam("item")->value();
  int itemCode = itemStr.toInt();
  if (itemCode <= 0 || itemCode > 255)
  {
    doc["ok"] = false;
    doc["error"] = "invalid item query parameter";
    doc["item"] = itemStr;
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  String frameHex;
  SpaCommandResult result = spaSendToggleDiagnostic(
      (uint8_t)itemCode,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex);

  doc["ok"] = result.accepted;
  doc["item"] = itemCode;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  JsonObject snapshot = doc.createNestedObject("snapshot");
  fillPumpDiagSnapshot(snapshot);

  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagToggleSequenceApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(4096);

  if (!request->hasParam("item"))
  {
    doc["ok"] = false;
    doc["error"] = "missing item query parameter";
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  const String itemStr = request->getParam("item")->value();
  int itemCode = itemStr.toInt();
  if (itemCode <= 0 || itemCode > 255)
  {
    doc["ok"] = false;
    doc["error"] = "invalid item query parameter";
    doc["item"] = itemStr;
    serializeJson(doc, *response);
    request->send(response);
    return;
  }

  int repeats = 1;
  if (request->hasParam("repeats"))
  {
    repeats = request->getParam("repeats")->value().toInt();
  }
  repeats = constrain(repeats, 1, 6);

  int gapMs = 1200;
  if (request->hasParam("gap_ms"))
  {
    gapMs = request->getParam("gap_ms")->value().toInt();
  }
  gapMs = constrain(gapMs, 200, 10000);

  int observeMs = 5000;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 0, 20000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  doc["item"] = itemCode;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["repeats"] = repeats;
  doc["gapMs"] = gapMs;
  doc["observeMs"] = observeMs;

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);

  JsonArray attempts = doc.createNestedArray("attempts");
  bool acceptedAny = false;
  bool rejectedAny = false;
  const unsigned long startMs = millis();

  for (int i = 0; i < repeats; i++)
  {
    String frameHex;
    SpaCommandResult result = spaSendToggleDiagnostic(
        (uint8_t)itemCode,
        useWifiDestination,
        includeZeroPad,
        SPA_COMMAND_SOURCE_WEB,
        &frameHex);

    JsonObject attempt = attempts.createNestedObject();
    attempt["index"] = i + 1;
    attempt["accepted"] = result.accepted;
    attempt["reason"] = result.reason;
    attempt["frame"] = frameHex;
    attempt["elapsedMs"] = millis() - startMs;

    if (result.accepted)
    {
      acceptedAny = true;
    }
    else
    {
      rejectedAny = true;
      break;
    }

    if (i < repeats - 1)
    {
      delay(gapMs);
    }
  }

  if (observeMs > 0)
  {
    delay(observeMs);
  }

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);

  const bool pump1Changed = before["pump1"].as<int>() != after["pump1"].as<int>();
  const bool light1Changed = before["light1"].as<int>() != after["light1"].as<int>();

  doc["acceptedAny"] = acceptedAny;
  doc["allAccepted"] = acceptedAny && !rejectedAny && (attempts.size() == static_cast<size_t>(repeats));
  doc["pump1Changed"] = pump1Changed;
  doc["light1Changed"] = light1Changed;
  doc["ok"] = acceptedAny;

  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagLight1NextCtsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2048);

  int observeMs = 2500;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 200, 12000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);
  before["ctsMs"] = rs485LastCtsMs();
  before["ctsCount"] = rs485CtsCount();
  before["armCount"] = rs485NextCtsArmCount();
  before["fireCount"] = rs485NextCtsFireCount();
  before["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  String frameHex;
  uint32_t armCount = 0;
  SpaCommandResult result = spaSendToggleOnNextCtsDiagnostic(
      0x11,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex,
      &armCount);

  doc["ok"] = result.accepted;
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["observeMs"] = observeMs;
  doc["armedAt"] = armCount;

  if (result.accepted)
  {
    const unsigned long waitStart = millis();
    bool fired = false;
    while (millis() - waitStart < static_cast<unsigned long>(observeMs))
    {
      if (rs485NextCtsFireCount() >= armCount)
      {
        fired = true;
        break;
      }
      delay(20);
    }
    doc["fired"] = fired;
    doc["firedAt"] = rs485NextCtsFireCount();
    doc["waitElapsedMs"] = millis() - waitStart;
  }
  else
  {
    doc["fired"] = false;
    doc["firedAt"] = rs485NextCtsFireCount();
    doc["waitElapsedMs"] = 0;
  }

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);
  after["ctsMs"] = rs485LastCtsMs();
  after["ctsCount"] = rs485CtsCount();
  after["armCount"] = rs485NextCtsArmCount();
  after["fireCount"] = rs485NextCtsFireCount();
  after["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  doc["light1Changed"] = before["light1"].as<int>() != after["light1"].as<int>();
  serializeJson(doc, *response);
  request->send(response);
}

void handleDiagLight1NextCtsWindowApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(16384);

  int observeMs = 6000;
  if (request->hasParam("observe_ms"))
  {
    observeMs = request->getParam("observe_ms")->value().toInt();
  }
  observeMs = constrain(observeMs, 500, 20000);

  int sampleMs = 250;
  if (request->hasParam("sample_ms"))
  {
    sampleMs = request->getParam("sample_ms")->value().toInt();
  }
  sampleMs = constrain(sampleMs, 100, 2000);

  String dest = "wifi";
  if (request->hasParam("dest"))
  {
    dest = request->getParam("dest")->value();
    dest.toLowerCase();
  }
  bool useWifiDestination = (dest != "id");

  bool includeZeroPad = true;
  if (request->hasParam("pad"))
  {
    String pad = request->getParam("pad")->value();
    pad.toLowerCase();
    includeZeroPad = !(pad == "none" || pad == "0");
  }

  doc["dest"] = useWifiDestination ? "wifi" : "id";
  doc["pad"] = includeZeroPad ? "00" : "none";
  doc["observeMs"] = observeMs;
  doc["sampleMs"] = sampleMs;

  JsonObject before = doc.createNestedObject("before");
  fillPumpDiagSnapshot(before);
  before["ctsMs"] = rs485LastCtsMs();
  before["ctsCount"] = rs485CtsCount();
  before["armCount"] = rs485NextCtsArmCount();
  before["fireCount"] = rs485NextCtsFireCount();
  before["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  String frameHex;
  uint32_t armCount = 0;
  SpaCommandResult result = spaSendToggleOnNextCtsDiagnostic(
      0x11,
      useWifiDestination,
      includeZeroPad,
      SPA_COMMAND_SOURCE_WEB,
      &frameHex,
      &armCount);
  doc["ok"] = result.accepted;
  doc["result"] = result.reason;
  doc["frame"] = frameHex;
  doc["armedAt"] = armCount;

  JsonArray samples = doc.createNestedArray("samples");
  const unsigned long startMs = millis();
  const int maxSamples = 120;
  bool fired = false;

  while ((millis() - startMs) < static_cast<unsigned long>(observeMs))
  {
    JsonObject s = samples.createNestedObject();
    s["tMs"] = millis() - startMs;
    s["ctsCount"] = rs485CtsCount();
    s["fireCount"] = rs485NextCtsFireCount();
    fillPumpDiagSnapshot(s);
    if (rs485NextCtsFireCount() >= armCount && result.accepted)
    {
      fired = true;
    }
    if (samples.size() >= static_cast<size_t>(maxSamples))
    {
      break;
    }
    delay(sampleMs);
  }

  doc["fired"] = fired;
  doc["firedAt"] = rs485NextCtsFireCount();
  doc["sampleCount"] = samples.size();

  JsonObject after = doc.createNestedObject("after");
  fillPumpDiagSnapshot(after);
  after["ctsMs"] = rs485LastCtsMs();
  after["ctsCount"] = rs485CtsCount();
  after["armCount"] = rs485NextCtsArmCount();
  after["fireCount"] = rs485NextCtsFireCount();
  after["queueDepth"] = static_cast<unsigned int>(uxQueueMessagesWaiting(spaWriteQueue));

  doc["light1Changed"] = before["light1"].as<int>() != after["light1"].as<int>();
  serializeJson(doc, *response);
  request->send(response);
}

void handleRs485(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2048);
  doc["rxGpio"] = rs485RxGpio();
  doc["txGpio"] = rs485TxGpio();
  doc["baud"] = rs485Baud();
  doc["autoTx"] = rs485AutoTxEnabled();
  doc["rawBytesToday"] = rs485Stats.rawBytesToday;
  doc["rawBytesYesterday"] = rs485Stats.rawBytesYesterday;
  doc["rawBytesNormalToday"] = rs485Stats.rawBytesNormalToday;
  doc["rawBytesNormalYesterday"] = rs485Stats.rawBytesNormalYesterday;
  doc["rawBytesInvertedToday"] = rs485Stats.rawBytesInvertedToday;
  doc["rawBytesInvertedYesterday"] = rs485Stats.rawBytesInvertedYesterday;
  doc["framesToday"] = rs485Stats.framesToday;
  doc["framesYesterday"] = rs485Stats.framesYesterday;
  doc["messagesToday"] = rs485Stats.messagesToday;
  doc["messagesYesterday"] = rs485Stats.messagesYesterday;
  doc["crcToday"] = rs485Stats.crcToday;
  doc["crcYesterday"] = rs485Stats.crcYesterday;
  doc["badFormatToday"] = rs485Stats.badFormatToday;
  doc["badFormatYesterday"] = rs485Stats.badFormatYesterday;
  doc["frameMarkersToday"] = rs485Stats.frameMarkersToday;
  doc["frameMarkersYesterday"] = rs485Stats.frameMarkersYesterday;
  doc["maxUartAvailableToday"] = rs485Stats.maxUartAvailableToday;
  doc["maxUartAvailableYesterday"] = rs485Stats.maxUartAvailableYesterday;
  doc["rawCaptureOverflowsToday"] = rs485Stats.rawCaptureOverflowsToday;
  doc["rawCaptureOverflowsYesterday"] = rs485Stats.rawCaptureOverflowsYesterday;
  doc["polaritySwitchesToday"] = rs485Stats.polaritySwitchesToday;
  doc["polaritySwitchesYesterday"] = rs485Stats.polaritySwitchesYesterday;
  doc["polarityInverted"] = rs485Stats.polarityInverted;
  doc["polarityLocked"] = rs485Stats.polarityLocked;
  doc["mode"] = rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal";
  doc["detectPhase"] = rs485Stats.polarityLocked ? 2 : (rs485Stats.polarityInverted ? 1 : 0);
  doc["lastByteMs"] = rs485Stats.lastByteMs;
  doc["lastValidFrameMs"] = rs485Stats.lastValidFrameMs;
  doc["lastCtsMs"] = rs485LastCtsMs();
  doc["ctsCount"] = rs485CtsCount();
  doc["nextCtsArmCount"] = rs485NextCtsArmCount();
  doc["nextCtsFireCount"] = rs485NextCtsFireCount();
  doc["health"] = rs485HealthCode();

  serializeJson(doc, *response);
  request->send(response);
}

void handleRs485Raw(AsyncWebServerRequest *request)
{
  int limit = 80;
  if (request->hasArg("limit"))
  {
    const int requested = request->arg("limit").toInt();
    if (requested > 0)
    {
      limit = requested;
    }
  }
  if (limit > RS485_RAW_CAPTURE_SIZE)
  {
    limit = RS485_RAW_CAPTURE_SIZE;
  }

  Rs485RawByte bytes[RS485_RAW_CAPTURE_SIZE];
  const int count = rs485GetRawRecent(bytes, limit);

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print("{\"count\":");
  response->print(count);
  response->print(",\"limit\":");
  response->print(limit);
  response->print(",\"rxGpio\":");
  response->print(rs485RxGpio());
  response->print(",\"txGpio\":");
  response->print(rs485TxGpio());
  response->print(",\"baud\":");
  response->print(rs485Baud());
  response->print(",\"mode\":\"");
  response->print(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal");
  response->print("\",\"bytesHex\":\"");
  for (int i = 0; i < count; i++)
  {
    char b[4];
    snprintf(b, sizeof(b), "%02X", bytes[i].value);
    if (i > 0)
    {
      response->print(' ');
    }
    response->print(b);
  }
  response->print("\",\"items\":[");
  for (int i = 0; i < count; i++)
  {
    char row[176];
    snprintf(row, sizeof(row),
             "%s{\"tMs\":%lu,\"gapMs\":%u,\"byte\":\"%02X\",\"dec\":%u,\"mode\":\"%s\",\"uartAvailable\":%u}",
             i > 0 ? "," : "",
             static_cast<unsigned long>(bytes[i].tMs),
             bytes[i].gapMs,
             bytes[i].value,
             bytes[i].value,
             bytes[i].polarityInverted ? "inverted_rx_tx" : "normal",
             bytes[i].uartAvailable);
    response->print(row);
  }
  response->print("]}");
  request->send(response);
}

void handleRs485History(AsyncWebServerRequest *request)
{
  int limit = 20;
  if (request->hasArg("limit"))
  {
    const int requested = request->arg("limit").toInt();
    if (requested > 0)
    {
      limit = requested;
    }
  }
  if (limit > RS485_HISTORY_SIZE)
  {
    limit = RS485_HISTORY_SIZE;
  }

  Rs485Snapshot snapshots[RS485_HISTORY_SIZE];
  const int count = rs485GetHistoryNewestFirst(snapshots, limit);

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(16384);
  doc["count"] = count;
  doc["limit"] = limit;
  JsonArray items = doc.createNestedArray("items");
  for (int i = 0; i < count; i++)
  {
    JsonObject row = items.createNestedObject();
    row["tMs"] = snapshots[i].tMs;
    row["health"] = snapshots[i].health;
    row["mode"] = snapshots[i].polarityInverted ? "inverted_rx_tx" : "normal";
    row["detectPhase"] = snapshots[i].detectPhase;
    row["rawBytesToday"] = snapshots[i].rawBytesToday;
    row["rawBytesNormalToday"] = snapshots[i].rawBytesNormalToday;
    row["rawBytesInvertedToday"] = snapshots[i].rawBytesInvertedToday;
    row["framesToday"] = snapshots[i].framesToday;
    row["messagesToday"] = snapshots[i].messagesToday;
    row["crcToday"] = snapshots[i].crcToday;
    row["badFormatToday"] = snapshots[i].badFormatToday;
    row["polaritySwitchesToday"] = snapshots[i].polaritySwitchesToday;
  }

  serializeJson(doc, *response);
  request->send(response);
}

/*

This is the balboa cloud emulation

*/

String encodeResponse(uint8_t rawData[BALBOA_MESSAGE_SIZE], uint8_t length)
{
  if (length)
  {
    unsigned char message[BALBOA_MESSAGE_SIZE];
    for (int i = 0; i < length - 2 && i < BALBOA_MESSAGE_SIZE; i++)
    {
      message[i] = (char)rawData[i + 1];
    }
    message[length - 2] = '\0';

    //  Log.verbose("Encode: %s\n", message);
    // Base64 encode the string
    unsigned char encoded[BALBOA_MESSAGE_SIZE * 2 + 1];
    int encodedLength = encode_base64(message, length - 2, encoded);
    encoded[encodedLength] = '\0';
    char encodedString[BALBOA_MESSAGE_SIZE * 2];
    strncpy((char *)encodedString, (char *)encoded, encodedLength);
    encodedString[encodedLength] = '\0';
    //  Log.verbose("Encoded: %s\n", encodedString);
    return String(encodedString);
  }
  else
  {
    return "";
  }
}

void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  // Log.verbose("[Web]: handleBody Request %s %s %d received from %p" CR, request->methodToString(), request->url().c_str(), index, request->client()->remoteIP());

  if (index == 0)
  {
    request->_tempObject = new String();
    if (request->_tempObject == nullptr)
    {
      Log.error("[Web]: handleBody String allocation failed (total=%d, len=%d)" CR, total, len);
      return;
    }
  }

  if (request->_tempObject == nullptr)
  {
    return;
  }

  // Append each body chunk; this works for both known-length and chunked transfers.
  String *body = (String *)request->_tempObject;
  for (size_t i = 0; i < len; i++)
  {
    body->concat((char)data[i]);
  }
}

// <sci_request version="1.0"><file_system><targets><device id="00 11 22 33 44 55 66 77"/></targets><commands><get_file path="PanelUpdate.txt"/></commands></file_system></sci_request>
// <sci_request version="1.0"><file_system><targets><device id="00 11 22 33 44 55 66 77"/></targets><commands><get_file path="SystemInformation.txt"/></commands></file_system></sci_request>
// <sci_request version="1.0"><file_system cache="false"><targets><device id="00 11 22 33 44 55 66 77" /></targets><commands><get_file path="SetupParameters.txt" /></commands></file_system></sci_request>

// <sci_request version="1.0"><data_service><targets><device id="00 11 22 33 44 55 66 77"/></targets><requests><device_request target_name="Request">Filters</device_request></requests></data_service></sci_request>

// <sci_request version="1.0"><data_service><targets><device id="00 11 22 33 44 55 66 77"/></targets><requests><device_request target_name="TempUnits">F</device_request></requests></data_service></sci_request>

String parseBody(String body)
{
  String response = "";
  if (body.indexOf("PanelUpdate.txt") > 0)
  {
    response = encodeResponse(spaStatusData.rawData, spaStatusData.rawDataLength);
  }
  else if (body.indexOf("DeviceConfiguration.txt") > 0)
  {
    response = encodeResponse(spaConfigurationData.rawData, spaConfigurationData.rawDataLength);
  }
  else if (body.indexOf("SetupParameters.txt") > 0)
  {
    response = encodeResponse(spaSettings0x04Data.rawData, spaSettings0x04Data.rawDataLength);
  }
  else if (body.indexOf("SystemInformation.txt") > 0)
  {
    response = encodeResponse(spaInformationData.rawData, spaInformationData.rawDataLength);
  }
  else if (body.indexOf("Filters") > 0)
  {
    //<device_request target_name="Filters">${encodedValue}</device_request>
    response = "<device_request target_name='Filters'>" + encodeResponse(spaFilterSettingsData.rawData, spaFilterSettingsData.rawDataLength) + "</device_request>";
  }
  else if (body.indexOf("device_request") > 0)
  {
    using namespace tinyxml2;
    XMLDocument xmlDocument;
    xmlDocument.Parse(body.c_str());
    tinyxml2::XMLElement *root = xmlDocument.FirstChildElement("sci_request");
    tinyxml2::XMLElement *dataService = (root ? root->FirstChildElement("data_service") : nullptr);
    tinyxml2::XMLElement *requests = (dataService ? dataService->FirstChildElement("requests") : nullptr);
    tinyxml2::XMLElement *deviceRequestElement = (requests ? requests->FirstChildElement("device_request") : nullptr);

    if (!deviceRequestElement)
    {
      response = "<device_request result='rejected' error='invalid_xml'></device_request>";
      return response;
    }

    const char *targetName = deviceRequestElement->Attribute("target_name");
    const char *deviceRequestValue = deviceRequestElement->GetText();
    String target = (targetName ? String(targetName) : "");
    String value = (deviceRequestValue ? String(deviceRequestValue) : "");

    if (target == "Button")
    {
      int separator = value.indexOf(':');
      String itemCodeRaw = (separator > 0 ? value.substring(0, separator) : value);
      String desiredStateRaw = (separator > 0 ? value.substring(separator + 1) : "");
      bool requestHasState = separator > 0;
      bool desiredOn = desiredStateRaw.equalsIgnoreCase("on");
      int itemCode = itemCodeRaw.toInt();
      if (itemCode <= 0 || itemCode > 255)
      {
        response = "<device_request target_name='Button' result='rejected' error='invalid_button_payload'>" + value + "</device_request>";
        return response;
      }

      int togglesToSend = toggleCountForButtonRequest((uint8_t)itemCode, requestHasState, desiredOn);
      Log.verbose("[Web]: Button request raw=%s item=%d desired=%s toggles=%d" CR, value.c_str(), itemCode, (requestHasState ? desiredStateRaw.c_str() : "n/a"), togglesToSend);
      if (togglesToSend <= 0)
      {
        response = "<device_request target_name='Button' result='accepted'>" + value + "</device_request>";
        Log.verbose("[Web]: Button request no-op; already in desired state" CR);
      }
      else
      {
        SpaCommandResult result = {false, SPA_COMMAND_INVALID_ARGUMENT, "unknown"};
        for (int i = 0; i < togglesToSend; i++)
        {
          result = spaSendToggleCommand((uint8_t)itemCode, SPA_COMMAND_SOURCE_WEB);
          if (!result.accepted)
          {
            break;
          }
        }

        if (result.accepted)
        {
          response = "<device_request target_name='Button' result='accepted'>" + value + "</device_request>";
        }
        else
        {
          response = "<device_request target_name='Button' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
        }
        Log.verbose("[Web]: Button request %s -> %s" CR, value.c_str(), result.reason);
      }
    }
    else if (target == "SetTemp")
    {
      float requested = value.toFloat();
      if (requested <= 0.0f)
      {
        response = "<device_request target_name='SetTemp' result='rejected' error='invalid_temp_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetTargetTemperature(requested, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='SetTemp' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='SetTemp' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: SetTemp request %s -> %s" CR, value.c_str(), result.reason);
    }
    else
    {
      response = "<device_request target_name='" + target + "' result='rejected' error='unsupported_target'>" + value + "</device_request>";
      Log.verbose("[Web]: Unsupported device_request target=%s value=%s" CR, target.c_str(), value.c_str());
    }
  }
  else
  {
    // Log.verbose("[Web]: Error Unknown object requested %s" CR, body.c_str());
  }
  return response;
}

void handleData(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: handleData Request %s %s received from %p" CR, request->methodToString(), request->url().c_str(), request->client()->remoteIP());

  if (request->_tempObject != nullptr)
  {
    // Log.verbose("[Web]: handleData _tempObject %s" CR, request->_tempObject);
    String *bodyPtr = (String *)request->_tempObject;
    String body = *bodyPtr;
    // Log.verbose("[Web]: handleData 1" CR);
    delete bodyPtr;
    // Log.verbose("[Web]: handleData 2" CR);
    request->_tempObject = nullptr;
    // Log.verbose("[Web]: handleData body %s" CR, body.c_str());

    String response = parseBody(body);
    if (response.length() == 0)
    {
      Log.verbose("[Web]: handleData no spa data yet for %s" CR, body.c_str());
      // Keep API responses explicit while avoiding noisy 404 loops during bench testing.
      request->send(200, "text/xml", "<response><ready>false</ready><error>no_spa_data_yet</error></response>");
      return;
    }
    // Log.verbose("[Web]: handleData response %s" CR, response.c_str());
    Log.verbose("[Web]: handleData %p %s %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str(), response.c_str());
    request->send(200, "text/xml", "<response><data>" + response + "</data></response>");
  }
  else
  {
    Log.verbose("[Web]: handleData no body" CR);
    request->send(200, "text/xml", "<noresponse></noresponse>");
  }
}

void handleLoginData(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: handleData Request %s %s received from %p" CR, request->methodToString(), request->url().c_str(), request->client()->remoteIP());

  if (request->_tempObject != nullptr)
  {
    // Log.verbose("[Web]: handleData _tempObject %s" CR, request->_tempObject);
    String *bodyPtr = (String *)request->_tempObject;
    String body = *bodyPtr;
    // Log.verbose("[Web]: handleData 1" CR);
    delete bodyPtr;
    // Log.verbose("[Web]: handleData 2" CR);
    request->_tempObject = nullptr;
    // Log.verbose("[Web]: handleData body %s" CR, body.c_str());

    // data.device.device_id
    // data.token

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(128);

    doc["username"] = WiFi.getHostname();
    doc["token"] = WiFi.macAddress();
    doc["device"]["device_id"] = WiFi.macAddress();

    serializeJsonPretty(doc, *response);
    request->send(response);

    Log.verbose("[Web]: handleData %p %s %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str(), response);
  }
  else
  {
    Log.verbose("[Web]: handleData no body" CR);
    request->send(200, "text/xml", "<noresponse></noresponse>");
  }
}

void handleOptionsData(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleOptionsData %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  request->send(200, "text/plain", "Data received");
}

void handleOptionsLoginData(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleOptionsLoginData %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  request->send(200, "text/plain", "Data received");
}

void handleSlash(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: handleSlash %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
  AsyncWebServerResponse *response = request->beginResponse(302); // Sends 302 Weiterleitung
  response->addHeader("Location", "index.html");
  request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  if (LittleFS.exists(request->url()))
  {
    Log.verbose("[Web]: LFS %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
    request->send(LittleFS, request->url(), String(), false);
    return;
  }

  Log.verbose(F("[Web]: handleNotFound() %s %s" CR), request->methodToString(), request->url().c_str());
  int headers = request->headers();
  int i;
  for (i = 0; i < headers; i++)
  {
    const AsyncWebHeader *h = request->getHeader(i);  // Add 'const' here
    Log.verbose("HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
  }

  int args = request->args();
  for (int i = 0; i < args; i++)
  {
    Log.verbose(F("ARG[%s]: %s" CR), request->argName(i).c_str(), request->arg(i).c_str());
  }

  request->send(404, "text/plain", "Not found");
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      if (levels)
      {
        listDir(fs, file.path(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(dirname);
      if (strcmp(dirname, "/") != 0)
      {
        Serial.print("/");
      }
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

String listDirToString(fs::FS &fs, const char *dirname, uint8_t levels)
{
  String response = "";
  File root = fs.open(dirname);
  if (!root)
  {
    // Serial.println("- failed to open directory");
    return String("");
  }
  if (!root.isDirectory())
  {
    // Serial.println(" - not a directory");
    return String("");
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      if (levels)
      {
        response += listDirToString(fs, file.path(), levels - 1);
      }
    }
    else
    {
      response += "  FILE: " + String(dirname);
      if (strcmp(dirname, "/") != 0)
      {
        response += "/";
      }
      response += String(file.name()) + "\tSIZE: " + String(file.size()) + "<BR>";
    }
    file = root.openNextFile();
  }
  return response;
}
