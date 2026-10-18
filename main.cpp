
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

// =====================================================
// SMART HOME AUTOMATION NODE
// ESP32 + FreeRTOS + MQTT
// Hardware-independent sensor simulation enabled
// =====================================================

// ---- Wi-Fi: enter your own credentials ----
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Public test broker. Use only for non-sensitive testing.
// Replace with your own broker for a real deployment.
const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// ---- Pin assignments for future hardware testing ----
constexpr uint8_t RELAY_PIN = 26;
constexpr uint8_t DHT_PIN   = 4;   // Reserved for future DHT sensor
constexpr uint8_t LDR_PIN   = 34;  // ADC input
constexpr uint8_t PIR_PIN   = 27;

// Relay modules can be active LOW. Verify your module
// before connecting a real relay or mains-powered load.
constexpr bool RELAY_ACTIVE_LOW = true;

// Set false only after implementing and wiring real sensors.
constexpr bool SIMULATION_MODE = true;

// ---- MQTT topics ----
const char* TOPIC_TELEMETRY = "student/smart-home-01/telemetry";
const char* TOPIC_STATUS    = "student/smart-home-01/status";
const char* TOPIC_COMMANDS  = "student/smart-home-01/command";

// ---- Application settings ----
constexpr float TEMP_THRESHOLD_C = 30.0f;
constexpr float HUMIDITY_LIMIT   = 75.0f;
constexpr int LIGHT_THRESHOLD    = 1800;

constexpr uint32_t SENSOR_PERIOD_MS = 2000;
constexpr uint32_t MQTT_PERIOD_MS  = 5000;

// ---- Data structures ----
struct SensorData {
  float temperatureC;
  float humidity;
  int lightLevel;
  bool motion;
  uint32_t timestampMs;
};

enum class ControlMode : uint8_t {
  AUTO_MODE,
  MANUAL_MODE
};

enum class CommandType : uint8_t {
  SET_AUTO,
  SET_MANUAL,
  RELAY_ON,
  RELAY_OFF
};

struct ControlCommand {
  CommandType type;
};

struct SystemStatus {
  ControlMode mode;
  bool relayOn;
  uint32_t lastSensorUpdate;
  uint32_t sensorReadCount;
  uint32_t commandCount;
};

// ---- FreeRTOS queues ----
QueueHandle_t sensorQueue = nullptr;
QueueHandle_t commandQueue = nullptr;

// ---- MQTT / Wi-Fi ----
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---- Shared status: protected by a mutex ----
SemaphoreHandle_t statusMutex = nullptr;

SystemStatus systemStatus = {
  ControlMode::AUTO_MODE,
  false,
  0,
  0,
  0
};

// ---- Task handles ----
TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
TaskHandle_t mqttTaskHandle = nullptr;
TaskHandle_t healthTaskHandle = nullptr;

// ---- Helpers ----
const char* modeName(ControlMode mode) {
  return mode == ControlMode::AUTO_MODE ? "AUTO" : "MANUAL";
}

void writeRelay(bool on) {
  const uint8_t level = RELAY_ACTIVE_LOW
      ? (on ? LOW : HIGH)
      : (on ? HIGH : LOW);

  digitalWrite(RELAY_PIN, level);
}

SystemStatus getStatusSnapshot() {
  SystemStatus copy;

  if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    copy = systemStatus;
    xSemaphoreGive(statusMutex);
  } else {
    copy.mode = ControlMode::AUTO_MODE;
    copy.relayOn = false;
    copy.lastSensorUpdate = 0;
    copy.sensorReadCount = 0;
    copy.commandCount = 0;
  }

  return copy;
}

