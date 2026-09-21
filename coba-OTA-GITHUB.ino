/*
  ============================================================
  ESP32 FreeRTOS + MQTT + WiFiManager + GitHub OTA
  ============================================================

  Board:
  ESP32 DevKit / ESP32-WROOM-32

  Arduino ESP32 Core:
  3.2.0

  SENSOR:
  MQ-2       -> GPIO 34
  Ultrasonic:
    TRIG     -> GPIO 5
    ECHO     -> GPIO 18

  MQTT:
    Broker   -> broker.emqx.io
    Port     -> 1883
    Topic    -> esp32/teris/sensor

  OTA:
    Version:
    https://raw.githubusercontent.com/
    terissuherlan22-stack/belajar-OTA-ESP32/main/version.txt

    Firmware:
    https://raw.githubusercontent.com/
    terissuherlan22-stack/belajar-OTA-ESP32/main/
    firmare/coba-OTA-GITHUB.ino.bin

  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include "esp_task_wdt.h"


// ============================================================
// FIRMWARE VERSION
// ============================================================

#define FIRMWARE_VERSION "1.0.10"


// ============================================================
// PIN SENSOR
// ============================================================

#define MQ2_PIN       34

#define ULTRASONIC_TRIG_PIN   5
#define ULTRASONIC_ECHO_PIN   18


// ============================================================
// WDT
// ============================================================

#define WDT_TIMEOUT_SECONDS 15


// ============================================================
// MQTT
// ============================================================

const char* MQTT_BROKER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_TOPIC = "esp32/teris/sensor";


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
// MQTT OBJECT
// ============================================================

WiFiClient mqttWiFiClient;

PubSubClient mqttClient(mqttWiFiClient);


// ============================================================
// SENSOR DATA
// ============================================================

struct SensorData {

  int mq2;

  float distance;

  unsigned long uptime;
};


// ============================================================
// QUEUE
// ============================================================

QueueHandle_t sensorQueue;


// ============================================================
// TASK HANDLE
// ============================================================

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t mqttTaskHandle   = NULL;
TaskHandle_t otaTaskHandle    = NULL;


// ============================================================
// WDT SETUP
// ESP32 Arduino Core 3.2.0
// ============================================================

void setupWDT() {

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };


  /*
    Pada ESP32 Arduino Core 3.x,
    WDT biasanya sudah diinisialisasi oleh sistem.

    Karena itu kita coba reconfigure terlebih dahulu.
  */

  esp_err_t result = esp_task_wdt_reconfigure(&wdt_config);


  if (result == ESP_OK) {

    Serial.println("[WDT] Reconfigured successfully");

  }
  else {

    Serial.print("[WDT] Reconfigure failed: ");
    Serial.println(result);
  }
}


// ============================================================
// READ ULTRASONIC
// ============================================================

float readUltrasonic() {

  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  delayMicroseconds(2);

  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);

  delayMicroseconds(10);

  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);


  unsigned long duration =
    pulseIn(
      ULTRASONIC_ECHO_PIN,
      HIGH,
      30000
    );


  if (duration == 0) {

    return -1.0;
  }


  float distance =
    duration * 0.0343 / 2.0;


  return distance;
}


// ============================================================
// SENSOR TASK
// ============================================================

void sensorTask(void* parameter) {

  /*
    Daftarkan task ini ke WDT
  */

  esp_task_wdt_add(NULL);


  Serial.println("[SENSOR TASK] Started");


  SensorData data;


  while (true) {

    /*
      Feed WDT
    */

    esp_task_wdt_reset();


    // --------------------------------------------------------
    // MQ-2
    // --------------------------------------------------------

    data.mq2 = analogRead(MQ2_PIN);


    // --------------------------------------------------------
    // ULTRASONIC
    // --------------------------------------------------------

    data.distance = readUltrasonic();


    // --------------------------------------------------------
    // UPTIME
    // --------------------------------------------------------

    data.uptime = millis();


    // --------------------------------------------------------
    // SEND DATA TO QUEUE
    // --------------------------------------------------------

    xQueueOverwrite(
      sensorQueue,
      &data
    );


    /*
      Feed WDT sebelum sleep
    */

    esp_task_wdt_reset();


    /*
      Sampling sensor setiap 1 detik
    */

    vTaskDelay(
      pdMS_TO_TICKS(1000)
    );
  }
}


// ============================================================
// MQTT CONNECT
// ============================================================

bool connectMQTT() {

  if (mqttClient.connected()) {

    return true;
  }


  Serial.println("[MQTT] Connecting...");


  /*
    Client ID dibuat berdasarkan MAC ESP32
    agar tidak mudah bentrok.
  */

  String clientID = "ESP32-TERIS-";

  clientID += WiFi.macAddress();

  clientID.replace(":", "");


  bool connected =
    mqttClient.connect(
      clientID.c_str()
    );


  if (connected) {

    Serial.println("[MQTT] Connected");

    return true;
  }


  Serial.print("[MQTT] Failed, state = ");

  Serial.println(
    mqttClient.state()
  );


  return false;
}


