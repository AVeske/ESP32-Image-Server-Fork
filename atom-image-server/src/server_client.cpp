#include "server_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "letter_sender.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

static String stationUrl;
static unsigned long lastPollAt = 0;
static QueueHandle_t manualLetters = nullptr;
static SemaphoreHandle_t resultLock = nullptr;
static bool workerReady = false;
static void serverWorker(void*);

struct ResultEntry {
    bool used = false;
    bool confirmed = false;
    int id = -1;
    char result[8] = {};
    unsigned long retryAt = 0;
};
static ResultEntry results[32];

bool queueStationLetter(char letter) {
    return workerReady && letter >= 'A' && letter <= 'Z' &&
           xQueueSend(manualLetters, &letter, 0) == pdTRUE;
}

// 1 = upstream confirmed, 0 = pending, -1 = unavailable/full, -2 = conflict.
int queueStationResult(int id, const char* result) {
    if (!workerReady) return -1;
    xSemaphoreTake(resultLock, portMAX_DELAY);
    int freeSlot = -1;
    for (int i = 0; i < 32; ++i) {
        if (results[i].used && results[i].id == id) {
            int state = strcmp(results[i].result, result) != 0 ? -2
                      : (results[i].confirmed ? 1 : 0);
            xSemaphoreGive(resultLock);
            return state;
        }
        if (!results[i].used) freeSlot = i;
    }
    // Completed entries may be evicted; pending results must never be lost.
    if (freeSlot < 0) {
        for (int i = 0; i < 32; ++i) {
            if (results[i].confirmed) { freeSlot = i; break; }
        }
    }
    if (freeSlot < 0) { xSemaphoreGive(resultLock); return -1; }
    results[freeSlot] = ResultEntry();
    results[freeSlot].used = true;
    results[freeSlot].id = id;
    strlcpy(results[freeSlot].result, result, sizeof(results[freeSlot].result));
    xSemaphoreGive(resultLock);
    return 0;
}
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
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        Serial.printf("AP: %s | IP: %s | clients: %u\n",
                      WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
    }
}

void initServerClient(const char* configuredStationUrl) {
    stationUrl = configuredStationUrl;
    lastPollAt = millis();
    lastNetworkLogAt = millis();
    manualLetters = xQueueCreate(16, sizeof(char));
    resultLock = xSemaphoreCreateMutex();
    if (!manualLetters || !resultLock) {
        Serial.println("ERROR: could not initialize network worker");
        return;
    }
    workerReady = true;
    if (xTaskCreate(serverWorker, "server-client", 12288, nullptr, 1, nullptr) != pdPASS) {
        workerReady = false;
        Serial.println("ERROR: could not start network worker");
    }
}

// "Atom Accepted" reports successful forwarding; "done" is reserved for actual robot completion.
static bool sendJobResult(int messageId, const char* result) {
    Serial.printf("Sending result to server: id=%d, result=%s\n", messageId, result);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    if (!http.begin(client, String(SERVER_URL) + "/api/result")) {
        Serial.println("ERROR: could not open result connection");
        return false;
    }
    http.addHeader("X-API-Key", ATOM_API_KEY);
    http.addHeader("Content-Type", "application/json");
    JsonDocument doc;
    doc["id"] = messageId;
    doc["result"] = result;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    Serial.printf("Result HTTP response code: %d\n", code);
    bool confirmed = code >= 200 && code < 300;
    if (confirmed) Serial.println("Server confirmed result");
    else {
        Serial.println("ERROR: could not send result to server");
        Serial.println(http.getString());
    }
    http.end();
    return confirmed;
}

void pollServer() {
    // Independent of the poll interval, including while WiFi is disconnected.
    unsigned long now = millis();
    if (now - lastNetworkLogAt >= NETWORK_LOG_INTERVAL_MS) {
        lastNetworkLogAt = now;
        logNetworkStatus();
    }
}

// Only the worker performs HTTP. M5.update(), DNS and the local web server stay responsive.
static void pollServerOnce() {
    if (millis() - lastPollAt < POLL_INTERVAL_MS) return;
    lastPollAt = millis();
    if (WiFi.status() != WL_CONNECTED) return;

    String url = String(SERVER_URL) + "/api/poll";
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
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

    if (!doc["msg"].is<int>() || doc["msg"].as<int>() < 0) {
        Serial.println("ERROR: invalid job message ID; job ignored");
        return;
    }
    int messageId = doc["msg"].as<int>();
    const char* job = doc["job"] | "";
    const char* argument = doc["arg"] | "";
    Serial.printf("Job received: id=%d, type=%s, argument=%s\n",
                  messageId, job, argument);
    if (strcmp(job, "letter") != 0) {
        Serial.printf("Job not implemented yet: %s\n", job);
        queueStationResult(messageId, "refused");
        return;
    }
    Serial.println("Job recognized: letter");
    if (!doc["arg"].is<const char*>() || doc["arg"].as<JsonString>().size() != 1) {
        Serial.println("ERROR: invalid letter argument");
        queueStationResult(messageId, "error");
        return;
    }
    char letter = argument[0];
    if (letter >= 'a' && letter <= 'z') letter -= 'a' - 'A';
    if (letter < 'A' || letter > 'Z') {
        Serial.println("ERROR: invalid letter argument");
        queueStationResult(messageId, "error");
        return;
    }
    Serial.printf("Forwarding letter %c to station...\n", letter);
    // Explicit HTTP transport preserves the button's existing transport choice.
    bool accepted = sendLetter(stationUrl, letter, LetterTransport::WIFI, messageId);
    if (accepted) Serial.printf("Station accepted letter %c\n", letter);
    else Serial.printf("ERROR: station rejected or could not receive letter %c\n", letter);
    if (accepted) sendJobResult(messageId, "Atom Accepted");
    else queueStationResult(messageId, "error");
}

static void serverWorker(void*) {
    int nextResultSlot = 0;
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            // Copy under the mutex, then release it before any network wait.
            for (int offset = 0; offset < 32; ++offset) {
                int i = (nextResultSlot + offset) % 32;
                xSemaphoreTake(resultLock, portMAX_DELAY);
                ResultEntry entry = results[i];
                xSemaphoreGive(resultLock);
                if (!entry.used || entry.confirmed ||
                    (entry.retryAt && (int32_t)(millis() - entry.retryAt) < 0)) continue;
                bool confirmed = sendJobResult(entry.id, entry.result);
                xSemaphoreTake(resultLock, portMAX_DELAY);
                results[i].confirmed = confirmed;
                results[i].retryAt = millis() + 5000;
                xSemaphoreGive(resultLock);
                // Round-robin prevents one failed report starving later jobs.
                nextResultSlot = (i + 1) % 32;
                break;
            }
            pollServerOnce();
        }
        char letter;
        if (xQueueReceive(manualLetters, &letter, 0) == pdTRUE) {
            bool sent = sendLetter(stationUrl, letter, LetterTransport::WIFI);
            Serial.printf("Manual letter %c: %s\n", letter, sent ? "station accepted" : "send failed");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
