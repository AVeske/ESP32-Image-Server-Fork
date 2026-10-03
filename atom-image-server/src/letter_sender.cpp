#include "letter_sender.h"

#include <WiFi.h>
#include <HTTPClient.h>

static String letterPayload(char letter) {
  String body = "{\"letter\":\"";
  body += letter;
  body += "\"}";
  return body;
}

static bool sendLetterWifi(const String& stationUrl, char letter) {

  HTTPClient http;

  String url = stationUrl + "/api/letter";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String body = letterPayload(letter);

  Serial.print("Sending letter to: ");
  Serial.println(url);

  Serial.print("Payload: ");
  Serial.println(body);

  int statusCode = http.POST(body);

  if (statusCode <= 0) {
    Serial.print("HTTP POST failed: ");
    Serial.println(statusCode);

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

bool sendLetter(const String& stationUrl, char letter) {
  if (LETTER_TRANSPORT == LetterTransport::SERIAL_PORT) {
    return sendLetterSerial(letter);
  }

  return sendLetterWifi(stationUrl, letter);
}