// ============================================================
// MQTT TASK
// ============================================================

void mqttTask(void* parameter) {

  /*
    Daftarkan MQTT Task ke WDT
  */

  esp_task_wdt_add(NULL);


  Serial.println("[MQTT TASK] Started");


  SensorData data;


  unsigned long lastPublish = 0;


  while (true) {

    /*
      Feed WDT
    */

    esp_task_wdt_reset();


    // --------------------------------------------------------
    // MQTT CONNECTION
    // --------------------------------------------------------

    if (!mqttClient.connected()) {

      connectMQTT();
    }


    mqttClient.loop();


    // --------------------------------------------------------
    // GET SENSOR DATA
    // --------------------------------------------------------

    if (
      xQueueReceive(
        sensorQueue,
        &data,
        pdMS_TO_TICKS(100)
      ) == pdTRUE
    ) {

      unsigned long now = millis();


      /*
        Publish setiap 5 detik
      */

      if (now - lastPublish >= 5000) {

        lastPublish = now;


        char payload[200];


        if (data.distance < 0) {

          snprintf(
            payload,
            sizeof(payload),

            "{\"mq2\":%d,"
            "\"distance\":null,"
            "\"uptime\":%lu}",

            data.mq2,
            data.uptime
          );

        }
        else {

          snprintf(
            payload,
            sizeof(payload),

            "{\"mq2\":%d,"
            "\"distance\":%.2f,"
            "\"uptime\":%lu}",

            data.mq2,
            data.distance,
            data.uptime
          );
        }


        if (mqttClient.connected()) {

          bool success =
            mqttClient.publish(
              MQTT_TOPIC,
              payload
            );


          if (success) {

            Serial.print("[MQTT] ");

            Serial.println(payload);
          }
          else {

            Serial.println(
              "[MQTT] Publish failed"
            );
          }
        }
      }
    }


    /*
      Feed WDT
    */

    esp_task_wdt_reset();


    /*
      Jangan loop terlalu cepat
    */

    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}


// ============================================================
// CHECK GITHUB VERSION
// ============================================================

String getRemoteVersion() {

  WiFiClientSecure client;

  client.setInsecure();


  HTTPClient http;


  Serial.println("[OTA] Checking version...");


  if (!http.begin(
        client,
        versionURL
      )) {

    Serial.println(
      "[OTA] HTTP begin failed"
    );

    return "";
  }


  int httpCode = http.GET();


  Serial.print(
    "[OTA] Version HTTP code: "
  );

  Serial.println(httpCode);


  if (httpCode != HTTP_CODE_OK) {

    http.end();

    return "";
  }


  String remoteVersion =
    http.getString();


  http.end();


  remoteVersion.trim();


  Serial.print(
    "[OTA] Remote version: "
  );

  Serial.println(remoteVersion);


  return remoteVersion;
}


// ============================================================
// DOWNLOAD FIRMWARE
// ============================================================

bool downloadFirmware() {

  WiFiClientSecure client;

  /*
    Untuk testing.
    Nanti kalau sudah production sebaiknya
    menggunakan certificate verification.
  */

  client.setInsecure();


  HTTPClient http;


  Serial.println(
    "[OTA] Downloading firmware..."
  );


  if (!http.begin(
        client,
        firmwareURL
      )) {

    Serial.println(
      "[OTA] HTTP begin failed"
    );

    return false;
  }


  /*
    Mulai HTTP GET
  */

  int httpCode = http.GET();


  Serial.print(
    "[OTA] Firmware HTTP code: "
  );

  Serial.println(httpCode);


  if (httpCode != HTTP_CODE_OK) {

    Serial.println(
      "[OTA] Firmware download failed"
    );

    http.end();

    return false;
  }


  /*
    Ukuran firmware
  */

  int contentLength =
    http.getSize();


  Serial.print(
    "[OTA] Firmware size: "
  );

  Serial.println(contentLength);


  /*
    Mulai OTA Update
  */

  bool canBegin;


  if (contentLength > 0) {

    canBegin =
      Update.begin(
        contentLength
      );

  }
  else {

    canBegin =
      Update.begin(
        UPDATE_SIZE_UNKNOWN
      );
  }


  if (!canBegin) {

    Serial.println(
      "[OTA] Update.begin failed"
    );

    Update.printError(Serial);

    http.end();

    return false;
  }


  /*
    Ambil stream HTTP
  */

  WiFiClient* stream =
    http.getStreamPtr();


  /*
    Timeout supaya readBytes()
    tidak menggantung terlalu lama.
  */

  stream->setTimeout(1000);


  uint8_t buffer[1024];


  size_t totalWritten = 0;


  unsigned long lastProgress = millis();


  Serial.println(
    "[OTA] Download started..."
  );


  while (
    http.connected() &&
    (
      contentLength <= 0 ||
      totalWritten < (size_t)contentLength
    )
  ) {

    /*
      ========================================================
      PENTING:
      Feed WDT di dalam loop OTA.
      Ini memperbaiki masalah:
      "Task watchdog got triggered - OTA Task"
      ========================================================
    */

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


        /*
          Tampilkan progress
        */

        if (
          millis() - lastProgress >= 1000
        ) {

          lastProgress = millis();


          if (contentLength > 0) {

            int progress =
              (
                totalWritten * 100
              ) /
              contentLength;


            Serial.print(
              "[OTA] Progress: "
            );

            Serial.print(progress);

            Serial.print("% (");

            Serial.print(totalWritten);

            Serial.print("/");

            Serial.print(contentLength);

            Serial.println(")");
          }
          else {

            Serial.print(
              "[OTA] Downloaded: "
            );

            Serial.println(
              totalWritten
            );
          }
        }
      }
    }


    /*
      Feed WDT lagi setelah proses download
    */

    esp_task_wdt_reset();


    /*
      Beri kesempatan task lain berjalan
    */

    vTaskDelay(
      pdMS_TO_TICKS(1)
    );
  }


  Serial.println(
    "[OTA] Download finished"
  );


  /*
    Pastikan ukuran sesuai
  */

  if (
    contentLength > 0 &&
    totalWritten != (size_t)contentLength
  ) {

    Serial.println(
      "[OTA] Firmware size mismatch"
    );


    Serial.print(
      "[OTA] Expected: "
    );

    Serial.println(contentLength);


    Serial.print(
      "[OTA] Received: "
    );

    Serial.println(totalWritten);


    Update.abort();

    http.end();

    return false;
  }


  /*
    Finalisasi OTA
  */

  if (!Update.end(true)) {

    Serial.println(
      "[OTA] Update.end failed"
    );

    Update.printError(Serial);

    http.end();

    return false;
  }


  /*
    Cek apakah firmware valid
  */

  if (!Update.isFinished()) {

    Serial.println(
      "[OTA] Update not finished"
    );

    http.end();

    return false;
  }


  Serial.println(
    "[OTA] Firmware update successful!"
  );


  http.end();


  return true;
}