// ---- MQTT command callback ----
// Supported payloads:
// MODE:AUTO
// MODE:MANUAL
// RELAY:ON
// RELAY:OFF
//
// Relay commands are accepted only in MANUAL mode.
// Commands are placed in a queue; callback does not
// directly manipulate the relay.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_COMMANDS) != 0 || length == 0 ||
      length >= 64) {
    return;
  }

  char message[64] = {0};
  memcpy(message, payload, length);
  message[length] = '\0';

  ControlCommand command;

  if (strcmp(message, "MODE:AUTO") == 0) {
    command.type = CommandType::SET_AUTO;
  } else if (strcmp(message, "MODE:MANUAL") == 0) {
    command.type = CommandType::SET_MANUAL;
  } else if (strcmp(message, "RELAY:ON") == 0) {
    command.type = CommandType::RELAY_ON;
  } else if (strcmp(message, "RELAY:OFF") == 0) {
    command.type = CommandType::RELAY_OFF;
  } else {
    Serial.printf("Unknown command: %s\n", message);
    return;
  }

  if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
    Serial.println("Command queue full; command rejected.");
  }
}

// =====================================================
// TASK 1: SENSOR ACQUISITION
// Reads simulated sensor data every 2 seconds.
// Replace the simulation block when adding real sensors.
// =====================================================
void sensorTask(void* parameter) {
  SensorData data = {};
  uint32_t sample = 0;

  for (;;) {
    if (SIMULATION_MODE) {
      // Repeatable demo signals for laptop/firmware testing.
      data.temperatureC = 24.0f + 8.0f * sinf(sample * 0.08f);
      data.humidity = 45.0f + 20.0f * sinf(sample * 0.05f);
      data.lightLevel = 1000 + (sample * 73) % 2500;
      data.motion = ((sample % 10) >= 6);
    } else {
      // TODO: Read actual temperature/humidity sensor,
      // LDR/ADC and PIR here. Do not use simulated values.
      data.temperatureC = NAN;
      data.humidity = NAN;
      data.lightLevel = 0;
      data.motion = false;
    }

    data.timestampMs = millis();

    // Keep the latest sample if the queue is full.
    if (xQueueSend(sensorQueue, &data, 0) != pdTRUE) {
      xQueueOverwrite(sensorQueue, &data);
    }

    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      systemStatus.lastSensorUpdate = millis();
      systemStatus.sensorReadCount++;
      xSemaphoreGive(statusMutex);
    }

    sample++;
    vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
  }
}

