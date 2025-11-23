/* Water motor Alexa - FINAL CORRECTED VERSION with Web Serial Monitor
 * - OTA server moved to port 81
 * - Web Serial Monitor added on http://<IP>:81/monitor
 * - Manual switch on D4 (GPIO2)
 * - Alexa using Espalexa (correct setValue() API)
 * - Water tank 100% safety lock (block ON)
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <vector> // Required for the log buffer

// ===== Alexa (Espalexa) =====
#include <Espalexa.h>
Espalexa espalexa;

// ===== OTA and Web Monitor Server on port 81 =====
ESP8266WebServer server(81);
ESP8266HTTPUpdateServer httpUpdater;

// ===== WIFI and Time =====
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST

LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Icons
byte wifiOn[8] = {B00000,B01110,B10001,B00100,B01010,B00000,B00100,B00000};
byte wifiOff[8] = {B10001,B11111,B11011,B00100,B01010,B10001,B10101,B00000};

// Pins
const int sensor1 = 14; // D5
const int sensor2 = 12; // D6
const int sensor3 = 13; // D7
const int sensor4 = 4;	// D2
const int relayPin = 16; // D0

// Manual switch (correct)
const int switchPin = 2; // D4 → GND (INPUT_PULLUP)

// State
bool wifiOK = false;
bool apModeLaunched = false;
unsigned long connectStartMillis = 0;

bool motorON = false;
unsigned long motorTime = 0;

bool timeSynced = false;
unsigned long lastSyncMillis = 0;
unsigned long offsetSeconds = 0;

unsigned long blinkTicker = 0;
bool blinkState = false;

String globalLevel = "0%";	
int lastSwitchState = HIGH;


// ===== Web Log Buffer and Logger Function =====
const int MAX_LOG_LINES = 50;
std::vector<String> logBuffer;

void log_and_print(String message) {
    // 1. Print to physical serial monitor
    Serial.println(message);

    // 2. Prepare for web log buffer
    String timestamp = "";
    if (timeSynced) {
        // Get current time string if synced
        timeClient.update(); 
        timestamp = timeClient.getFormattedTime();
    } else {
        // Use uptime seconds if not synced
        timestamp = String(millis()/1000) + "s";
    }

    // Add timestamped message to log buffer
    logBuffer.push_back("[" + timestamp + "] " + message);

    // 3. Maintain buffer size (remove oldest entry if over limit)
    if (logBuffer.size() > MAX_LOG_LINES) {
        logBuffer.erase(logBuffer.begin()); 
    }
}


// ---------------------- SAFE MOTOR ON ----------------------
void requestMotorOn(String source, String level)
{
	if (level == "100%")
	{
		motorON = false;
		digitalWrite(relayPin, LOW);

		// Alexa device = OFF
		espalexa.getDevice(0)->setValue(0);

		log_and_print("BLOCKED: Tank full -> ON rejected (" + source + ")");
		return;
	}

	motorON = true;
	digitalWrite(relayPin, HIGH);
	motorTime = millis();

	// Alexa device = ON
	espalexa.getDevice(0)->setValue(255);

	log_and_print("Motor ON by " + source);
}


// ---------------------- SAFE MOTOR OFF ----------------------
void requestMotorOff(String source)
{
	motorON = false;
	digitalWrite(relayPin, LOW);

	espalexa.getDevice(0)->setValue(0);

	log_and_print("Motor OFF by " + source);
}


// ---------------------- Alexa Callback ----------------------
void alexaCallback(uint8_t device_id, bool state)
{
	String level = globalLevel;

	if (state)
	{
		if (level == "100%")
		{
			// Block ON
			motorON = false;
			digitalWrite(relayPin, LOW);
			espalexa.getDevice(0)->setValue(0);

			log_and_print("Alexa tried ON but tank is full -> BLOCKED");
		}
		else
		{
			requestMotorOn("Alexa", level);
		}
	}
	else
	{
		requestMotorOff("Alexa");
	}
}


// ---------------------- Alexa Setup ----------------------
void setupAlexa()
{
	espalexa.addDevice("Water Motor", alexaCallback);
	espalexa.begin();
	log_and_print("Alexa device added: Water Motor");
}


// ---------------------- WiFiManager AP Callback ----------------------
void configModeCallback(WiFiManager *myWiFiManager)
{
	lcd.clear();
	lcd.setCursor(0, 0); lcd.print("Enter AP Mode");
	lcd.setCursor(0, 1); lcd.print("SSID:");
	lcd.setCursor(5, 1); lcd.print(myWiFiManager->getConfigPortalSSID());

	log_and_print("Config Portal: " + myWiFiManager->getConfigPortalSSID());
}


// ---------------------- Web Monitor Handlers ----------------------

// Endpoint to send the log data as a JSON array
void handleSerialData() {
    String json = "[";
    for (size_t i = 0; i < logBuffer.size(); ++i) {
        if (i > 0) json += ",";
        
        // Simple JSON escaping for standard log messages
        String escaped_log = logBuffer[i];
        escaped_log.replace("\"", "\\\""); 
        escaped_log.replace("\r", "");
        escaped_log.replace("\n", "");
        json += "\"" + escaped_log + "\"";
    }
    json += "]";

    server.send(200, "application/json", json);
}

// Endpoint to serve the HTML for the log viewer
void handleMonitorPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Water Motor Monitor</title>
    <!-- Use Tailwind CSS for responsive, mobile-friendly design -->
    <script src="https://cdn.tailwindcss.com"></script>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap');
        body { font-family: 'Inter', sans-serif; background-color: #f3f4f6; }
        /* Style for the terminal-like log area */
        #log-area {
            height: 70vh;
            overflow-y: scroll;
            background-color: #1f2937; /* Dark background */
            color: #4ade80; /* Green text for serial output */
            font-family: monospace;
            white-space: pre-wrap; /* Wrap long lines */
            padding: 1rem;
            border-radius: 0.5rem;
            border: 1px solid #374151;
            scroll-behavior: smooth;
        }
        .log-entry {
            line-height: 1.4;
            padding: 2px 0;
            border-bottom: 1px dotted rgba(255,255,255,0.1);
        }
        .log-entry:last-child { border-bottom: none; }
    </style>
