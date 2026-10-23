#include "server_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "secrets.h"

static unsigned long lastPollAt = 0;
static unsigned long lastNetworkLogAt = 0;
static const unsigned long NETWORK_LOG_INTERVAL_MS = 60000;

static void logNetworkStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi: connected | SSID: %s | IP: %s | Gateway: %s | DNS: %s | RSSI: %d dBm\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
                      WiFi.RSSI());
    } else {
        Serial.printf("WiFi: disconnected | status: %d | mode: %d\n",
                      WiFi.status(), WiFi.getMode());
    }
}

void initServerClient() {
    lastPollAt = millis();
    lastNetworkLogAt = millis();
}

void pollServer() {
    // Independent of the poll interval, including while WiFi is disconnected.
    unsigned long now = millis();
    if (now - lastNetworkLogAt >= NETWORK_LOG_INTERVAL_MS) {
        lastNetworkLogAt = now;
        logNetworkStatus();
    }
    if (millis() - lastPollAt < POLL_INTERVAL_MS) return;
    lastPollAt = millis();
    if (WiFi.status() != WL_CONNECTED) return;

    String url = String(SERVER_URL) + "/api/poll";
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("Poll error: could not open HTTPS connection");
        return;
    }
    http.addHeader("X-API-Key", ATOM_API_KEY);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST("{}");
    if (httpCode <= 0) {
        Serial.printf("Poll error: %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return;
    }
    if (httpCode != 200) {
        if (httpCode == 401) Serial.println("Poll error: unauthorized; check ATOM_API_KEY");
        else Serial.printf("Poll error: HTTP %d\n", httpCode);
        http.end();
        return;
    }
    String response = http.getString();
    http.end();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error) {
        Serial.printf("Poll error: invalid JSON (%s)\n", error.c_str());
        return;
    }
    if (doc["msg"].isNull()) return;

    int messageId = doc["msg"] | -1;
    const char* job = doc["job"] | "";
    const char* argument = doc["arg"] | "";
    Serial.printf("Job received: id=%d, type=%s, argument=%s\n",
                  messageId, job, argument);
}
