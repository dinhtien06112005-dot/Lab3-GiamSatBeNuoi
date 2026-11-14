#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ================= PIN =================

#define TRIG_PIN 5
#define ECHO_PIN 18

#define DHT_PIN 4
#define DHT_TYPE DHT22

#define PUMP_PIN 26
#define VALVE_PIN 27

// ================= SENSOR =================

#define TANK_HEIGHT 2.0

DHT dht(DHT_PIN, DHT_TYPE);

// ================= WIFI =================

const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ================= MQTT =================

const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;

const char* topic_data = "benuoi/demo01/data";
const char* topic_pump = "benuoi/demo01/pump/cmd";
const char* topic_valve = "benuoi/demo01/valve/cmd";
const char* topic_mode = "benuoi/demo01/mode/cmd";
const char* topic_status = "benuoi/demo01/status";

WiFiClient espClient;
PubSubClient client(espClient);

// ================= DATA =================

struct SensorData {
  float distance;
  float waterLevel;
  float temperature;
};

QueueHandle_t sensorQueue;
QueueHandle_t alertQueue;

// ================= CONTROL =================

volatile bool manualMode = false;
volatile bool manualPump = false;
volatile bool manualValve = false;

volatile bool pumpState = false;
volatile bool valveState = false;

volatile int pumpPWM = 0;

float currentWaterLevel = -1;

// ================= ULTRASONIC =================

float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1;
  }

  return duration * 0.0343 / 2;
}

// ================= MQTT CALLBACK =================

void callback(char* topic, byte* payload, unsigned int length) {

  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();

  Serial.print("MQTT Topic: ");
  Serial.println(topic);

  Serial.print("Message: ");
  Serial.println(message);

  if (String(topic) == topic_mode) {

    if (message == "MANUAL") {
      manualMode = true;
    }

    if (message == "AUTO") {
      manualMode = false;
      manualPump = false;
      manualValve = false;
    }
  }

  if (String(topic) == topic_pump && manualMode) {

    if (message == "ON") {
      manualPump = true;
    }

    if (message == "OFF") {
      manualPump = false;
    }
  }

  if (String(topic) == topic_valve && manualMode) {

    if (message == "ON") {
      manualValve = true;
    }

    if (message == "OFF") {
      manualValve = false;
    }
  }
}

// ================= MQTT CONNECT =================

