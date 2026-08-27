#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 0
#include <ElegantOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>
#include <math.h>

// =====================================================
// USER SETTINGS
// =====================================================

// Factory/default Wi-Fi credentials.
// Saved credentials in NVS override these values.
const char* DEFAULT_WIFI_SSID     = "";
const char* DEFAULT_WIFI_PASSWORD = "";

// Runtime Wi-Fi credentials loaded from ESP32 NVS.
String wifiSSID;
String wifiPassword;

Preferences preferences;

// Fallback Access Point used when normal Wi-Fi is unavailable.
// Change the password if desired. Minimum 8 characters for WPA2 AP mode.
const char* AP_SSID     = "Nidec-Fan-Setup";
const char* AP_PASSWORD = "fancontrol";

// ElegantOTA authentication.
// CHANGE THESE before deployment.
const char* OTA_USERNAME = "admin";
const char* OTA_PASSWORD = "";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS  = 30000UL;
const unsigned long WIFI_LOST_AP_DELAY_MS   = 20000UL;

// Set your location here
const double LATITUDE  = 19.2183;
const double LONGITUDE = 72.9781;

// India Standard Time (UTC + 5:30)
const long GMT_OFFSET_SEC      = 19800;
const int  DAYLIGHT_OFFSET_SEC = 0;

// Wind -> fan mapping
const float WIND_MIN_KMH = 0.0;
const float WIND_MAX_KMH = 50.0;
const float WIND_MIN_PWM_PERCENT = 10.0f;
const float WIND_MAX_PWM_PERCENT = 50.0f;

// Smooth fan transitions instead of sudden jumps.
const float PWM_RAMP_STEP_PERCENT = 0.25f;
const unsigned long PWM_RAMP_INTERVAL_MS = 50UL;

// Weather refresh interval
const unsigned long WEATHER_INTERVAL_MS = 1UL * 60UL * 1000UL;

// =====================================================
// PINS
// =====================================================

#define FAN_PWM_PIN   33
#define FAN_TACH_PIN  32
#define SDA_PIN       25
#define SCL_PIN       26

// =====================================================
// OLED - SH1106 128x64 using U8g2
// =====================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE
);

const uint8_t OLED_BRIGHTNESS = 100;

// OLED anti-burn-in movement
// The WHOLE information panel moves together like a ping-pong ball.
// Text structure stays fixed relative to itself while the panel bounces
// from the left/right/top/bottom edges of the display.
unsigned long lastDisplayMove = 0;
const unsigned long DISPLAY_MOVE_INTERVAL_MS = 90;

int displayOffsetX = 0;
int displayOffsetY = 0;
int displayDirectionX = 1;
int displayDirectionY = 1;

// Compact panel size. Using the 5x8 font lets the complete information
// block travel through a much larger portion of the 128x64 OLED.
const int MAIN_PANEL_WIDTH  = 84;
const int MAIN_PANEL_HEIGHT = 43;

// =====================================================
// PWM / TACH
// =====================================================

#define PWM_FREQ       25000
#define PWM_RESOLUTION 10
#define PWM_MAX        1023

#define TACH_PULSES_PER_REV 2

volatile uint32_t tachPulses = 0;
float currentRPM = 0.0;
float currentPulsesPerSecond = 0.0;

unsigned long lastRPMTime = 0;
const unsigned long RPM_INTERVAL_MS = 1000;

// =====================================================
// FAN CONTROL
// =====================================================

enum FanMode {
  MODE_WIND,
  MODE_MANUAL
};

FanMode fanMode = MODE_WIND;

float manualPWMPercent = 30.0f;
float requestedPWMPercent = 10.0f;
float activePWMPercent = 10.0f;

unsigned long lastPWMRamp = 0;

float currentWindKmh = 0.0f;
bool windDataValid = false;
unsigned long lastWeatherFetch = 0;
unsigned long lastWeatherSuccess = 0;
unsigned long lastWeatherFailure = 0;

// Request OLED interception animation after a successful wind-data update.
// The animation is played from loop() so the HTTP fetch can finish cleanly first.
bool interceptionRequested = false;


// =====================================================
// SAVED WIFI CREDENTIALS
// =====================================================

void loadWiFiCredentials() {
  preferences.begin("wifi", true);

  wifiSSID = preferences.getString(
    "ssid",
    DEFAULT_WIFI_SSID
  );

  wifiPassword = preferences.getString(
    "password",
    DEFAULT_WIFI_PASSWORD
  );

  preferences.end();

  wifiSSID.trim();

  Serial.print("Configured WiFi SSID: ");
  Serial.println(wifiSSID.length() ? wifiSSID : "(none)");
}

bool saveWiFiCredentials(
  const String& newSSID,
  const String& newPassword
) {
  if (newSSID.length() == 0) {
    return false;
  }

  preferences.begin("wifi", false);

  size_t writtenSSID =
    preferences.putString("ssid", newSSID);

  size_t writtenPassword =
    preferences.putString("password", newPassword);

  preferences.end();

  return writtenSSID > 0 &&
         writtenPassword > 0;
}

// =====================================================
// NETWORK / NTP
// =====================================================

WebServer server(80);
DNSServer dnsServer;

bool apMode = false;
bool otaInProgress = false;
unsigned int otaProgressPercent = 0;

unsigned long lastWiFiRetry = 0;
unsigned long wifiLostSince = 0;

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
const char* NTP_SERVER_3 = "time.cloudflare.com";

