#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiManager.h>
#include "esp_task_wdt.h"

// ============================================================
// FIRMWARE VERSION
// ============================================================

#define FIRMWARE_VERSION "1.0.12"

// ============================================================
// LED
// ============================================================

#define LED_PIN 2

// ============================================================
// WDT
// ============================================================

#define WDT_TIMEOUT_SECONDS 15

// ============================================================
// OTA URL
// ============================================================

const char* versionURL =
  "https://raw.githubusercontent.com/"
  "terissuherlan22-stack/belajar-OTA-ESP32/"
  "main/version.txt";

const char* firmwareURL =
  "https://raw.githubusercontent.com/"
  "terissuherlan22-stack/belajar-OTA-ESP32/"
  "main/firmare/coba-OTA-GITHUB.ino.bin";

// ============================================================
// WDT SETUP
// ============================================================

void setupWDT() {

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_err_t result =
    esp_task_wdt_reconfigure(&wdt_config);

  if (result == ESP_OK) {
    Serial.println("[WDT] Configured");
  } else {
    Serial.print("[WDT] Reconfigure failed: ");
    Serial.println(result);
  }
}

// ============================================================
// GET REMOTE VERSION
// ============================================================

String getRemoteVersion() {

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  Serial.println("[OTA] Checking version...");

  if (!http.begin(client, versionURL)) {
    Serial.println("[OTA] HTTP begin failed");
    return "";
  }

  int httpCode = http.GET();

  Serial.print("[OTA] HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return "";
  }

  String remoteVersion = http.getString();

  http.end();

  remoteVersion.trim();

  Serial.print("[OTA] Remote version: ");
  Serial.println(remoteVersion);

  return remoteVersion;
}

// ============================================================
// DOWNLOAD FIRMWARE
// ============================================================

bool downloadFirmware() {

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  Serial.println("[OTA] Downloading firmware...");

  if (!http.begin(client, firmwareURL)) {
    Serial.println("[OTA] HTTP begin failed");
    return false;
  }

  int httpCode = http.GET();

  Serial.print("[OTA] Firmware HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.println("[OTA] Download failed");
    http.end();
    return false;
  }

  int contentLength = http.getSize();

  Serial.print("[OTA] Firmware size: ");
  Serial.println(contentLength);

  if (!Update.begin(contentLength)) {

    Serial.println("[OTA] Update.begin failed");

    Update.printError(Serial);

    http.end();

    return false;
  }

  WiFiClient* stream =
    http.getStreamPtr();

  stream->setTimeout(1000);

  uint8_t buffer[1024];

  size_t totalWritten = 0;

  unsigned long lastProgress = millis();

  Serial.println("[OTA] Download started");

  while (
    http.connected() &&
    totalWritten < (size_t)contentLength
  ) {

    // Feed WDT
    esp_task_wdt_reset();

    size_t available =
      stream->available();

    if (available > 0) {

      size_t bytesToRead =
        min(
          available,
          sizeof(buffer)
        );

      int bytesRead =
        stream->readBytes(
          buffer,
          bytesToRead
        );

      if (bytesRead > 0) {

        size_t written =
          Update.write(
            buffer,
            bytesRead
          );

        if (written != (size_t)bytesRead) {

          Serial.println(
            "[OTA] Update.write failed"
          );

          Update.printError(Serial);

          Update.abort();

          http.end();

          return false;
        }

        totalWritten += written;

        // Progress setiap 1 detik
        if (
          millis() - lastProgress >= 1000
        ) {

          lastProgress = millis();

          int progress =
            (totalWritten * 100) /
            contentLength;

          Serial.print("[OTA] Progress: ");
          Serial.print(progress);
          Serial.print("% (");
          Serial.print(totalWritten);
          Serial.print("/");
          Serial.print(contentLength);
          Serial.println(")");
        }
      }
    }

    // Feed WDT lagi
    esp_task_wdt_reset();

    vTaskDelay(pdMS_TO_TICKS(1));
  }

  http.end();

  Serial.println("[OTA] Download finished");

  if (
    totalWritten != (size_t)contentLength
  ) {

    Serial.println(
      "[OTA] Firmware size mismatch"
    );

    Update.abort();

    return false;
  }

  if (!Update.end(true)) {

    Serial.println(
      "[OTA] Update.end failed"
    );

    Update.printError(Serial);

    return false;
  }

  if (!Update.isFinished()) {

    Serial.println(
      "[OTA] Update not finished"
    );

    return false;
  }

  Serial.println(
    "[OTA] Update successful!"
  );

  return true;
}

