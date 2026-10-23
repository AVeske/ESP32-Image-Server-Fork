#include "letter_sender.h"

#include <WiFi.h>
#include <HTTPClient.h>

static String letterPayload(char letter, int jobId = -1) {
  String body = "{\"letter\":\"";
  body += letter;
  body += "\"";
  if (jobId >= 0) body += ",\"id\":" + String(jobId);
  body += "}";
  return body;
}

static bool sendLetterWifi(const String& stationUrl, char letter, int jobId) {

  HTTPClient http;

  String url = stationUrl + "/api/letter";

  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");

  String body = letterPayload(letter, jobId);

  Serial.print("Sending letter to: ");
  Serial.println(url);

  Serial.print("Payload: ");
  Serial.println(body);

  int statusCode = http.POST(body);

  if (statusCode <= 0) {
    Serial.print("HTTP POST failed: ");
    Serial.printf("%d (%s)\n", statusCode, http.errorToString(statusCode).c_str());

    http.end();
    return false;
  }

  Serial.print("HTTP response: ");
  Serial.println(statusCode);

  String response = http.getString();
  Serial.println(response);

  http.end();

  return statusCode >= 200 && statusCode < 300;
}

// The COM port is shared with the debug log, so the payload goes out as a line
// of its own — the station picks out the lines starting with {"letter".
static bool sendLetterSerial(char letter) {
  String body = letterPayload(letter);

  size_t written = Serial.println(body);
  Serial.flush();

  return written >= body.length();
}

bool sendLetter(const String& stationUrl, char letter, LetterTransport transport, int jobId) {
  if (transport == LetterTransport::SERIAL_PORT) {
    return sendLetterSerial(letter);
  }

  return sendLetterWifi(stationUrl, letter, jobId);
}
