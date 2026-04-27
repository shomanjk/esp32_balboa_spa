#include "spaWebServer.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoLog.h>
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
void handleRs485(AsyncWebServerRequest *request);
void handleRs485History(AsyncWebServerRequest *request);
void handleSlash(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleData(AsyncWebServerRequest *request);
void handleLoginData(AsyncWebServerRequest *request);
void handleOptionsData(AsyncWebServerRequest *request);
void handleOptionsLoginData(AsyncWebServerRequest *request);
void handleepdpanel(AsyncWebServerRequest *request);
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
bool serverSetup = false;

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
    server.on("/api/rs485/history", HTTP_GET, handleRs485History);
    server.on("/api/rs485", HTTP_GET, handleRs485);
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

#define webMenuStatus String("<nav aria-label='Portal navigation'><form class='top-nav'><button class='active' formaction='/status'>SPA Status</button><button formaction='/config'>SPA Config</button><button formaction='/state'>ESP State</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define webMenuConfig String("<nav aria-label='Portal navigation'><form class='top-nav'><button formaction='/status'>SPA Status</button><button class='active' formaction='/config'>SPA Config</button><button formaction='/state'>ESP State</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#define webMenuState String("<nav aria-label='Portal navigation'><form class='top-nav'><button formaction='/status'>SPA Status</button><button formaction='/config'>SPA Config</button><button class='active' formaction='/state'>ESP State</button><button formaction='/index.html'>SPA Website</button></form></nav>")

#ifdef spaEpaper
#define ePaper String("<img class='panel-image' src='panel.jpg' alt='Spa Panel'>")
#else
#define ePaper String("")
#endif

void handleStatus(AsyncWebServerRequest *request)
{
  Log.verbose("[Web]: Request %s received from %p" CR, request->url().c_str(), request->client()->remoteIP());
  String html = "<html>" + head + "<body><a class='skip-link' href='#mainContent'>Skip to main content</a><div class='page'>" + webMenuStatus + "<main id='mainContent'>" + ePaper + "<section class='panel'><h1>Spa Status</h1><ul>";
  html += "<li><b>lastUpdate:</b> " + formatNumberWithCommas(spaStatusData.lastUpdate) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaStatusData.magicNumber) + "</li>";
  html += "<li class='spacer'></li><li><b>Free Heap: </b>" + formatNumberWithCommas(ESP.getFreeHeap()) + "</li>";
  html += "<li><b>Free PSRAM: </b>" + formatNumberWithCommas(ESP.getFreePsram()) + "</li>";
  html += "<li><b>Free Stack: </b>" + formatNumberWithCommas(uxTaskGetStackHighWaterMark(NULL)) + "</li>";

  html += "<li class='spacer'></li><li><b>Current Temp: </b>" + String(spaStatusData.currentTemp) + "°C</li>";
  html += "<li><b>Set Temp: </b>" + String(spaStatusData.setTemp) + "°C</li>";
  html += "<li><b>High Set Temp: </b>" + String(spaStatusData.highSetTemp) + "°C</li>";
  html += "<li><b>Low Set Temp: </b>" + String(spaStatusData.lowSetTemp) + "°C</li>";
  html += "<li><b>Temp Range: </b>" + getMapDescription(spaStatusData.tempRange, tempRangeMap) + "</li>";
  html += "<li><b>Temp Scale: </b>" + String(spaStatusData.tempScale) + "</li>";

  html += "<li><b>Spa State: </b>" + getMapDescription(spaStatusData.spaState, spaStateMap) + "</li>";
  html += "<li><b>Init Mode: </b>" + getMapDescription(spaStatusData.initMode, initModeMap) + "</li>";
  html += "<li><b>Heating Mode: </b>" + getMapDescription(spaStatusData.heatingMode, heatingModeMap) + "</li>";
  html += "<li><b>Heating State: </b>" + String(spaStatusData.heatingState) + "</li>";
  html += "<li><b>Needs Heat: </b>" + String(spaStatusData.needsHeat) + "</li>";

  html += "<li><b>Time: </b>" + String(spaStatusData.time) + "</li>";
  html += "<li><b>Clock Mode: </b>" + String(spaStatusData.clockMode) + "</li>";
  html += "<li><b>Filter Mode: </b>" + getMapDescription(spaStatusData.filterMode, filterModeMap) + "</li>";
  html += "<li><b>Pump 1: </b>" + getMapDescription(spaStatusData.pump1, pumpMap) + "</li>";
  html += "<li><b>Pump 2: </b>" + getMapDescription(spaStatusData.pump2, pumpMap) + "</li>";
  html += "<li><b>Pump 3: </b>" + getMapDescription(spaStatusData.pump3, pumpMap) + "</li>";
  html += "<li><b>Pump 4: </b>" + getMapDescription(spaStatusData.pump4, pumpMap) + "</li>";
  html += "<li><b>Pump 5: </b>" + getMapDescription(spaStatusData.pump5, pumpMap) + "</li>";
  html += "<li><b>Pump 6: </b>" + getMapDescription(spaStatusData.pump6, pumpMap) + "</li>";
  html += "<li><b>Circulation Pump: </b>" + getMapDescription(spaStatusData.circ, onOffMap) + "</li>";
  html += "<li><b>Blower: </b>" + getMapDescription(spaStatusData.blower, onOffMap) + "</li>";
  html += "<li><b>Light 1: </b>" + getMapDescription(spaStatusData.light1, onOffMap) + "</li>";
  html += "<li><b>Light 2: </b>" + getMapDescription(spaStatusData.light2, onOffMap) + "</li>";
  html += "<li><b>Mister: </b>" + getMapDescription(spaStatusData.mister, onOffMap) + "</li>";
  html += "<li><b>Panel Locked: </b>" + getMapDescription(spaStatusData.panelLocked, lockedMap) + "</li>";
  html += "<li><b>Settings Lock: </b>" + getMapDescription(spaStatusData.settingsLock, lockedMap) + "</li>";
  html += "<li><b>M8 Cycle Time: </b>" + String(spaStatusData.m8CycleTime) + "</li>";
  html += "<li><b>Notification: </b>" + String(spaStatusData.notification) + "</li>";
  html += "<li><b>Flags 19: </b>" + String(spaStatusData.flags19) + "</li>";

  html += "<li class='spacer'></li><li><b>Heater On Time Today: </b>" + formatNumberWithCommas(spaStatusData.heaterOnTimeToday) + "(sec)</li>";
  html += "<li><b>Heater On Time Yesterday: </b>" + formatNumberWithCommas(spaStatusData.heaterOnTimeYesterday) + "(sec)</li>";
  html += "<li><b>Filter On Time Today: </b>" + formatNumberWithCommas(spaStatusData.filterOnTimeToday) + "(sec)</li>";
  html += "<li><b>Filter On Time Yesterday: </b>" + formatNumberWithCommas(spaStatusData.filterOnTimeYesterday) + "(sec)</li>";
  html += "<li class='spacer'></li><li><b>Temperature History: </b>" + historyToString(spaStatusData.temperatureHistory) + "</li>";
  html += "<li><b>Heat History: </b>" + historyToString(spaStatusData.heatOn->history()) + "</li>";
  html += "<li><b>Filter History: </b>" + historyToString(spaStatusData.filterOn->history()) + "</li>";

  html += "</ul></section></main></div></body></html>";
  // Add more fields as needed
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
    html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaConfigurationData.lastUpdate) + "</li>";
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
    html += "</ul></section><section class='panel'><h1>Filter Configuration</h1><ul>";
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
  html += "<li><b>Uptime: </b>" + formatNumberWithCommas(millis() / 1000) + "</li>";
  html += "<li><b>Time: </b>" + formatNumberWithCommas(getTime()) + "</li>";
  html += "<li><b>Refresh Time: </b>" + formatNumberWithCommas(getTime() + 60 * 60) + "</li>";
  html += "<li><b>Restart Reason: </b>" + getLastRestartReason() + "</li>";
  html += "<li><b>Firmware Version: </b>" + String(VERSION) + "</li>";
  html += "<li><b>Firmware Build: </b>" + String(BUILD) + "</li>";
  String release = String(__DATE__) + " - " + String(__TIME__);
  html += "<li><b>Release: </b>" + release + "</li>";
  html += "<li><b>Build Definition: </b>" + buildDefinitionString + "</li>";

  html += "<li class='spacer'></li><li><b>getTime(): </b>" + formatNumberWithCommas(getTime()) + "</li>";
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
  html += "<li><b>Valid Frames (today): </b>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</li>";
  html += "<li><b>CRC Errors (today): </b>" + formatNumberWithCommas(rs485Stats.crcToday) + "</li>";
  html += "<li><b>Format Errors (today): </b>" + formatNumberWithCommas(rs485Stats.badFormatToday) + "</li>";
  html += "<li><b>Mode Switches (today): </b>" + formatNumberWithCommas(rs485Stats.polaritySwitchesToday) + "</li>";
  html += "<li class='spacer'></li><li><b>Raw Bytes (yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesYesterday) + "</li>";
  html += "<li><b>Raw Bytes (normal yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalYesterday) + "</li>";
  html += "<li><b>Raw Bytes (inverted yesterday): </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedYesterday) + "</li>";
  html += "<li><b>Frame Attempts (yesterday): </b>" + formatNumberWithCommas(rs485Stats.framesYesterday) + "</li>";
  html += "<li><b>Valid Frames (yesterday): </b>" + formatNumberWithCommas(rs485Stats.messagesYesterday) + "</li>";
  html += "<li><b>CRC Errors (yesterday): </b>" + formatNumberWithCommas(rs485Stats.crcYesterday) + "</li>";
  html += "<li><b>Format Errors (yesterday): </b>" + formatNumberWithCommas(rs485Stats.badFormatYesterday) + "</li>";
  html += "<li><b>Mode Switches (yesterday): </b>" + formatNumberWithCommas(rs485Stats.polaritySwitchesYesterday) + "</li>";
  html += "<li class='spacer'></li><li><b>Last Byte Millis: </b>" + formatNumberWithCommas(rs485Stats.lastByteMs) + "</li>";
  html += "<li><b>Last Valid Frame Millis: </b>" + formatNumberWithCommas(rs485Stats.lastValidFrameMs) + "</li>";
  html += "<li><b>Polarity Inverted (raw): </b>" + String(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>Health Code (raw): </b>" + rsHealth + "</li>";

  html += "</ul></section><section class='panel'><h1>RS485 Raw Counters</h1><ul>";
  html += "<li class='spacer'></li><li><b>rs485 messagesToday: </b>" + formatNumberWithCommas(rs485Stats.messagesToday) + "</li>";
  html += "<li><b>rs485 rawBytesToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesToday) + "</li>";
  html += "<li><b>rs485 rawBytesNormalToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalToday) + "</li>";
  html += "<li><b>rs485 rawBytesInvertedToday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedToday) + "</li>";
  html += "<li><b>rs485 framesToday: </b>" + formatNumberWithCommas(rs485Stats.framesToday) + "</li>";
  html += "<li><b>rs485 crcToday: </b>" + formatNumberWithCommas(rs485Stats.crcToday) + "</li>";
  html += "<li><b>rs485 messagesYesterday: </b>" + formatNumberWithCommas(rs485Stats.messagesYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesNormalYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesNormalYesterday) + "</li>";
  html += "<li><b>rs485 rawBytesInvertedYesterday: </b>" + formatNumberWithCommas(rs485Stats.rawBytesInvertedYesterday) + "</li>";
  html += "<li><b>rs485 framesYesterday: </b>" + formatNumberWithCommas(rs485Stats.framesYesterday) + "</li>";
  html += "<li><b>rs485 crcYesterday: </b>" + formatNumberWithCommas(rs485Stats.crcYesterday) + "</li>";
  html += "<li><b>rs485 badFormatToday: </b>" + formatNumberWithCommas(rs485Stats.badFormatToday) + "</li>";
  html += "<li><b>rs485 badFormatYesterday: </b>" + formatNumberWithCommas(rs485Stats.badFormatYesterday) + "</li>";
  html += "<li><b>rs485 polarityInverted: </b>" + String(rs485Stats.polarityInverted) + "</li>";
  html += "<li><b>rs485 polarityLocked: </b>" + String(rs485Stats.polarityLocked) + "</li>";
  html += "<li><b>rs485 mode: </b>" + String(rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal") + "</li>";
  html += "<li><b>rs485 detectPhase: </b>" + String(rs485Stats.polarityLocked ? "2" : (rs485Stats.polarityInverted ? "1" : "0")) + "</li>";
  html += "<li><b>rs485 lastByteMs: </b>" + formatNumberWithCommas(rs485Stats.lastByteMs) + "</li>";
  html += "<li><b>rs485 lastValidFrameMs: </b>" + formatNumberWithCommas(rs485Stats.lastValidFrameMs) + "</li>";
#endif

  appendWifiStateSection(html);
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaStatusData.lastUpdate) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaStatusData.magicNumber) + "</li>";

  html += "</ul></section><section class='panel'><h1>Configuration Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaConfigurationData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaConfigurationData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaConfigurationData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaConfigurationData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaConfigurationData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Preferences Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaPreferencesData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaPreferencesData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaPreferencesData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaPreferencesData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaPreferencesData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Filters Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaFilterSettingsData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaFilterSettingsData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaFilterSettingsData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaFilterSettingsData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaFilterSettingsData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Information Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaInformationData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaInformationData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaInformationData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaInformationData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaInformationData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>Fault Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaFaultLogData.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaFaultLogData.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaFaultLogData.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaFaultLogData)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaFaultLogData)) + "</li>";

  html += "</ul></section><section class='panel'><h1>spaSettings0x04Data Status</h1><ul>";
  html += "<li><b>lastUpdate: </b>" + formatNumberWithCommas(spaSettings0x04Data.lastUpdate) + "</li>";
  html += "<li><b>lastRequest: </b>" + formatNumberWithCommas(spaSettings0x04Data.lastRequest) + "</li>";
  html += "<li><b>magicNumber: </b>" + String(spaSettings0x04Data.magicNumber) + "</li>";
  html += "<li><b>staleData: </b>" + String(staleData(spaSettings0x04Data)) + "</li>";
  html += "<li><b>retryRequest: </b>" + String(retryRequest(spaSettings0x04Data)) + "</li>";

  html += "</ul></section></div></main></div></body></html>";

  request->send(200, "text/html", html);
  Log.verbose("[Web]: handleStatus %p %s %s" CR, request->client()->remoteIP(), request->methodToString(), request->url().c_str());

  // Log.verbose(F("[Web]: Response sent %s" CR), html.c_str());
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

