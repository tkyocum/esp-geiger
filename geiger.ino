#include <stdint.h>
#include <string.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <InfluxDbClient.h>

// 4 translates to D2 on the NodeMCU v3 ESP8266 board
#define PIN    4

#define MQTT_HOST   "x.x.x.x"
#define MQTT_PORT   1883
#define MQTT_TOPIC    "geiger"
#define LOG_PERIOD 10

#define INFLUXDB_URL "http://x.x.x.x:8086"
#define INFLUXDB_TOKEN "token"
#define INFLUXDB_ORG "orgid"
#define INFLUXDB_BUCKET "geiger"

static char esp_id[16];
const char* ssid     = "ssid";
const char* password = "password";

const bool mqtt_enabled = false;
const bool influxdb_enabled = true;

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);
static InfluxDBClient InfluxDBClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);

static volatile unsigned long counts = 0;
static unsigned long int secidx_prev = 0;
static unsigned long int count_prev = 0;
static unsigned long int second_prev = 0;
static int secondcounts[60];

ICACHE_RAM_ATTR static void tube_impulse(void)
{
    counts++;
}

void setup(void)
{
  Serial.begin(115200);
  sprintf(esp_id, "%08X", ESP.getChipId());

  WiFi.begin(ssid, password);
  Serial.print("\n\r \n\rConnecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("Geiger Counter");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

    // start counting
    memset(secondcounts, 0, sizeof(secondcounts));
    Serial.println("Starting count ...");
    pinMode(PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN), tube_impulse, FALLING);
}

static bool mqtt_send(const char *topic, const char *value, bool retained)
{
    bool result = false;
    if (!mqttClient.connected() && mqtt_enabled == true) {
        Serial.print("Connecting to MQTT...");
        mqttClient.setServer(MQTT_HOST, MQTT_PORT);
        result = mqttClient.connect(esp_id, topic, 0, retained, "offline");
        Serial.println(result ? "OK" : "FAIL");
    }
    if (mqttClient.connected() && mqtt_enabled == true) {
        Serial.print("Publishing ");
        Serial.print(value);
        Serial.print(" to ");
        Serial.print(topic);
        Serial.print("...");
        result = mqttClient.publish(topic, value, retained);
        Serial.println(result ? "OK" : "FAIL");
    }
    return result;
}

void loop()
{
    unsigned long int second = millis() / 1000;
    unsigned long int secidx = second % 60;
    if (secidx != secidx_prev) {
        unsigned long int count = counts;
        secondcounts[secidx_prev] = count - count_prev;
        count_prev = count;
        secidx_prev = secidx;
    }
    // report every LOG_PERIOD
    if ((second - second_prev) >= LOG_PERIOD) {
        second_prev = second;

        // calculate CPM sum
        int cpm = 0;
        for (int i = 0; i < 60; i++) {
            cpm += secondcounts[i];
        }
        
        // calculate microsieverts from CPM using J305 tube conversion factor
        float uS = 0.00812 * cpm;

        // send to MQTT (if enabled)
        char message[16];
        snprintf(message, sizeof(message), "%d cpm", cpm);
        
        if (!mqtt_send(MQTT_TOPIC, message, true) && mqtt_enabled == true) {
            Serial.println("Restarting ESP...");
            ESP.restart();
        }

        // send to InfluxDB (if enabled)
        if (influxdb_enabled == true) {
            Serial.print("Publishing ");
            Serial.print(cpm);
            Serial.print(" CPM to ");
            Serial.print(INFLUXDB_URL);
            Serial.print("...\n");
            Point pointDevice("counts");
            pointDevice.addTag("device", esp_id);
            pointDevice.addField("cpm", cpm);
            pointDevice.addField("uS", uS);
            InfluxDBClient.writePoint(pointDevice);
        }
    }
    // keep MQTT alive
    if (mqtt_enabled == true) { 
      mqttClient.loop();
    }
}