// =====================================================
// TASK 2: CONTROL LOGIC
// Owns relay state and applies AUTO/MANUAL policy.
// Demo policy: relay ON if temperature is high OR
// motion is detected in low light.
// =====================================================
void controlTask(void* parameter) {
  SensorData latest = {};
  bool haveSample = false;
  bool relayOn = false;
  ControlMode mode = ControlMode::AUTO_MODE;

  for (;;) {
    ControlCommand command;

    while (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
      switch (command.type) {
        case CommandType::SET_AUTO:
          mode = ControlMode::AUTO_MODE;
          Serial.println("Control mode: AUTO");
          break;

        case CommandType::SET_MANUAL:
          mode = ControlMode::MANUAL_MODE;
          Serial.println("Control mode: MANUAL");
          break;

        case CommandType::RELAY_ON:
          if (mode == ControlMode::MANUAL_MODE) {
            relayOn = true;
          } else {
            Serial.println("Relay command rejected: switch to MANUAL first.");
          }
          break;

        case CommandType::RELAY_OFF:
          if (mode == ControlMode::MANUAL_MODE) {
            relayOn = false;
          } else {
            Serial.println("Relay command rejected: switch to MANUAL first.");
          }
          break;
      }

      if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        systemStatus.mode = mode;
        systemStatus.commandCount++;
        xSemaphoreGive(statusMutex);
      }
    }

    SensorData incoming;
    if (xQueueReceive(sensorQueue, &incoming, pdMS_TO_TICKS(100)) == pdTRUE) {
      latest = incoming;
      haveSample = true;
    }

    if (mode == ControlMode::AUTO_MODE && haveSample) {
      const bool highTemperature =
          !isnan(latest.temperatureC) &&
          latest.temperatureC >= TEMP_THRESHOLD_C;

      const bool motionInLowLight =
          latest.motion && latest.lightLevel < LIGHT_THRESHOLD;

      relayOn = highTemperature || motionInLowLight;
    }

    writeRelay(relayOn);

    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      systemStatus.mode = mode;
      systemStatus.relayOn = relayOn;
      xSemaphoreGive(statusMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =====================================================
// TASK 3: MQTT COMMUNICATION
// Owns MQTT client operations and publishes telemetry.
// =====================================================
void mqttTask(void* parameter) {
  uint32_t lastPublish = 0;
  uint32_t lastWiFiAttempt = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWiFiAttempt >= 10000) {
        lastWiFiAttempt = millis();
        Serial.println("Connecting to Wi-Fi...");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }

      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!mqtt.connected()) {
      String clientId = "SmartHomeNode-";
      clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

      Serial.println("Connecting to MQTT broker...");

      if (mqtt.connect(clientId.c_str())) {
        Serial.println("MQTT connected.");
        mqtt.publish(TOPIC_STATUS, "online", true);
        mqtt.subscribe(TOPIC_COMMANDS);
      } else {
        Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
    }

    mqtt.loop();

    if (millis() - lastPublish >= MQTT_PERIOD_MS) {
      lastPublish = millis();

      SensorData data;
      const bool haveData =
          xQueuePeek(sensorQueue, &data, 0) == pdTRUE;

      const SystemStatus status = getStatusSnapshot();

      if (haveData) {
        char payload[256];

        snprintf(payload, sizeof(payload),
          "{\"temperature_c\":%.2f,"
          "\"humidity_pct\":%.2f,"
          "\"light_level\":%d,"
          "\"motion\":%s,"
          "\"mode\":\"%s\","
          "\"relay_on\":%s,"
          "\"simulated\":%s}",
          data.temperatureC,
          data.humidity,
          data.lightLevel,
          data.motion ? "true" : "false",
          modeName(status.mode),
          status.relayOn ? "true" : "false",
          SIMULATION_MODE ? "true" : "false"
        );

        mqtt.publish(TOPIC_TELEMETRY, payload);
        Serial.println(payload);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =====================================================
// TASK 4: SYSTEM HEALTH MONITOR
// Logs task health and sensor freshness.
// =====================================================
void healthTask(void* parameter) {
  for (;;) {
    const SystemStatus status = getStatusSnapshot();
    const uint32_t now = millis();

    const bool sensorStale =
        status.lastSensorUpdate == 0 ||
        (now - status.lastSensorUpdate > SENSOR_PERIOD_MS * 3);

    Serial.printf(
      "[HEALTH] mode=%s relay=%s samples=%lu commands=%lu sensor_stale=%s heap=%u\n",
      modeName(status.mode),
      status.relayOn ? "ON" : "OFF",
      (unsigned long)status.sensorReadCount,
      (unsigned long)status.commandCount,
      sensorStale ? "YES" : "NO",
      (unsigned int)ESP.getFreeHeap()
    );

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  writeRelay(false);

  WiFi.mode(WIFI_STA);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);

  sensorQueue = xQueueCreate(1, sizeof(SensorData));
  commandQueue = xQueueCreate(8, sizeof(ControlCommand));
  statusMutex = xSemaphoreCreateMutex();

  if (sensorQueue == nullptr ||
      commandQueue == nullptr ||
      statusMutex == nullptr) {
    Serial.println("FATAL: Failed to allocate RTOS resources.");
    while (true) {
      delay(1000);
    }
  }

  BaseType_t ok1 = xTaskCreate(
      sensorTask, "SensorTask", 3072, nullptr, 2, &sensorTaskHandle);

  BaseType_t ok2 = xTaskCreate(
      controlTask, "ControlTask", 4096, nullptr, 3, &controlTaskHandle);

  BaseType_t ok3 = xTaskCreate(
      mqttTask, "MQTTTask", 6144, nullptr, 1, &mqttTaskHandle);

  BaseType_t ok4 = xTaskCreate(
      healthTask, "HealthTask", 3072, nullptr, 1, &healthTaskHandle);

  if (ok1 != pdPASS || ok2 != pdPASS ||
      ok3 != pdPASS || ok4 != pdPASS) {
    Serial.println("FATAL: Failed to create one or more tasks.");
    writeRelay(false);
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Smart Home Automation Node started.");
  Serial.println("Simulation mode is enabled.");
  Serial.println("MQTT commands: MODE:AUTO, MODE:MANUAL, RELAY:ON, RELAY:OFF");
}

void loop() {
  // Application logic runs in FreeRTOS tasks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