// =====================================================
// WEB PAGE
// =====================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nidec Wind Fan</title>
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#0f172a;color:#fff;text-align:center;margin:0;padding:16px}
.card{max-width:430px;margin:auto;background:#1e293b;border-radius:18px;padding:16px;box-shadow:0 8px 30px #0005}
h2{margin:2px 0 4px}
.sub{font-size:12px;color:#94a3b8;margin-bottom:12px}
.banner{padding:9px 12px;border-radius:10px;margin:10px 0;font-size:13px;font-weight:bold}
.online{background:#14532d;color:#bbf7d0}.offline{background:#7f1d1d;color:#fecaca}.ap{background:#78350f;color:#fde68a}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.metric{padding:13px;background:#111827;border-radius:12px}
.metric.wide{grid-column:1/-1}
.label{font-size:12px;color:#94a3b8}
.big{font-size:27px;font-weight:bold;color:#38bdf8;margin-top:3px}
.meta{font-size:12px;color:#cbd5e1;margin-top:5px}
.row{margin:10px 0;padding:12px;background:#111827;border-radius:12px}
.switch{position:relative;display:inline-block;width:58px;height:32px}.switch input{display:none}
.sliderSwitch{position:absolute;cursor:pointer;inset:0;background:#475569;border-radius:30px;transition:.2s}
.sliderSwitch:before{content:"";position:absolute;height:24px;width:24px;left:4px;top:4px;background:#fff;border-radius:50%;transition:.2s}
input:checked+.sliderSwitch{background:#22c55e}input:checked+.sliderSwitch:before{transform:translateX(26px)}
input[type=range]{width:92%;accent-color:#38bdf8}
button{border:0;border-radius:12px;padding:12px 15px;font-size:14px;color:#fff;background:#2563eb;cursor:pointer;margin:4px}
button.warn{background:#f97316}.disabled{opacity:.42}
.actions{display:flex;gap:8px;justify-content:center;flex-wrap:wrap}
.statusline{font-size:12px;color:#cbd5e1;line-height:1.55;margin-top:10px}
</style>
</head>
<body>
<div class="card">
  <h2>Nidec Wind Fan</h2>
  <div class="sub">Wind-aware PWM controller</div>

  <div id="netBanner" class="banner offline">Connecting...</div>

  <div class="grid">
    <div class="metric">
      <div class="label">Wind speed</div>
      <div class="big"><span id="wind">--</span> km/h</div>
      <div class="meta" id="windAge">No weather data</div>
    </div>
    <div class="metric">
      <div class="label">Fan RPM</div>
      <div class="big"><span id="rpm">--</span></div>
      <div class="meta"><span id="rssi">--</span></div>
    </div>
    <div class="metric wide">
      <div class="label">PWM output / target</div>
      <div class="big"><span id="pwm">--</span>% <span style="font-size:16px;color:#94a3b8">-> <span id="targetPwm">--</span>%</span></div>
    </div>
  </div>

  <div class="row">
    <div class="label">Control mode</div>
    <div style="display:flex;justify-content:center;align-items:center;gap:12px;margin-top:8px">
      <span>Manual</span>
      <label class="switch">
        <input id="modeToggle" type="checkbox">
        <span class="sliderSwitch"></span>
      </label>
      <span>Wind</span>
    </div>
  </div>

  <div class="row" id="manualBox">
    <div class="label">Manual fan speed</div>
    <input id="manualSlider" type="range" min="10" max="100" step="1" value="30">
    <div><span id="manualValue">30</span>%</div>
  </div>

  <div class="row" id="wifiSetupBox" style="display:none">
    <div class="label">Wi-Fi setup</div>

    <select id="wifiSSIDSelect"
            style="width:92%;margin-top:10px;padding:10px;border-radius:8px;background:#0f172a;color:white;border:1px solid #475569">
      <option value="">Select Wi-Fi network</option>
    </select>

    <input id="wifiSSIDManual"
           type="text"
           placeholder="Or enter SSID manually"
           autocomplete="off"
           style="width:92%;margin-top:8px;padding:10px;border-radius:8px;background:#0f172a;color:white;border:1px solid #475569">

    <input id="wifiPassword"
           type="password"
           placeholder="Wi-Fi password"
           autocomplete="new-password"
           style="width:92%;margin-top:8px;padding:10px;border-radius:8px;background:#0f172a;color:white;border:1px solid #475569">

    <div class="actions" style="margin-top:10px">
      <button id="wifiScanBtn">Scan Networks</button>
      <button id="wifiSaveBtn">Save & Reboot</button>
    </div>

    <div class="meta" id="wifiSetupMsg">
      Saved password is never displayed.
    </div>
  </div>

  <div class="row actions">
    <button id="interceptBtn" class="warn">Missile Interception</button>
    <button id="otaBtn">Firmware Update (OTA)</button>
  </div>

  <div class="statusline">
    <div id="networkInfo">IP: --</div>
    <div id="weatherState">Weather: --</div>
  </div>
</div>

<script>
let sliderTimer = null;

async function refresh(){
  try{
    const r = await fetch('/status', {cache:'no-store'});
    const d = await r.json();

    document.getElementById('wind').textContent = d.wind_valid ? Number(d.wind).toFixed(1) : '--';
    document.getElementById('pwm').textContent = Number(d.pwm).toFixed(1);
    document.getElementById('targetPwm').textContent = Number(d.target_pwm).toFixed(1);
    document.getElementById('rpm').textContent = Math.round(d.rpm);

    const windAge = document.getElementById('windAge');
    if(d.wind_valid){
      windAge.textContent = d.weather_age >= 0 ? ('Updated ' + d.weather_age + ' sec ago') : 'Updated';
    }else{
      windAge.textContent = 'No weather data';
    }

    const toggle = document.getElementById('modeToggle');
    toggle.checked = (d.mode === 'wind');

    const slider = document.getElementById('manualSlider');
    if(document.activeElement !== slider){
      slider.value = Math.round(d.manual);
      document.getElementById('manualValue').textContent = Math.round(d.manual);
    }

    const manual = d.mode === 'manual';
    document.getElementById('manualBox').className = manual ? 'row' : 'row disabled';
    slider.disabled = !manual;

    const banner = document.getElementById('netBanner');
    if(d.ap_mode){
      banner.className = 'banner ap';
      banner.textContent = 'OFFLINE - FALLBACK AP';
    }else if(d.online){
      banner.className = 'banner online';
      banner.textContent = 'ONLINE - ' + d.ssid;
    }else{
      banner.className = 'banner offline';
      banner.textContent = 'OFFLINE';
    }

    document.getElementById('rssi').textContent =
      d.online ? (d.rssi + ' dBm') : 'No STA link';

    document.getElementById('networkInfo').textContent =
      'IP: ' + d.ip + (d.online ? (' | ' + d.ssid) : '');

    document.getElementById('weatherState').textContent =
      d.wind_valid
        ? ('Weather: ' + (d.online ? 'current/last successful' : 'STALE - no Internet'))
        : 'Weather: unavailable';

    document.getElementById('otaBtn').disabled = d.ota_active;
    document.getElementById('otaBtn').textContent =
      d.ota_active ? ('OTA ' + d.ota_progress + '%') : 'Firmware Update (OTA)';

    document.getElementById('wifiSetupBox').style.display =
      d.ap_mode ? 'block' : 'none';
  }catch(e){
    const banner=document.getElementById('netBanner');
    banner.className='banner offline';
    banner.textContent='Controller not responding';
  }
}

function setMode(){
  const mode = document.getElementById('modeToggle').checked ? 'wind' : 'manual';
  fetch('/mode?value=' + mode).then(refresh);
}

function sendManual(v){
  fetch('/manual?value=' + encodeURIComponent(v));
}

function manualChanged(v){
  document.getElementById('manualValue').textContent = v;
  clearTimeout(sliderTimer);
  sliderTimer = setTimeout(()=>sendManual(v), 120);
}


async function scanWifi(){
  const msg = document.getElementById('wifiSetupMsg');
  const select = document.getElementById('wifiSSIDSelect');

  msg.textContent = 'Scanning...';
  select.innerHTML = '<option value="">Scanning...</option>';

  try{
    const r = await fetch('/wifi-scan', {cache:'no-store'});
    const d = await r.json();

    select.innerHTML = '<option value="">Select Wi-Fi network</option>';

    if(!Array.isArray(d.networks) || d.networks.length === 0){
      msg.textContent = 'No Wi-Fi networks found.';
      return;
    }

    d.networks.forEach(n=>{
      const o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
      select.appendChild(o);
    });

    msg.textContent = d.networks.length + ' network(s) found.';
  }catch(e){
    select.innerHTML = '<option value="">Select Wi-Fi network</option>';
    msg.textContent = 'Wi-Fi scan failed.';
  }
}

async function saveWifi(){
  const selectSSID =
    document.getElementById('wifiSSIDSelect').value.trim();

  const manualSSID =
    document.getElementById('wifiSSIDManual').value.trim();

  const ssid =
    manualSSID.length ? manualSSID : selectSSID;

  const password =
    document.getElementById('wifiPassword').value;

  const msg =
    document.getElementById('wifiSetupMsg');

  if(!ssid){
    msg.textContent = 'Please select or enter an SSID.';
    return;
  }

  msg.textContent = 'Saving Wi-Fi settings...';

  const body =
    'ssid=' + encodeURIComponent(ssid) +
    '&password=' + encodeURIComponent(password);

  try{
    const r = await fetch('/wifi-save', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:body
    });

    if(!r.ok){
      msg.textContent = 'Failed to save Wi-Fi settings.';
      return;
    }

    msg.textContent = 'Saved. ESP32 is rebooting...';
  }catch(e){
    msg.textContent = 'ESP32 is rebooting...';
  }
}

const modeToggle = document.getElementById('modeToggle');
modeToggle.addEventListener('change', setMode);

const manualSlider = document.getElementById('manualSlider');
manualSlider.addEventListener('input', e=>manualChanged(e.target.value));
manualSlider.addEventListener('change', e=>{
  clearTimeout(sliderTimer);
  sendManual(e.target.value);
});

document.getElementById('interceptBtn').addEventListener('click', ()=>fetch('/intercept'));
document.getElementById('otaBtn').addEventListener('click', ()=>window.location.href='/update');
document.getElementById('wifiScanBtn').addEventListener('click', scanWifi);
document.getElementById('wifiSaveBtn').addEventListener('click', saveWifi);
document.getElementById('wifiSSIDSelect').addEventListener('change', e=>{
  if(e.target.value){
    document.getElementById('wifiSSIDManual').value = '';
  }
});

setInterval(refresh,1000);
refresh();
</script>
</body>
</html>
)rawliteral";

// =====================================================
// HELPERS
// =====================================================

void IRAM_ATTR tachISR() {
  tachPulses++;
}

void writeFanPWM(float percent) {
  percent = constrain(percent, 0.0f, 100.0f);
  activePWMPercent = percent;

  int gpioDuty = PWM_MAX - (int)roundf((percent * PWM_MAX) / 100.0f);
  gpioDuty = constrain(gpioDuty, 0, PWM_MAX);
  ledcWrite(FAN_PWM_PIN, gpioDuty);
}

void requestFanPWM(float percent) {
  requestedPWMPercent = constrain(percent, 0.0f, 100.0f);
}

float windToPWM(float windKmh) {
  if (windKmh <= WIND_MIN_KMH) return WIND_MIN_PWM_PERCENT;
  if (windKmh >= WIND_MAX_KMH) return WIND_MAX_PWM_PERCENT;

  float fraction = (windKmh - WIND_MIN_KMH) / (WIND_MAX_KMH - WIND_MIN_KMH);
  float pct = WIND_MIN_PWM_PERCENT +
              fraction * (WIND_MAX_PWM_PERCENT - WIND_MIN_PWM_PERCENT);
  return constrain(pct, WIND_MIN_PWM_PERCENT, WIND_MAX_PWM_PERCENT);
}

void applyFanControl() {
  if (fanMode == MODE_WIND) {
    float target = windDataValid ? windToPWM(currentWindKmh) : WIND_MIN_PWM_PERCENT;
    requestFanPWM(target);
  } else {
    requestFanPWM(manualPWMPercent);
  }
}

void updateFanRamp() {
  unsigned long now = millis();
  if (now - lastPWMRamp < PWM_RAMP_INTERVAL_MS) return;
  lastPWMRamp = now;

  float diff = requestedPWMPercent - activePWMPercent;

  if (fabsf(diff) <= PWM_RAMP_STEP_PERCENT) {
    if (fabsf(diff) > 0.001f) writeFanPWM(requestedPWMPercent);
    return;
  }

  if (diff > 0.0f)
    writeFanPWM(activePWMPercent + PWM_RAMP_STEP_PERCENT);
  else
    writeFanPWM(activePWMPercent - PWM_RAMP_STEP_PERCENT);
}

// =====================================================
// WEATHER
// =====================================================

bool fetchWindSpeed() {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[320];
  snprintf(
    url,
    sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=wind_speed_10m&wind_speed_unit=kmh",
    LATITUDE,
    LONGITUDE
  );

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("Weather HTTP begin failed");
    return false;
  }

  http.setTimeout(5000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP error: %d\n", code);
    http.end();
    lastWeatherFailure = millis();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Weather JSON error: ");
    Serial.println(err.c_str());
    lastWeatherFailure = millis();
    return false;
  }

  if (!doc["current"]["wind_speed_10m"].is<float>() &&
      !doc["current"]["wind_speed_10m"].is<int>()) {
    Serial.println("Wind value not found in weather response");
    lastWeatherFailure = millis();
    return false;
  }

  currentWindKmh = doc["current"]["wind_speed_10m"].as<float>();
  windDataValid = true;
  lastWeatherFetch = millis();
  lastWeatherSuccess = millis();

  Serial.printf("Wind: %.1f km/h\n", currentWindKmh);
  applyFanControl();

  // Trigger the existing interception animation only after a
  // successful weather response and valid wind-speed update.
  interceptionRequested = true;

  return true;
}

// =====================================================
// OLED
// =====================================================

void updateDisplayBounce() {
  if (millis() - lastDisplayMove < DISPLAY_MOVE_INTERVAL_MS) return;
  lastDisplayMove = millis();

  displayOffsetX += displayDirectionX;
  displayOffsetY += displayDirectionY;

  const int maxX = 128 - MAIN_PANEL_WIDTH;
  const int maxY = 64 - MAIN_PANEL_HEIGHT;

  if (displayOffsetX >= maxX) {
    displayOffsetX = maxX;
    displayDirectionX = -1;
  } else if (displayOffsetX <= 0) {
    displayOffsetX = 0;
    displayDirectionX = 1;
  }

  if (displayOffsetY >= maxY) {
    displayOffsetY = maxY;
    displayDirectionY = -1;
  } else if (displayOffsetY <= 0) {
    displayOffsetY = 0;
    displayDirectionY = 1;
  }
}

void showMessage(const char* line1, const char* line2 = "") {
  // Startup messages remain structured and shift together slightly.
  static int msgX = 0, msgY = 0;
  static int msgDX = 1, msgDY = 1;

  u8g2.setFont(u8g2_font_6x13_tf);
  int w1 = u8g2.getStrWidth(line1);
  int w2 = strlen(line2) ? u8g2.getStrWidth(line2) : 0;
  int blockW = max(w1, w2);
  int blockH = strlen(line2) ? 30 : 14;

  int maxX = max(0, 128 - blockW);
  int maxY = max(0, 64 - blockH);

  msgX += msgDX;
  msgY += msgDY;
  if (msgX >= maxX) { msgX = maxX; msgDX = -1; }
  if (msgX <= 0)    { msgX = 0;    msgDX = 1;  }
  if (msgY >= maxY) { msgY = maxY; msgDY = -1; }
  if (msgY <= 0)    { msgY = 0;    msgDY = 1;  }

  u8g2.clearBuffer();
  u8g2.drawStr(msgX, msgY + 12, line1);
  if (strlen(line2) > 0)
    u8g2.drawStr(msgX, msgY + 28, line2);
  u8g2.sendBuffer();
}

void showIPAddress() {
  String ip = WiFi.localIP().toString();
  unsigned long started = millis();

  int x = 0, y = 0;
  int dx = 1, dy = 1;

  u8g2.setFont(u8g2_font_6x13_tf);
  const char* title = "WiFi connected";
  int titleW = u8g2.getStrWidth(title);

  u8g2.setFont(u8g2_font_7x14B_tf);
  int ipW = u8g2.getStrWidth(ip.c_str());

  int blockW = max(titleW, ipW);
  int blockH = 34;
  int maxX = max(0, 128 - blockW);
  int maxY = max(0, 64 - blockH);

  while (millis() - started < 3500) {
    x += dx;
    y += dy;
    if (x >= maxX) { x = maxX; dx = -1; }
    if (x <= 0)    { x = 0;    dx = 1;  }
    if (y >= maxY) { y = maxY; dy = -1; }
    if (y <= 0)    { y = 0;    dy = 1;  }

    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_6x13_tf);
    u8g2.drawStr(x, y + 12, title);

    u8g2.setFont(u8g2_font_7x14B_tf);
    u8g2.drawStr(x, y + 31, ip.c_str());

    u8g2.sendBuffer();
    delay(45);
  }
}

void drawMainScreen() {
  updateDisplayBounce();

  char windLine[24];
  char pwmLine[24];
  char rpmLine[24];
  char timeLine[24];

  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo, 50);

  if (windDataValid)
    snprintf(windLine, sizeof(windLine), "Wind: %.1f km/h", currentWindKmh);
  else
    snprintf(windLine, sizeof(windLine), "Wind: --.- km/h");

  snprintf(
    pwmLine,
    sizeof(pwmLine),
    "PWM : %.1f%% %s",
    activePWMPercent,
    fanMode == MODE_WIND ? "W" : "M"
  );

  snprintf(rpmLine, sizeof(rpmLine), "RPM : %.0f", currentRPM);

  if (haveTime)
    strftime(timeLine, sizeof(timeLine), "%d/%m %H:%M:%S", &timeinfo);
  else
    snprintf(timeLine, sizeof(timeLine), "Time syncing...");

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  // IMPORTANT: all four lines use exactly the SAME X/Y offset.
  // This preserves the panel structure while the complete panel bounces.
  int x = displayOffsetX;
  int y = displayOffsetY;

  u8g2.drawStr(x, y + 8,  windLine);
  u8g2.drawStr(x, y + 19, pwmLine);
  u8g2.drawStr(x, y + 30, rpmLine);
  u8g2.drawStr(x, y + 41, timeLine);

  u8g2.sendBuffer();
}

void serviceNetworkDuringAnimation() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  ElegantOTA.loop();
}

void responsiveDelay(unsigned long durationMs) {
  unsigned long started = millis();
  while (millis() - started < durationMs) {
    serviceNetworkDuringAnimation();
    delay(2);
  }
}

// =====================================================
// IRON-DOME-STYLE ANIMATION - TWO INCOMING MISSILES
// =====================================================
// =====================================================
// CURVED TRAJECTORY HELPER
// Quadratic Bezier curve
// =====================================================

float bezierPoint(float start, float control, float end, float t)
{
  float inv = 1.0f - t;

  return
    (inv * inv * start) +
    (2.0f * inv * t * control) +
    (t * t * end);
}


// =====================================================
// REALISTIC TWO-MISSILE INTERCEPTION ANIMATION
// =====================================================

void showInterceptionAnimation()
{
  const int MISSILES = 2;

  // ---------------------------------------------------
  // Incoming missile starting positions
  // ---------------------------------------------------

  float incomingStartX[MISSILES];

  incomingStartX[0] =
    (float)random(5, 60);

  incomingStartX[1] =
    (float)random(68, 123);

  // Occasionally allow missiles to cross sides
  if (random(0, 3) == 0)
  {
    incomingStartX[0] =
      (float)random(75, 123);
  }

  if (random(0, 3) == 0)
  {
    incomingStartX[1] =
      (float)random(5, 53);
  }


  float incomingStartY[MISSILES] =
  {
    -2.0f,
    -2.0f
  };


  // ---------------------------------------------------
  // Interception points
  // ---------------------------------------------------

  float interceptX[MISSILES] =
  {
    (float)random(25, 62),
    (float)random(67, 108)
  };

  float interceptY[MISSILES] =
  {
    (float)random(20, 37),
    (float)random(18, 39)
  };


  // ---------------------------------------------------
  // Curvature of incoming missiles
  // ---------------------------------------------------

  float incomingControlX[MISSILES];
  float incomingControlY[MISSILES];

  for (int m = 0; m < MISSILES; m++)
  {
    incomingControlX[m] =
      (
        incomingStartX[m] +
        interceptX[m]
      ) / 2.0f
      +
      random(-18, 19);

    incomingControlY[m] =
      random(6, 18);
  }


  // ---------------------------------------------------
  // Interceptor launch positions
  // ---------------------------------------------------

  float interceptorStartX[MISSILES];

  // Launchers aren't always in exactly the same position

  interceptorStartX[0] =
    (float)random(8, 35);

  interceptorStartX[1] =
    (float)random(93, 120);

  float interceptorStartY[MISSILES] =
  {
    62.0f,
    62.0f
  };


  // ---------------------------------------------------
  // Curved interceptor trajectory control points
  // ---------------------------------------------------

  float interceptorControlX[MISSILES];
  float interceptorControlY[MISSILES];

  for (int m = 0; m < MISSILES; m++)
  {
    // Push the curve sideways rather than flying straight
    // at the incoming missile.

    float midpoint =
      (
        interceptorStartX[m] +
        interceptX[m]
      ) / 2.0f;

    interceptorControlX[m] =
      midpoint +
      random(-25, 26);

    // Interceptor climbs strongly first
    interceptorControlY[m] =
      random(30, 47);
  }


  // ---------------------------------------------------
  // Detection / reaction delay
  // ---------------------------------------------------
  //
  // Incoming missiles appear first.
  // Interceptors launch only after several frames.
  // ---------------------------------------------------

  int interceptorLaunchFrame[MISSILES];

  interceptorLaunchFrame[0] =
    random(12, 20);

  interceptorLaunchFrame[1] =
    random(18, 29);


  // ---------------------------------------------------
  // Independent intercept times
  // ---------------------------------------------------

  int interceptFrame[MISSILES];

  interceptFrame[0] =
    random(47, 57);

  interceptFrame[1] =
    random(52, 64);


  int totalFrames =
    max(
      interceptFrame[0],
      interceptFrame[1]
    ) + 22;


  // ===================================================
  // MAIN ANIMATION
  // ===================================================

  for (
    int frame = 0;
    frame < totalFrames;
    frame++
  )
  {
    u8g2.clearBuffer();


    // -------------------------------------------------
    // Ground
    // -------------------------------------------------

    u8g2.drawHLine(
      0,
      63,
      128
    );


    // Defended area / city
    u8g2.drawBox(
      55,
      59,
      4,
      4
    );

    u8g2.drawBox(
      62,
      57,
      5,
      6
    );

    u8g2.drawBox(
      71,
      60,
      4,
      3
    );


    // =================================================
    // EACH MISSILE
    // =================================================

    for (
      int m = 0;
      m < MISSILES;
      m++
    )
    {

      // =================================================
      // INCOMING MISSILE
      // =================================================

      if (frame <= interceptFrame[m])
      {
        float p =
          (float)frame /
          (float)interceptFrame[m];

        p = constrain(
          p,
          0.0f,
          1.0f
        );


        // Mild acceleration toward target
        float easedP =
          p * p *
          (
            3.0f -
            2.0f * p
          );


        float ix =
          bezierPoint(
            incomingStartX[m],
            incomingControlX[m],
            interceptX[m],
            easedP
          );

        float iy =
          bezierPoint(
            incomingStartY[m],
            incomingControlY[m],
            interceptY[m],
            easedP
          );


        // Missile body
        if (
          ix >= 0 &&
          ix < 128 &&
          iy >= 0 &&
          iy < 64
        )
        {
          u8g2.drawDisc(
            (int)ix,
            (int)iy,
            1
          );
        }


        // ------------------------------------------------
        // Incoming missile trail
        // ------------------------------------------------

        for (
          int t = 1;
          t <= 7;
          t++
        )
        {
          float tp =
            easedP -
            t * 0.022f;

          if (tp < 0)
            continue;


          float tx =
            bezierPoint(
              incomingStartX[m],
              incomingControlX[m],
              interceptX[m],
              tp
            );


          float ty =
            bezierPoint(
              incomingStartY[m],
              incomingControlY[m],
              interceptY[m],
              tp
            );


          // Broken/flickering trail
          if (
            ((frame + t + m) % 2) == 0 &&
            tx >= 0 &&
            tx < 128 &&
            ty >= 0 &&
            ty < 64
          )
          {
            u8g2.drawPixel(
              (int)tx,
              (int)ty
            );
          }
        }
      }


      // =================================================
      // INTERCEPTOR
      // =================================================

      if (
        frame >= interceptorLaunchFrame[m] &&
        frame <= interceptFrame[m]
      )
      {
        float p =
          (float)
          (
            frame -
            interceptorLaunchFrame[m]
          )
          /
          (float)
          (
            interceptFrame[m] -
            interceptorLaunchFrame[m]
          );


        p = constrain(
          p,
          0.0f,
          1.0f
        );


        // Fast acceleration immediately after launch
        float accelerated =
          1.0f -
          powf(
            1.0f - p,
            2.2f
          );


        float mx =
          bezierPoint(
            interceptorStartX[m],
            interceptorControlX[m],
            interceptX[m],
            accelerated
          );


        float my =
          bezierPoint(
            interceptorStartY[m],
            interceptorControlY[m],
            interceptY[m],
            accelerated
          );


        // Interceptor
        if (
          mx >= 0 &&
          mx < 128 &&
          my >= 0 &&
          my < 64
        )
        {
          u8g2.drawDisc(
            (int)mx,
            (int)my,
            1
          );
        }


        // ------------------------------------------------
        // Exhaust trail
        // ------------------------------------------------

        for (
          int t = 1;
          t <= 9;
          t++
        )
        {
          float tp =
            accelerated -
            t * 0.025f;

          if (tp < 0)
            continue;


          float tx =
            bezierPoint(
              interceptorStartX[m],
              interceptorControlX[m],
              interceptX[m],
              tp
            );


          float ty =
            bezierPoint(
              interceptorStartY[m],
              interceptorControlY[m],
              interceptY[m],
              tp
            );


          if (
            tx >= 0 &&
            tx < 128 &&
            ty >= 0 &&
            ty < 64
          )
          {
            // Exhaust intentionally irregular
            if (
              random(0, 5) != 0
            )
            {
              u8g2.drawPixel(
                (int)tx,
                (int)ty
              );
            }
          }
        }


        // Random hot exhaust spark
        if (
          random(0, 3) == 0
        )
        {
          int sx =
            (int)mx +
            random(-2, 3);

          int sy =
            (int)my +
            random(2, 5);

          if (
            sx >= 0 &&
            sx < 128 &&
            sy >= 0 &&
            sy < 64
          )
          {
            u8g2.drawPixel(
              sx,
              sy
            );
          }
        }
      }


      // =================================================
      // INTERCEPTION EXPLOSION
      // =================================================

      if (
        frame >= interceptFrame[m]
      )
      {
        int age =
          frame -
          interceptFrame[m];


        if (age < 18)
        {
          // Initial bright flash
          if (age < 3)
          {
            u8g2.drawDisc(
              (int)interceptX[m],
              (int)interceptY[m],
              2
            );
          }


          // Expanding shock ring
          if (age < 10)
          {
            int radius =
              1 + age;

            if (radius > 7)
              radius = 7;

            u8g2.drawCircle(
              (int)interceptX[m],
              (int)interceptY[m],
              radius
            );
          }


          // ------------------------------------------------
          // Explosion fragments
          // ------------------------------------------------

          for (
            int p = 0;
            p < 14;
            p++
          )
          {
            float angle =
              (
                p * 0.45f
              )
              +
              (
                m * 0.31f
              );


            float distance =
              age *
              (
                0.35f +
                (p % 5) *
                0.10f
              );


            int px =
              (int)interceptX[m] +
              (int)
              (
                cosf(angle) *
                distance
              );


            // Slight gravity
            int py =
              (int)interceptY[m] +
              (int)
              (
                sinf(angle) *
                distance
              )
              +
              (
                age *
                age
              ) / 55;


            if (
              px >= 0 &&
              px < 128 &&
              py >= 0 &&
              py < 64 &&
              random(0, 20) > age
            )
            {
              u8g2.drawPixel(
                px,
                py
              );
            }
          }
        }
      }
    }


    // =================================================
    // DISPLAY FRAME
    // =================================================

    u8g2.sendBuffer();

    responsiveDelay(40);
  }


  // ===================================================
  // END ANIMATION
  // ===================================================

  u8g2.clearBuffer();
  u8g2.sendBuffer();

  responsiveDelay(120);
}
// =====================================================
// FALLBACK AP / CAPTIVE PORTAL
// =====================================================

void startFallbackAP() {
  if (apMode) return;

  Serial.println();
  Serial.println("Normal WiFi unavailable - starting fallback AP");

  // Keep STA enabled so the ESP32 can continue retrying the configured Wi-Fi
  // while also exposing its own local access point.
  WiFi.mode(WIFI_AP_STA);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("Failed to start fallback AP");
    return;
  }

  apMode = true;

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);

  Serial.print("Fallback AP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Fallback AP IP: ");
  Serial.println(apIP);
  Serial.print("Captive portal / control page: http://");
  Serial.println(apIP);
  Serial.print("ElegantOTA: http://");
  Serial.print(apIP);
  Serial.println("/update");

  // Move the AP information while it is shown so it is not static on the OLED.
  for (int i = 0; i < 10; i++) {
    showMessage(i % 2 ? AP_SSID : "Fallback AP", apIP.toString().c_str());
    delay(180);
  }
}

void stopFallbackAP() {
  if (!apMode) return;
  if (otaInProgress) {
    Serial.println("Fallback AP retained because OTA is in progress");
    return;
  }

  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apMode = false;

  Serial.println("Fallback AP stopped - station WiFi restored");
}

void handleCaptivePortalRedirect() {
  if (apMode) {
    String target = String("http://") + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Location", target, true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// =====================================================
// WEB HANDLERS
// =====================================================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  JsonDocument doc;

  bool online = (WiFi.status() == WL_CONNECTED);
  long weatherAge = -1;
  if (windDataValid && lastWeatherSuccess > 0) {
    weatherAge = (long)((millis() - lastWeatherSuccess) / 1000UL);
  }

  doc["wind"] = currentWindKmh;
  doc["wind_valid"] = windDataValid;
  doc["weather_age"] = weatherAge;

  doc["pwm"] = activePWMPercent;
  doc["target_pwm"] = requestedPWMPercent;
  doc["rpm"] = currentRPM;
  doc["manual"] = manualPWMPercent;
  doc["mode"] = fanMode == MODE_WIND ? "wind" : "manual";

  doc["online"] = online;
  doc["ap_mode"] = apMode;
  doc["ssid"] = online ? WiFi.SSID() : "";
  doc["rssi"] = online ? WiFi.RSSI() : 0;
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  doc["ota_active"] = otaInProgress;
  doc["ota_progress"] = otaProgressPercent;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleMode() {
  String value = server.arg("value");

  if (value == "manual") fanMode = MODE_MANUAL;
  else fanMode = MODE_WIND;

  applyFanControl();
  server.send(200, "text/plain", "OK");
}

void handleManual() {
  if (server.hasArg("value")) {
    manualPWMPercent = constrain(server.arg("value").toFloat(), 10.0f, 100.0f);
    if (fanMode == MODE_MANUAL) applyFanControl();
  }
  server.send(200, "text/plain", "OK");
}


void handleWiFiScan() {
  if (!apMode) {
    server.send(
      403,
      "application/json",
      "{\"error\":\"WiFi setup is available only in fallback AP mode\"}"
    );
    return;
  }

  int count = WiFi.scanNetworks(
    false,   // async
    true     // show hidden
  );

  JsonDocument doc;
  JsonArray networks =
    doc["networks"].to<JsonArray>();

  // Avoid duplicate SSIDs in the dropdown.
  for (int i = 0; i < count; i++) {
    String ssid = WiFi.SSID(i);

    if (ssid.length() == 0) {
      continue;
    }

    bool duplicate = false;

    for (JsonObject n : networks) {
      if (n["ssid"].as<String>() == ssid) {
        duplicate = true;
        break;
      }
    }

    if (duplicate) {
      continue;
    }

    JsonObject network =
      networks.add<JsonObject>();

    network["ssid"] = ssid;
    network["rssi"] = WiFi.RSSI(i);
    network["channel"] = WiFi.channel(i);
    network["secure"] =
      WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }

  WiFi.scanDelete();

  String json;
  serializeJson(doc, json);

  server.send(
    200,
    "application/json",
    json
  );
}

void handleWiFiSave() {
  if (!apMode) {
    server.send(
      403,
      "text/plain",
      "WiFi setup is available only in fallback AP mode"
    );
    return;
  }

  if (!server.hasArg("ssid")) {
    server.send(
      400,
      "text/plain",
      "Missing SSID"
    );
    return;
  }

  String newSSID =
    server.arg("ssid");

  String newPassword =
    server.hasArg("password")
      ? server.arg("password")
      : "";

  newSSID.trim();

  if (newSSID.length() == 0 ||
      newSSID.length() > 32) {
    server.send(
      400,
      "text/plain",
      "Invalid SSID"
    );
    return;
  }

  if (newPassword.length() > 63) {
    server.send(
      400,
      "text/plain",
      "Password is too long"
    );
    return;
  }

  if (!saveWiFiCredentials(
        newSSID,
        newPassword
      )) {
    server.send(
      500,
      "text/plain",
      "Failed to save WiFi settings"
    );
    return;
  }

  // Update runtime variables too, although the reboot
  // below will reload them from NVS.
  wifiSSID = newSSID;
  wifiPassword = newPassword;

  Serial.print("New WiFi SSID saved: ");
  Serial.println(wifiSSID);
  Serial.println("WiFi password saved to NVS");
  Serial.println("Rebooting...");

  server.send(
    200,
    "text/html",
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "</head><body>"
    "<h2>Wi-Fi settings saved</h2>"
    "<p>The ESP32 is restarting and will try the new network.</p>"
    "</body></html>"
  );

  delay(1000);
  ESP.restart();
}

void handleIntercept() {
  // Reply first so the browser does not wait for the animation.
  server.send(200, "text/plain", "OK");
  delay(20);
  showInterceptionAnimation();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(600);

  randomSeed(esp_random());

  // I2C / OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(OLED_BRIGHTNESS);
  showMessage("Nidec Wind Fan", "v270826_0920");
  delay(2000);

  // PWM
  ledcAttach(FAN_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  requestedPWMPercent = WIND_MIN_PWM_PERCENT;
  writeFanPWM(WIND_MIN_PWM_PERCENT);

  // Tach
  pinMode(FAN_TACH_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachISR, FALLING);

  // ---------------------------------------------------
  // Web server routes are registered before Wi-Fi setup
  // so the same page works in STA mode or fallback AP mode.
  // ---------------------------------------------------
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/mode", handleMode);
  server.on("/manual", handleManual);
  server.on("/wifi-scan", HTTP_GET, handleWiFiScan);
  server.on("/wifi-save", HTTP_POST, handleWiFiSave);
  server.on("/intercept", handleIntercept);

  // Common captive-portal probe URLs used by Android / iOS / Windows.
  server.on("/generate_204", handleCaptivePortalRedirect);
  server.on("/gen_204", handleCaptivePortalRedirect);
  server.on("/hotspot-detect.html", handleCaptivePortalRedirect);
  server.on("/library/test/success.html", handleCaptivePortalRedirect);
  server.on("/ncsi.txt", handleCaptivePortalRedirect);
  server.on("/connecttest.txt", handleCaptivePortalRedirect);
  server.on("/redirect", handleCaptivePortalRedirect);
  server.on("/fwlink", handleCaptivePortalRedirect);
  server.onNotFound(handleCaptivePortalRedirect);

  // ElegantOTA adds /update and works over either normal Wi-Fi or fallback AP.
  // Authentication protects firmware upload from other LAN/AP clients.
  ElegantOTA.setAuth(OTA_USERNAME, OTA_PASSWORD);

  ElegantOTA.onStart([]() {
    otaInProgress = true;
    otaProgressPercent = 0;
    Serial.println("OTA update started");
  });

  ElegantOTA.onProgress([](size_t current, size_t final) {
    if (final > 0) {
      otaProgressPercent = (unsigned int)((current * 100U) / final);
    }
  });

  ElegantOTA.onEnd([](bool success) {
    otaProgressPercent = success ? 100 : otaProgressPercent;
    Serial.println(success ? "OTA update completed" : "OTA update failed");
    otaInProgress = false;
  });

  ElegantOTA.begin(&server);

  // ---------------------------------------------------
  // Load saved Wi-Fi credentials from NVS.
  // Saved values override the factory defaults.
  // ---------------------------------------------------
  loadWiFiCredentials();

  // ---------------------------------------------------
  // Try normal Wi-Fi for a limited time.
  // ---------------------------------------------------
  
  showMessage("Connecting WiFi", "Please wait...");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  if (wifiSSID.length() > 0) {
    WiFi.begin(
      wifiSSID.c_str(),
      wifiPassword.c_str()
    );
  }

  Serial.print("Connecting to WiFi");

  unsigned long wifiStart = millis();
  bool wifiBlink = false;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
    showMessage(wifiBlink ? "Connecting WiFi" : "", "Please wait...");
    wifiBlink = !wifiBlink;
    delay(350);
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    showIPAddress();
  } else {
    Serial.println("WiFi connection timed out");
    startFallbackAP();
  }

  // NTP will synchronize whenever STA Internet connectivity is available.
  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    NTP_SERVER_1,
    NTP_SERVER_2,
    NTP_SERVER_3
  );

  server.begin();
  Serial.println("Web server started");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Control page: http://");
    Serial.println(WiFi.localIP());
    Serial.print("ElegantOTA: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/update");

    // Initial weather fetch only when Internet connectivity is available.
    fetchWindSpeed();
  }

  lastWiFiRetry = millis();
  lastPWMRamp = millis();
  lastRPMTime = millis();
  lastDisplayMove = millis();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  unsigned long now = millis();

  // Captive-portal DNS must be serviced while the fallback AP is active.
  if (apMode) {
    dnsServer.processNextRequest();
  }

  server.handleClient();
  ElegantOTA.loop();

  // Smoothly move actual PWM toward the current requested target.
  updateFanRamp();

  // ---------------------------------------------------
  // Wi-Fi fallback / recovery handling
  // ---------------------------------------------------
  if (WiFi.status() == WL_CONNECTED) {
    wifiLostSince = 0;

    // If station Wi-Fi recovered while the fallback AP was running,
    // return to normal STA-only operation.
    if (apMode) {
      stopFallbackAP();
      showIPAddress();
      lastWeatherFetch = 0;  // force a fresh weather update
    }
  } else {
    if (wifiLostSince == 0) {
      wifiLostSince = now;
    }

    // If normal Wi-Fi stays down long enough, expose the local captive portal.
    if (!apMode && now - wifiLostSince >= WIFI_LOST_AP_DELAY_MS) {
      startFallbackAP();
    }

    // Continue retrying the configured Wi-Fi in the background.
    if (now - lastWiFiRetry >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiRetry = now;
      Serial.println("Retrying normal WiFi...");

      if (wifiSSID.length() > 0) {
        WiFi.reconnect();
      }
    }
  }

  // Play the existing missile-interception animation after every
  // successful wind-data update. Failed API requests never set the flag.
  if (interceptionRequested) {
    interceptionRequested = false;
    showInterceptionAnimation();
    drawMainScreen();
  }

  // RPM every second
  if (now - lastRPMTime >= RPM_INTERVAL_MS) {
    unsigned long elapsed = now - lastRPMTime;
    lastRPMTime = now;

    noInterrupts();
    uint32_t pulses = tachPulses;
    tachPulses = 0;
    interrupts();

    currentPulsesPerSecond = pulses * (1000.0f / elapsed);
    currentRPM = (currentPulsesPerSecond * 60.0f) / TACH_PULSES_PER_REV;

    Serial.printf(
      "Mode:%s Wind:%.1f km/h PWM:%.1f%% Target:%.1f%% Pulses/s:%.1f RPM:%.0f\n",
      fanMode == MODE_WIND ? "WIND" : "MANUAL",
      currentWindKmh,
      activePWMPercent,
      requestedPWMPercent,
      currentPulsesPerSecond,
      currentRPM
    );

    drawMainScreen();
  }

  // Weather update
  if (lastWeatherFetch == 0 || now - lastWeatherFetch >= WEATHER_INTERVAL_MS) {
    // Prevent rapid retries on failure
    lastWeatherFetch = now;
    fetchWindSpeed();
  }

  // If Wi-Fi reconnects automatically, everything else continues.
  delay(2);
}
