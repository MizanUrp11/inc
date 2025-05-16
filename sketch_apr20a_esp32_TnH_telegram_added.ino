#define FIRMWARE_VERSION "1.2.1"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <SparkFunHTU21D.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <time.h>
#include <Arduino.h>
#include <driver/ledc.h>  // <-- Add this line

Preferences preferences;

float setTemperature;
float setHumidity;
int currentFanSpeed = 0;     // Global variable to track speed
float tempHysteresis = 0.5;  // Default 0.5°C
float humHysteresis = 3.0;   // Default 3%



const char* ssid = "Mizan";
const char* password = "Mizan1105009#?";

// Your Telegram Bot Token
const char* botToken = "7779193835:AAExMnFFaZp0y3__lBlGzTLO-tL48kY3tkQ";

// Your Telegram numeric user ID (from @userinfobot)
// Make chat_id a String, not int64_t
const String chat_id = "5248882270";

WiFiClientSecure secured_client;
UniversalTelegramBot bot(botToken, secured_client);
HTU21D mySensor;

unsigned long lastCheck = 0;
const long checkInterval = 1000;

bool autoSendEnabled = false;
unsigned long lastAutoSend = 0;
unsigned long autoSendInterval = 600000;  // Default 10 minutes

unsigned long lastTelegramMessageID = 0;

// 4-channel relay
// const int relayPins[] = { 16, 17, 18, 19 };  // 4 GPIO pins for 4 relays
#define RELAY_FAN 16                         // IN1 → Fan (সবসময় অন)
#define RELAY_HEATER 17                      // IN2 → Heater
#define RELAY_HUMIDIFIER 18                  // IN3 → Humidifier (পানি স্প্রে)
#define RELAY_DEHUMIDIFIER 19                // IN4 → Dehumidifier (Fan বা Exhaust)

unsigned long bootTime = 0;  // Device boot time in millis

void setup() {

  Serial.begin(115200);
  Wire.begin();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }
  Serial.println("\n✅ Connected to WiFi");

  // if (WiFi.status() != WL_CONNECTED) {
  //   Serial.println("❌ Failed to connect to WiFi!");
  //   // You can either retry after some delay or reboot
  // }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC (no offset)

  Serial.print("⏳ Waiting for NTP time sync");
  time_t now = time(nullptr);
  while (now < 1700000000) {  // wait until time is synced (any valid timestamp after 2023)
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\n✅ Time synchronized");


  // Optional: Skip certificate verification
  secured_client.setInsecure();

  // Initialize sensor (no return type, so we assume success)
  mySensor.begin();
  Serial.println("✅ Sensor initialized!");

  bootTime = millis();


  // ✅ TEST sending a Telegram message immediately
  bool sent = bot.sendMessage(String(chat_id), "✅ Bot is online and ready! lets /start", "");
  Serial.println(sent ? "📤 Message sent successfully!" : "❌ Failed to send message.");

  // Load settings at startup
  preferences.begin("telegramBot", true);  // Read-only mode

  autoSendEnabled = preferences.getBool("autoSend", false);
  autoSendInterval = preferences.getULong("interval", 600000);
  lastTelegramMessageID = preferences.getULong("lastMsgID", 0);
  setTemperature = preferences.getFloat("setTemp", 37.5);  // Default 37.5°C
  setHumidity = preferences.getFloat("setHum", 55.0);      // Default 55%
  tempHysteresis = preferences.getFloat("tempHyst", 0.5);
  humHysteresis = preferences.getFloat("humHyst", 3.0);
  preferences.end();

  if (autoSendEnabled) {
    String msg = "🔁 Power restored.\n📡 Auto-logging resumed.\n⏱ Interval: " + String(autoSendInterval / 60000) + " min";
    bot.sendMessage(chat_id, msg, "");
  }

  // 4-channel relay


  // রিলে পিন সেট করুন
  pinMode(RELAY_FAN, OUTPUT);
  pinMode(RELAY_HEATER, OUTPUT);
  pinMode(RELAY_HUMIDIFIER, OUTPUT);
  pinMode(RELAY_DEHUMIDIFIER, OUTPUT);

  // শুরুতে Fan সবসময় চালু রাখবো
  // digitalWrite(RELAY_FAN, LOW);  // LOW → Relay ON
  setFanSpeed(true);


  // // Fan speed setup
  // ledcSetup(0, 25000, 8);       // Channel 0, 25kHz PWM, 8-bit resolution
  // ledcAttachPin(RELAY_FAN, 0);  // GPIO16 → Channel 0
  // setFanSpeed(100);             // Default fan ON full speed at startup
}