// ============================================================
// OTA CHECK
// ============================================================

void checkOTA() {

  Serial.println();
  Serial.println(
    "=============================="
  );

  Serial.println(
    "[OTA] Checking GitHub OTA"
  );


  Serial.print(
    "[OTA] Current version: "
  );

  Serial.println(
    FIRMWARE_VERSION
  );


  String remoteVersion =
    getRemoteVersion();


  /*
    Kalau gagal membaca version.txt
  */

  if (remoteVersion.length() == 0) {

    Serial.println(
      "[OTA] Cannot get remote version"
    );

    Serial.println(
      "=============================="
    );

    return;
  }


  /*
    Bandingkan versi
  */

  if (
    remoteVersion ==
    FIRMWARE_VERSION
  ) {

    Serial.println(
      "[OTA] Firmware already up to date"
    );

    Serial.println(
      "=============================="
    );

    return;
  }


  /*
    Versi berbeda
  */

  Serial.println(
    "[OTA] New firmware detected!"
  );


  Serial.print(
    "[OTA] New version: "
  );

  Serial.println(remoteVersion);


  /*
    Download firmware
  */

  bool success =
    downloadFirmware();


  if (success) {

    Serial.println(
      "[OTA] OTA SUCCESS"
    );

    Serial.println(
      "[OTA] Restarting ESP32..."
    );


    /*
      Feed WDT sebelum restart
    */

    esp_task_wdt_reset();


    vTaskDelay(
      pdMS_TO_TICKS(1000)
    );


    ESP.restart();
  }
  else {

    Serial.println(
      "[OTA] OTA FAILED"
    );
  }


  Serial.println(
    "=============================="
  );
}


// ============================================================
// OTA TASK
// ============================================================

void otaTask(void* parameter) {

  /*
    Daftarkan OTA Task ke WDT
  */

  esp_task_wdt_add(NULL);


  Serial.println("[OTA TASK] Started");


  /*
    Tunggu ESP32 benar-benar selesai boot
  */

  for (int i = 0; i < 10; i++) {

    esp_task_wdt_reset();

    vTaskDelay(
      pdMS_TO_TICKS(1000)
    );
  }


  /*
    Testing:
    cek OTA setiap 10 detik.

    Untuk production sebaiknya 5 menit atau lebih.
  */

  const unsigned long OTA_INTERVAL =
    10000;


  unsigned long lastOTA =
    millis() - OTA_INTERVAL;


  while (true) {

    /*
      Feed WDT
    */

    esp_task_wdt_reset();


    unsigned long now =
      millis();


    if (
      now - lastOTA >= OTA_INTERVAL
    ) {

      lastOTA = now;


      /*
        Jalankan OTA
      */

      checkOTA();
    }


    /*
      Feed WDT
    */

    esp_task_wdt_reset();


    vTaskDelay(
      pdMS_TO_TICKS(100)
    );
  }
}


