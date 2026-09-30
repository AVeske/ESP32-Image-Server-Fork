#include "letter_sender.h"

#include <WiFi.h>
#include <HTTPClient.h>

bool sendLetter(const String& stationUrl, char letter) {

  HTTPClient http;

  String url = stationUrl + "/letter";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"letter\":\"";
  body += letter;
  body += "\"}";

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