void handleRs485(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(768);
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
  doc["polaritySwitchesToday"] = rs485Stats.polaritySwitchesToday;
  doc["polaritySwitchesYesterday"] = rs485Stats.polaritySwitchesYesterday;
  doc["polarityInverted"] = rs485Stats.polarityInverted;
  doc["polarityLocked"] = rs485Stats.polarityLocked;
  doc["mode"] = rs485Stats.polarityInverted ? "inverted_rx_tx" : "normal";
  doc["detectPhase"] = rs485Stats.polarityLocked ? 2 : (rs485Stats.polarityInverted ? 1 : 0);
  doc["lastByteMs"] = rs485Stats.lastByteMs;
  doc["lastValidFrameMs"] = rs485Stats.lastValidFrameMs;
  doc["health"] = rs485HealthCode();

  serializeJson(doc, *response);
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
    tinyxml2::XMLElement *deviceRequestElement = xmlDocument.FirstChildElement("sci_request")
                                                     ->FirstChildElement("data_service")
                                                     ->FirstChildElement("requests")
                                                     ->FirstChildElement("device_request");

    const char *targetName = deviceRequestElement->Attribute("target_name");

    // Get the value inside the <device_request> element
    const char *deviceRequestValue = deviceRequestElement->GetText();

    Log.verbose("[Web]: Button requested %s %s" CR, targetName, deviceRequestValue);
    // response = encodeResponse(spaFilterSettingsData.rawData, spaFilterSettingsData.rawDataLength);
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