// ============================================================
// SETUP WIFI
// ============================================================

void setupWiFi() {

  Serial.println();
  Serial.println(
    "[WIFI] Starting WiFiManager..."
  );


  WiFi.mode(WIFI_STA);


  WiFiManager wm;


  /*
    Nama AP konfigurasi
  */

  bool result =
    wm.autoConnect(
      "ESP32-IOT-SETUP"
    );


  if (!result) {

    Serial.println(
      "[WIFI] Failed to connect"
    );

    delay(3000);

    ESP.restart();
  }


  Serial.println(
    "[WIFI] Connected!"
  );


  Serial.print(
    "[WIFI] SSID: "
  );

  Serial.println(
    WiFi.SSID()
  );


  Serial.print(
    "[WIFI] IP: "
  );

  Serial.println(
    WiFi.localIP()
  );


  Serial.print(
    "[WIFI] RSSI: "
  );

  Serial.print(
    WiFi.RSSI()
  );

  Serial.println(" dBm");
}


// ============================================================
// SETUP MQTT
// ============================================================

void setupMQTT() {

  mqttClient.setServer(
    MQTT_BROKER,
    MQTT_PORT
  );


  mqttClient.setBufferSize(512);


  Serial.println(
    "[MQTT] Configuration ready"
  );
}


// ============================================================
// SETUP SENSOR
// ============================================================

void setupSensor() {

  pinMode(
    MQ2_PIN,
    INPUT
  );


  pinMode(
    ULTRASONIC_TRIG_PIN,
    OUTPUT
  );


  pinMode(
    ULTRASONIC_ECHO_PIN,
    INPUT
  );


  digitalWrite(
    ULTRASONIC_TRIG_PIN,
    LOW
  );


  /*
    ADC ESP32
  */

  analogReadResolution(12);


  Serial.println(
    "[SENSOR] Sensor initialized"
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);


  delay(1000);


  Serial.println();
  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println(
    " ESP32 IoT FreeRTOS + MQTT + OTA"
  );

  Serial.println(
    "========================================"
  );


  Serial.print(
    "Firmware Version: "
  );

  Serial.println(
    FIRMWARE_VERSION
  );


  Serial.print(
    "ESP32 Chip Revision: "
  );

  Serial.println(
    ESP.getChipRevision()
  );


  Serial.print(
    "Free Heap: "
  );

  Serial.println(
    ESP.getFreeHeap()
  );


  // ----------------------------------------------------------
  // WDT
  // ----------------------------------------------------------

  setupWDT();


  // ----------------------------------------------------------
  // SENSOR
  // ----------------------------------------------------------

  setupSensor();


  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  setupWiFi();


  // ----------------------------------------------------------
  // MQTT
  // ----------------------------------------------------------

  setupMQTT();


  // ----------------------------------------------------------
  // QUEUE
  // ----------------------------------------------------------

  sensorQueue =
    xQueueCreate(
      1,
      sizeof(SensorData)
    );


  if (sensorQueue == NULL) {

    Serial.println(
      "[ERROR] Queue creation failed!"
    );

    while (true) {

      delay(1000);
    }
  }


  Serial.println(
    "[RTOS] Queue created"
  );


  // ==========================================================
  // CREATE SENSOR TASK
  // ==========================================================

  xTaskCreatePinnedToCore(
    sensorTask,
    "Sensor Task",
    4096,
    NULL,
    2,
    &sensorTaskHandle,
    1
  );


  // ==========================================================
  // CREATE MQTT TASK
  // ==========================================================

  xTaskCreatePinnedToCore(
    mqttTask,
    "MQTT Task",
    6144,
    NULL,
    2,
    &mqttTaskHandle,
    0
  );


  // ==========================================================
  // CREATE OTA TASK
  // ==========================================================

  xTaskCreatePinnedToCore(
    otaTask,
    "OTA Task",
    8192,
    NULL,
    1,
    &otaTaskHandle,
    0
  );


  Serial.println();
  Serial.println(
    "[RTOS] All tasks started"
  );


  Serial.println(
    "========================================"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  /*
    Semua pekerjaan utama sudah dijalankan
    oleh FreeRTOS Tasks.

    loop() cukup idle.
  */

  vTaskDelay(
    pdMS_TO_TICKS(1000)
  );
}