void reconnectMQTT() {

  while (!client.connected()) {

    Serial.println("Dang ket noi MQTT...");

    String clientId = "ESP32-BEN-NUOI-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str())) {

      Serial.println("MQTT Connected!");

      client.subscribe(topic_pump);
      client.subscribe(topic_valve);
      client.subscribe(topic_mode);

    } else {

      Serial.print("MQTT failed, state: ");
      Serial.println(client.state());

      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

// ================= TASK 1: READ SENSOR =================

void TaskReadSensor(void *pvParameters) {

  SensorData data;

  while (1) {

    data.distance = readDistance();

    if (data.distance >= 0) {

      data.waterLevel = TANK_HEIGHT - data.distance / 100.0;

      if (data.waterLevel < 0) {
        data.waterLevel = 0;
      }

      if (data.waterLevel > TANK_HEIGHT) {
        data.waterLevel = TANK_HEIGHT;
      }

    } else {

      data.waterLevel = -1;
    }

    data.temperature = dht.readTemperature();

    if (isnan(data.temperature)) {
      data.temperature = -999;
    }

    xQueueOverwrite(sensorQueue, &data);
    xQueueOverwrite(alertQueue, &data);

    Serial.println("========== SENSOR ==========");

    Serial.print("Distance: ");
    Serial.print(data.distance);
    Serial.println(" cm");

    Serial.print("Water Level: ");
    Serial.print(data.waterLevel);
    Serial.println(" m");

    Serial.print("Temperature: ");
    Serial.print(data.temperature);
    Serial.println(" C");

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ================= TASK 2: ALERT AND CONTROL =================

void TaskAlert(void *pvParameters) {

  SensorData data;

  bool actionActive = false;
  unsigned long actionStart = 0;
  unsigned long actionDuration = 0;

  int actionType = 0;
  int actionPWM = 0;

  bool lowPumpTriggered = false;
  bool highPumpTriggered = false;
  bool valveTriggered = false;

  while (1) {

    if (xQueueReceive(alertQueue, &data, pdMS_TO_TICKS(50))) {
      currentWaterLevel = data.waterLevel;
    }

    // ===== MANUAL MODE =====

    if (manualMode) {

      actionActive = false;
      actionType = 0;

      pumpState = manualPump;
      valveState = manualValve;

      pumpPWM = manualPump ? 255 : 0;

    }

    // ===== AUTOMATIC MODE =====

    else {

      if (currentWaterLevel >= 0) {

        // Reset trigger when leaving its water-level range

        if (currentWaterLevel < 1.0 || currentWaterLevel >= 1.5) {
          lowPumpTriggered = false;
        }

        if (currentWaterLevel < 1.5 || currentWaterLevel > 2.0) {
          highPumpTriggered = false;
        }

        if (currentWaterLevel >= 0.5) {
          valveTriggered = false;
        }

        // Start a new automatic action

        if (!actionActive) {

          if (currentWaterLevel >= 1.0 &&
              currentWaterLevel < 1.5 &&
              !lowPumpTriggered) {

            actionType = 1;
            actionPWM = 128;
            actionDuration = 10000;

            lowPumpTriggered = true;
            actionActive = true;
            actionStart = millis();

          }

          else if (currentWaterLevel >= 1.5 &&
                   currentWaterLevel <= 2.0 &&
                   !highPumpTriggered) {

            actionType = 1;
            actionPWM = 255;
            actionDuration = 20000;

            highPumpTriggered = true;
            actionActive = true;
            actionStart = millis();

          }

          else if (currentWaterLevel < 0.5 &&
                   !valveTriggered) {

            actionType = 2;
            actionPWM = 0;
            actionDuration = 10000;

            valveTriggered = true;
            actionActive = true;
            actionStart = millis();
          }
        }

        // Stop action after its duration

        if (actionActive &&
            millis() - actionStart >= actionDuration) {

          actionActive = false;
          actionType = 0;
        }
      }

      pumpState = actionActive && actionType == 1;
      valveState = actionActive && actionType == 2;

      pumpPWM = pumpState ? actionPWM : 0;
    }

    // ===== OUTPUT =====

    analogWrite(PUMP_PIN, pumpPWM);

    digitalWrite(
      VALVE_PIN,
      valveState ? HIGH : LOW
    );

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ================= TASK 3: MQTT =================

void TaskSendMQTT(void *pvParameters) {

  SensorData data;

  unsigned long lastStatus = 0;
  unsigned long lastConnectAttempt = 0;

  char payload[256];
  char statusPayload[128];

  while (1) {

    // WiFi connection

    if (WiFi.status() != WL_CONNECTED) {

      WiFi.begin(ssid, password);

      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }

      Serial.println("WiFi Connected!");
    }

    // MQTT connection

    if (!client.connected()) {

      if (millis() - lastConnectAttempt >= 3000) {

        lastConnectAttempt = millis();

        String clientId = "ESP32-BEN-NUOI-";
        clientId += String(random(0xffff), HEX);

        if (client.connect(clientId.c_str())) {

          Serial.println("MQTT Connected!");

          client.subscribe(topic_pump);
          client.subscribe(topic_valve);
          client.subscribe(topic_mode);
        }
      }
    }

    if (client.connected()) {

      client.loop();

      // Send sensor data

      if (xQueueReceive(sensorQueue, &data, pdMS_TO_TICKS(50))) {

        snprintf(
          payload,
          sizeof(payload),
          "{\"distance\":%.2f,\"waterLevel\":%.2f,\"temperature\":%.2f}",
          data.distance,
          data.waterLevel,
          data.temperature
        );

        client.publish(topic_data, payload);

        Serial.println("Da gui du lieu MQTT:");
        Serial.println(payload);
      }

      // Send equipment status

      if (millis() - lastStatus >= 500) {

        lastStatus = millis();

        snprintf(
          statusPayload,
          sizeof(statusPayload),
          "{\"mode\":\"%s\",\"pump\":%s,\"valve\":%s,\"pumpPWM\":%d}",
          manualMode ? "MANUAL" : "AUTO",
          pumpState ? "true" : "false",
          valveState ? "true" : "false",
          pumpPWM
        );

        client.publish(topic_status, statusPayload);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================= SETUP =================

void setup() {

  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(VALVE_PIN, OUTPUT);

  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(VALVE_PIN, LOW);

  dht.begin();

  analogWrite(PUMP_PIN, 0);

  // Create queues

  sensorQueue = xQueueCreate(1, sizeof(SensorData));
  alertQueue = xQueueCreate(1, sizeof(SensorData));

  if (sensorQueue == NULL || alertQueue == NULL) {

    Serial.println("Queue creation failed!");

    while (1) {
      delay(1000);
    }
  }

  // WiFi and MQTT

  WiFi.mode(WIFI_STA);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(512);

  // Create 3 FreeRTOS tasks

  xTaskCreatePinnedToCore(
    TaskReadSensor,
    "ReadSensor",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    TaskAlert,
    "AlertControl",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  xTaskCreatePinnedToCore(
    TaskSendMQTT,
    "SendMQTT",
    6144,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println("ESP32 System Started!");
}

void loop() {

  vTaskDelay(pdMS_TO_TICKS(1000));
}
