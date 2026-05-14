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
#include <memory>
#include <spaMessage.h>
#include <spaUtilities.h>
#include <restartReason.h>
#include <faultCapture.h>
#include <rs485.h>
#include <mqttModule.h>
#include "../../src/config.h"
#include "../../src/main.h"

// Local functions

void handleConfig(AsyncWebServerRequest *request);
void handleStatus(AsyncWebServerRequest *request);
void handleState(AsyncWebServerRequest *request);
void handleVersion(AsyncWebServerRequest *request);
void handleWifi(AsyncWebServerRequest *request);
void handleMqtt(AsyncWebServerRequest *request);
void handleStatusControlsApi(AsyncWebServerRequest *request);
void handleStatusSummaryApi(AsyncWebServerRequest *request);
void handleStatusHistoriesApi(AsyncWebServerRequest *request);
void handleStateLittleFsApi(AsyncWebServerRequest *request);
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

static bool sendNotModifiedIfEtagMatches(AsyncWebServerRequest *request, const String &etag)
{
  if (!request || etag.length() == 0 || !request->hasHeader("If-None-Match"))
  {
    return false;
  }
  const AsyncWebHeader *matchHeader = request->getHeader("If-None-Match");
  if (!matchHeader)
  {
    return false;
  }
  const String matchValue = matchHeader->value();
  if (matchValue.indexOf(etag) < 0)
  {
    return false;
  }
  AsyncWebServerResponse *notModified = request->beginResponse(304);
  notModified->addHeader("ETag", etag);
  notModified->addHeader("Cache-Control", "no-cache");
  request->send(notModified);
  return true;
}

/** Send HTML with ETag. Uses callback body delivery so ESPAsyncWebServer does not keep a second
 *  full copy of the page in `AsyncBasicResponse::_content` (which can peak at ~2–3× RAM for
 *  large `/status` and reset TCP mid-transfer on ESP32). */
