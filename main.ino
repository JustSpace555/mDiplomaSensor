#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <Ed25519.h>
#include <ArduinoJson.h>
#include <SHA256.h>

const int sensorPin = A0;
String ssid = "Wserver";
String pass = "Rdfhnbhf28";
String host = "http://192.168.1.5:3000";

uint8_t privateKey[32];
uint8_t publicKey[32];
float gasLiters;

int oldInputValue = 0;
int newInputValue = 0;

HTTPClient httpClient;
WiFiClient wifiClient;

void setup() {
  Serial.begin(9600);
  Serial.println();

  connectToWiFi();
  configTime(0, 0, "pool.ntp.org");

  EEPROM.begin(68);
  bool isRegistered = checkIsSensorHasBeenRegistered();
  if (!isRegistered) {
    generateKeys();
    int i = 1;
    while (!registerSensor()) {
      Serial.println("Unsuccessfull atempt to register. Will try again in 10 seconds");
      delay(10000);
      Serial.printf("Attempt to register #%d\n", ++i);
    }
  }
}

void loop() {
  newInputValue = analogRead(sensorPin);
  Serial.printf("New analog input value: %d\n", newInputValue);
  newInputValue = map(newInputValue, 1024, 500, 0, 100);
  float newGasLevel = gasLiters * newInputValue / 100;
  Serial.printf("New gas level: %d%. Total amount: %f\n", newInputValue, newGasLevel);
  if (abs(oldInputValue - newInputValue) >= 5) {
    sendNewTransaction(newGasLevel);
    oldInputValue = newInputValue;
  }
  delay(2000);
}

void connectToWiFi() {
  Serial.println("Connecting to WiFi");
  WiFi.begin(ssid, pass);
  int atempt = 0;

  wl_status_t status = WiFi.status();
  while (status != WL_CONNECTED) {
    Serial.printf("Connecting to WiFi; Attempt #%d; Status: ", ++atempt);
    if (status == WL_IDLE_STATUS) {
        Serial.println("WL_IDLE_STATUS");
    } else if (status == WL_NO_SSID_AVAIL) {
      Serial.println("WL_NO_SSID_AVAIL");
    } else if (status == WL_SCAN_COMPLETED) {
      Serial.println("WL_SCAN_COMPLETED");
    } else if (status == WL_CONNECT_FAILED) {
      Serial.println("WL_CONNECT_FAILED");
    } else if (status == WL_CONNECTION_LOST) {
      Serial.println("WL_CONNECTION_LOST");
    } else if (status == WL_WRONG_PASSWORD) {
      Serial.println("WL_WRONG_PASSWORD");
    } else if (status == WL_DISCONNECTED) {
      Serial.println("WL_DISCONNECTED");
    } else {
      Serial.println("No status");
    }
    delay(1000);
    status = WiFi.status();
  }
  Serial.println();

  Serial.printf("IP address: %s\n", WiFi.localIP().toString());
}

bool checkIsSensorHasBeenRegistered() {
  Serial.println("Checking has sensor been registered");
  if (EEPROM.length() == 0) {
    return false;
  }

  Serial.println("Getting data from ROM:");
  EEPROM.get(0, privateKey);
  Serial.printf("Private key: %s\n", privateKey);
  EEPROM.get(32, publicKey);
  Serial.printf("Public key: %s\n", publicKey);
  EEPROM.get(64, gasLiters);
  Serial.printf("Gas liters: %f\n", gasLiters);
  EEPROM.end();

  return gasLiters > 0;
}

void generateKeys() {
  Serial.println("Generating key pair values");
  Ed25519::generatePrivateKey(privateKey);
  EEPROM.put(0, privateKey);
  Serial.printf("Private key generated: %s\n", (char *)privateKey);

  Ed25519::derivePublicKey(publicKey, privateKey);
  Serial.printf("Public key generated: %s\n", (char *)publicKey);
  EEPROM.put(32, publicKey);
}

bool registerSensor() {
  Serial.println("Registering sensor");

  Serial.println("Sending register request");

  unsigned int espChipId = ESP.getChipId();
  Serial.printf("ESP chip id: %u\n", espChipId);
  httpClient.begin(wifiClient, host + "/sensor/register/" + String(espChipId));
  httpClient.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonArray array = root.createNestedArray("publicKey");
  for (int i = 0; i < 32; i++) {
    array.add((unsigned char) publicKey[i]);
  }

  String json;
  serializeJson(root, json);
  Serial.print("Body: ");
  serializeJson(doc, Serial);
  Serial.println();

  int responseCode = httpClient.POST(json);
  gasLiters = httpClient.getString().toFloat();
  EEPROM.put(64, gasLiters);
  EEPROM.commit();
  EEPROM.end();

  Serial.printf("Response code: %d\n", responseCode);
  Serial.printf("Gas liters: %d\n", gasLiters);

  httpClient.end();

  return responseCode >= 200 && responseCode < 300;
}

void sendNewTransaction(float currentGasLevel) {
  Serial.println("Getting tips");
  httpClient.begin(wifiClient, host + "/sensor/tips");
  int responseCode = httpClient.GET();
  if (responseCode < 200 || responseCode >= 300) {
    Serial.printf("Error, response code: %d, message: %s\n", responseCode, httpClient.getString());
    httpClient.end();
    return;
  }

  String tips = httpClient.getString();
  Serial.printf("Tips: %s\n", tips);

  String firstTip;
  String secondTip;
  if (tips.length() == 0) {
    firstTip = "Empty hash #1";
    secondTip = "Empty hash #2";
  }
  else if (tips.indexOf(",") == -1) {
    firstTip = tips;
    secondTip = "Empty hash #3";
  }
  else {
    firstTip = tips.substring(0, tips.indexOf(","));
    secondTip = tips.substring(tips.indexOf(",") + 1, tips.length());
  }
  Serial.printf("First tip: %s\nSecond tip: %s\n", firstTip, secondTip);

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  unsigned int espChipId = ESP.getChipId();
  root["id"] = espChipId;

  JsonArray array = root.createNestedArray("prevIds");
  array.add(firstTip);
  array.add(secondTip);

  root["gasLevel"] = String(currentGasLevel);
  unsigned long currentTime = getTime();
  root["epochTime"] = currentTime;

  String messageToHash = String(espChipId) + "," + firstTip + "," + secondTip + "," +
    String(currentGasLevel) + "," +
    String(currentTime);
  
  char tempArray[messageToHash.length() + 1];
  messageToHash.toCharArray(tempArray, messageToHash.length() + 1);
  SHA256 hasher = SHA256();
  hasher.update(tempArray, messageToHash.length() + 1);

  size_t hashSize = hasher.hashSize();
  uint8_t sha256Hash[hashSize];
  hasher.finalize(sha256Hash, hashSize);
  Serial.printf("Hash: %s\n", (char *)sha256Hash);

  uint8_t result[64];
  Ed25519::sign(result, privateKey, publicKey, sha256Hash, hashSize);

  Serial.printf("Signed hash: %s\n", (char *)result);

  root["signedHash"] = result;

  String body;
  serializeJson(root, body);
  Serial.print("Body: ");
  serializeJson(root, Serial);
  Serial.println();

  httpClient.begin(wifiClient, host + "/sensor/newTransaction");
  httpClient.addHeader("Content-Type", "application/json");
  responseCode = httpClient.POST(body);

  Serial.printf("Response code: %d\n", responseCode);
  if (responseCode < 200 || responseCode >= 300) {
    Serial.println(httpClient.getString());
  }
  httpClient.end();
}

unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return(0);
  }
  time(&now);
  return now;
}