// ============================================================
// CHECK OTA
// ============================================================

void checkOTA() {

  Serial.println();
  Serial.println("==============================");
  Serial.println("[OTA] OTA CHECK");

  Serial.print("[OTA] Current version: ");
  Serial.println(FIRMWARE_VERSION);

  String remoteVersion =
    getRemoteVersion();

  if (remoteVersion.length() == 0) {

    Serial.println(
      "[OTA] Failed to get remote version"
    );

    Serial.println("==============================");

    return;
  }

  if (
    remoteVersion ==
    FIRMWARE_VERSION
  ) {

    Serial.println(
      "[OTA] Firmware already up to date"
    );

    Serial.println("==============================");

    return;
  }

  Serial.println(
    "[OTA] NEW FIRMWARE DETECTED!"
  );

  Serial.print(
    "[OTA] New version: "
  );

  Serial.println(remoteVersion);

  bool success =
    downloadFirmware();

  if (success) {

    Serial.println(
      "[OTA] OTA SUCCESS!"
    );

    Serial.println(
      "[OTA] Restarting..."
    );

    esp_task_wdt_reset();

    delay(1000);

    ESP.restart();
  }

  Serial.println("==============================");
}

// ============================================================
// OTA TASK
// ============================================================

void otaTask(void* parameter) {

  esp_task_wdt_add(NULL);

  Serial.println("[OTA TASK] Started");

  // Tunggu 10 detik setelah boot
  for (int i = 0; i < 10; i++) {

    esp_task_wdt_reset();

    vTaskDelay(
      pdMS_TO_TICKS(1000)
    );
  }

  // Cek OTA setiap 10 detik
  const unsigned long OTA_INTERVAL =
    10000;

  unsigned long lastOTA =
    millis() - OTA_INTERVAL;

  while (true) {

    esp_task_wdt_reset();

    unsigned long now =
      millis();

    if (
      now - lastOTA >= OTA_INTERVAL
    ) {

      lastOTA = now;

      checkOTA();
    }

    esp_task_wdt_reset();

    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}

// ============================================================
// BLINK TASK
// ============================================================

void blinkTask(void* parameter) {

  esp_task_wdt_add(NULL);

  Serial.println("[BLINK TASK] Started");

  while (true) {

    digitalWrite(
      LED_PIN,
      HIGH
    );

    Serial.println("[LED] ON");

    esp_task_wdt_reset();

    vTaskDelay(
      pdMS_TO_TICKS(100)
    );


    digitalWrite(
      LED_PIN,
      LOW
    );

    Serial.println("[LED] OFF");

    esp_task_wdt_reset();

    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}

// ============================================================
// WIFI
// ============================================================

void setupWiFi() {

  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  bool result =
    wm.autoConnect(
      "ESP32-OTA-SETUP"
    );

  if (!result) {

    Serial.println(
      "[WIFI] Failed"
    );

    delay(3000);

    ESP.restart();
  }

  Serial.println(
    "[WIFI] Connected"
  );

  Serial.print(
    "[WIFI] IP: "
  );

  Serial.println(
    WiFi.localIP()
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    " ESP32 BLINK OTA"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "Firmware: "
  );

  Serial.println(
    FIRMWARE_VERSION
  );


  // LED
  pinMode(
    LED_PIN,
    OUTPUT
  );

  digitalWrite(
    LED_PIN,
    LOW
  );


  // WDT
  setupWDT();


  // WiFi
  setupWiFi();


  // Blink Task
  xTaskCreatePinnedToCore(
    blinkTask,
    "Blink Task",
    2048,
    NULL,
    1,
    NULL,
    1
  );


  // OTA Task
  xTaskCreatePinnedToCore(
    otaTask,
    "OTA Task",
    8192,
    NULL,
    1,
    NULL,
    0
  );


  Serial.println(
    "[SYSTEM] Tasks started"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  vTaskDelay(
    pdMS_TO_TICKS(1000)
  );
}