void loop() {
  if (millis() - lastCheck > checkInterval) {
    int numNewMessages = bot.getUpdates(lastTelegramMessageID + 1);
    while (numNewMessages) {
      Serial.println("🔔 New message received");
      handleMessages(numNewMessages);
      lastTelegramMessageID = bot.last_message_received;  // Update after processing messages
      numNewMessages = bot.getUpdates(lastTelegramMessageID + 1);
    }
    lastCheck = millis();
  }
  // Auto-send sensor readings
  if (autoSendEnabled && millis() - lastAutoSend > autoSendInterval) {
    float temp = mySensor.readTemperature();
    float humidity = mySensor.readHumidity();

    controlTemperature(temp);
    controlHumidity(humidity);

    String message = "📡 Auto-update:\n";
    message += "🌡 Temp: " + String(temp, 1) + " °C\n";
    message += "💧 Humidity: " + String(humidity, 1) + " %";

    bot.sendMessage(String(chat_id), message, "");
    Serial.println("📤 Auto-sent sensor data");
    logToGoogleSheets(temp, humidity);

    lastAutoSend = millis();
  }
}


void handleMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    String from = bot.messages[i].from_name;
    String sender_id = bot.messages[i].chat_id;  // 🔁 Use String here!

    time_t now = time(nullptr);
    unsigned long messageTimestamp = bot.messages[i].date.toInt();

    if (now < messageTimestamp || now - messageTimestamp > 30) {
      Serial.println("⏱ Ignoring old or future-dated message");
      continue;
    }

    Serial.println("📩 Message: " + text);
    Serial.println("👤 From: " + from);
    Serial.print("💬 Sender chat ID: ");
    Serial.println(sender_id);

    if (sender_id != String(chat_id)) {
      bot.sendMessage(sender_id, "🚫 Access denied", "");
      continue;
    }

    if (text == "/start") {
      String welcomeMsg = "✅ Welcome to the ESP32 Bot!\n\n";
      welcomeMsg += "📡 Available Commands:\n";
      welcomeMsg += "/sensor - 📈 Get current temperature & humidity\n";
      welcomeMsg += "/status - 🛑 Get current status of devices\n";
      welcomeMsg += "/uptime - ⏳ Show how long device has been running\n";
      welcomeMsg += "/reboot - ♻️ Reboot the device\n";
      welcomeMsg += "/start_auto - 🔄 Start auto-sending updates\n";
      welcomeMsg += "/stop_auto - ⏹ Stop auto-sending updates\n";
      welcomeMsg += "/interval <minutes> - ⏱ Set auto-send interval\n";
      welcomeMsg += "/settemp <value> - 🌡 Set target temperature\n";
      welcomeMsg += "/sethum <value> - 💧 Set target humidity\n";
      welcomeMsg += "/settemphyst <value> - 🌡 Set temp hysteresis (±°C)\n";
      welcomeMsg += "/sethumhyst <value> - 💧 Set humidity hysteresis (±%)\n";
      welcomeMsg += "/settings - ⚙️ Show current settings\n";
      welcomeMsg += "/log_now - 📝 Log sensor data manually\n";
      welcomeMsg += "/update <url> - 🆙 OTA firmware update\n";
      welcomeMsg += "/version - 📦 Show firmware version\n";

      String keyboardJson = "[[\"/sensor\", \"/status\"],[\"/settings\",\"/fan\"]]";

      bot.sendMessageWithReplyKeyboard(chat_id, welcomeMsg, "", keyboardJson, true);
    }



    if (text == "/sensor") {
      float temp = mySensor.readTemperature();
      float humidity = mySensor.readHumidity();

      // Temperature Control
      if (temp < setTemperature) {
        digitalWrite(RELAY_HEATER, LOW);  // Heater ON
      } else {
        digitalWrite(RELAY_HEATER, HIGH);  // Heater OFF
      }

      // Humidity Control
      if (humidity < setHumidity) {
        digitalWrite(RELAY_HUMIDIFIER, LOW);     // Humidifier ON
        digitalWrite(RELAY_DEHUMIDIFIER, HIGH);  // Dehumidifier OFF
      } else if (humidity > setHumidity) {
        digitalWrite(RELAY_HUMIDIFIER, HIGH);   // Humidifier OFF
        digitalWrite(RELAY_DEHUMIDIFIER, LOW);  // Dehumidifier ON
      } else {
        digitalWrite(RELAY_HUMIDIFIER, HIGH);    // Humidifier OFF
        digitalWrite(RELAY_DEHUMIDIFIER, HIGH);  // Dehumidifier OFF
      }

      // Send sensor data back to user
      String message = "🌡 Temp: " + String(temp, 1) + " °C\n";
      message += "💧 Humidity: " + String(humidity, 1) + " %";

      bot.sendMessage(sender_id, message, "");
    }


    if (text == "/start_auto") {
      autoSendEnabled = true;
      saveSettings();
      bot.sendMessage(String(chat_id), "📡 Auto-updates ENABLED. You'll receive readings every 10 minutes.", "");
      // logToGoogleSheets(temp, humidity);
    }

    if (text == "/stop_auto") {
      autoSendEnabled = false;
      saveSettings();
      bot.sendMessage(String(chat_id), "🛑 Auto-updates DISABLED.", "");
    }

    if (text.startsWith("/interval")) {
      // Split by space and get the number
      int spaceIndex = text.indexOf(' ');
      if (spaceIndex > 0) {
        String numStr = text.substring(spaceIndex + 1);
        int minutes = numStr.toInt();
        if (minutes > 0 && minutes <= 1440) {    // limit to 24 hours max
          autoSendInterval = minutes * 60000UL;  // convert to ms
          saveSettings();
          String reply = "⏱ Auto-send interval set to " + String(minutes) + " minute(s).";
          bot.sendMessage(String(chat_id), reply, "");
        } else {
          bot.sendMessage(String(chat_id), "⚠️ Invalid interval. Please enter 1 to 1440 minutes.", "");
        }
      } else {
        bot.sendMessage(String(chat_id), "❓ Usage: /interval <minutes>\nExample: /interval 5", "");
      }
    }

    if (text.startsWith("/update")) {
      int spaceIndex = text.indexOf(' ');
      if (spaceIndex > 0) {
        String url = text.substring(spaceIndex + 1);
        bot.sendMessage(sender_id, "⏳ Starting firmware update from:\n" + url, "");

        // ✅ Save message ID BEFORE the device restarts from update
        preferences.begin("telegramBot", false);
        preferences.putULong("lastMsgID", bot.last_message_received);
        preferences.end();

        t_httpUpdate_return ret = httpUpdate.update(secured_client, url);

        switch (ret) {
          case HTTP_UPDATE_OK:
            bot.sendMessage(sender_id, "✅ Update successful with latest version! Rebooting...", "");
            break;
          case HTTP_UPDATE_NO_UPDATES:
            bot.sendMessage(sender_id, "ℹ️ No update available.", "");
            break;
          case HTTP_UPDATE_FAILED:
            bot.sendMessage(sender_id, "❌ Update failed!\nError: " + String(httpUpdate.getLastError()) + "\n" + httpUpdate.getLastErrorString(), "");
            break;
        }
      } else {
        bot.sendMessage(sender_id, "❓ Usage:\n/update http://yourserver.com/firmware.bin", "");
      }
    }

    if (text == "/version") {
      bot.sendMessage(sender_id, "📦 Firmware Version: " FIRMWARE_VERSION, "");
    }

    // Manual logging
    if (text == "/log_now") {
      float temp = mySensor.readTemperature();
      float humidity = mySensor.readHumidity();
      logToGoogleSheets(temp, humidity);
      bot.sendMessage(sender_id, "📄 Data logged to Google Sheets.", "");
    }

    if (text.startsWith("/settemp")) {
      int spaceIndex = text.indexOf(' ');
      if (spaceIndex > 0) {
        String tempStr = text.substring(spaceIndex + 1);
        float tempVal = tempStr.toFloat();
        if (tempVal >= 30.0 && tempVal <= 45.0) {  // নিরাপদ সীমা
          setTemperature = tempVal;
          saveSettings();
          bot.sendMessage(sender_id, "🌡 Set temperature updated to: " + String(setTemperature, 1) + " °C", "");
        } else {
          bot.sendMessage(sender_id, "⚠️ Invalid temperature! (Allowed: 30°C - 45°C)", "");
        }
      } else {
        bot.sendMessage(sender_id, "❓ Usage:\n/settemp <value>\nExample: /settemp 37.5", "");
      }
    }

    if (text.startsWith("/sethum")) {
      int spaceIndex = text.indexOf(' ');
      if (spaceIndex > 0) {
        String humStr = text.substring(spaceIndex + 1);
        float humVal = humStr.toFloat();
        if (humVal >= 30.0 && humVal <= 90.0) {  // নিরাপদ সীমা
          setHumidity = humVal;
          saveSettings();
          bot.sendMessage(sender_id, "💧 Set humidity updated to: " + String(setHumidity, 1) + " %", "");
        } else {
          bot.sendMessage(sender_id, "⚠️ Invalid humidity! (Allowed: 30% - 90%)", "");
        }
      } else {
        bot.sendMessage(sender_id, "❓ Usage:\n/sethum <value>\nExample: /sethum 55", "");
      }
    }

    if (text == "/settings") {
      String settingsInfo = "⚙️ Current Settings:\n";
      settingsInfo += "🌡 Set Temperature: " + String(setTemperature, 1) + " °C\n";
      settingsInfo += "↔️ Temp Hysteresis: ±" + String(tempHysteresis, 1) + " °C\n";
      settingsInfo += "💧 Set Humidity: " + String(setHumidity, 1) + " %\n";
      settingsInfo += "↔️ Humidity Hysteresis: ±" + String(humHysteresis, 1) + " %";

      bot.sendMessage(sender_id, settingsInfo, "");
    }


    if (text == "/status") {
      String statusMsg = "📊 Device Status:\n";

      statusMsg += "🌀 Fan: ";
      int fanDuty = ledcRead(0);
      if (fanDuty == 0) {
        statusMsg += "OFF ❌\n";
      } else {
        statusMsg += String(map(fanDuty, 0, 255, 0, 100)) + "% ✅\n";
      }


      statusMsg += "🔥 Heater: ";
      statusMsg += (digitalRead(RELAY_HEATER) == LOW) ? "ON ✅\n" : "OFF ❌\n";

      statusMsg += "💧 Humidifier: ";
      statusMsg += (digitalRead(RELAY_HUMIDIFIER) == LOW) ? "ON ✅\n" : "OFF ❌\n";

      statusMsg += "🌬 Dehumidifier: ";
      statusMsg += (digitalRead(RELAY_DEHUMIDIFIER) == LOW) ? "ON ✅\n" : "OFF ❌\n";

      bot.sendMessage(sender_id, statusMsg, "");
    }

    if (text == "/uptime") {
      unsigned long seconds = (millis() - bootTime) / 1000;
      unsigned int days = seconds / 86400;
      seconds %= 86400;
      unsigned int hours = seconds / 3600;
      seconds %= 3600;
      unsigned int minutes = seconds / 60;
      seconds %= 60;

      String uptimeMsg = "⏳ Uptime: ";
      if (days > 0) uptimeMsg += String(days) + "d ";
      if (hours > 0 || days > 0) uptimeMsg += String(hours) + "h ";
      if (minutes > 0 || hours > 0 || days > 0) uptimeMsg += String(minutes) + "m ";
      uptimeMsg += String(seconds) + "s";

      bot.sendMessage(sender_id, uptimeMsg, "");
    }

    if (text == "/reboot") {
      // ✅ Save last message ID before reboot
      preferences.begin("telegramBot", false);
      preferences.putULong("lastMsgID", bot.last_message_received);
      preferences.end();

      bot.sendMessage(sender_id, "♻️ Rebooting device...", "");
      delay(1000);    // Give time to send message
      ESP.restart();  // Reboot the ESP32
    }

    if (text.startsWith("/settemphyst ")) {
      String valueStr = text.substring(14);
      float value = valueStr.toFloat();

      if (value > 0) {
        tempHysteresis = value;
        preferences.putFloat("tempHyst", tempHysteresis);
        bot.sendMessage(sender_id, "✅ Temp hysteresis set to ±" + String(tempHysteresis, 1) + " °C", "");
      } else {
        bot.sendMessage(sender_id, "⚠️ Invalid temperature hysteresis value.", "");
      }
    }


    if (text.startsWith("/sethumhyst ")) {
      String valueStr = text.substring(13);
      float value = valueStr.toFloat();

      if (value > 0) {
        humHysteresis = value;
        preferences.putFloat("humHyst", humHysteresis);
        bot.sendMessage(sender_id, "✅ Humidity hysteresis set to ±" + String(humHysteresis, 1) + " %", "");
      } else {
        bot.sendMessage(sender_id, "⚠️ Invalid humidity hysteresis value.", "");
      }
    }


    if (text.startsWith("/fan")) {
      String arg = text.substring(5);
      arg.trim();
      arg.toLowerCase();

      if (arg.length() == 0) {
        String keyboardJson = "[[\"/fan on\", \"/fan off\"],[\"/settings\",\"/start\"]]";
        bot.sendMessageWithReplyKeyboard(
          sender_id,
          "🔧 Use the buttons to control the fan.",
          "",
          keyboardJson,
          true);
      } else if (arg == "on") {
        setFanSpeed(true);
        bot.sendMessage(sender_id, "🌀 Fan turned ON", "");
      } else if (arg == "off") {
        setFanSpeed(false);
        bot.sendMessage(sender_id, "🛑 Fan turned OFF", "");
      } else {
        bot.sendMessage(sender_id, "⚠️ Invalid value! Use:\n/fan on or /fan off only.", "");
      }
    }
  }
}