</head>
<body class="p-4">
    <div class="max-w-3xl mx-auto">
        <h1 class="text-3xl font-bold text-gray-800 mb-2">💧 Water Motor Monitor</h1>
        <p class="text-sm text-gray-600 mb-4">Access on your iPhone via http://<IP>:81/monitor</p>

        <div id="log-area">
            Awaiting data...
        </div>

        <div class="mt-4 flex flex-col sm:flex-row items-center justify-between">
            <button id="refresh-btn" onclick="fetchData()" class="w-full sm:w-auto bg-indigo-600 hover:bg-indigo-700 text-white font-bold py-2 px-4 rounded-lg shadow-md transition duration-150 ease-in-out">
                Manual Refresh
            </button>
            <span id="status" class="ml-0 sm:ml-4 mt-2 sm:mt-0 text-sm text-gray-500">Last updated: Never</span>
        </div>
    </div>

    <script>
        const logArea = document.getElementById('log-area');
        const statusSpan = document.getElementById('status');
        const refreshBtn = document.getElementById('refresh-btn');
        let autoScroll = true;

        // Check if user has scrolled up to disable auto-scroll
        logArea.onscroll = function() {
            // Check if the bottom of the content is visible within a 20px tolerance
            autoScroll = logArea.scrollHeight - logArea.clientHeight <= logArea.scrollTop + 20;
        };

        async function fetchData() {
            refreshBtn.disabled = true;
            refreshBtn.textContent = 'Refreshing...';

            try {
                const response = await fetch('/data/serial');
                if (!response.ok) throw new Error('Network response was not ok');
                
                const logData = await response.json();

                // Build new HTML content
                let newContent = '';
                logData.forEach(line => {
                    newContent += '<div class="log-entry">' + escapeHtml(line) + '</div>';
                });
                logArea.innerHTML = newContent;

                // Only scroll to the bottom if auto-scroll is enabled (user hasn't scrolled up)
                if (autoScroll) {
                   logArea.scrollTop = logArea.scrollHeight;
                }

                statusSpan.textContent = 'Last updated: ' + new Date().toLocaleTimeString();
            } catch (error) {
                console.error('Error fetching serial data:', error);
                logArea.innerHTML = '<div class="log-entry text-red-400">Error: Could not connect to ESP8266. Check IP/Port.</div>' + logArea.innerHTML;
                statusSpan.textContent = 'Last updated: Error fetching data.';
            } finally {
                refreshBtn.disabled = false;
                refreshBtn.textContent = 'Manual Refresh';
            }
        }

        function escapeHtml(unsafe) {
            return unsafe
                 .replace(/&/g, "&amp;")
                 .replace(/</g, "&lt;")
                 .replace(/>/g, "&gt;")
                 .replace(/"/g, "&quot;")
                 .replace(/'/g, "&#039;");
        }

        // Fetch data on page load
        fetchData();

        // Set up auto-refresh (every 2 seconds)
        setInterval(fetchData, 2000);
    </script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}


// ---------------------- OTA Setup ----------------------
void setupWebOTA()
{
	httpUpdater.setup(&server, "/update", "kbc", "987654321");
    // Add the web serial monitor handlers
    server.on("/monitor", handleMonitorPage); 
    server.on("/data/serial", handleSerialData);
    
	server.begin();
	log_and_print("Web Interface Ready:");
    log_and_print(" - OTA: http://" + WiFi.localIP().toString() + ":81/update");
    log_and_print(" - Monitor: http://" + WiFi.localIP().toString() + ":81/monitor");
}


// =============================== SETUP ===============================
void setup()
{
	Serial.begin(115200);

	pinMode(sensor1, INPUT_PULLUP);
	pinMode(sensor2, INPUT_PULLUP);
	pinMode(sensor3, INPUT_PULLUP);
	pinMode(sensor4, INPUT_PULLUP);
	pinMode(relayPin, OUTPUT);
	digitalWrite(relayPin, LOW);

	pinMode(switchPin, INPUT_PULLUP);

	Wire.begin(0, 5);
	lcd.init(); lcd.backlight();
	lcd.createChar(0, wifiOn);
	lcd.createChar(1, wifiOff);

	lcd.setCursor(6,0); lcd.print("K.B.C");
	lcd.setCursor(0,1); lcd.print("Home Automation");
	delay(2000); lcd.clear();

	lcd.setCursor(0,0); lcd.print("Water Level:");
	lcd.setCursor(0,1); lcd.print("Motor:OFF ");
	lcd.setCursor(10,1); lcd.write(1);
	lcd.setCursor(11,1); lcd.print("--:--");

	WiFi.setAutoReconnect(true);
	WiFi.begin();
	connectStartMillis = millis();

	setupAlexa();
}


// =============================== LOOP ===============================
void loop()
{
	bool isConnected = (WiFi.status() == WL_CONNECTED);

	// --- WiFi AP Logic ---
	if (!isConnected && !apModeLaunched && millis() - connectStartMillis > 30000)
	{
		WiFiManager wm;
		wm.setAPCallback(configModeCallback);
		wm.setConfigPortalTimeout(180);
		wm.startConfigPortal("KBC-Setup", "12345678");
		apModeLaunched = true;
		isConnected = (WiFi.status() == WL_CONNECTED);
	}

	// OTA and Web Monitor
	if (isConnected) server.handleClient();

	// Alexa
	espalexa.loop();

	// When WiFi connects
	if (isConnected && !wifiOK)
	{
		wifiOK = true;
		timeClient.begin();
		setupWebOTA();

		lcd.clear();
		lcd.setCursor(0,0); lcd.print("Water Level:");
		lcd.setCursor(0,1); lcd.print("WiFi Connected");
		delay(1500);
        log_and_print("WiFi connection established. IP: " + WiFi.localIP().toString());
	}

	// Time
	if (isConnected && timeClient.update())
	{
		timeSynced = true;
		lastSyncMillis = millis();
		offsetSeconds = timeClient.getSeconds()
			+ timeClient.getMinutes()*60
			+ timeClient.getHours()*3600;
	}

	String timeStr = "--:--";
	if (timeSynced)
	{
		unsigned long elapsed = (millis() - lastSyncMillis) / 1000;
		unsigned long total = offsetSeconds + elapsed;
		int h = (total / 3600) % 24;
		int m = (total / 60) % 60;
		char buf[6];
		sprintf(buf, "%02d:%02d", h, m);
		timeStr = buf;
	}

	// ==== Read Sensors ====
	bool s1=false,s2=false,s3=false;
	int s4c=0;

	for (int i=0; i<7; i++)
	{
		if (digitalRead(sensor1)==LOW) s1=true;
		if (digitalRead(sensor2)==LOW) s2=true;
		if (digitalRead(sensor3)==LOW) s3=true;
		if (digitalRead(sensor4)==LOW) s4c++;
		delay(10);
	}

	bool s4 = (s4c >= 5);

	String level;
	if (s4&&s3&&s2&&s1) level="100%";
	else if (s3&&s2&&s1) level="75%";
	else if (s2&&s1) level="50%";
	else if (s1) level="25%";
	else level="0%";

	globalLevel = level;

	lcd.setCursor(0,0);
	lcd.print("Water Level:");
	lcd.setCursor(12,0);
	lcd.print(level + " ");

	// ==== Auto Logic ====
	if (level=="0%" && !motorON) log_and_print("Level 0% detected. Starting motor..."); requestMotorOn("System", level);
	if (level=="100%" && motorON) log_and_print("Level 100% detected. Stopping motor..."); requestMotorOff("System");

	// ==== Manual Switch ====
	int sw = digitalRead(switchPin);
	if (lastSwitchState == HIGH && sw == LOW)
	{
		if (motorON) { log_and_print("Manual Switch pressed. Stopping motor."); requestMotorOff("Switch"); }
		else { log_and_print("Manual Switch pressed. Starting motor."); requestMotorOn("Switch", level); }

		delay(80);
	}
	lastSwitchState = sw;

	// ==== LCD bottom line ====
	lcd.setCursor(0,1);
	if (motorON)
	{
		int mins = (millis() - motorTime)/60000;
		char buf[16];
		sprintf(buf, "Motor:ON %02dM", mins);
		lcd.print(buf);
	}
	else
	{
		lcd.print("Motor:OFF ");
		lcd.setCursor(10,1);
		lcd.write(wifiOK ? 0 : 1);
	}

	lcd.setCursor(11,1);
	lcd.print(timeStr);

	delay(200);
}
