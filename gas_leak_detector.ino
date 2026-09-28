/*
 * Gas leak detector
 * NodeMCU (ESP8266) + MQ-9 sensor + relay-driven solenoid gas valve
 *
 * When the MQ-9 reading goes above the configured threshold, the relay is
 * energised (closing a normally-open solenoid valve) and the buzzer and alarm
 * LED are activated. Settings are changed from a web panel served by the board.
 *
 * Requires: ESP8266 Arduino core, ArduinoJson 6.x
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <DNSServer.h>

// ---------- Network ----------
// The board always runs its own access point. To also join an existing
// network, fill in HOME_SSID / HOME_PASSWORD (leave HOME_SSID empty to skip).
const char* AP_SSID = "Gas Detector";
const char* AP_PASSWORD = "change-me-123";  // at least 8 characters
const char* HOME_SSID = "";
const char* HOME_PASSWORD = "";

const IPAddress AP_IP(192, 168, 1, 1);
const byte DNS_PORT = 53;

// ---------- Pins ----------
#define MQ9_PIN A0
#define RELAY_PIN D1
#define EXTERNAL_LED_PIN D2
#define BUZZER_PIN D3
#define BUILTIN_LED D4  // active low

// ---------- Limits and defaults ----------
const int THRESHOLD_MIN = 30;
const int THRESHOLD_MAX = 1000;
const unsigned long MANUAL_TIMEOUT_MIN = 5000;
const unsigned long MANUAL_TIMEOUT_MAX = 180000;
const unsigned long MANUAL_PERMANENT = 0xFFFFFFFF;

const unsigned long SENSOR_INTERVAL = 1000;
const unsigned long BEEP_INTERVAL = 500;
const unsigned long BUZZER_TEST_MS = 500;

const int DEFAULT_CO_THRESHOLD = 400;
const int DEFAULT_GAS_THRESHOLD = 350;
const int DEFAULT_SMOKE_THRESHOLD = 300;
const int DEFAULT_GENERAL_THRESHOLD = 400;
const unsigned long DEFAULT_MANUAL_TIMEOUT = 30000;

// ---------- State ----------
ESP8266WebServer server(80);
DNSServer dnsServer;

int sensorValue = 0;
int levelPercent = 0;

bool alarmStatus = false;
bool relayStatus = false;
bool buzzerMuted = false;
bool relayLockMode = false;

bool manualControl = false;
unsigned long lastManualControlTime = 0;
unsigned long manualTimeout = DEFAULT_MANUAL_TIMEOUT;

int coThreshold = DEFAULT_CO_THRESHOLD;
int gasThreshold = DEFAULT_GAS_THRESHOLD;
int smokeThreshold = DEFAULT_SMOKE_THRESHOLD;
int generalThreshold = DEFAULT_GENERAL_THRESHOLD;
bool useGeneralThreshold = true;

unsigned long lastBuzzerBeep = 0;
bool buzzerBeepState = false;
bool buzzerTesting = false;
unsigned long buzzerTestStart = 0;

// ---------- Web panel ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Gas Detection System</title>
    <link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css'>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; font-family: 'Segoe UI', sans-serif; }
        body { background: linear-gradient(135deg, #1a2980, #26d0ce); min-height: 100vh; padding: 20px; }
        .container { max-width: 1200px; margin: 0 auto; background: rgba(255,255,255,0.95); border-radius: 25px; box-shadow: 0 20px 50px rgba(0,0,0,0.3); overflow: hidden; }
        .header { background: linear-gradient(135deg, #2c3e50, #3498db); color: white; padding: 20px; text-align: center; }
        .header h1 { font-size: 2rem; margin-bottom: 10px; }
        .control-mode { background: #27ae60; color: white; padding: 10px; border-radius: 5px; margin: 10px 20px; text-align: center; transition: background 0.3s; }
        .control-mode.manual { background: #f39c12; }
        .main-content { padding: 20px; }
        .sensor-value { text-align: center; margin: 20px 0; }
        .sensor-value .value { font-size: 4rem; font-weight: bold; color: #2c3e50; }
        .gas-cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; margin: 20px 0; }
        .gas-card { background: white; border-radius: 15px; padding: 20px; box-shadow: 0 5px 15px rgba(0,0,0,0.1); border-left: 5px solid; }
        .gas-card.co { border-color: #e74c3c; }
        .gas-card.gas { border-color: #3498db; }
        .gas-card.smoke { border-color: #7f8c8d; }
        .progress-bar { background: #eee; border-radius: 10px; overflow: hidden; margin: 8px 0; }
        .progress { height: 20px; transition: width 0.3s; }
        .controls { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin: 20px 0; }
        .control-card { background: white; border-radius: 15px; padding: 20px; box-shadow: 0 5px 15px rgba(0,0,0,0.1); }
        .btn-grid { display: flex; flex-wrap: wrap; }
        .btn { padding: 12px 20px; border: none; border-radius: 8px; cursor: pointer; font-size: 1rem; margin: 5px; transition: all 0.3s; }
        .btn:hover { transform: translateY(-2px); box-shadow: 0 5px 10px rgba(0,0,0,0.2); }
        .btn-primary { background: #3498db; color: white; }
        .btn-danger { background: #e74c3c; color: white; }
        .btn-success { background: #27ae60; color: white; }
        .btn-warning { background: #f39c12; color: white; }
        .btn-info { background: #17a2b8; color: white; }
        .btn-secondary { background: #95a5a6; color: white; }
        .btn-permanent { background: #8e44ad; color: white; }
        .status { padding: 10px; border-radius: 5px; margin: 10px 0; }
        .status-normal { background: #d5f4e6; color: #27ae60; }
        .status-alarm { background: #fadbd8; color: #e74c3c; animation: alarmPulse 1s infinite; }
        @keyframes alarmPulse { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }
        .threshold-control { margin: 15px 0; padding: 15px; background: #f8f9fa; border-radius: 10px; }
        .slider { width: 100%; margin: 10px 0; }
        .mode-control { display: flex; align-items: center; gap: 15px; margin: 15px 0; padding: 15px; background: white; border-radius: 10px; border: 2px solid #e9ecef; }
        .led-indicator { display: inline-block; width: 20px; height: 20px; border-radius: 50%; margin: 0 5px 0 10px; vertical-align: middle; }
        .led-on { background: #27ae60; box-shadow: 0 0 10px #27ae60; }
        .led-off { background: #95a5a6; }
        .timer-control { background: #fff3cd; padding: 15px; border-radius: 10px; margin: 15px 0; border: 1px solid #ffeaa7; }
        .timer-display { background: #3498db; color: white; padding: 10px; border-radius: 5px; margin: 10px 0; text-align: center; }
        .timer-display.infinite { background: #8e44ad; }
        .timer-display.expired { background: #e74c3c; }
        .lock-mode { background: #fadbd8; border-left: 5px solid #e74c3c; }
        .lock-mode.active { animation: lockPulse 2s infinite; }
        @keyframes lockPulse { 0% { background: #fadbd8; } 50% { background: #f8c9c5; } 100% { background: #fadbd8; } }
        @media (max-width: 768px) { .container { margin: 10px; } .header h1 { font-size: 1.5rem; } }
    </style>
</head>
<body>
    <div class='container'>
        <div class='header'>
            <h1><i class='fas fa-shield-alt'></i> MQ-9 Gas Detection System</h1>
            <p>Host: <span id='ipAddress'></span> | Updated: <span id='updateTime'>--:--:--</span></p>
        </div>

        <div class='control-mode' id='controlMode'>
            <i class='fas fa-robot' id='controlIcon'></i>
            <span id='controlModeText'>Mode: Automatic</span>
            <div id='timerDisplay'></div>
        </div>

        <div class='main-content'>
            <div class='sensor-value'>
                <div class='value' id='sensorValue'>0</div>
                <p>MQ-9 sensor reading</p>
                <div class='status' id='alarmStatus'>Status: Normal</div>
            </div>

            <div class='timer-control'>
                <h3><i class='fas fa-clock'></i> Manual control timer</h3>
                <div class='mode-control'>
                    <div>
                        <strong>Manual control duration:</strong>
                        <div style='margin-top: 10px;'>
                            <input type='range' class='slider' id='timerSlider' min='5' max='180' value='30' oninput='updateTimerValue(this.value)'>
                            <div style='display: flex; justify-content: space-between;'>
                                <span>5 s</span>
                                <span id='timerValueDisplay'>30 s</span>
                                <span>3 min</span>
                            </div>
                        </div>
                        <button class='btn btn-primary' onclick='saveTimerSetting()'>Save</button>
                        <button class='btn btn-permanent' onclick='setPermanentManual()'>Permanent</button>
                        <button class='btn btn-success' onclick='returnToAuto()'>Back to auto</button>
                    </div>
                </div>
                <p><i class='fas fa-info-circle'></i> How long the system stays in manual mode after the relay is switched by hand.</p>
            </div>

            <div class='mode-control lock-mode' id='lockModeControl'>
                <div>
                    <strong><i class='fas fa-lock'></i> Relay lock:</strong>
                    <p id='lockModeStatus'>Disabled - the relay turns off automatically once the gas level drops.</p>
                    <div>
                        <button class='btn btn-danger' onclick='toggleRelayLock()' id='lockBtn'>Enable relay lock</button>
                        <p><i class='fas fa-info-circle'></i> When enabled, a relay tripped by gas stays on until you switch it off manually.</p>
                    </div>
                </div>
            </div>

            <div class='mode-control'>
                <div>
                    <strong><i class='fas fa-lightbulb'></i> Indicators:</strong>
                    <span class='led-indicator' id='relayLed'></span>Relay
                    <span class='led-indicator' id='builtinLed'></span>Onboard LED
                    <span class='led-indicator' id='externalLed'></span>External LED
                </div>
                <div>
                    <strong><i class='fas fa-cogs'></i> Threshold mode:</strong>
                    <label><input type='radio' name='thresholdMode' value='general' checked onchange='changeMode(true)'> General</label>
                    <label><input type='radio' name='thresholdMode' value='individual' onchange='changeMode(false)'> Per gas</label>
                </div>
            </div>

            <div class='threshold-control'>
                <h3><i class='fas fa-sliders-h'></i> Thresholds</h3>

                <div id='generalThresholdControl'>
                    <p>General threshold: <span id='generalThresholdValue'>400</span></p>
                    <input type='range' class='slider' id='generalSlider' min='30' max='1000' value='400' oninput='updateGeneralValue(this.value)'>
                    <button class='btn btn-primary' onclick='saveGeneralThreshold()'>Save general threshold</button>
                </div>

                <div id='individualThresholdControl' style='display:none;'>
                    <div>
                        <p>CO threshold: <span id='coThresholdValue'>400</span></p>
                        <input type='range' class='slider' id='coSlider' min='30' max='1000' value='400' oninput='updateCOValue(this.value)'>
                        <button class='btn btn-danger' onclick='saveGasThreshold("CO")'>Save CO</button>
                    </div>
                    <div>
                        <p>GAS threshold: <span id='gasThresholdValue'>350</span></p>
                        <input type='range' class='slider' id='gasSlider' min='30' max='1000' value='350' oninput='updateGasValue(this.value)'>
                        <button class='btn btn-primary' onclick='saveGasThreshold("GAS")'>Save GAS</button>
                    </div>
                    <div>
                        <p>SMOKE threshold: <span id='smokeThresholdValue'>300</span></p>
                        <input type='range' class='slider' id='smokeSlider' min='30' max='1000' value='300' oninput='updateSmokeValue(this.value)'>
                        <button class='btn btn-secondary' onclick='saveGasThreshold("SMOKE")'>Save SMOKE</button>
                    </div>
                </div>
            </div>

            <div class='gas-cards'>
                <div class='gas-card co'>
                    <h3><i class='fas fa-skull-crossbones'></i> Carbon monoxide (CO)</h3>
                    <p>Level: <span id='coLevel'>0</span>%</p>
                    <div class='progress-bar'><div class='progress' id='coProgress' style='width:0%;background:#e74c3c;'></div></div>
                    <p>Current threshold: <span id='currentCOThreshold'>400</span></p>
                </div>
                <div class='gas-card gas'>
                    <h3><i class='fas fa-fire'></i> Natural gas (GAS)</h3>
                    <p>Level: <span id='gasLevel'>0</span>%</p>
                    <div class='progress-bar'><div class='progress' id='gasProgress' style='width:0%;background:#3498db;'></div></div>
                    <p>Current threshold: <span id='currentGasThreshold'>350</span></p>
                </div>
                <div class='gas-card smoke'>
                    <h3><i class='fas fa-smog'></i> Smoke (SMOKE)</h3>
                    <p>Level: <span id='smokeLevel'>0</span>%</p>
                    <div class='progress-bar'><div class='progress' id='smokeProgress' style='width:0%;background:#7f8c8d;'></div></div>
                    <p>Current threshold: <span id='currentSmokeThreshold'>300</span></p>
                </div>
            </div>

            <div class='controls'>
                <div class='control-card'>
                    <h3><i class='fas fa-bolt'></i> Relay control</h3>
                    <div class='btn-grid'>
                        <button class='btn btn-danger' onclick='controlRelay("on")'>Turn on</button>
                        <button class='btn btn-success' onclick='controlRelay("off")'>Turn off</button>
                        <button class='btn btn-warning' onclick='controlRelay("toggle")'>Toggle</button>
                    </div>
                    <p>Relay status: <span id='relayStatus'>Off</span></p>
                    <p><i class='fas fa-info-circle'></i> In manual mode the relay is controlled only by the user.</p>
                </div>

                <div class='control-card'>
                    <h3><i class='fas fa-bell'></i> Buzzer control</h3>
                    <div class='btn-grid'>
                        <button class='btn btn-info' onclick='controlBuzzer("mute")'>Mute</button>
                        <button class='btn btn-info' onclick='controlBuzzer("unmute")'>Unmute</button>
                        <button class='btn btn-warning' onclick='controlBuzzer("test")'>Test</button>
                    </div>
                    <p>Buzzer status: <span id='buzzerStatus'>Active</span></p>
                    <p><i class='fas fa-info-circle'></i> The buzzer sounds while gas is detected.</p>
                </div>
            </div>

            <div class='control-card' style='border: 2px solid #e74c3c;'>
                <h3><i class='fas fa-redo'></i> Reset settings</h3>
                <p>Restores every setting to its default value:</p>
                <ul style='margin: 10px 0 10px 20px;'>
                    <li>General threshold: 400</li>
                    <li>Automatic mode</li>
                    <li>Buzzer active</li>
                    <li>Relay lock: disabled</li>
                    <li>Manual control timer: 30 s</li>
                </ul>
                <button class='btn btn-danger' onclick='resetSettings()'>
                    <i class='fas fa-exclamation-triangle'></i> Reset settings
                </button>
            </div>
        </div>
    </div>

    <script>
        function post(url, body, okMessage) {
            return fetch(url, {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: body ? JSON.stringify(body) : undefined
            })
            .then(r => r.json())
            .then(data => {
                if (data.status === 'OK') {
                    if (okMessage) alert(okMessage(data));
                    loadSettings();
                    updateData();
                } else {
                    alert(data.error || 'Request failed');
                }
            })
            .catch(() => alert('Connection error'));
        }

        function updateData() {
            fetch('/data')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('sensorValue').textContent = data.sensorValue;
                    ['co', 'gas', 'smoke'].forEach(k => {
                        document.getElementById(k + 'Level').textContent = data[k + 'Level'];
                        document.getElementById(k + 'Progress').style.width = data[k + 'Level'] + '%';
                    });

                    const alarm = document.getElementById('alarmStatus');
                    if (data.alarmStatus) {
                        alarm.textContent = 'Status: GAS DETECTED';
                        alarm.className = 'status status-alarm';
                    } else {
                        alarm.textContent = 'Status: Normal';
                        alarm.className = 'status status-normal';
                    }

                    const relay = document.getElementById('relayStatus');
                    relay.textContent = data.relayStatus ? 'On' : 'Off';
                    relay.style.color = data.relayStatus ? '#e74c3c' : '#27ae60';
                    document.getElementById('buzzerStatus').textContent = data.buzzerMuted ? 'Muted' : 'Active';

                    updateLedStatus(data.relayStatus, data.builtinLed, data.externalLed);
                    updateControlModeUI(data.manualControl, data.remainingTime, data.isInfinite);
                    updateLockModeUI(data.relayLockMode);
                    document.getElementById('updateTime').textContent = new Date().toLocaleTimeString();
                })
                .catch(e => console.error(e));
        }

        function updateLedStatus(relayOn, builtinOn, externalOn) {
            document.getElementById('relayLed').className = 'led-indicator ' + (relayOn ? 'led-on' : 'led-off');
            document.getElementById('builtinLed').className = 'led-indicator ' + (builtinOn ? 'led-on' : 'led-off');
            document.getElementById('externalLed').className = 'led-indicator ' + (externalOn ? 'led-on' : 'led-off');
        }

        function updateControlModeUI(isManual, timeLeft, isInfinite) {
            const mode = document.getElementById('controlMode');
            const text = document.getElementById('controlModeText');
            const icon = document.getElementById('controlIcon');
            const timer = document.getElementById('timerDisplay');

            if (isManual) {
                mode.className = 'control-mode manual';
                icon.className = 'fas fa-user';
                text.textContent = 'Mode: Manual';
                if (isInfinite) {
                    timer.innerHTML = '<div class="timer-display infinite"><i class="fas fa-infinity"></i> Permanent</div>';
                } else if (timeLeft > 0) {
                    timer.innerHTML = '<div class="timer-display"><i class="fas fa-clock"></i> ' + timeLeft + ' s remaining</div>';
                } else {
                    timer.innerHTML = '<div class="timer-display expired"><i class="fas fa-exclamation-triangle"></i> Expired</div>';
                }
            } else {
                mode.className = 'control-mode';
                icon.className = 'fas fa-robot';
                text.textContent = 'Mode: Automatic';
                timer.innerHTML = '';
            }
        }

        function updateLockModeUI(isLocked) {
            const control = document.getElementById('lockModeControl');
            const status = document.getElementById('lockModeStatus');
            const btn = document.getElementById('lockBtn');

            if (isLocked) {
                control.classList.add('active');
                status.textContent = 'Enabled - the relay can only be switched off by the user.';
                btn.textContent = 'Disable relay lock';
                btn.className = 'btn btn-success';
            } else {
                control.classList.remove('active');
                status.textContent = 'Disabled - the relay turns off automatically once the gas level drops.';
                btn.textContent = 'Enable relay lock';
                btn.className = 'btn btn-danger';
            }
        }

        function loadSettings() {
            fetch('/settings')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('generalThresholdValue').textContent = data.generalThreshold;
                    document.getElementById('generalSlider').value = data.generalThreshold;

                    document.getElementById('coThresholdValue').textContent = data.coThreshold;
                    document.getElementById('coSlider').value = data.coThreshold;
                    document.getElementById('currentCOThreshold').textContent = data.coThreshold;

                    document.getElementById('gasThresholdValue').textContent = data.gasThreshold;
                    document.getElementById('gasSlider').value = data.gasThreshold;
                    document.getElementById('currentGasThreshold').textContent = data.gasThreshold;

                    document.getElementById('smokeThresholdValue').textContent = data.smokeThreshold;
                    document.getElementById('smokeSlider').value = data.smokeThreshold;
                    document.getElementById('currentSmokeThreshold').textContent = data.smokeThreshold;

                    const general = data.useGeneralThreshold;
                    document.querySelector('input[value="' + (general ? 'general' : 'individual') + '"]').checked = true;
                    document.getElementById('generalThresholdControl').style.display = general ? 'block' : 'none';
                    document.getElementById('individualThresholdControl').style.display = general ? 'none' : 'block';

                    document.getElementById('timerSlider').value = data.manualTimeoutSeconds;
                    updateTimerValue(data.manualTimeoutSeconds);
                    updateLockModeUI(data.relayLockMode);
                });
        }

        function updateTimerValue(val) {
            document.getElementById('timerValueDisplay').textContent = val == 0 ? 'Permanent' : val + ' s';
        }

        function saveTimerSetting() {
            const val = parseInt(document.getElementById('timerSlider').value);
            post('/set-timer', {timeout: val * 1000}, () => 'Timer set to ' + val + ' s');
        }

        function setPermanentManual() {
            post('/set-timer', {timeout: 0}, () => 'Manual mode set to permanent');
        }

        function returnToAuto() {
            post('/return-to-auto', null, () => 'Returned to automatic mode');
        }

        function toggleRelayLock() {
            post('/toggle-relay-lock', null, d => d.relayLockMode ? 'Relay lock enabled' : 'Relay lock disabled');
        }

        function resetSettings() {
            if (confirm('Reset all settings to their defaults?')) {
                post('/reset-settings', null, () => 'Settings reset');
            }
        }

        function changeMode(isGeneral) {
            post('/toggle-threshold-mode', {useGeneral: isGeneral});
        }

        function updateGeneralValue(v) { document.getElementById('generalThresholdValue').textContent = v; }
        function updateCOValue(v) { document.getElementById('coThresholdValue').textContent = v; }
        function updateGasValue(v) { document.getElementById('gasThresholdValue').textContent = v; }
        function updateSmokeValue(v) { document.getElementById('smokeThresholdValue').textContent = v; }

        function saveGeneralThreshold() {
            const val = parseInt(document.getElementById('generalSlider').value);
            post('/set-threshold', {threshold: val}, () => 'General threshold saved: ' + val);
        }

        function saveGasThreshold(gasType) {
            const val = parseInt(document.getElementById(gasType.toLowerCase() + 'Slider').value);
            post('/set-gas-threshold', {gasType: gasType, threshold: val}, () => gasType + ' threshold saved: ' + val);
        }

        function controlRelay(action) {
            post('/control', {device: 'relay', action: action});
        }

        function controlBuzzer(action) {
            fetch('/buzzer?action=' + action).then(() => updateData());
        }

        document.getElementById('ipAddress').textContent = window.location.hostname;
        loadSettings();
        updateData();
        setInterval(updateData, 2000);
        setInterval(loadSettings, 10000);
    </script>
</body>
</html>
)rawliteral";

// ---------- Helpers ----------
bool readJson(DynamicJsonDocument& doc) {
  return deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok;
}

void sendOk() {
  server.send(200, "application/json", "{\"status\":\"OK\"}");
}

void sendError(const char* message) {
  server.send(400, "application/json", String("{\"error\":\"") + message + "\"}");
}

void printManualTimeout() {
  if (manualTimeout == MANUAL_PERMANENT) {
    Serial.println("permanent");
  } else {
    Serial.print(manualTimeout / 1000);
    Serial.println(" s");
  }
}

// Effective threshold. The MQ-9 has a single analog output, so in per-gas mode
// the lowest of the three thresholds is the one that can trip first.
int currentThreshold() {
  if (useGeneralThreshold) return generalThreshold;
  return min(coThreshold, min(gasThreshold, smokeThreshold));
}

void activateRelay(bool on) {
  relayStatus = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  digitalWrite(EXTERNAL_LED_PIN, on ? HIGH : LOW);
  digitalWrite(BUILTIN_LED, on ? LOW : HIGH);
}

void manualRelay(bool on) {
  activateRelay(on);
  manualControl = true;
  lastManualControlTime = millis();
  Serial.print("Manual: relay ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(", manual mode for ");
  printManualTimeout();
}

void updateBuzzer() {
  unsigned long now = millis();

  if (buzzerTesting) {
    if (now - buzzerTestStart < BUZZER_TEST_MS) {
      digitalWrite(BUZZER_PIN, HIGH);
      return;
    }
    buzzerTesting = false;
  }

  if (alarmStatus && !buzzerMuted) {
    if (now - lastBuzzerBeep >= BEEP_INTERVAL) {
      lastBuzzerBeep = now;
      buzzerBeepState = !buzzerBeepState;
      digitalWrite(BUZZER_PIN, buzzerBeepState ? HIGH : LOW);
    }
  } else {
    buzzerBeepState = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ---------- HTTP handlers ----------
void handleControl() {
  DynamicJsonDocument doc(256);
  if (!readJson(doc)) return sendError("Invalid JSON");

  String device = doc["device"] | "";
  String action = doc["action"] | "";

  if (device != "relay") return sendError("Unknown device");

  if (action == "on") manualRelay(true);
  else if (action == "off") manualRelay(false);
  else if (action == "toggle") manualRelay(!relayStatus);
  else return sendError("Unknown action");

  sendOk();
}

void handleData() {
  DynamicJsonDocument doc(512);
  doc["sensorValue"] = sensorValue;
  doc["coLevel"] = levelPercent;
  doc["gasLevel"] = levelPercent;
  doc["smokeLevel"] = levelPercent;
  doc["alarmStatus"] = alarmStatus;
  doc["relayStatus"] = relayStatus;
  doc["buzzerMuted"] = buzzerMuted;
  doc["builtinLed"] = !digitalRead(BUILTIN_LED);
  doc["externalLed"] = digitalRead(EXTERNAL_LED_PIN);
  doc["manualControl"] = manualControl;
  doc["relayLockMode"] = relayLockMode;

  bool permanent = (manualTimeout == MANUAL_PERMANENT);
  doc["isInfinite"] = permanent;

  unsigned long remaining = 0;
  if (manualControl && !permanent) {
    unsigned long elapsed = millis() - lastManualControlTime;
    if (elapsed < manualTimeout) remaining = (manualTimeout - elapsed) / 1000;
  }
  doc["remainingTime"] = remaining;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSettings() {
  DynamicJsonDocument doc(512);
  doc["generalThreshold"] = generalThreshold;
  doc["coThreshold"] = coThreshold;
  doc["gasThreshold"] = gasThreshold;
  doc["smokeThreshold"] = smokeThreshold;
  doc["useGeneralThreshold"] = useGeneralThreshold;
  doc["relayLockMode"] = relayLockMode;
  doc["manualTimeoutSeconds"] = (manualTimeout == MANUAL_PERMANENT) ? 0 : manualTimeout / 1000;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSensorInfo() {
  DynamicJsonDocument doc(256);
  doc["value"] = sensorValue;
  doc["alarm"] = alarmStatus;
  doc["relay"] = relayStatus;
  doc["manualControl"] = manualControl;
  doc["relayLockMode"] = relayLockMode;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetTimer() {
  DynamicJsonDocument doc(128);
  if (!readJson(doc) || doc["timeout"].isNull()) return sendError("Missing timeout");

  unsigned long timeout = doc["timeout"];
  if (timeout == 0) {
    manualTimeout = MANUAL_PERMANENT;
  } else if (timeout >= MANUAL_TIMEOUT_MIN && timeout <= MANUAL_TIMEOUT_MAX) {
    manualTimeout = timeout;
  } else {
    return sendError("Timeout must be 5-180 s, or 0 for permanent");
  }

  Serial.print("Manual timeout: ");
  printManualTimeout();
  sendOk();
}

void handleReturnToAuto() {
  manualControl = false;
  Serial.println("Returned to automatic mode");
  sendOk();
}

void handleBuzzer() {
  String action = server.arg("action");

  if (action == "mute") {
    buzzerMuted = true;
  } else if (action == "unmute") {
    buzzerMuted = false;
  } else if (action == "test") {
    buzzerTesting = true;
    buzzerTestStart = millis();
  } else {
    return sendError("Unknown action");
  }

  sendOk();
}

void handleToggleRelayLock() {
  relayLockMode = !relayLockMode;
  Serial.print("Relay lock: ");
  Serial.println(relayLockMode ? "enabled" : "disabled");

  server.send(200, "application/json",
              String("{\"status\":\"OK\",\"relayLockMode\":") + (relayLockMode ? "true" : "false") + "}");
}

void handleResetSettings() {
  coThreshold = DEFAULT_CO_THRESHOLD;
  gasThreshold = DEFAULT_GAS_THRESHOLD;
  smokeThreshold = DEFAULT_SMOKE_THRESHOLD;
  generalThreshold = DEFAULT_GENERAL_THRESHOLD;
  useGeneralThreshold = true;
  buzzerMuted = false;
  relayLockMode = false;
  manualTimeout = DEFAULT_MANUAL_TIMEOUT;
  manualControl = false;
  activateRelay(false);  // the next sensor cycle re-applies it if gas is still present

  Serial.println("Settings reset to defaults");
  sendOk();
}

void handleSetThreshold() {
  DynamicJsonDocument doc(128);
  if (!readJson(doc)) return sendError("Invalid JSON");

  int value = doc["threshold"] | 0;
  if (value < THRESHOLD_MIN || value > THRESHOLD_MAX) return sendError("Threshold must be between 30 and 1000");

  generalThreshold = value;
  Serial.print("General threshold: ");
  Serial.println(generalThreshold);
  server.send(200, "application/json", "{\"status\":\"OK\",\"newThreshold\":" + String(generalThreshold) + "}");
}

void handleSetGasThreshold() {
  DynamicJsonDocument doc(128);
  if (!readJson(doc)) return sendError("Invalid JSON");

  String gasType = doc["gasType"] | "";
  int value = doc["threshold"] | 0;
  if (value < THRESHOLD_MIN || value > THRESHOLD_MAX) return sendError("Threshold must be between 30 and 1000");

  if (gasType == "CO") coThreshold = value;
  else if (gasType == "GAS") gasThreshold = value;
  else if (gasType == "SMOKE") smokeThreshold = value;
  else return sendError("Unknown gas type");

  Serial.print(gasType);
  Serial.print(" threshold: ");
  Serial.println(value);
  server.send(200, "application/json",
              "{\"status\":\"OK\",\"gasType\":\"" + gasType + "\",\"newThreshold\":" + String(value) + "}");
}

void handleToggleThresholdMode() {
  DynamicJsonDocument doc(128);
  if (!readJson(doc)) return sendError("Invalid JSON");

  if (doc["useGeneral"].isNull()) {
    useGeneralThreshold = !useGeneralThreshold;
  } else {
    useGeneralThreshold = doc["useGeneral"];
  }

  Serial.print("Threshold mode: ");
  Serial.println(useGeneralThreshold ? "general" : "per gas");
  server.send(200, "application/json",
              String("{\"status\":\"OK\",\"useGeneralThreshold\":") + (useGeneralThreshold ? "true" : "false") + "}");
}

void handleNotFound() {
  // Captive portal: send clients that use a foreign hostname to the panel.
  String host = server.hostHeader();
  if (host != AP_IP.toString() && host != WiFi.localIP().toString()) {
    server.sendHeader("Location", "http://" + AP_IP.toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send_P(200, "text/html", INDEX_HTML);
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nGas detection system starting");

  pinMode(MQ9_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(EXTERNAL_LED_PIN, OUTPUT);
  pinMode(BUILTIN_LED, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(EXTERNAL_LED_PIN, LOW);
  digitalWrite(BUILTIN_LED, HIGH);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", AP_IP);

  Serial.print("Access point: ");
  Serial.print(AP_SSID);
  Serial.print(" at ");
  Serial.println(WiFi.softAPIP());

  // Non-blocking: sensing starts immediately, the connection completes in the background.
  if (strlen(HOME_SSID) > 0) {
    WiFi.begin(HOME_SSID, HOME_PASSWORD);
    Serial.print("Connecting to ");
    Serial.println(HOME_SSID);
  }

  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/index.html", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/data", handleData);
  server.on("/settings", handleSettings);
  server.on("/sensorinfo", handleSensorInfo);
  server.on("/buzzer", handleBuzzer);
  server.on("/control", HTTP_POST, handleControl);
  server.on("/set-timer", HTTP_POST, handleSetTimer);
  server.on("/return-to-auto", HTTP_POST, handleReturnToAuto);
  server.on("/toggle-relay-lock", HTTP_POST, handleToggleRelayLock);
  server.on("/reset-settings", HTTP_POST, handleResetSettings);
  server.on("/set-threshold", HTTP_POST, handleSetThreshold);
  server.on("/set-gas-threshold", HTTP_POST, handleSetGasThreshold);
  server.on("/toggle-threshold-mode", HTTP_POST, handleToggleThresholdMode);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Web panel ready at http://192.168.1.1");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  updateBuzzer();

  static bool staReported = false;
  if (!staReported && WiFi.status() == WL_CONNECTED) {
    staReported = true;
    Serial.print("Connected to network, IP: ");
    Serial.println(WiFi.localIP());
  }

  static unsigned long lastRead = 0;
  if (millis() - lastRead < SENSOR_INTERVAL) return;
  lastRead = millis();

  sensorValue = analogRead(MQ9_PIN);
  levelPercent = map(constrain(sensorValue, 0, 800), 0, 800, 0, 100);
  int threshold = currentThreshold();

  if (manualControl && manualTimeout != MANUAL_PERMANENT &&
      millis() - lastManualControlTime >= manualTimeout) {
    manualControl = false;
    Serial.println("Manual timeout, back to automatic mode");
  }

  // The alarm follows the sensor even in manual mode; only the relay is overridden.
  bool gasDetected = sensorValue > threshold;
  if (gasDetected != alarmStatus) {
    alarmStatus = gasDetected;
    Serial.println(gasDetected ? "Gas detected" : "Gas level back to normal");
  }

  if (!manualControl) {
    if (gasDetected && !relayStatus) {
      activateRelay(true);
      Serial.println("Auto: relay ON");
    } else if (!gasDetected && relayStatus && !relayLockMode) {
      activateRelay(false);
      Serial.println("Auto: relay OFF");
    }
  }

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 5000) {
    lastPrint = millis();
    Serial.printf("sensor=%d threshold=%d alarm=%d relay=%d mode=%s lock=%d muted=%d\n",
                  sensorValue, threshold, alarmStatus, relayStatus,
                  manualControl ? "manual" : "auto", relayLockMode, buzzerMuted);
  }
}