void saveSettings() {
  preferences.begin("telegramBot", false);  // Read/write mode
  preferences.putBool("autoSend", autoSendEnabled);
  preferences.putULong("interval", autoSendInterval);
  preferences.putFloat("setTemp", setTemperature);
  preferences.putFloat("setHum", setHumidity);
  preferences.end();
}

// Link to google log sheet
void logToGoogleSheets(float temperature, float humidity) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String serverUrl = "https://script.google.com/macros/s/AKfycbyJAuSXpr7BYfN6DguLru8ArwG6b3eYvb1gUwdNC6oQTJfjF-mJz15mwfrSO9BDg0PT/exec";

  String jsonPayload = "{\"temperature\":" + String(temperature, 2) + ",\"humidity\":" + String(humidity, 2) + "}";

  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    Serial.print("📄 Google Sheets log success: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("❌ Google Sheets log failed: ");
    Serial.println("Error: " + http.errorToString(httpResponseCode));
    Serial.println("Payload: " + jsonPayload);
    Serial.println("URL: " + serverUrl);
  }

  http.end();
}

// Fan with pwm
// void setFanSpeed(int percent) {
//   percent = constrain(percent, 0, 100);
//   int dutyCycle = map(percent, 0, 100, 0, 255);  // 8-bit resolution
//   ledcWrite(0, dutyCycle);                       // Channel 0
//   currentFanSpeed = percent;                     // Store speed
//   Serial.println("🌀 Fan speed set to " + String(percent) + "%");
// }