static void sendHtmlWithEtag(AsyncWebServerRequest *request, String &html, const String &etag)
{
  if (!request)
  {
    return;
  }
  if (sendNotModifiedIfEtagMatches(request, etag))
  {
    return;
  }
  const size_t bodyLen = html.length();
  auto sharedBody = std::make_shared<String>(std::move(html));
  AsyncWebServerResponse *response = request->beginResponse(
      "text/html; charset=utf-8",
      bodyLen,
      [sharedBody](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (index >= sharedBody->length())
        {
          return 0;
        }
        size_t n = sharedBody->length() - index;
        if (n > maxLen)
        {
          n = maxLen;
        }
        memcpy(buffer, sharedBody->c_str() + index, n);
        return n;
      });
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

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

static const char *mqttStateName(int s)
{
  switch (s)
  {
  case 0:
    return "Connected";
  case -1:
    return "Disconnected";
  case -2:
    return "Connect failed";
  case -3:
    return "Connection lost";
  case -4:
    return "Timeout";
  case -5:
    return "Bad credentials";
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

  html += "</section>";
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
    server.on("/", HTTP_GET, handleStatus);
    server.on("/state", HTTP_GET, handleState);
    server.on("/config", HTTP_GET, handleConfig);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/api/version", HTTP_GET, handleVersion);
    server.on("/api/wifi", HTTP_GET, handleWifi);
    server.on("/api/mqtt", HTTP_GET, handleMqtt);
    server.on("/api/status/controls", HTTP_GET, handleStatusControlsApi);
    server.on("/api/status/summary", HTTP_GET, handleStatusSummaryApi);
    server.on("/api/status/histories", HTTP_GET, handleStatusHistoriesApi);
    server.on("/api/state/littlefs", HTTP_GET, handleStateLittleFsApi);
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

#define style String("<style>:root{--bg:#f4f7f8;--panel:#fff;--text:#1f2933;--muted:#5f6c7b;--brand:#037e52;--brandActive:#4b5563;--border:#d4dbe1;--focus:#0f4a87;--space-1:6px;--space-2:10px;--space-3:14px;--space-4:20px;}*{box-sizing:border-box;}body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text);line-height:1.5;}html,body{max-width:100%;overflow-x:hidden;}img,canvas{display:block;max-width:100%;height:auto;}.skip-link{position:absolute;left:10px;top:-48px;z-index:999;background:#0f4a87;color:#fff;padding:10px 12px;border-radius:6px;text-decoration:none;}.skip-link:focus{top:10px;outline:3px solid #fff;outline-offset:2px;}.page{max-width:980px;margin:0 auto;padding:var(--space-3);}h1{color:#0f4a87;font-size:1.05rem;margin:0 0 var(--space-2) 0;line-height:1.3;}.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:var(--space-3);margin-bottom:var(--space-3);box-shadow:0 1px 2px rgba(0,0,0,.04);}ul{list-style:none;margin:0;padding:0;}li{padding:var(--space-1) 0;border-bottom:1px dashed #e5eaef;overflow-wrap:anywhere;word-break:break-word;}li:last-child{border-bottom:none;}.spacer{height:8px;border-bottom:none;padding:0;}.top-nav{display:flex;flex-wrap:wrap;gap:var(--space-1);margin-bottom:var(--space-3);}.top-nav a{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;flex:1 1 170px;font-weight:600;transition:background-color .15s ease,transform .15s ease}.top-nav a.active{background-color:var(--brandActive);color:#fff}@media (hover:hover){.top-nav a:hover{background-color:var(--brandActive)}}.top-nav a:focus-visible{outline:3px solid var(--focus);outline-offset:2px}.top-nav a:active{transform:translateY(1px)}@media (prefers-reduced-motion:reduce){.top-nav a{transition:none}}.top-nav-mobile{display:none}.top-nav-mobile__summary{display:block;cursor:pointer;list-style:none;padding:0;margin:0}.top-nav-mobile__summary::-webkit-details-marker{display:none}.top-nav-mobile__summary::marker{content:''}.top-nav-mobile__summary-inner{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 14px;background:var(--panel);border:1px solid var(--border);border-radius:8px;font-weight:600;font-size:15px;line-height:1.2;box-shadow:0 1px 3px rgba(0,0,0,.06)}.top-nav-mobile__context{color:var(--text);flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.top-nav-mobile__menu{display:flex;align-items:center;gap:6px;color:var(--brand);font-weight:700;flex-shrink:0}.top-nav-mobile__chev{border:solid currentColor;border-width:0 2px 2px 0;display:inline-block;padding:3px;transform:rotate(45deg);transition:transform .15s ease;margin-top:-2px}.top-nav-mobile[open] .top-nav-mobile__chev{transform:rotate(225deg);margin-top:2px}.top-nav-mobile__panel{display:flex;flex-direction:column;gap:var(--space-1);margin-top:var(--space-2);padding:var(--space-2);background:var(--panel);border:1px solid var(--border);border-radius:8px}.top-nav-mobile__panel a{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;font-weight:600;width:100%;box-sizing:border-box;transition:background-color .15s ease,transform .15s ease}.top-nav-mobile__panel a.active{background-color:var(--brandActive);color:#fff}@media (hover:hover){.top-nav-mobile__panel a:hover{background-color:var(--brandActive)}}.top-nav-mobile__panel a:focus-visible{outline:3px solid var(--focus);outline-offset:2px}.top-nav-mobile__panel a:active{transform:translateY(1px)}.portal-nav-scroll-sentinel{height:1px;width:100%;margin:0;padding:0;border:0;pointer-events:none;opacity:0;position:relative}@media (prefers-reduced-motion:reduce){.top-nav-mobile__chev{transition:none}.top-nav-mobile__panel a{transition:none}}button{border:none;color:#fff;padding:12px 16px;text-align:center;text-decoration:none;display:inline-flex;justify-content:center;align-items:center;font-size:15px;line-height:1.2;min-height:44px;cursor:pointer;background-color:var(--brand);border-radius:8px;flex:1 1 170px;font-weight:600;transition:background-color .15s ease,transform .15s ease;}.active{background-color:var(--brandActive);color:#fff;}@media (hover:hover){button:hover{background-color:var(--brandActive);}}button:focus-visible{outline:3px solid var(--focus);outline-offset:2px;}button:active{transform:translateY(1px);}.panel-image{width:100%;max-width:600px;margin:0 auto var(--space-3) auto;border-radius:8px;}.chart-title{margin:12px 0 6px 0;color:var(--muted);}.chart-wrap{width:100%;max-width:100%;overflow:hidden;border:1px solid #ccc;background:#fff;border-radius:6px;}#wf-rssi,#wf-quality{font-weight:700;}@media (max-width:640px){.top-nav{display:none !important}.top-nav-mobile{display:block;margin-bottom:var(--space-3)}.top-nav-mobile__summary{position:sticky;top:0;z-index:50}.top-nav-mobile__summary .top-nav-mobile__summary-inner{background:var(--panel)}body.portal-nav-compact .top-nav-mobile__summary-inner{padding:6px 10px;font-size:0.88rem}body.portal-nav-compact .top-nav-mobile__menu-text{display:none}.page{padding:var(--space-2);}button{flex:1 1 100%;width:100%;}.log-controls button,.range-toggle button,.status-temp-units-toggle button,.equip-btn{width:auto!important;flex:0 1 auto!important;min-width:0}.panel{padding:var(--space-2);}h1{font-size:1rem;}}@media (prefers-reduced-motion:reduce){button{transition:none;}}</style>")

#define icon String("<link rel='icon' href='/assets/style/hottubbing.webp' type='image/x-icon' />")

#define portalNavScrollScript String("<script>(function(){var m=window.matchMedia('(max-width:640px)');var io=null;function setup(){document.body.classList.remove('portal-nav-compact');if(io){io.disconnect();io=null;}if(!m.matches)return;var s=document.querySelector('.portal-nav-scroll-sentinel');if(!s)return;io=new IntersectionObserver(function(e){e.forEach(function(x){document.body.classList.toggle('portal-nav-compact',!x.isIntersecting);});},{threshold:0});io.observe(s);}if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',setup);else setup();m.addEventListener('change',setup);})();</script>")

#define headStatus String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Spa Status</title>") + icon + style + portalNavScrollScript + String("</head>")
#define headConfig String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Spa Config</title>") + icon + style + portalNavScrollScript + String("</head>")
#define headState String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>ESP State</title>") + icon + style + portalNavScrollScript + String("</head>")

#define webMenuStatus String("<nav aria-label='Portal navigation' class='portal-nav'><div class='top-nav'><a class='active' aria-current='page' href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Spa Status</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a class='active' aria-current='page' href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>")

#define webMenuConfig String("<nav aria-label='Portal navigation' class='portal-nav'><div class='top-nav'><a href='/status'>Spa Status</a><a class='active' aria-current='page' href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Spa Config</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a class='active' aria-current='page' href='/config'>Spa Config</a><a href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>")

#define webMenuState String("<nav aria-label='Portal navigation' class='portal-nav'><div class='top-nav'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a class='active' aria-current='page' href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>ESP State</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a class='active' aria-current='page' href='/state'>ESP State</a><a href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>")

#define webMenuLogs String("<nav aria-label='Portal navigation' class='portal-nav'><div class='top-nav'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a class='active' aria-current='page' href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div><details class='top-nav-mobile'><summary class='top-nav-mobile__summary'><span class='top-nav-mobile__summary-inner'><span class='top-nav-mobile__context'>Logs</span><span class='top-nav-mobile__menu'><span class='top-nav-mobile__chev' aria-hidden='true'></span><span class='top-nav-mobile__menu-text'>Menu</span></span></span></summary><div class='top-nav-mobile__panel' role='group' aria-label='Portal pages'><a href='/status'>Spa Status</a><a href='/config'>Spa Config</a><a href='/state'>ESP State</a><a class='active' aria-current='page' href='/logs'>Logs</a><a href='/index.html'>Spa Website</a></div></details></nav><div class='portal-nav-scroll-sentinel' aria-hidden='true'></div>")

#define headLogs String("<head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1,viewport-fit=cover'><title>Spa Logs</title>") + icon + style + portalNavScrollScript + String("<style>.log-pre{min-height:260px;max-height:70vh;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:12px;border-radius:8px;white-space:pre-wrap;word-break:break-word;margin:0;border:1px solid var(--border)}.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:12px}.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px}.log-controls label{font-size:14px;color:var(--muted)}.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px}</style></head>")

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

/** Primary local date/time for UI display. */
static String statusLastUpdateDisplayHtml(unsigned long epoch)
{
  time_t t = static_cast<time_t>(epoch);
  return statusFormatEpochLocalHuman(t);
}

/** Seconds since last spa status frame was applied (gateway clock); 0 if unknown. */
static unsigned long statusSnapshotAgeSec()
{
  time_t now = getTime();
  if (now <= 0 || spaStatusData.lastUpdate == 0)
  {
    return 0;
  }
  if ((time_t)spaStatusData.lastUpdate > now)
  {
    return 0;
  }
  return (unsigned long)(now - (time_t)spaStatusData.lastUpdate);
}

/** One-line subtitle: relative age + gateway-local time of last bus status apply. */
static String statusSnapshotSubtitle()
{
  if (spaStatusData.lastUpdate == 0)
  {
    return String("No spa status yet");
  }
  const String human = statusLastUpdateDisplayHtml(spaStatusData.lastUpdate);
  const unsigned long age = statusSnapshotAgeSec();
  if (age == 0 && getTime() <= 0)
  {
    return String("Snapshot at ") + human;
  }
  String out = "Updated ";
  if (age < 60UL)
  {
    out += String(age) + "s ago";
  }
  else if (age < 3600UL)
  {
    out += String(age / 60UL) + "m " + String(age % 60UL) + "s ago";
  }
  else
  {
    const unsigned long h = age / 3600UL;
    const unsigned long m = (age % 3600UL) / 60UL;
    out += String(h) + "h " + String(m) + "m ago";
  }
  out += " \xc2\xb7 ";
  out += human;
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

static void appendStatusKvRow(String &html, const char *label, const String &value, const char *ddId = nullptr, const char *ddTitle = nullptr)
{
  html += "<div class=\"kv-row\"><dt>";
  html += label;
  html += "</dt><dd";
  if (ddId != nullptr && ddId[0] != '\0')
  {
    html += " id=\"";
    html += ddId;
    html += "\"";
  }
  if (ddTitle != nullptr && ddTitle[0] != '\0')
  {
    html += " title=\"";
    html += ddTitle;
    html += "\"";
  }
  html += ">";
  html += value;
  html += "</dd></div>";
}

/** Status byte9 bit1 (mask 0x02): panel 12h vs 24h clock display. */
static String statusPanelClockFormatLabel(uint8_t clockModeFromStatus)
{
  return ((clockModeFromStatus & 0x02) != 0) ? String("24-hour") : String("12-hour (AM/PM)");
}

/** Gateway wall clock as HH:MM for \"sync panel time\" (uses `getTime()` / local TZ). */
static String statusGatewayLocalTimeHHMM()
{
  time_t t = getTime();
  if (t <= 0)
  {
    return String("--:--");
  }
  struct tm tmStore;
  struct tm *p = localtime_r(&t, &tmStore);
  if (p == nullptr)
  {
    return String("--:--");
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", p->tm_hour, p->tm_min);
  return String(buf);
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

/** CSS suffix for `equip-cell--*`: off, low, high, on. Null when card is `equip-absent`. */
static const char *statusPumpEquipStateClass(unsigned pumpId, bool configuredAbsent)
{
  if (configuredAbsent)
  {
    return nullptr;
  }
  const uint8_t cfg = statusPumpConfigSpeed(pumpId);
  if (cfg == 1)
  {
    return statusPumpIsOn(pumpId) ? "on" : "off";
  }
  const uint8_t raw = statusPumpRawState(pumpId);
  if (raw == 0)
  {
    return "off";
  }
  if (raw == 1)
  {
    return "low";
  }
  return "high";
}

static const char *statusBinaryEquipStateClass(bool configuredAbsent, bool on)
{
  if (configuredAbsent)
  {
    return nullptr;
  }
  return on ? "on" : "off";
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

static void appendStatusControlCell(String &html, const char *label, const char *equipKey, const String &value, bool configuredAbsent, int buttonCode, const char *desiredState, const char *equipStateClass)
{
  if (configuredAbsent)
  {
    html += "<div class=\"equip-cell equip-absent\" title=\"Not installed (spa configuration)\"";
  }
  else
  {
    html += "<div class=\"equip-cell";
    if (equipStateClass != nullptr && equipStateClass[0] != '\0')
    {
      html += " equip-cell--";
      html += equipStateClass;
    }
    html += "\"";
  }
  if (equipKey != nullptr && equipKey[0] != '\0')
  {
    html += " data-equip=\"";
    html += equipKey;
    html += "\"";
  }
  html += ">";
  html += "<div class=\"equip-label\">";
  html += label;
  html += "</div><div class=\"equip-val\" data-role=\"value\">";
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

/** Stored high/low setpoint for a band; em dash when never populated (<=0). */
static String statusBandStoredSetpointText(float v)
{
  if (!statusSpaTempReady() || v <= 0.0f)
  {
    return String("\xe2\x80\x94");
  }
  return statusFormattedTempWithUnit(v);
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

static void appendStatusHistoriesSection(String &html)
{
  html += "<div id=\"statusTempHistSection\" class=\"history-block status-temp-hist-anchor\">";
  html += "<h3>Temperature history</h3>";
  html += "<p class=\"chart-caption\">Samples left (older) to right (newer). Raw list index 0 is newest.</p>";
  html += "<div class=\"chart-wrap\"><canvas id=\"statusTempHistChart\" height=\"140\" aria-label=\"Temperature history chart\"></canvas></div>";
  html += "<details class=\"history-raw\"><summary>Raw temperature values</summary><pre>";
  html += historyToString(spaStatusData.temperatureHistory);
  html += "</pre></details></div>";

  html += "<div id=\"statusHeatHistSection\" class=\"history-block status-temp-hist-anchor\">";
  html += "<h3>Heater on-time history</h3>";
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
}

void handleStatus(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());
  String html;
  html.reserve(64000);
  const char *statusStyle =
      "<style>"
      "html{scroll-behavior:smooth;}"
      ".status-page-head{display:flex;flex-wrap:wrap;align-items:baseline;justify-content:space-between;gap:10px 16px;margin:0 0 var(--space-3) 0;}"
      ".status-page-head .status-page-title{margin:0;}"
      ".status-page-title{color:#0f4a87;font-size:1.1rem;line-height:1.3;}"
      ".status-snapshot-meta{margin:0;flex:1 1 220px;text-align:right;font-size:0.82rem;line-height:1.35;color:var(--muted);max-width:46em;}"
      "@media (max-width:520px){.status-snapshot-meta{text-align:left;flex:1 1 100%;}}"
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
      ".kv-dd-with-inline-action{display:flex;flex-wrap:wrap;align-items:center;gap:8px;column-gap:10px;}"
      ".kv-dd-current-temp{flex-wrap:nowrap;}"
      ".kv-dd-current-temp > span{white-space:nowrap;}"
      ".status-temp-chart-link{color:#0f4a87;display:inline-flex;align-items:center;vertical-align:middle;"
      "text-decoration:none;border-radius:6px;padding:3px;line-height:0;}"
      ".status-temp-chart-link:hover{background:#e8f0fa;}"
      ".status-temp-chart-link:focus-visible{outline:2px solid #0f4a87;outline-offset:2px;}"
      ".status-temp-units-toggle{display:inline-flex;align-items:center;gap:0;overflow:hidden;border:1px solid #d4dbe1;border-radius:8px;background:#fff;}"
      ".status-temp-units-toggle button{flex:0 0 auto;min-height:0;padding:1px 5px;border:0;border-right:1px solid #d4dbe1;background:#fff;color:var(--muted);font-size:.62rem;line-height:1;cursor:pointer;}"
      ".status-temp-units-toggle button:last-child{border-right:0;}"
      ".status-temp-units-toggle button:hover{background:#f4f7f8;color:#1f2933;}"
      ".status-temp-units-toggle button:focus-visible{outline:2px solid #0f4a87;outline-offset:2px;position:relative;z-index:1;}"
      ".status-temp-units-toggle button:disabled{background:#0f4a87;color:#fff;cursor:default;opacity:1;}"
      ".heat-panel-head{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:flex-start;gap:10px;margin:0 0 6px 0;}"
      ".heat-panel-head h2{margin:0;}"
      ".heat-hint{font-size:0.82rem;color:var(--muted);margin:0 0 12px 0;line-height:1.45;max-width:52em;}"
      ".heat-hero-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:0 0 12px 0;align-items:stretch;}"
      "@media (max-width:560px){.heat-hero-grid{grid-template-columns:1fr;}#statusHeatHero{order:-1;}}"
      ".heat-hero{display:flex;align-items:center;gap:12px;padding:12px 14px;border-radius:10px;border:1px solid var(--border);margin:0;background:#fafbfc;min-width:0;}"
      ".heat-hero-icon{flex-shrink:0;line-height:0;color:#0f4a87;}"
      ".heat-hero--ok{border-color:#b8cfe8;background:#f2f7fc;}.heat-hero--ok .heat-hero-icon{color:#0f4a87;}"
      ".heat-hero--init{border-color:#e6c200;background:#fffbeb;}.heat-hero--init .heat-hero-icon{color:#b8860b;}"
      ".heat-hero--alert{border-color:#e57373;background:#fff5f5;}.heat-hero--alert .heat-hero-icon{color:#c62828;}"
      ".heat-hero--heat-idle{border-color:#dde2e8;background:#eef1f4;}.heat-hero--heat-idle .heat-hero-icon{color:#5f6c7b;}"
      ".heat-hero--heat-on{border-color:#ffab91;background:#ffe8e0;}.heat-hero--heat-on .heat-hero-icon{color:#bf360c;}"
      ".heat-hero--heat-alt{border-color:#ffe082;background:#fff8e1;}.heat-hero--heat-alt .heat-hero-icon{color:#8d6e00;}"
      ".heat-hero--heat-reserved{border-color:#cfd8dc;background:#eceff1;}.heat-hero--heat-reserved .heat-hero-icon{color:#546e7a;}"
      "#statusHeatHero .heat-hero-icon{color:#d32f2f;}"
      "@keyframes heatHeroPulse{0%,100%{box-shadow:0 0 0 0 rgba(211,47,47,0);}"
      "50%{box-shadow:0 0 22px 8px rgba(211,47,47,0.42);}}"
      ".heat-hero--heat-on,.heat-hero--heat-alt{animation:heatHeroPulse 3.2s ease-in-out infinite;}"
      "@media (prefers-reduced-motion:reduce){.heat-hero--heat-on,.heat-hero--heat-alt{animation:none;}}"
      ".heat-hero-label{font-size:0.78rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:0.03em;}"
      ".heat-hero-val{font-size:1.12rem;font-weight:700;margin-top:2px;line-height:1.25;}"
      ".heat-hero-val--emph{font-size:1.22rem;}"
      ".heat-chips{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 12px 0;align-items:center;}"
      ".heat-chip{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:999px;font-size:0.84rem;font-weight:600;border:1px solid var(--border);background:#fff;}"
      ".heat-chip svg{flex-shrink:0;}"
      ".heat-chip--heat-idle{color:#5f6c7b;background:#eef1f4;border-color:#dde2e8;}"
      ".heat-chip--heat-on{color:#8b2500;background:#ffe8e0;border-color:#ffab91;}"
      ".heat-chip--heat-alt{color:#6d4c00;background:#fff8e1;border-color:#ffe082;}"
      ".heat-chip--need-yes{color:#b71c1c;background:#ffebee;border-color:#ffcdd2;}"
      ".heat-chip--need-no{color:#455a64;background:#eceff1;border-color:#cfd8dc;}"
      ".heat-chip-lbl{font-weight:500;opacity:0.88;margin-right:2px;}"
      "details.heat-raw{margin-top:10px;}details.heat-raw summary{cursor:pointer;font-size:0.84rem;color:var(--muted);font-weight:600;}"
      "details.heat-raw pre{margin:8px 0 0 0;padding:8px 10px;background:#fafbfc;border:1px solid var(--border);border-radius:8px;font-size:0.78rem;line-height:1.5;font-family:ui-monospace,Courier,monospace;}"
      ".equip-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin-top:var(--space-2);}"
      ".equip-cell{border:1px solid var(--border);border-radius:8px;padding:8px 10px 8px 14px;background:#fafbfc;}"
      ".equip-cell--off{background:#f4f6f8;border-color:#dde3e9;box-shadow:inset 4px 0 0 0 #b0bec5;}"
      ".equip-cell--low{background:#fff8e6;border-color:#e6c86a;box-shadow:inset 4px 0 0 0 #e6a000;}"
      ".equip-cell--high,.equip-cell--on{background:#e8f5f0;border-color:#7ebda3;box-shadow:inset 4px 0 0 0 #2e8b6e;}"
      ".equip-cell.equip-absent{opacity:0.58;background:#eef1f4;color:var(--muted);border-color:#dde2e8;box-shadow:inset 4px 0 0 0 #c5ced6;}"
      ".equip-cell.equip-absent .equip-label{color:#7a8794;}"
      ".equip-cell.equip-absent .equip-val{font-weight:500;color:#5f6c7b;}"
      ".equip-label{font-size:0.82rem;color:var(--muted);font-weight:600;}"
      ".equip-val{font-weight:600;margin-top:2px;line-height:1.35;}"
      ".equip-actions{margin-top:8px;}"
      ".equip-btn{background:#0f4a87;color:#fff;border:none;border-radius:6px;padding:6px 10px;cursor:pointer;font-size:.84rem;}"
      ".equip-btn:disabled{opacity:.55;cursor:not-allowed;}"
      ".range-bands{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0;}"
      "@media (max-width:520px){.range-bands{grid-template-columns:1fr;}}"
      ".range-band{border:1px solid var(--border);border-radius:8px;padding:10px 12px;background:#fafbfc;}"
      "button.range-band{display:block;width:100%;margin:0;text-align:start;font:inherit;color:inherit;"
      "appearance:none;-webkit-appearance:none;border-radius:8px;cursor:pointer;transition:border-color .15s,box-shadow .15s,transform .12s;}"
      "button.range-band:not(:disabled):hover{border-color:#8b96a3;box-shadow:0 2px 8px rgba(0,0,0,.08);transform:translateY(-1px);}"
      "button.range-band:not(:disabled):active{transform:translateY(0);box-shadow:0 1px 3px rgba(0,0,0,.06);}"
      "button.range-band:focus-visible{outline:2px solid #0f4a87;outline-offset:2px;z-index:1;position:relative;}"
      "button.range-band:disabled{cursor:default;opacity:1;}"
      ".range-band-active-low{border-color:#0f4a87;background:#e8f0fa;box-shadow:0 0 0 2px rgba(15,74,135,0.12);}"
      ".range-band-active-high{border-color:#b71c1c;background:#fde8e8;box-shadow:0 0 0 2px rgba(183,28,28,0.16);}"
      ".range-band-title{font-size:0.82rem;font-weight:600;color:var(--muted);margin:0 0 6px 0;display:flex;align-items:center;justify-content:space-between;gap:8px;width:100%;}"
      ".range-band-indicator{font-size:0.95rem;line-height:1;color:#8b96a3;}"
      ".range-band-active-low .range-band-indicator,.range-band-active-high .range-band-indicator{color:#1f2933;}"
      ".range-band-temp{font-size:1.15rem;font-weight:700;margin:0;line-height:1.25;display:block;width:100%;}"
      ".range-hint{font-size:0.82rem;color:var(--muted);margin:8px 0 0 0;line-height:1.35;}"
      ".status-control-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px;}"
      ".status-control-row input{border:1px solid var(--border);border-radius:6px;padding:6px 8px;min-width:90px;}"
      ".status-control-result{margin-top:8px;font-size:.84rem;color:var(--muted);}"
      ".status-temp-hist-anchor{scroll-margin-top:16px;}"
      ".history-block{margin-top:var(--space-2);}"
      ".history-block h3{margin:0 0 6px 0;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      ".history-block pre{margin:0;padding:10px;background:#fafbfc;border:1px solid var(--border);border-radius:8px;"
      "font-size:0.8rem;line-height:1.45;overflow-x:auto;white-space:pre-wrap;word-break:break-word;font-family:ui-monospace,Courier,monospace;}"
      ".chart-caption{font-size:0.82rem;color:var(--muted);margin:0 0 6px 0;line-height:1.35;}"
      ".history-block .chart-wrap{max-width:100%;overflow:hidden;}"
      ".history-raw{margin-top:8px;}details.history-raw summary{cursor:pointer;font-size:0.88rem;color:var(--muted);font-weight:600;}"
      "</style>";

  // Materialize `headStatus` into its own `String` before chaining more appends. A single
  // giant `a + b + c + …` expression creates many short-lived temporaries; on embedded
  // targets that has been associated with rare truncated `/status` HTML (missing `<head>` / CSS).
  const String statusHeadClosed = headStatus;
  html = F("<html>");
  html += statusHeadClosed;
  html += String(statusStyle);
  html += F("<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>");
  html += webMenuStatus;
  html += F("<main id='mainContent'>");
  html += ePaper;
  html += F("<div class=\"status-page-head\"><h1 class=\"status-page-title\">Spa Status</h1>"
            "<p class=\"status-snapshot-meta\" id=\"statusSnapshotMeta\" title=\"Last spa status frame applied (gateway local time)\">");
  html += statusSnapshotSubtitle();
  html += F("</p></div><div class=\"status-layout\">");

  {
    float setMin = 50.0f;
    float setMax = 104.0f;
    spaProtocolActiveSetpointBand(setMin, setMax);
    const String setMinStr = String(setMin, spaStatusData.tempScale ? 1 : 0);
    const String setMaxStr = String(setMax, spaStatusData.tempScale ? 1 : 0);
    const bool activeHigh = (spaStatusData.tempRange != 0);
    html += "<section class=\"panel\"><h2>Temperatures</h2><dl class=\"kv\">";
    html += "<div class=\"kv-row\"><dt>Current Temp</dt><dd class=\"kv-dd-with-inline-action kv-dd-current-temp\"><span>";
    html += statusFormattedTempWithUnit(spaStatusData.currentTemp);
    html += "</span><a href=\"#statusTempHistSection\" class=\"status-temp-chart-link\" data-history-anchor=\"statusTempHistSection\" title=\"Jump to temperature chart\" aria-label=\"Jump to temperature chart\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M4 19V5\"/><path d=\"M4 19h16\"/><path d=\"M8 17V9\"/><path d=\"M12 17v-5\"/><path d=\"M16 17V6\"/><path d=\"M20 17v-9\"/></svg></a>";
    html += "<span class=\"status-temp-units-toggle\" role=\"group\" aria-label=\"Temperature units\">";
    html += "<button id=\"statusTempUnitsToggleC\" type=\"button\" onclick=\"statusSendTempUnits('C')\"";
    html += spaStatusData.tempScale ? " disabled" : "";
    html += " title=\"Use Celsius\">C</button>";
    html += "<button id=\"statusTempUnitsToggleF\" type=\"button\" onclick=\"statusSendTempUnits('F')\"";
    html += spaStatusData.tempScale ? "" : " disabled";
    html += " title=\"Use Fahrenheit\">F</button>";
    html += "</span></dd></div>";
    html += "</dl>";
    html += "<div class=\"range-bands\" role=\"group\" aria-label=\"Temperature range setpoints\">";
    html += "<button type=\"button\" id=\"statusBandLow\" data-range-band=\"low\" class=\"range-band";
    html += activeHigh ? "" : " range-band-active-low";
    html += "\" onclick=\"statusSendButton(this)\" data-button=\"80\" data-state=\"off\" aria-label=\"Use low temperature range\" aria-pressed='";
    html += activeHigh ? "false" : "true";
    html += "' title=\"Switch to low range\"";
    html += activeHigh ? "" : " disabled";
    html += "><span class=\"range-band-title\"><span>Low range setpoint</span><span class=\"range-band-indicator\" aria-hidden=\"true\">▼</span></span><span id=\"statusBandLowVal\" class=\"range-band-temp\">";
    html += statusBandStoredSetpointText(spaStatusData.lowSetTemp);
    html += "</span></button><button type=\"button\" id=\"statusBandHigh\" data-range-band=\"high\" class=\"range-band";
    html += activeHigh ? " range-band-active-high" : "";
    html += "\" onclick=\"statusSendButton(this)\" data-button=\"80\" data-state=\"on\" aria-label=\"Use high temperature range\" aria-pressed='";
    html += activeHigh ? "true" : "false";
    html += "' title=\"Switch to high range\"";
    html += activeHigh ? " disabled" : "";
    html += "><span class=\"range-band-title\"><span>High range setpoint</span><span class=\"range-band-indicator\" aria-hidden=\"true\">▲</span></span><span id=\"statusBandHighVal\" class=\"range-band-temp\">";
    html += statusBandStoredSetpointText(spaStatusData.highSetTemp);
    html += "</span></button></div>";
    html += "<p class=\"range-hint\">Click a range above to switch. Set temp applies to the highlighted range only.</p>";
    html += "<div class=\"status-control-row\"><label for=\"statusSetTempInput\" class=\"equip-label\">Set temp <span id=\"statusSetTempScopeLabel\">";
    html += activeHigh ? "(high range)" : "(low range)";
    html += "</span></label>";
    html += "<input id=\"statusSetTempInput\" type=\"number\" min=\"";
    html += setMinStr;
    html += "\" max=\"";
    html += setMaxStr;
    html += "\" step=\"";
    html += (spaStatusData.tempScale ? "0.5" : "1");
    html += "\" value=\"";
    html += String(spaStatusData.setTemp, spaStatusData.tempScale ? 1 : 0);
    html += "\" />";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSendSetTemp()\">Send</button></div>";
    html += "<div id=\"statusSetTempResult\" class=\"status-control-result\"></div>";
    html += "<div id=\"statusTempUnitsResult\" class=\"status-control-result\"></div>";
    html += "</section>";
  }

  {
    const String spaStateTxt = getMapDescription(spaStatusData.spaState, spaStateMap);
    const String initTxt = getMapDescription(spaStatusData.initMode, initModeMap);
    const String heatModeTxt = getMapDescription(spaStatusData.heatingMode, heatingModeMap);
    const String heatStateTxt = getMapDescription(spaStatusData.heatingState, heatingStateMap);
    const uint8_t ss = spaStatusData.spaState;
    const uint8_t im = spaStatusData.initMode;
    const uint8_t hs = spaStatusData.heatingState;
    String heroClass = "heat-hero heat-hero--ok";
    if (im == 2)
    {
      heroClass = "heat-hero heat-hero--alert";
    }
    else if (ss == 1 || im == 1)
    {
      heroClass = "heat-hero heat-hero--init";
    }
    String heatHeroClass = "heat-hero heat-hero--heat-idle";
    if (hs == 1)
    {
      heatHeroClass = "heat-hero heat-hero--heat-on";
    }
    else if (hs == 2)
    {
      heatHeroClass = "heat-hero heat-hero--heat-alt";
    }
    else if (hs == 3)
    {
      heatHeroClass = "heat-hero heat-hero--heat-reserved";
    }
    html += "<section class=\"panel\"><div class=\"heat-panel-head\"><h2>Spa and heating</h2>";
    html += "<a href=\"#statusHeatHistSection\" class=\"status-temp-chart-link\" data-history-anchor=\"statusHeatHistSection\" title=\"Jump to heater on-time chart\" aria-label=\"Jump to heater on-time chart\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M4 19V5\"/><path d=\"M4 19h16\"/><path d=\"M8 17V9\"/><path d=\"M12 17v-5\"/><path d=\"M16 17V6\"/><path d=\"M20 17v-9\"/></svg></a></div>";
    html += "<p class=\"heat-hint\"><b>Heating mode</b> is what the spa is set up for (ready/rest). <b>Heater state</b> is what the heater is doing right now (idle vs actively heating).</p>";
    html += "<div class=\"heat-hero-grid\"><div id=\"statusSpaHero\" class=\"";
    html += heroClass;
    html += "\"><span class=\"heat-hero-icon\" aria-hidden=\"true\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\">";
    html += "<path d=\"M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z\"/></svg></span>";
    html += "<div><div class=\"heat-hero-label\">Spa State</div><div id=\"statusSpaStateHero\" class=\"heat-hero-val\">";
    html += spaStateTxt;
    html += "</div></div></div><div id=\"statusHeatHero\" class=\"";
    html += heatHeroClass;
    html += "\"><span class=\"heat-hero-icon\" aria-hidden=\"true\">";
    html += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"28\" height=\"28\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">";
    html += "<path d=\"M8.5 14.5A2.5 2.5 0 0 0 11 12c0-1.38-.5-2-1-3-1.072-2.143-.224-4.054 2-6 .5 2.5 2 4.9 4 6.5 2 1.6 3 3.5 3 5.5a7 7 0 1 1-14 0c0-1.153.433-2.294 1-3a2.5 2.5 0 0 0 2.5 2.5z\"/></svg></span>";
    html += "<div><div class=\"heat-hero-label\">Heater State</div><div id=\"statusHeatStateHero\" class=\"heat-hero-val heat-hero-val--emph\">";
    html += heatStateTxt;
    html += "</div></div></div></div><dl class=\"kv\">";
    html += "<div class=\"kv-row\"><dt>Init mode</dt><dd id=\"statusInitModeVal\">";
    html += initTxt;
    html += "</dd></div><div class=\"kv-row\"><dt>Heating mode</dt><dd id=\"statusHeatingModeVal\">";
    html += heatModeTxt;
    html += "</dd></div></dl>";
    html += "<details class=\"heat-raw\"><summary>Raw status codes</summary><pre id=\"statusHeatRawPre\">spaState=";
    html += String(spaStatusData.spaState);
    html += " initMode=";
    html += String(spaStatusData.initMode);
    html += " heatingMode=";
    html += String(spaStatusData.heatingMode);
    html += " heatingState=";
    html += String(spaStatusData.heatingState);
    html += " needsHeat=";
    html += String(spaStatusData.needsHeat ? 1 : 0);
    html += "</pre></details></section>";
  }

  html += "<section class=\"panel status-span-full\"><h2>Equipment</h2><div class=\"equip-grid\">";
  appendStatusControlCell(html, "Pump 1", "pump1", statusPumpDisplayState(1), statusPumpConfiguredAbsent(1), 4, statusPumpIsOn(1) ? "off" : "on", statusPumpEquipStateClass(1, statusPumpConfiguredAbsent(1)));
  appendStatusControlCell(html, "Pump 2", "pump2", statusPumpDisplayState(2), statusPumpConfiguredAbsent(2), 5, statusPumpIsOn(2) ? "off" : "on", statusPumpEquipStateClass(2, statusPumpConfiguredAbsent(2)));
  appendStatusControlCell(html, "Pump 3", "pump3", statusPumpDisplayState(3), statusPumpConfiguredAbsent(3), 6, statusPumpIsOn(3) ? "off" : "on", statusPumpEquipStateClass(3, statusPumpConfiguredAbsent(3)));
  appendStatusControlCell(html, "Pump 4", "pump4", statusPumpDisplayState(4), statusPumpConfiguredAbsent(4), 7, statusPumpIsOn(4) ? "off" : "on", statusPumpEquipStateClass(4, statusPumpConfiguredAbsent(4)));
  appendStatusControlCell(html, "Pump 5", "pump5", statusPumpDisplayState(5), statusPumpConfiguredAbsent(5), 8, statusPumpIsOn(5) ? "off" : "on", statusPumpEquipStateClass(5, statusPumpConfiguredAbsent(5)));
  appendStatusControlCell(html, "Pump 6", "pump6", statusPumpDisplayState(6), statusPumpConfiguredAbsent(6), 9, statusPumpIsOn(6) ? "off" : "on", statusPumpEquipStateClass(6, statusPumpConfiguredAbsent(6)));
  appendStatusControlCell(html, "Circulation Pump", "circ", getMapDescription(spaStatusData.circ, onOffMap), statusCircConfiguredAbsent(), 0, nullptr, statusBinaryEquipStateClass(statusCircConfiguredAbsent(), spaStatusData.circ != 0));
  appendStatusControlCell(html, "Blower", "blower", getMapDescription(spaStatusData.blower, onOffMap), statusBlowerConfiguredAbsent(), 12, spaStatusData.blower == 0 ? "on" : "off", statusBinaryEquipStateClass(statusBlowerConfiguredAbsent(), spaStatusData.blower != 0));
  appendStatusControlCell(html, "Light 1", "light1", getMapDescription(spaStatusData.light1, onOffMap), statusLightConfiguredAbsent(1), 17, spaStatusData.light1 ? "off" : "on", statusBinaryEquipStateClass(statusLightConfiguredAbsent(1), spaStatusData.light1 != 0));
  appendStatusControlCell(html, "Light 2", "light2", getMapDescription(spaStatusData.light2, onOffMap), statusLightConfiguredAbsent(2), 18, spaStatusData.light2 ? "off" : "on", statusBinaryEquipStateClass(statusLightConfiguredAbsent(2), spaStatusData.light2 != 0));
  appendStatusControlCell(html, "Mister", "mister", getMapDescription(spaStatusData.mister, onOffMap), statusMisterConfiguredAbsent(), 14, spaStatusData.mister ? "off" : "on", statusBinaryEquipStateClass(statusMisterConfiguredAbsent(), spaStatusData.mister != 0));
  html += "</div><div id=\"statusButtonResult\" class=\"status-control-result\"></div></section>";

  {
    String clockRawTitle = String("Raw status flag (status byte 9 & 0x02): ") + String(spaStatusData.clockMode);
    html += "<section class=\"panel\"><h2>Panel clock and filter cycles</h2>";
    html += "<p class=\"chart-caption\">Times are the <b>spa panel clock</b> from RS485 status (not the ESP clock on <a href='/state'>/state</a>). "
            "<b>Panel clock format</b> is how the physical panel shows time (12h vs 24h). "
            "<b>Filter cycle (status)</b> is which programmed daily filter window the controller reports as active; schedule start/duration is on <a href='/config'>/config</a>.</p>";
    html += "<dl class=\"kv\">";
    appendStatusKvRow(html, "Panel time", String(spaStatusData.time), "statusPanelTimeVal", nullptr);
    html += "<div class=\"kv-row\"><dt>Panel clock format</dt><dd class=\"kv-dd-with-inline-action\"><span id=\"statusClockFormatVal\" title=\"";
    html += clockRawTitle;
    html += "\">";
    html += statusPanelClockFormatLabel(spaStatusData.clockMode);
    html += "</span><span class=\"status-temp-units-toggle\" role=\"group\" aria-label=\"Panel clock format\">";
    html += "<button id=\"statusClockFormat12Btn\" type=\"button\" onclick=\"statusSendTimeFormat(12)\"";
    html += (spaStatusData.clockMode & 0x02) ? "" : " disabled";
    html += " title=\"Use 12-hour clock\">12</button>";
    html += "<button id=\"statusClockFormat24Btn\" type=\"button\" onclick=\"statusSendTimeFormat(24)\"";
    html += (spaStatusData.clockMode & 0x02) ? " disabled" : "";
    html += " title=\"Use 24-hour clock\">24</button>";
    html += "</span></dd></div>";
    appendStatusKvRow(html, "Filter cycle (status)", String(getMapDescription(spaStatusData.filterMode, filterModeMap)), "statusFilterModeVal", nullptr);
    html += "</dl>";
    html += "<p class=\"range-hint\" style=\"margin-top:10px\">Set panel clock sends Balboa <code>0x21</code> using the current 12h/24h format flag from status.</p>";
    html += "<div class=\"status-control-row\"><label for=\"statusPanelTimeInput\" class=\"equip-label\">Set panel time</label>";
    html += "<input id=\"statusPanelTimeInput\" type=\"time\" step=\"60\" value=\"";
    html += String(spaStatusData.time);
    html += "\" />";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSendPanelTime()\">Send to spa</button>";
    html += "<button class=\"equip-btn\" type=\"button\" onclick=\"statusSyncPanelTimeFromGateway()\">Sync from gateway</button></div>";
    html += "<div id=\"statusSystemTimeResult\" class=\"status-control-result\"></div>";
    html += "<div id=\"statusTimeFormatResult\" class=\"status-control-result\"></div></section>";
  }

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
  html += "<p class=\"chart-caption\">Load this on demand to keep first render fast on weak Wi-Fi.</p>";
  html += "<button class=\"equip-btn\" type=\"button\" id=\"statusLoadHistoriesBtn\">Load history charts</button>";
  html += "<div id=\"statusHistoriesContainer\"></div>";
  html += "</section>";

  html += "<script>"
          "async function statusSendSci(payload){"
          "const body='<sci_request version=\"1.0\"><data_service><targets><device id=\"00 11 22 33 44 55 66 77\"/></targets><requests>'+payload+'</requests></data_service></sci_request>';"
          "const r=await fetch('/devices/sci',{method:'POST',headers:{'Content-Type':'application/xml'},body});"
          "return await r.text();"
          "}"
          "function statusBackoffMs(base,max,fails){var e=Math.min(max,base*Math.pow(2,Math.min(6,fails)));var j=Math.floor(Math.random()*Math.max(250,Math.floor(e*0.35)));return Math.min(max,e+j);}"
          "async function statusFetchJson(url,timeoutMs){const ctl=new AbortController();const t=setTimeout(function(){ctl.abort();},timeoutMs||5000);"
          "try{const r=await fetch(url,{cache:'no-store',signal:ctl.signal});if(!r.ok)throw new Error('http_'+r.status);return await r.json();}finally{clearTimeout(t);}}"
          "async function statusFetchControls(){return await statusFetchJson('/api/status/summary',4200);}"
          "function statusScaleHeat(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/60);return b;}"
          "function statusScaleFilter(a){var b=[],i;for(i=0;i<a.length;i++)b.push(a[i]/3600);return b;}"
          "function statusDrawLineChart(id,data,isTemp,isCelsius){"
          "var c=document.getElementById(id);if(!c||!data||data.length<1)return;"
          "var ctx=c.getContext('2d');if(!ctx)return;"
          "function fmtY(v){if(isTemp)return isCelsius?Number(v).toFixed(1):String(Math.round(Number(v)));return Number(v).toFixed(2);}"
          "function draw(W,H){"
          "ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);ctx.strokeStyle='#ccc';ctx.strokeRect(0.5,0.5,W-1,H-1);"
          "var lo=Infinity,hi=-Infinity,i,v;"
          "for(i=0;i<data.length;i++){v=Number(data[i]);if(!isFinite(v))continue;if(v<lo)lo=v;if(v>hi)hi=v;}"
          "if(!isFinite(lo)||!isFinite(hi)){ctx.fillStyle='#333';ctx.font='12px sans-serif';ctx.fillText('No data',10,H/2);return;}"
          "if(hi-lo<1e-6){lo-=0.5;hi+=0.5;}"
          "var pad=30,pw=W-2*pad,ph=H-22;"
          "function yOf(val){return 14+ph-(val-lo)/(hi-lo)*ph;}"
          "ctx.fillStyle='#333';ctx.font='11px sans-serif';ctx.fillText(fmtY(lo),4,H-6);ctx.fillText(fmtY(hi),4,12);"
          "ctx.strokeStyle='#037e52';ctx.lineWidth=1.5;ctx.beginPath();"
          "for(i=0;i<data.length;i++){var px=pad+i*pw/Math.max(1,data.length-1);var py=yOf(Number(data[i]));if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py);}"
          "ctx.stroke();}"
          "function render(){var p=c.parentElement;var pg=document.querySelector('.page');"
          "var cap=(pg&&pg.clientWidth)?pg.clientWidth:((document.documentElement&&document.documentElement.clientWidth)||window.innerWidth||980);"
          "var raw=p?p.clientWidth-2:280;if(raw<0)raw=0;"
          "var W=Math.min(1000,cap,Math.max(260,raw));var H=parseInt(c.getAttribute('height')||'140',10)||140;"
          "H=Math.min(260,H);"
          "var dpr=Math.min(2,Math.max(1,window.devicePixelRatio||1));"
          "c.width=Math.round(W*dpr);c.height=Math.round(H*dpr);c.style.width=W+'px';c.style.height=H+'px';"
          "ctx.setTransform(1,0,0,1,0,0);ctx.scale(dpr,dpr);draw(W,H);}"
          "var raf=0;function scheduleRender(){if(raf)return;raf=window.requestAnimationFrame(function(){raf=0;render();});}"
          "window.addEventListener('resize',scheduleRender,{passive:true});window.addEventListener('orientationchange',scheduleRender,{passive:true});"
          "c.addEventListener('webglcontextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});"
          "c.addEventListener('contextlost',function(e){if(e&&e.preventDefault)e.preventDefault();},{passive:false});"
          "c.addEventListener('contextrestored',scheduleRender,{passive:true});"
          "scheduleRender();}"
          "function statusRenderHistoryCharts(j){"
          "if(!j)return;var tc=!!j.tempIsCelsius;"
          "statusDrawLineChart('statusTempHistChart',j.tempHistory||[],true,tc);"
          "statusDrawLineChart('statusHeatHistChart',statusScaleHeat(j.heatSeconds||[]),false,false);"
          "statusDrawLineChart('statusFilterHistChart',statusScaleFilter(j.filterSeconds||[]),false,false);}"
          "function statusScrollToHistoryAnchor(id){var el=document.getElementById(id);if(el)el.scrollIntoView({behavior:'smooth',block:'start'});}"
          "let statusHistoriesLoading=false;"
          "async function statusLoadHistories(opt){"
          "opt=opt||{};var scrollTo=opt.scrollTo||'';"
          "var c=document.getElementById('statusHistoriesContainer');var btn=document.getElementById('statusLoadHistoriesBtn');"
          "if(!c)return false;"
          "if(c.getAttribute('data-loaded')==='1'){if(scrollTo)statusScrollToHistoryAnchor(scrollTo);return true;}"
          "if(statusHistoriesLoading)return false;"
          "statusHistoriesLoading=true;if(btn){btn.disabled=true;btn.textContent='Loading...';}"
          "try{var j=await statusFetchJson('/api/status/histories',9000);"
          "c.innerHTML=(j&&j.html)||'';statusRenderHistoryCharts(j);c.setAttribute('data-loaded','1');"
          "if(btn)btn.textContent='History loaded';if(scrollTo)statusScrollToHistoryAnchor(scrollTo);"
          "statusHistoriesLoading=false;return true;"
          "}catch(e){if(btn){btn.disabled=false;btn.textContent='Retry history load';}statusHistoriesLoading=false;return false;}}"
          "function statusEquipCell(key){return document.querySelector('[data-equip=\"'+key+'\"]');}"
          "function statusSetEquipValue(key,text){var c=statusEquipCell(key);if(!c)return;var v=c.querySelector('[data-role=\"value\"]');if(v)v.textContent=text;}"
          "function statusSetEquipStateClass(key,state){var c=statusEquipCell(key);if(!c||c.classList.contains('equip-absent'))return;"
          "var states=['equip-cell--off','equip-cell--low','equip-cell--high','equip-cell--on'];for(var i=0;i<states.length;i++)c.classList.remove(states[i]);"
          "if(state==='off'||state==='low'||state==='high'||state==='on')c.classList.add('equip-cell--'+state);}"
          "function statusPumpVisualState(snap,num){var cfg=Number(snap['pump'+num+'Config']||0);if(cfg===1)return snap['pump'+num+'On']?'on':'off';"
          "var raw=Number(snap['pump'+num]||0);if(raw===0)return 'off';if(raw===1)return 'low';return 'high';}"
          "function statusBinaryEquipFromSnap(snap,key){if(typeof snap[key]==='undefined')return 'off';return Number(snap[key])>0?'on':'off';}"
          "function statusSetButtonState(code,desired){var btn=document.querySelector('button[data-button=\"'+code+'\"]');if(!btn)return;"
          "btn.setAttribute('data-state',desired);btn.textContent='Turn '+(desired==='on'?'On':'Off');}"
          "function statusPumpDisplay(raw){if(raw===0)return 'Off';if(raw===1)return 'Low';if(raw===2)return 'High';return String(raw);}"
          "function statusOnOff(v){return Number(v)>0?'On':'Off';}"
          "function statusPumpUiValue(snap,num){var cfg=Number(snap['pump'+num+'Config']||0);if(cfg===1)return statusOnOff(snap['pump'+num+'On']);return statusPumpDisplay(Number(snap['pump'+num]||0));}"
          "function statusFormatBandTemp(snap,v){var n=Number(v);if(!isFinite(n)||n<=0)return'\xe2\x80\x94';var c=!!snap.tempScaleCelsius;"
          "if(c)return n.toFixed(1)+'\\u00b0C';return String(Math.round(n))+'\\u00b0F';}"
          "function statusApplySnapshotMeta(snap){var el=document.getElementById('statusSnapshotMeta');if(!el||!snap)return;"
          "if(typeof snap.snapshotMeta==='string'){el.textContent=snap.snapshotMeta;"
          "if(typeof snap.snapshotAtLocal==='string'&&snap.snapshotAtLocal.length)el.title=snap.snapshotAtLocal;}}"
          "function statusApplyHeatingSnap(snap){"
          "if(typeof snap.spaStateText==='undefined')return;"
          "var hero=document.getElementById('statusSpaStateHero');if(hero)hero.textContent=snap.spaStateText;"
          "var heroEl=document.getElementById('statusSpaHero');"
          "if(heroEl){heroEl.className='heat-hero';var im=Number(snap.initMode||0);var ss=Number(snap.spaState||0);"
          "if(im===2)heroEl.classList.add('heat-hero--alert');else if(ss===1||im===1)heroEl.classList.add('heat-hero--init');else heroEl.classList.add('heat-hero--ok');}"
          "var hsn=Number(snap.heatingState||0);var hth=document.getElementById('statusHeatStateHero');if(hth)hth.textContent=snap.heatingStateText||'';"
          "var hhero=document.getElementById('statusHeatHero');if(hhero){hhero.className='heat-hero';"
          "if(hsn===1)hhero.classList.add('heat-hero--heat-on');else if(hsn===2)hhero.classList.add('heat-hero--heat-alt');else if(hsn===3)hhero.classList.add('heat-hero--heat-reserved');else hhero.classList.add('heat-hero--heat-idle');}"
          "var imv=document.getElementById('statusInitModeVal');if(imv)imv.textContent=snap.initModeText||'';"
          "var hmv=document.getElementById('statusHeatingModeVal');if(hmv)hmv.textContent=snap.heatingModeText||'';"
          "var raw=document.getElementById('statusHeatRawPre');if(raw)raw.textContent='spaState='+snap.spaState+' initMode='+snap.initMode+' heatingMode='+snap.heatingMode+' heatingState='+snap.heatingState+' needsHeat='+snap.needsHeat;"
          "}"
          "function statusApplySnapshot(snap){"
          "if(!snap)return;"
          "statusSetEquipValue('pump1',statusPumpUiValue(snap,1));statusSetEquipStateClass('pump1',statusPumpVisualState(snap,1));"
          "statusSetEquipValue('pump2',statusPumpUiValue(snap,2));statusSetEquipStateClass('pump2',statusPumpVisualState(snap,2));"
          "statusSetEquipValue('pump3',statusPumpUiValue(snap,3));statusSetEquipStateClass('pump3',statusPumpVisualState(snap,3));"
          "statusSetEquipValue('pump4',statusPumpUiValue(snap,4));statusSetEquipStateClass('pump4',statusPumpVisualState(snap,4));"
          "statusSetEquipValue('pump5',statusPumpUiValue(snap,5));statusSetEquipStateClass('pump5',statusPumpVisualState(snap,5));"
          "statusSetEquipValue('pump6',statusPumpUiValue(snap,6));statusSetEquipStateClass('pump6',statusPumpVisualState(snap,6));"
          "if(typeof snap.circ!=='undefined'){statusSetEquipValue('circ',statusOnOff(snap.circ));statusSetEquipStateClass('circ',statusBinaryEquipFromSnap(snap,'circ'));}"
          "statusSetEquipValue('blower',statusOnOff(snap.blower));statusSetEquipStateClass('blower',statusBinaryEquipFromSnap(snap,'blower'));"
          "statusSetEquipValue('light1',statusOnOff(snap.light1));statusSetEquipStateClass('light1',statusBinaryEquipFromSnap(snap,'light1'));"
          "statusSetEquipValue('light2',statusOnOff(snap.light2));statusSetEquipStateClass('light2',statusBinaryEquipFromSnap(snap,'light2'));"
          "statusSetEquipValue('mister',statusOnOff(snap.mister));statusSetEquipStateClass('mister',statusBinaryEquipFromSnap(snap,'mister'));"
          "statusSetButtonState(4,snap.pump1On?'off':'on');"
          "statusSetButtonState(5,snap.pump2On?'off':'on');"
          "statusSetButtonState(6,snap.pump3On?'off':'on');"
          "statusSetButtonState(7,snap.pump4On?'off':'on');"
          "statusSetButtonState(8,snap.pump5On?'off':'on');"
          "statusSetButtonState(9,snap.pump6On?'off':'on');"
          "statusSetButtonState(12,Number(snap.blower)>0?'off':'on');"
          "statusSetButtonState(17,Number(snap.light1)>0?'off':'on');"
          "statusSetButtonState(18,Number(snap.light2)>0?'off':'on');"
          "statusSetButtonState(14,Number(snap.mister)>0?'off':'on');"
          "var tr=Number(snap.tempRange||0);var hi=document.getElementById('statusBandHigh');var lo=document.getElementById('statusBandLow');"
          "if(hi){hi.classList.toggle('range-band-active-high',tr===1);hi.classList.remove('range-band-active-low');}"
          "if(lo){lo.classList.toggle('range-band-active-low',tr===0);lo.classList.remove('range-band-active-high');}"
          "var hv=document.getElementById('statusBandHighVal');var lv=document.getElementById('statusBandLowVal');"
          "if(hv)hv.textContent=statusFormatBandTemp(snap,snap.highSetTemp);if(lv)lv.textContent=statusFormatBandTemp(snap,snap.lowSetTemp);"
          "var lbl=document.getElementById('statusSetTempScopeLabel');if(lbl)lbl.textContent=tr===1?'(high range)':'(low range)';"
          "if(lo){lo.disabled=(tr===0);lo.setAttribute('aria-pressed',tr===0?'true':'false');}"
          "if(hi){hi.disabled=(tr===1);hi.setAttribute('aria-pressed',tr===1?'true':'false');}"
          "var setInput=document.getElementById('statusSetTempInput');"
          "if(setInput){if(typeof snap.setTempMin!=='undefined')setInput.min=String(snap.setTempMin);if(typeof snap.setTempMax!=='undefined')setInput.max=String(snap.setTempMax);"
          "if(typeof snap.tempScaleCelsius!=='undefined')setInput.step=snap.tempScaleCelsius?'0.5':'1';"
          "if(document.activeElement!==setInput&&typeof snap.setTemp!=='undefined')setInput.value=String(snap.tempScaleCelsius?Number(snap.setTemp).toFixed(1):Math.round(Number(snap.setTemp)));}"
          "var uC=!!snap.tempScaleCelsius;var cBtn=document.getElementById('statusTempUnitsToggleC');var fBtn=document.getElementById('statusTempUnitsToggleF');"
          "if(cBtn)cBtn.disabled=uC;if(fBtn)fBtn.disabled=!uC;"
          "var pt=document.getElementById('statusPanelTimeVal');if(pt&&typeof snap.panelTime==='string')pt.textContent=snap.panelTime;"
          "var cf=document.getElementById('statusClockFormatVal');if(cf&&typeof snap.clockFormat==='string'){cf.textContent=snap.clockFormat;if(typeof snap.clockModeRaw!=='undefined')cf.title='Raw status flag (status byte 9 & 0x02): '+snap.clockModeRaw;}"
          "var f12=document.getElementById('statusClockFormat12Btn');var f24=document.getElementById('statusClockFormat24Btn');var is24=String(snap.clockFormat||'').toLowerCase().indexOf('24')>=0;"
          "if(f12)f12.disabled=!is24;if(f24)f24.disabled=is24;"
          "var fm=document.getElementById('statusFilterModeVal');if(fm&&typeof snap.filterModeText==='string')fm.textContent=snap.filterModeText;"
          "var tIn=document.getElementById('statusPanelTimeInput');if(tIn&&document.activeElement!==tIn&&typeof snap.panelTime==='string')tIn.value=snap.panelTime;"
          "statusApplyHeatingSnap(snap);statusApplySnapshotMeta(snap);"
          "}"
          "let statusPollTimer=0;let statusPollBusy=false;let statusPollFailures=0;let statusLastSnapshotAgeSec=0;const statusPollBaseMs=2000;const statusPollMaxMs=25000;const statusFlakyFailThreshold=3;const statusStaleAgeSecThreshold=10;"
          "function statusConnState(msg){var el=document.getElementById('statusButtonResult');if(el&&msg)el.textContent=msg;}"
          "function statusShouldShowFlaky(){if(statusPollFailures<statusFlakyFailThreshold)return false;if(statusLastSnapshotAgeSec===0)return true;return statusLastSnapshotAgeSec>=statusStaleAgeSecThreshold;}"
          "function statusSchedulePoll(ms){statusStopPolling();statusPollTimer=setTimeout(statusPollOnce,Math.max(250,ms||statusPollBaseMs));}"
          "async function statusPollOnce(){"
          "if(statusPollBusy||document.hidden)return;"
          "statusPollBusy=true;"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);statusLastSnapshotAgeSec=Number(snap&&snap.snapshotAgeSec||0);statusPollFailures=0;statusConnState('');statusSchedulePoll(statusPollBaseMs);}catch(e){statusPollFailures++;if(statusShouldShowFlaky())statusConnState('Connection is flaky, showing last known values...');statusSchedulePoll(statusBackoffMs(statusPollBaseMs,statusPollMaxMs,statusPollFailures));}"
          "statusPollBusy=false;"
          "}"
          "function statusStartPolling(){if(statusPollTimer||statusPollBusy)return;statusPollFailures=0;statusPollOnce();}"
          "function statusStopPolling(){if(!statusPollTimer)return;clearTimeout(statusPollTimer);statusPollTimer=0;}"
          "document.addEventListener('visibilitychange',function(){if(document.hidden){statusStopPolling();}else{statusStartPolling();}});"
          "window.addEventListener('beforeunload',statusStopPolling);"
          "document.getElementById('statusLoadHistoriesBtn').addEventListener('click',function(){statusLoadHistories({});});"
          "document.addEventListener('click',function(e){"
          "var t=e.target&&e.target.closest?e.target.closest('a.status-temp-chart-link[data-history-anchor]'):null;"
          "if(!t)return;var anchor=t.getAttribute('data-history-anchor');if(!anchor)return;"
          "var hc=document.getElementById('statusHistoriesContainer');"
          "if(!hc||hc.getAttribute('data-loaded')==='1')return;"
          "e.preventDefault();statusLoadHistories({scrollTo:anchor});});"
          "statusStartPolling();"
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
          "if(code===80)return on?(Number(snap.tempRange||0)===1):(Number(snap.tempRange||0)===0);"
          "return false;}"
          "async function statusWaitForButtonState(code,desired){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(statusButtonMatch(snap,code,desired))return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForSetTemp(target){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(Math.abs(Number(snap.setTemp)-Number(target))<0.26)return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForTempUnits(units){"
          "var wantC=(String(units||'').toUpperCase()==='C');"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(!!snap.tempScaleCelsius===wantC)return true;}catch(e){}}"
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
          "var pv=parseFloat(v);var mn=parseFloat(input.min);var mx=parseFloat(input.max);"
          "if(isFinite(pv)&&isFinite(mn)&&isFinite(mx)&&(pv<mn-1e-6||pv>mx+1e-6)){statusSetResult('statusSetTempResult','Enter a value between '+mn+' and '+mx+' for the active range.');return;}"
          "try{const xml='<device_request target_name=\"SetTemp\">'+v+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSetTempResult','SetTemp response: '+out);return;}"
          "statusSetResult('statusSetTempResult','SetTemp accepted; waiting for spa status update...');"
          "const changed=await statusWaitForSetTemp(v);"
          "if(changed){statusSetResult('statusSetTempResult','SetTemp accepted and state changed.');setTimeout(function(){location.reload();},500);}else{statusSetResult('statusSetTempResult','SetTemp accepted, but setpoint did not change yet.');}"
          "}catch(e){statusSetResult('statusSetTempResult','SetTemp failed: '+e);}"
          "}"
          "async function statusSendTempUnits(units){"
          "var t=String(units||'').toUpperCase();if(t!=='C'&&t!=='F'){statusSetResult('statusTempUnitsResult','Invalid temp units request.');return;}"
          "if(!confirm('Change temperature units to '+(t==='C'?'Celsius':'Fahrenheit')+'?')){statusSetResult('statusTempUnitsResult','Temperature units change canceled.');return;}"
          "var cBtn=document.getElementById('statusTempUnitsToggleC');var fBtn=document.getElementById('statusTempUnitsToggleF');"
          "try{if(cBtn)cBtn.disabled=true;if(fBtn)fBtn.disabled=true;"
          "const xml='<device_request target_name=\"TempUnits\">'+t+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusTempUnitsResult','TempUnits response: '+out);return;}"
          "statusSetResult('statusTempUnitsResult','TempUnits accepted; waiting for spa status update...');"
          "const changed=await statusWaitForTempUnits(t);"
          "if(changed){statusSetResult('statusTempUnitsResult','Temperature units updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusTempUnitsResult','Command accepted; temperature units did not update yet.');}}"
          "catch(e){statusSetResult('statusTempUnitsResult','TempUnits failed: '+e);}finally{"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);}catch(_e){}"
          "if(cBtn)cBtn.disabled=false;if(fBtn)fBtn.disabled=false;}"
          "}"
          "async function statusWaitForPanelTime(target){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();if(String(snap.panelTime||'')===String(target))return true;}catch(e){}}"
          "return false;}"
          "async function statusWaitForTimeFormat(use24){"
          "for(var i=0;i<10;i++){await new Promise(function(res){setTimeout(res,650);});"
          "try{var snap=await statusFetchControls();var is24=String(snap.clockFormat||'').toLowerCase().indexOf('24')>=0;if(is24===!!use24)return true;}catch(e){}}"
          "return false;}"
          "async function statusSendTimeFormat(fmt){"
          "var f=Number(fmt);if(f!==12&&f!==24){statusSetResult('statusTimeFormatResult','Invalid time format request.');return;}"
          "if(!confirm('Change panel clock format to '+f+'-hour?')){statusSetResult('statusTimeFormatResult','Time format change canceled.');return;}"
          "var f12=document.getElementById('statusClockFormat12Btn');var f24=document.getElementById('statusClockFormat24Btn');"
          "try{if(f12)f12.disabled=true;if(f24)f24.disabled=true;"
          "const xml='<device_request target_name=\"TimeFormat\">'+String(f)+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusTimeFormatResult','TimeFormat response: '+out);return;}"
          "statusSetResult('statusTimeFormatResult','TimeFormat accepted; waiting for spa...');"
          "const changed=await statusWaitForTimeFormat(f===24);"
          "if(changed){statusSetResult('statusTimeFormatResult','Panel clock format updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusTimeFormatResult','Command accepted; panel clock format did not update yet.');}"
          "}catch(e){statusSetResult('statusTimeFormatResult','TimeFormat failed: '+e);}finally{"
          "try{var snap=await statusFetchControls();statusApplySnapshot(snap);}catch(_e){}"
          "if(f12)f12.disabled=false;if(f24)f24.disabled=false;}"
          "}"
          "async function statusSendPanelTime(){"
          "var input=document.getElementById('statusPanelTimeInput');if(!input)return;var v=(input.value||'').trim();"
          "if(!/^\\d{1,2}:\\d{2}$/.test(v)){statusSetResult('statusSystemTimeResult','Enter a valid time (HH:MM).');return;}"
          "var p=v.split(':');var hh=String(Number(p[0])||0);var mm=String(p[1]||'00');if(mm.length===1)mm='0'+mm;if(hh.length===1)hh='0'+hh;v=hh+':'+mm;"
          "try{const xml='<device_request target_name=\"SystemTime\">'+v+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSystemTimeResult','SystemTime response: '+out);return;}"
          "statusSetResult('statusSystemTimeResult','SystemTime accepted; waiting for spa...');"
          "const changed=await statusWaitForPanelTime(v);"
          "if(changed){statusSetResult('statusSystemTimeResult','Panel time updated.');statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusSystemTimeResult','Command accepted; panel time did not match yet.');}"
          "}catch(e){statusSetResult('statusSystemTimeResult','SystemTime failed: '+e);}"
          "}"
          "async function statusSyncPanelTimeFromGateway(){"
          "try{var snap=await statusFetchControls();var gt=snap.gatewayTimeHHMM;if(!gt||gt==='--:--'){statusSetResult('statusSystemTimeResult','Gateway clock not available (sync ESP time / NTP).');return;}"
          "const xml='<device_request target_name=\"SystemTime\">'+gt+'</device_request>';"
          "const out=await statusSendSci(xml);if(out.indexOf('result=\\'accepted\\'')<0){statusSetResult('statusSystemTimeResult','SystemTime response: '+out);return;}"
          "statusSetResult('statusSystemTimeResult','Sync sent; waiting for spa...');"
          "const changed=await statusWaitForPanelTime(gt);"
          "if(changed){statusSetResult('statusSystemTimeResult','Panel time synced from gateway.');var inp=document.getElementById('statusPanelTimeInput');if(inp)inp.value=gt;statusApplySnapshot(await statusFetchControls());}"
          "else{statusSetResult('statusSystemTimeResult','Command accepted; panel time did not match yet.');}"
          "}catch(e){statusSetResult('statusSystemTimeResult','Sync failed: '+e);}"
          "}"
          "</script>";
  html += "</div></main></div></body></html>";
  String etag = String("W/\"status-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaStatusData.lastUpdate) + "-" + String(spaConfigurationData.lastUpdate) + "\"";
  const size_t statusOutLen = html.length();
  if (!html.startsWith("<html>"))
  {
    Log.error("[Web]: /status assemble missing <html> prefix len=%u from %p" CR,
              static_cast<unsigned>(statusOutLen), request->client()->remoteIP());
  }
  sendHtmlWithEtag(request, html, etag);
  // Never log full `html` here: /status payload is ~40KB+; printf-style verbose would blow stack/heap.
  // `html` is moved into the response callback; log length captured before `sendHtmlWithEtag`.
  Log.verbose(F("[Web]: /status sent len=%u" CR), static_cast<unsigned>(statusOutLen));
}

void handleConfig(AsyncWebServerRequest *request)
{
  // Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());

  String html;
  html.reserve(24000);
  html = "<html>" + headConfig + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuConfig + "<main id='mainContent'>" + ePaper + "<section class='panel'><h1>Spa Configuration</h1><ul>";
  if (spaConfigurationData.lastUpdate == 0)
  {
    html += "<li><b>Spa Configuration not available</b></li>";
    html += "</ul></section>";
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
    html += "</ul></section><section class='panel'><h1>LittleFS Configuration</h1>";
    html += "<p class='chart-caption'>Load on demand to avoid large payloads on weak links.</p>";
    html += "<button class='equip-btn' type='button' id='cfgLoadLittleFsBtn'>Load LittleFS file list</button>";
    html += "<ul id='cfgLittleFsContainer'></ul>";
    // Add more fields as needed
    html += "</section><script>(function(){var btn=document.getElementById('cfgLoadLittleFsBtn');var box=document.getElementById('cfgLittleFsContainer');if(!btn||!box)return;"
            "btn.addEventListener('click',function(){if(btn.disabled)return;btn.disabled=true;btn.textContent='Loading...';"
            "fetch('/api/state/littlefs',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){"
            "box.innerHTML='<li>'+(j&&j.html?j.html:'(empty)')+'</li>';btn.textContent='LittleFS loaded';}).catch(function(){btn.disabled=false;btn.textContent='Retry LittleFS load';});});})();</script>";
  }
  html += "</main></div></body></html>";
  String etag = String("W/\"cfg-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaConfigurationData.lastUpdate) + "-" + String(spaFilterSettingsData.lastUpdate) + "\"";
  sendHtmlWithEtag(request, html, etag);
  // Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
  Log.verbose("[Web]: handleConfig %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
}

time_t testLastCheckedTime = getTime();

void handleState(AsyncWebServerRequest *request)
{
  // Log.verbose(F("[Web]: handleStatus()" CR));
  String stateEnhancements = "<style>.state-grid{display:grid;grid-template-columns:1fr;gap:14px;}@media (min-width:980px){.state-grid{grid-template-columns:1fr 1fr;}.state-grid .panel{margin-bottom:0;}}.diag-badge{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.88rem;}.state-toolbar{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin:0 0 10px 0;}.state-freshness{width:100%;border-collapse:collapse;margin-top:8px;}.state-freshness th,.state-freshness td{padding:8px;border-bottom:1px solid var(--border);text-align:left;vertical-align:top;}.state-freshness th{font-size:13px;color:var(--muted);}body .advanced-panel{display:none;}body.show-advanced .advanced-panel{display:block;}body .advanced-only{display:none;}body.show-advanced .advanced-only{display:list-item;}button.fw-check-btn{background:var(--panel)!important;color:var(--text)!important;border:1px solid var(--border)!important;flex:0 0 auto!important;width:auto!important;min-width:auto!important;padding:8px 14px!important;font-size:14px!important;font-weight:600!important;}#fwUpdateResult.fw-update-msg{display:block;width:100%;max-width:100%;margin:0;font-size:14px;font-weight:600;line-height:1.35;color:var(--muted);}.fw-compare{display:flex;flex-direction:column;gap:12px;margin:0;}.fw-compare-cols{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:start;}@media (max-width:420px){.fw-compare-cols{grid-template-columns:1fr;}}.fw-compare-item{display:flex;flex-direction:column;gap:6px;min-width:0;}.fw-compare-label{font-size:12px;font-weight:600;color:var(--muted);letter-spacing:.02em;}.fw-actions{display:flex;flex-wrap:nowrap;align-items:center;gap:10px;width:100%;box-sizing:border-box;overflow-x:auto;-webkit-overflow-scrolling:touch;}.fw-actions .fw-check-btn{flex:0 0 auto;}.fw-actions .gh-sponsor-embed{flex:0 0 auto;flex-shrink:0;line-height:0;align-self:center;}.fw-pill{display:inline-block;border-radius:999px;padding:3px 10px;font-size:12px;font-weight:700;line-height:1.2;border:1px solid var(--border);background:#f3f4f6;color:#374151;}.fw-pill-current{background:#edf7ff;color:#0f4a87;border-color:#b7d6f2;}.fw-pill-latest{background:#f7f7f7;color:#4b5563;}.sub-card{border:1px solid var(--border);background:#f8fafc;border-radius:10px;padding:10px 12px;margin:8px 0;}.sub-card-title{font-size:13px;font-weight:700;letter-spacing:.01em;color:var(--muted);text-transform:uppercase;margin:0 0 8px 0;}.sub-card-row{display:flex;flex-wrap:wrap;align-items:center;gap:10px;}.gh-sponsor-embed iframe{display:block;border:0;border-radius:6px;vertical-align:middle;}</style>";
  String html;
  html.reserve(32000);
  html = "<html>" + headState + stateEnhancements + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuState + "<main id='mainContent'>" + ePaper;
  html += "<section class='panel'><div class='state-toolbar'><h1 style='margin:0'>ESP State</h1><label style='font-size:14px'><input id='toggleAdvanced' type='checkbox'/> Show advanced diagnostics</label></div>";
  html += "<p style='margin:0 0 10px 0;font-size:14px;color:var(--muted)'>Signal-first layout keeps daily health visible. Data/API shortcuts are available below for direct endpoint access.</p></section>";
  html += "<div class='state-grid'><section class='panel'><h1>System Health</h1><ul>";
  html += "<li><b>Uptime: </b>" + formatNumberWithCommas(millis() / 1000) + " s</li>";
  html += "<li><b>Current Time: </b>" + webWallClockDisplayHtml(getTime()) + "</li>";
  html += "<li><b>Restart Reason: </b>" + getLastRestartReason() + "</li>";
  {
    String fwPillDisplay = String(VERSION);
    if (fwPillDisplay.length() > 0 && fwPillDisplay.charAt(0) != 'v' && fwPillDisplay.charAt(0) != 'V')
    {
      fwPillDisplay = String("v") + fwPillDisplay;
    }
    html += "<li class='sub-card'><p class='sub-card-title'>Firmware Update</p>"
            "<div class='fw-compare'>"
            "<div class='fw-compare-cols'>"
            "<div class='fw-compare-item'><span class='fw-compare-label'>This gateway</span>"
            "<span id=\"fwCurrentBadge\" class=\"fw-pill fw-pill-current\">" + fwPillDisplay + "</span></div>"
            "<div class='fw-compare-item'><span class='fw-compare-label'>GitHub latest</span>"
            "<span id=\"fwLatestBadge\" class=\"fw-pill fw-pill-latest\">&#8212;</span></div></div>"
            "<div class='fw-actions'>"
            "<button type=\"button\" id=\"fwCheckUpdates\" class=\"fw-check-btn\" "
            "data-fw-version=\"" + String(VERSION) + "\" "
            "data-api-latest=\"" + String(FIRMWARE_REPO_RELEASES_LATEST_API_URL) + "\" "
            "data-releases=\"" + String(FIRMWARE_REPO_RELEASES_URL) + "\">Check for updates</button>"
            "<span class='gh-sponsor-embed'><iframe src=\"" + String(FIRMWARE_SPONSOR_BUTTON_SRC) + "\" title=\"Sponsor on GitHub\" width=\"114\" height=\"32\" loading=\"lazy\" referrerpolicy=\"no-referrer-when-downgrade\"></iframe></span></div>"
            "<span id=\"fwUpdateResult\" class=\"fw-update-msg\" aria-live=\"polite\"></span></div>"
            "<div class='sub-card-row' style='margin-top:8px'><b>Firmware Build: </b><span>" + String(BUILD) + "</span></div>"
            "<div class='sub-card-row' style='margin-top:8px'><b>Firmware repo: </b><a href=\"" + String(FIRMWARE_REPO_README_URL) + "\" target=\"_blank\" rel=\"noopener\">README</a>"
            " &middot; <a href=\"" + String(FIRMWARE_REPO_RELEASES_URL) + "\" target=\"_blank\" rel=\"noopener\">Releases</a></div></li>";
  }
  html += "<li><b>Free Heap: </b>" + formatNumberWithCommas(ESP.getFreeHeap()) + "</li>";
  html += "<li class='advanced-only'><b>Free PSRAM: </b>" + formatNumberWithCommas(ESP.getFreePsram()) + "</li>";
  html += "<li class='advanced-only'><b>Free Stack: </b>" + formatNumberWithCommas(uxTaskGetStackHighWaterMark(NULL)) + "</li>";
  String release = String(__DATE__) + " - " + String(__TIME__);
  html += "<li class='advanced-only'><b>Release: </b>" + release + "</li>";
  html += "<li class='advanced-only'><b>Build Definition: </b>" + buildDefinitionString + "</li>";

#ifdef LOCAL_CLIENT
  String rsHealth = String(rs485HealthCode());
  unsigned long rs485LastValidAgeMs = 0;
  if (rs485Stats.lastValidFrameMs > 0)
  {
    rs485LastValidAgeMs = (millis() >= rs485Stats.lastValidFrameMs) ? (millis() - rs485Stats.lastValidFrameMs) : 0;
  }
  html += "<li class='spacer'></li><li><b>RS485 Health: </b><span class='diag-badge' style='font-weight:700;color:#fff;background:" + rs485HealthColor(rsHealth) + "'>" + rs485HealthLabel(rsHealth) + "</span></li>";
  html += "<li><b>RS485 Mode: </b><span class='diag-badge' style='font-weight:700;color:#fff;background:" + String(rs485Stats.polarityInverted ? "#0f4a87" : "#4b5563") + "'>" + String(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal") + "</span></li>";
  html += "<li><b>Valid Frames (today): </b>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</li>";
  html += "<li><b>CRC Errors (today): </b>" + formatNumberWithCommas(rs485Stats.crcToday) + "</li>";
  if (rs485Stats.lastValidFrameMs > 0)
  {
    html += "<li><b>Last Valid Frame Age: </b>" + formatNumberWithCommas(rs485LastValidAgeMs) + " ms</li>";
  }
  else
  {
    html += "<li><b>Last Valid Frame Age: </b>n/a</li>";
  }
#endif

  appendWifiStateSection(html);
  html += "<section class='panel'><h1>MQTT</h1><ul>";
  html += "<li><b>Status: </b>" + String(mqtt.connected() ? "Connected" : "Disconnected");
  html += " (" + String(mqtt.state()) + ", " + String(mqttStateName(mqtt.state())) + ")</li>";
  html += "<li><b>Broker: </b>" + String(MQTT_SERVER) + ":" + String(MQTT_PORT) + "</li>";
  html += "<li><b>Topic Root: </b>" + mqttTopic + "</li>";
  html += "<li><b>Command Topic: </b>" + mqttTopic + "cmd/#</li>";
  html += "<li><b>LWT Topic: </b>" + mqttTopic + "node/state</li>";
  html += "<li><b>HA Discovery: </b>" + String(MQTT_HA_DISCOVERY ? "enabled" : "disabled") +
          " (prefix: " + String(MQTT_DISCOVERY_PREFIX) + ", temp unit: " + String(MQTT_HA_TEMP_UNIT) + ")</li>";
  html += "<li class='advanced-only'><b>MQTT State Legend: </b>0=Connected, -1=Disconnected, -2=Connect failed, -3=Connection lost, -4=Timeout, -5=Bad credentials</li>";
  html += "<li><details><summary><b>MQTT credentials and configuration note</b></summary>"
          "<p style='margin:8px 0 0 0'>Credentials are hidden in the portal by design. Update <code>MQTT_SERVER</code>, <code>MQTT_PORT</code>, <code>BROKER_LOGIN</code>, and <code>BROKER_PASS</code> in <code>src/config.h</code>, then rebuild/reflash firmware.</p>"
          "<p style='margin:8px 0 0 0'>Other MQTT behavior is configured in <code>src/config.h</code> via <code>MQTT_HA_DISCOVERY</code>, <code>MQTT_DISCOVERY_PREFIX</code>, and <code>MQTT_HA_TEMP_UNIT</code>.</p>"
          "</details></li>";
  html += "<li class='advanced-only'><b>Spa status struct magic (ESP RAM): </b>" + String(spaStatusData.magicNumber) +
          " <span style=\"font-size:12px;color:var(--muted)\">(expected 0x12345678 after init; not from spa controller)</span></li>";

  html += "</ul></section><section class='panel'><h1>Spa Data Freshness</h1>";
  html += "<p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Status Snapshot is the live spa status frame stream used for controls/telemetry. Dataset rows below are separate config/info blocks with independent freshness and retry behavior.</p>";
  html += "<table class='state-freshness'><thead><tr><th>Dataset</th><th>Last Update</th><th>Stale</th><th>Retry</th></tr></thead><tbody>";
  html += "<tr><td>Status Snapshot</td><td>" + statusLastUpdateDisplayHtml(spaStatusData.lastUpdate) + "</td><td>" + String(staleData(spaStatusData) ? "yes" : "no") + "</td><td>n/a</td></tr>";
  html += "<tr><td>Configuration</td><td>" + statusLastUpdateDisplayHtml(spaConfigurationData.lastUpdate) + "</td><td>" + String(staleData(spaConfigurationData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaConfigurationData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Preferences</td><td>" + statusLastUpdateDisplayHtml(spaPreferencesData.lastUpdate) + "</td><td>" + String(staleData(spaPreferencesData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaPreferencesData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Filters</td><td>" + statusLastUpdateDisplayHtml(spaFilterSettingsData.lastUpdate) + "</td><td>" + String(staleData(spaFilterSettingsData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaFilterSettingsData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Information</td><td>" + statusLastUpdateDisplayHtml(spaInformationData.lastUpdate) + "</td><td>" + String(staleData(spaInformationData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaInformationData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Fault</td><td>" + statusLastUpdateDisplayHtml(spaFaultLogData.lastUpdate) + "</td><td>" + String(staleData(spaFaultLogData) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaFaultLogData) ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>spaSettings0x04Data</td><td>" + statusLastUpdateDisplayHtml(spaSettings0x04Data.lastUpdate) + "</td><td>" + String(staleData(spaSettings0x04Data) ? "yes" : "no") + "</td><td>" + String(retryRequest(spaSettings0x04Data) ? "yes" : "no") + "</td></tr>";
  html += "</tbody></table></section>";

  html += "<section class='panel'><h1 id='api-shortcuts'>API Shortcuts</h1><p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Raw/history endpoints can be noisy and large.</p><ul>";
  html += "<li><b>Live Wi-Fi snapshot: </b><a href='/api/wifi' target='_blank' rel='noopener'>GET /api/wifi</a></li>";
  html += "<li><b>Live MQTT snapshot: </b><a href='/api/mqtt' target='_blank' rel='noopener'>GET /api/mqtt</a></li>";
  html += "<li><b>Firmware metadata: </b><a href='/api/version' target='_blank' rel='noopener'>GET /api/version</a></li>";
  html += "<li><b>RS485 summary diagnostics: </b><a href='/api/rs485' target='_blank' rel='noopener'>GET /api/rs485</a></li>";
  html += "<li><b>RS485 raw byte trace: </b><a href='/api/rs485/raw?limit=200' target='_blank' rel='noopener'>GET /api/rs485/raw?limit=200</a></li>";
  html += "<li><b>RS485 history snapshots: </b><a href='/api/rs485/history?limit=200' target='_blank' rel='noopener'>GET /api/rs485/history?limit=200</a></li>";
  html += "</ul></section>";
  html += "<section class='panel'><h1>LittleFS Inventory</h1><p style='margin:0 0 8px 0;font-size:14px;color:var(--muted)'>Load on demand when needed for debugging.</p>"
          "<button type='button' id='stateLoadLittleFs' class='fw-check-btn'>Load LittleFS files</button>"
          "<ul id='stateLittleFsBox' style='margin-top:10px'></ul></section>";

  html += "<section class='panel advanced-panel'><h1>Advanced Diagnostics</h1>";

#ifdef LOCAL_CLIENT
  html += "<details><summary>RS485 deep counters</summary><ul>";
  html += "<li><b>Hint: </b>" + rs485HealthHint(rsHealth) + "</li>";
  html += "<li><b>Mode Hint: </b>" + rs485ModeHint(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>Detect Phase: </b>" + String(rs485Stats.polarityLocked ? "2 (locked)" : (rs485Stats.polarityInverted ? "1 (testing inverted_rx_tx)" : "0 (testing normal)")) + "</li>";
  html += "<li><b>Polarity Locked: </b>" + String(rs485Stats.polarityLocked ? "yes" : "no") + "</li>";
  html += "<li class='spacer'></li><li><b>Raw Bytes (today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesToday) + "</li>";
  html += "<li><b>Raw Bytes (normal today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalToday) + "</li>";
  html += "<li><b>Raw Bytes (inverted today): </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedToday) + "</li>";
  html += "<li><b>Frame Attempts (today): </b>" + formatNumberWithCommas(rs485Stats.framesToday) + "</li>";
  html += "<li><b>0x7E Markers (today): </b>" + formatNumberWithCommas(rs485Stats.frameMarkersToday) + "</li>";
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
  html += "<li><b>UART Pins: </b>RX GPIO " + String(rs485RxGpio()) + ", TX GPIO " + String(rs485TxGpio()) + ", " + String(rs485Baud()) + " baud</li>";
  html += "<li><b>AUTO_TX: </b>" + String(rs485AutoTxEnabled() ? "true" : "false") + "</li>";
  html += "<li><b>Polarity Inverted (raw): </b>" + String(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>Health Code (raw): </b>" + rsHealth + "</li>";
  html += "</ul></details>";
#endif
  html += "</section></div><script>(function(){var t=document.getElementById('toggleAdvanced');if(!t)return;t.addEventListener('change',function(){document.body.classList.toggle('show-advanced',t.checked);});})();</script>";
  html += "<script>(function(){var btn=document.getElementById('stateLoadLittleFs');var box=document.getElementById('stateLittleFsBox');if(!btn||!box)return;"
          "btn.addEventListener('click',function(){if(btn.disabled)return;btn.disabled=true;btn.textContent='Loading...';"
          "fetch('/api/state/littlefs',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){"
          "box.innerHTML='<li>'+(j&&j.html?j.html:'(empty)')+'</li>';btn.textContent='LittleFS loaded';}).catch(function(){btn.disabled=false;btn.textContent='Retry LittleFS load';});});})();</script>";
  html += "<script>(function(){var btn=document.getElementById('fwCheckUpdates');if(!btn)return;var el=document.getElementById('fwUpdateResult');var cur=document.getElementById('fwCurrentBadge');var latest=document.getElementById('fwLatestBadge');var apiLatest=btn.getAttribute('data-api-latest');var releases=btn.getAttribute('data-releases');var fw=btn.getAttribute('data-fw-version');function norm(s){return String(s||'').trim().replace(/^v/i,'');}function dispTag(s){s=String(s||'').trim();if(!s)return'\u2014';return/^v/i.test(s)?s:('v'+s);}function cmpSemver(a,b){var pa=norm(a).split('.').map(function(x){return parseInt(x,10)||0;});var pb=norm(b).split('.').map(function(x){return parseInt(x,10)||0;});var n=Math.max(pa.length,pb.length,3);for(var i=0;i<n;i++){var da=(pa[i]||0),db=(pb[i]||0);if(da<db)return-1;if(da>db)return 1;}return 0;}function setMsg(state,text){var colors={idle:'var(--muted)',checking:'#b26a00',ok:'#1b5e20',warn:'#b00020',error:'#6b7280'};el.style.color=colors[state]||colors.idle;el.textContent=text;}if(cur)cur.textContent=dispTag(fw);btn.addEventListener('click',function(){setMsg('checking','Checking GitHub releases...');fetch(apiLatest,{headers:{'Accept':'application/vnd.github+json'}}).then(function(r){if(!r.ok)throw new Error('http');return r.json();}).then(function(j){var tag=j.tag_name||'';if(latest)latest.textContent=dispTag(tag);var c=cmpSemver(fw,tag);if(c>=0){setMsg('ok','Up to date.');}else{setMsg('warn','Update available — open Releases to download.');}}).catch(function(){if(latest)latest.textContent='\u2014';el.style.color='var(--muted)';el.textContent='Could not reach GitHub. ';var a=document.createElement('a');a.href=releases;a.textContent='Open Releases';a.target='_blank';a.rel='noopener';el.appendChild(a);el.appendChild(document.createTextNode(' to compare manually.'));});});})();</script></main></div></body></html>";

  String etag = String("W/\"state-") + String(VERSION) + "-" + String(BUILD) + "-" + String(spaStatusData.lastUpdate) + "-" + String(spaConfigurationData.lastUpdate) + "\"";
  sendHtmlWithEtag(request, html, etag);
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
  String html;
  html.reserve(28000);
  html = "<html class=\"logs-portal\">" + headLogs + "<body class=\"logs-portal\"><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page logs-page'>" + webMenuLogs + "<main id='mainContent'><section class='panel logs-panel'><div class='logs-stack'><h1>Device logs</h1>";
  html += "<p style='color:var(--muted);font-size:14px;margin-top:0'>Recent lines are buffered on the gateway; include/exclude filters run in the browser. Logs are teed to USB <code>Serial</code> (monitor baud) and this ring. For a live tail without USB, use this page or <code>GET /api/logs</code> (optional WebSocket tail). If the firmware was built with <code>TELNET_LOG</code>, <code>TelnetStream</code> also listens on TCP port 23; the global logger is <em>not</em> switched to Telnet (see Wi‑Fi boot messages).</p>";
  html += R"HTML(<style>
html.logs-portal,body.logs-portal{min-height:100svh;min-height:100dvh}
body.logs-portal{display:flex;flex-direction:column;margin:0;box-sizing:border-box;padding-bottom:env(safe-area-inset-bottom,0)}
body.logs-portal>.page.logs-page{flex:1 1 auto;display:flex;flex-direction:column;min-height:0;width:100%;max-width:980px;margin:0 auto;padding:max(var(--space-3),env(safe-area-inset-left,0)) max(var(--space-3),env(safe-area-inset-right,0)) max(var(--space-3),env(safe-area-inset-bottom,0))}
body.logs-portal #mainContent{flex:1 1 auto;display:flex;flex-direction:column;min-height:0}
body.logs-portal .logs-panel{flex:1 1 auto;display:flex;flex-direction:column;min-height:0;margin-bottom:0}
body.logs-portal .logs-stack{flex:0 0 auto}
.log-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
.log-controls input[type=text]{flex:1 1 140px;min-width:120px;padding:8px;border:1px solid var(--border);border-radius:6px;font-size:14px}
.log-controls label{font-size:14px;color:var(--muted)}
.log-controls select{padding:8px;border-radius:6px;border:1px solid var(--border);font-size:14px}
.preset-row{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 10px 0}
.preset-row button{flex:0 0 auto;padding:8px 11px;font-size:13px;min-height:36px}
.status-row{display:flex;align-items:center;gap:10px;margin:0 0 10px 0;color:var(--muted);font-size:13px}
.log-view{flex:1 1 12rem;min-height:0;overflow:auto;background:#0f172a;color:#e2e8f0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;line-height:1.45;padding:8px;border-radius:8px;border:1px solid var(--border)}
.log-line{display:flex;gap:8px;padding:2px 4px;border-radius:4px;white-space:pre-wrap;word-break:break-word}
.log-seq{color:#93a8c5;min-width:56px}
.log-tag{display:inline-block;padding:0 6px;border-radius:999px;background:#233148;color:#d7e3f4;font-size:11px}
.lvl-e{background:rgba(190,24,36,.2)} .lvl-w{background:rgba(202,138,4,.2)} .lvl-i{background:rgba(2,132,199,.16)} .lvl-v{background:rgba(71,85,105,.2)}
#newBadge{display:none}
@media (max-width:640px){body.logs-portal>.page.logs-page{padding-left:max(var(--space-2),env(safe-area-inset-left,0));padding-right:max(var(--space-2),env(safe-area-inset-right,0))}}
</style>)HTML";
  html += "<div class='preset-row'><button type='button' id='pAll'>All</button><button type='button' id='pErr'>Errors only</button><button type='button' id='pRs'>RS485</button><button type='button' id='pBridge'>BridgeDiag</button><button type='button' id='pWifi'>WiFi</button></div>";
  html += "<div class='log-controls'><label>Level <select id='lvl'><option value='0'>SILENT</option><option value='1'>FATAL</option><option value='2'>ERROR</option><option value='3'>WARNING</option><option value='4'>INFO/NOTICE</option><option value='5'>TRACE</option><option value='6'>VERBOSE</option></select></label>";
  html += "<button type='button' id='applyLvl'>Apply level</button>";
  html += "<label>Include <input type='text' id='fInc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label>Exclude <input type='text' id='fExc' placeholder='substring' autocapitalize='off' autocomplete='off'/></label>";
  html += "<label><input type='checkbox' id='pause'/> Pause</label>";
  html += "<label><input type='checkbox' id='hideIdleCts' checked/> Hide idle CTS</label>";
  html += "<label><input type='checkbox' id='showHidden'/> Show hidden</label>";
  html += "<label><input type='checkbox' id='useWs' checked/> WebSocket tail</label>";
  html += "<button type='button' id='newBadge'>0 new lines</button>";
  html += "<button type='button' id='clr'>Clear view</button><button type='button' id='copyTxt'>Copy</button><button type='button' id='dlTxt'>Download .log</button><button type='button' id='dlJson'>Download .json</button>";
  html += "</div>";
  html += "<div class='status-row'><span id='streamMode'>poll</span><span id='renderCount'>0 lines</span><span id='hiddenCount'>hidden idle CTS: 0</span><span id='connState'></span></div></div>";
  html += "<div id='logView' class='log-view' aria-live='polite'></div></section></main></div><script>";
  html += R"JS((function(){
var logView=document.getElementById('logView'),since=0,pollMs=1000,pollMaxMs=20000,timer,ws,useWs=true,newBuffered=0;
var pollFailures=0,wsRetryTimer=null,wsOpenEver=false,pollInFlight=null;
var fInc=document.getElementById('fInc'),fExc=document.getElementById('fExc'),sel=document.getElementById('lvl');
var pauseEl=document.getElementById('pause'),newBadge=document.getElementById('newBadge');
var hideIdleCtsEl=document.getElementById('hideIdleCts'),showHiddenEl=document.getElementById('showHidden');
var streamMode=document.getElementById('streamMode'),renderCount=document.getElementById('renderCount'),hiddenCountEl=document.getElementById('hiddenCount'),connState=document.getElementById('connState');
var rendered=[],maxRendered=8000;
var hiddenIdleCts=0;
function abortLogPoll(){if(pollInFlight){pollInFlight.abort();pollInFlight=null;}}
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
function isVisibleRecord(rec){
var line=rec.t;
var hiddenByIdleCts=hideIdleCtsEl.checked&&isIdleCtsLine(line);
if(hiddenByIdleCts&&!showHiddenEl.checked)return false;
if(!passes(line))return false;
return true;
}
function renderLine(rec){var tag=getTag(rec.t),cls=getLevelClass(rec.t);var body=esc(rec.t);if(tag){body=body.replace('['+tag+']','<span class=\"log-tag\">['+esc(tag)+']</span>');}
return '<div class=\"log-line '+cls+'\"><span class=\"log-seq\">#'+rec.s+'</span><span>'+body+'</span></div>';}
function refreshFromRendered(){var out='',n=0,h=0;for(var i=0;i<rendered.length;i++){var line=rendered[i].t;var hiddenByIdleCts=hideIdleCtsEl.checked&&isIdleCtsLine(line);if(hiddenByIdleCts){h++;}if(!isVisibleRecord(rendered[i]))continue;out+=renderLine(rendered[i]);n++;}hiddenIdleCts=h;hiddenCountEl.textContent='hidden idle CTS: '+String(hiddenIdleCts);var stick=(logView.scrollTop+logView.clientHeight+20)>=logView.scrollHeight;logView.innerHTML=out;renderCount.textContent=n+' lines';if(stick){logView.scrollTop=logView.scrollHeight;}}
function appendLines(arr){if(!arr)return;for(var j=0;j<arr.length;j++){rendered.push({s:arr[j].s,t:arr[j].t});if(rendered.length>maxRendered)rendered.shift();}refreshFromRendered();}
function receiveLines(arr){if(!arr||!arr.length)return;var atBottom=(logView.scrollTop+logView.clientHeight+20)>=logView.scrollHeight;
if(atBottom){appendLines(arr);newBuffered=0;newBadge.style.display='none';}
else{newBuffered+=arr.length;newBadge.textContent=String(newBuffered)+' new lines';newBadge.style.display='inline-flex';for(var k=0;k<arr.length;k++){rendered.push({s:arr[k].s,t:arr[k].t});if(rendered.length>maxRendered)rendered.shift();}renderCount.textContent=rendered.length+' lines';}}
function capSel(mx){for(var i=0;i<sel.options.length;i++){var o=sel.options[i];o.disabled=(parseInt(o.value,10)>mx);}if((parseInt(sel.value,10)||0)>mx)sel.value=String(mx);}
function nextPollDelay(){var e=Math.min(pollMaxMs,pollMs*Math.pow(2,Math.min(6,pollFailures)));var j=Math.floor(Math.random()*Math.max(250,Math.floor(e*0.35)));return Math.min(pollMaxMs,e+j);}
function schedulePoll(ms){stopPoll();timer=setTimeout(poll,Math.max(250,ms||pollMs));}
function fetchJsonTimeout(url,timeoutMs){var ctl=new AbortController();var t=setTimeout(function(){ctl.abort();},timeoutMs||5000);return fetch(url,{cache:'no-store',signal:ctl.signal}).then(function(r){if(!r.ok)throw new Error('http_'+r.status);return r.json();}).finally(function(){clearTimeout(t);});}
function poll(){if(document.hidden||pauseEl.checked)return;var ctl=new AbortController();pollInFlight=ctl;var t=setTimeout(function(){ctl.abort();},4200);schedulePoll(pollMs);
fetch('/api/logs?since='+since+'&limit=120',{cache:'no-store',signal:ctl.signal}).then(function(r){if(!r.ok)throw new Error('http_'+r.status);return r.json();}).then(function(j){if(pauseEl.checked||pollInFlight!==ctl)return;pollFailures=0;connState.textContent='ok';
if(typeof j.compileMaxLevel==='number')capSel(j.compileMaxLevel);
var lines=j.lines||[];
receiveLines(lines);
if(lines.length>0&&typeof lines[lines.length-1].s==='number'){since=lines[lines.length-1].s;}
else if(typeof j.newestSeq==='number'){since=j.newestSeq;}
}).catch(function(){if(pauseEl.checked||pollInFlight!==ctl)return;pollFailures++;connState.textContent='poll retrying...';schedulePoll(nextPollDelay());}).finally(function(){clearTimeout(t);if(pollInFlight===ctl)pollInFlight=null;});}
function startPoll(){stopPoll();streamMode.textContent='poll';pollFailures=0;poll();}
function stopPoll(){if(timer){clearTimeout(timer);timer=null;}}
function clearWsRetry(){if(wsRetryTimer){clearTimeout(wsRetryTimer);wsRetryTimer=null;}}
function scheduleWsReconnect(){clearWsRetry();if(document.hidden||pauseEl.checked||!useWs)return;var wait=Math.min(20000,1000*Math.pow(2,Math.min(6,pollFailures)));wsRetryTimer=setTimeout(connectWs,wait);}
function connectWs(){if(document.hidden||pauseEl.checked||!useWs)return;streamMode.textContent='ws';clearWsRetry();var p=location.protocol==='https:'?'wss:':'ws:';ws=new WebSocket(p+'//'+location.host+'/api/logs/ws');connState.textContent='connecting';
ws.onopen=function(){pollFailures=0;wsOpenEver=true;connState.textContent='ws-open';};
ws.onmessage=function(ev){if(pauseEl.checked)return;try{var o=JSON.parse(ev.data);if(o.lines)receiveLines(o.lines);if(o.d)receiveLines(o.d);}catch(e){}};
ws.onerror=function(){connState.textContent='ws-error';};
ws.onclose=function(){ws=null;pollFailures++;if(!useWs)return;connState.textContent='ws-closed';if(wsOpenEver&&!pauseEl.checked){startPoll();}scheduleWsReconnect();};}
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
document.getElementById('pause').addEventListener('change',function(){if(this.checked){stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}}else if(useWs)connectWs();else startPoll();});
document.getElementById('useWs').addEventListener('change',function(){useWs=this.checked;stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}});
document.getElementById('clr').addEventListener('click',function(){rendered=[];refreshFromRendered();});
document.getElementById('copyTxt').addEventListener('click',function(){
var txt='';for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))txt+=rendered[i].t+'\n';}
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
document.getElementById('dlTxt').addEventListener('click',function(){var txt='';for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))txt+=rendered[i].t+'\n';}dl('spa-logs-'+Date.now()+'.log',txt,'text/plain');});
document.getElementById('dlJson').addEventListener('click',function(){var out=[];for(var i=0;i<rendered.length;i++){if(isVisibleRecord(rendered[i]))out.push(rendered[i]);}dl('spa-logs-'+Date.now()+'.json',JSON.stringify(out,null,2),'application/json');});
document.getElementById('applyLvl').addEventListener('click',function(){var v=parseInt(sel.value,10);fetch('/api/logs/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({level:v})}).then(function(){return fetchJsonTimeout('/api/logs/config',5000);}).then(function(c){if(typeof c.currentLevel==='number')sel.value=String(c.currentLevel);if(typeof c.compileMaxLevel==='number')capSel(c.compileMaxLevel);}).catch(function(){});});
fetchJsonTimeout('/api/logs/config',5000).then(function(c){sel.value=String(c.currentLevel||0);capSel(c.compileMaxLevel||6);}).catch(function(){});
document.addEventListener('visibilitychange',function(){if(document.hidden){stopPoll();clearWsRetry();abortLogPoll();if(ws){ws.close();ws=null;}}else if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}});
if(!pauseEl.checked){if(useWs)connectWs();else startPoll();}
})();)JS";
  html += "</script></body></html>";
  String etag = String("W/\"logs-") + String(VERSION) + "-" + String(BUILD) + "\"";
  sendHtmlWithEtag(request, html, etag);
  Log.verbose("[Web]: handleLogsPage %p" CR, request->client()->remoteIP());
}

void handleVersion(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
#if defined(DIAG_FAULT_CAPTURE)
  DynamicJsonDocument doc(3072);
#else
  DynamicJsonDocument doc(512);
#endif
  doc["version"] = VERSION;
  doc["build"] = BUILD;
  doc["hostname"] = WiFi.getHostname();
  doc["ip"] = WiFi.localIP().toString();
  doc["restartReason"] = getLastRestartReason();
  doc["repoReadmeUrl"] = FIRMWARE_REPO_README_URL;
  doc["releasesUrl"] = FIRMWARE_REPO_RELEASES_URL;
  doc["releasesLatestApiUrl"] = FIRMWARE_REPO_RELEASES_LATEST_API_URL;
#if defined(DIAG_FAULT_CAPTURE)
  faultCaptureAppendToJson(doc.to<JsonObject>());
#endif
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

void handleMqtt(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(1024);
  const int stateCode = mqtt.state();
  const bool connected = mqtt.connected();
  doc["enabled"] = true;
  doc["connected"] = connected;
  doc["stateCode"] = stateCode;
  doc["stateName"] = mqttStateName(stateCode);
  doc["brokerHost"] = MQTT_SERVER;
  doc["brokerPort"] = MQTT_PORT;
  doc["topicRoot"] = mqttTopic;
  doc["commandTopic"] = mqttTopic + "cmd/#";
  doc["lwtTopic"] = mqttTopic + "node/state";
  doc["haDiscoveryEnabled"] = MQTT_HA_DISCOVERY ? true : false;
  doc["haDiscoveryPrefix"] = MQTT_DISCOVERY_PREFIX;
  doc["haTempUnit"] = MQTT_HA_TEMP_UNIT;
  doc["reconnectIntervalMs"] = 30000;
  doc["lastReconnectAttemptMs"] = mqttLastReconnectAttempt;
  if (mqttLastReconnectAttempt > 0 && millis() >= mqttLastReconnectAttempt)
  {
    doc["lastReconnectAttemptMsAgo"] = millis() - mqttLastReconnectAttempt;
  }
  else
  {
    doc["lastReconnectAttemptMsAgo"] = -1;
  }
  doc["credentialsExposed"] = false;
  doc["credentialNote"] = "Update MQTT_SERVER/MQTT_PORT/BROKER_LOGIN/BROKER_PASS in src/config.h and reflash.";
  serializeJson(doc, *response);
  request->send(response);
}

static void fillStatusSnapshotDoc(DynamicJsonDocument &doc)
{
  doc["lastUpdate"] = spaStatusData.lastUpdate;
  doc["snapshotAgeSec"] = statusSnapshotAgeSec();
  doc["snapshotAtLocal"] = statusLastUpdateDisplayHtml(spaStatusData.lastUpdate);
  doc["snapshotMeta"] = statusSnapshotSubtitle();
  doc["tempScaleCelsius"] = spaStatusData.tempScale ? true : false;
  doc["tempRange"] = spaStatusData.tempRange;
  doc["spaState"] = spaStatusData.spaState;
  doc["spaStateText"] = getMapDescription(spaStatusData.spaState, spaStateMap);
  doc["initMode"] = spaStatusData.initMode;
  doc["initModeText"] = getMapDescription(spaStatusData.initMode, initModeMap);
  doc["heatingMode"] = spaStatusData.heatingMode;
  doc["heatingModeText"] = getMapDescription(spaStatusData.heatingMode, heatingModeMap);
  doc["heatingState"] = spaStatusData.heatingState;
  doc["heatingStateText"] = getMapDescription(spaStatusData.heatingState, heatingStateMap);
  doc["needsHeat"] = spaStatusData.needsHeat ? 1 : 0;
  doc["highSetTemp"] = spaStatusData.highSetTemp;
  doc["lowSetTemp"] = spaStatusData.lowSetTemp;
  doc["setTemp"] = spaStatusData.setTemp;
  {
    float bandMin = 0;
    float bandMax = 0;
    spaProtocolActiveSetpointBand(bandMin, bandMax);
    doc["setTempMin"] = bandMin;
    doc["setTempMax"] = bandMax;
  }
  doc["light1"] = spaStatusData.light1 ? 1 : 0;
  doc["light2"] = spaStatusData.light2 ? 1 : 0;
  doc["pump1"] = spaStatusData.pump1;
  doc["pump2"] = spaStatusData.pump2;
  doc["pump3"] = spaStatusData.pump3;
  doc["pump4"] = spaStatusData.pump4;
  doc["pump5"] = spaStatusData.pump5;
  doc["pump6"] = spaStatusData.pump6;
  doc["pump1Config"] = statusPumpConfigSpeed(1);
  doc["pump2Config"] = statusPumpConfigSpeed(2);
  doc["pump3Config"] = statusPumpConfigSpeed(3);
  doc["pump4Config"] = statusPumpConfigSpeed(4);
  doc["pump5Config"] = statusPumpConfigSpeed(5);
  doc["pump6Config"] = statusPumpConfigSpeed(6);
  doc["pump1On"] = statusPumpIsOn(1);
  doc["pump2On"] = statusPumpIsOn(2);
  doc["pump3On"] = statusPumpIsOn(3);
  doc["pump4On"] = statusPumpIsOn(4);
  doc["pump5On"] = statusPumpIsOn(5);
  doc["pump6On"] = statusPumpIsOn(6);
  doc["circ"] = spaStatusData.circ ? 1 : 0;
  doc["blower"] = spaStatusData.blower;
  doc["mister"] = spaStatusData.mister ? 1 : 0;
  doc["panelTime"] = String(spaStatusData.time);
  doc["clockFormat"] = statusPanelClockFormatLabel(spaStatusData.clockMode);
  doc["clockModeRaw"] = spaStatusData.clockMode;
  doc["filterModeText"] = String(getMapDescription(spaStatusData.filterMode, filterModeMap));
  doc["gatewayTimeHHMM"] = statusGatewayLocalTimeHHMM();
}

void handleStatusControlsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2560);
  fillStatusSnapshotDoc(doc);
  serializeJson(doc, *response);
  request->send(response);
}

void handleStatusSummaryApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(2560);
  fillStatusSnapshotDoc(doc);
  wl_status_t st = WiFi.status();
  doc["wifiConnected"] = (st == WL_CONNECTED);
  doc["wifiStatus"] = static_cast<int>(st);
  doc["wifiStatusName"] = wifiStatusName(st);
  if (st == WL_CONNECTED)
  {
    doc["wifiRssi"] = WiFi.RSSI();
  }
  serializeJson(doc, *response);
  request->send(response);
}

void handleStatusHistoriesApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(16384);
  String historiesHtml;
  historiesHtml.reserve(7000);
  appendStatusHistoriesSection(historiesHtml);
  doc["html"] = historiesHtml;
  doc["tempIsCelsius"] = (statusSpaTempReady() && spaStatusData.tempScale) ? 1 : 0;

  JsonArray tempHistory = doc.createNestedArray("tempHistory");
  for (int i = GRAPH_MAX_READINGS - 1; i >= 0; i--)
  {
    tempHistory.add(spaStatusData.temperatureHistory[i]);
  }
  JsonArray heatSeconds = doc.createNestedArray("heatSeconds");
  for (int i = GRAPH_MAX_READINGS - 1; i >= 0; i--)
  {
    heatSeconds.add(spaStatusData.heatOn->history()[i]);
  }
  JsonArray filterSeconds = doc.createNestedArray("filterSeconds");
  for (int i = GRAPH_MAX_READINGS - 1; i >= 0; i--)
  {
    filterSeconds.add(spaStatusData.filterOn->history()[i]);
  }

  serializeJson(doc, *response);
  request->send(response);
}

void handleStateLittleFsApi(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(6144);
  doc["html"] = listDirToString(LittleFS, "/", 3);
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

      int togglesToSend = spaToggleCountForButtonRequest((uint8_t)itemCode, requestHasState, desiredOn);
      Log.verbose("[Web]: Button request raw=%s item=%d desired=%s toggles=%d" CR, value.c_str(), itemCode, (requestHasState ? desiredStateRaw.c_str() : "n/a"), togglesToSend);
      if (togglesToSend < 0)
      {
        response = "<device_request target_name='Button' result='rejected' error='invalid_button_payload'>" + value + "</device_request>";
      }
      else if (togglesToSend == 0)
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
    else if (target == "SystemTime")
    {
      value.trim();
      const int colon = value.indexOf(':');
      if (colon <= 0 || colon >= (int)value.length() - 1)
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='invalid_time_payload'>" + value + "</device_request>";
        return response;
      }
      const int hour = value.substring(0, colon).toInt();
      const int minute = value.substring(colon + 1).toInt();
      if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='invalid_time_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetSpaPanelClockTime((uint8_t)hour, (uint8_t)minute, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='SystemTime' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='SystemTime' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: SystemTime request %s -> %s" CR, value.c_str(), result.reason);
    }
    else if (target == "TimeFormat")
    {
      value.trim();
      bool parsed = false;
      bool use24 = false;
      if (value == "24")
      {
        parsed = true;
        use24 = true;
      }
      else if (value == "12")
      {
        parsed = true;
        use24 = false;
      }

      if (!parsed)
      {
        response = "<device_request target_name='TimeFormat' result='rejected' error='invalid_time_format_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetSpaPanelClockFormat(use24, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='TimeFormat' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='TimeFormat' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: TimeFormat request %s -> %s" CR, value.c_str(), result.reason);
    }
    else if (target == "TempUnits")
    {
      value.trim();
      bool parsed = false;
      bool celsius = false;
      if (value.equalsIgnoreCase("C") || value.equalsIgnoreCase("Celsius"))
      {
        parsed = true;
        celsius = true;
      }
      else if (value.equalsIgnoreCase("F") || value.equalsIgnoreCase("Fahrenheit"))
      {
        parsed = true;
        celsius = false;
      }

      if (!parsed)
      {
        response = "<device_request target_name='TempUnits' result='rejected' error='invalid_temp_units_payload'>" + value + "</device_request>";
        return response;
      }

      SpaCommandResult result = spaSetTemperatureScale(celsius, SPA_COMMAND_SOURCE_WEB);
      if (result.accepted)
      {
        response = "<device_request target_name='TempUnits' result='accepted'>" + value + "</device_request>";
      }
      else
      {
        response = "<device_request target_name='TempUnits' result='rejected' error='" + String(result.reason) + "'>" + value + "</device_request>";
      }
      Log.verbose("[Web]: TempUnits request %s -> %s" CR, value.c_str(), result.reason);
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

/** Cache-Control for static LittleFS files: hashed SPA chunks vs entry HTML. */
static void addCacheControlForLittleFsPath(AsyncWebServerResponse *response, const String &urlPath)
{
  if (!response)
  {
    return;
  }
  const char *cacheControl;
  if (urlPath.indexOf("/assets/") >= 0)
  {
    cacheControl = "public, max-age=31536000, immutable";
  }
  else if (urlPath == "/index.html" || urlPath.endsWith("/index.html"))
  {
    cacheControl = "no-cache";
  }
  else
  {
    cacheControl = "public, max-age=3600";
  }
  response->addHeader("Cache-Control", cacheControl);
}

static void sendLittleFsFileWithCache(AsyncWebServerRequest *request, const String &path)
{
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, String(), false);
  addCacheControlForLittleFsPath(response, path);
  request->send(response);
}

void handleNotFound(AsyncWebServerRequest *request)
{
  if (LittleFS.exists(request->url()))
  {
    Log.verbose("[Web]: LFS %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());
    sendLittleFsFileWithCache(request, request->url());
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