void setFanSpeed(bool on) {
  digitalWrite(RELAY_FAN, on ? HIGH : LOW);
}

void controlTemperature(float temp) {
  if (temp < setTemperature - tempHysteresis) {
    digitalWrite(RELAY_HEATER, LOW);  // Heater ON
  } else if (temp > setTemperature + tempHysteresis) {
    digitalWrite(RELAY_HEATER, HIGH);  // Heater OFF
  }
  // Else, keep current state
}

void controlHumidity(float humidity) {
  if (humidity < setHumidity - humHysteresis) {
    digitalWrite(RELAY_HUMIDIFIER, LOW);     // Humidifier ON
    digitalWrite(RELAY_DEHUMIDIFIER, HIGH);  // Dehumidifier OFF
  } else if (humidity > setHumidity + humHysteresis) {
    digitalWrite(RELAY_HUMIDIFIER, HIGH);   // Humidifier OFF
    digitalWrite(RELAY_DEHUMIDIFIER, LOW);  // Dehumidifier ON
  } else {
    digitalWrite(RELAY_HUMIDIFIER, HIGH);    // OFF
    digitalWrite(RELAY_DEHUMIDIFIER, HIGH);  // OFF
  }
}


/*****Google sheet app script**************
function doPost(e) {
  try {
    var data = JSON.parse(e.postData.contents);

    var temperature = data.temperature;
    var humidity = data.humidity;
    var timestamp = new Date();
    var sheetName = Utilities.formatDate(timestamp, Session.getScriptTimeZone(), "yyyy-MM-dd");

    var ss = SpreadsheetApp.getActiveSpreadsheet();
    var sheet = ss.getSheetByName(sheetName);

    if (!sheet) {
      // Create new sheet for the day
      sheet = ss.insertSheet(sheetName);
      sheet.appendRow(["Timestamp", "Temperature (°C)", "Humidity (%)"]);
    }

    // Log data to the appropriate sheet
    sheet.appendRow([timestamp, temperature, humidity]);

    return ContentService.createTextOutput("Success").setMimeType(ContentService.MimeType.TEXT);

  } catch (err) {
    return ContentService.createTextOutput("Error: " + err).setMimeType(ContentService.MimeType.TEXT);
  }
}

*/