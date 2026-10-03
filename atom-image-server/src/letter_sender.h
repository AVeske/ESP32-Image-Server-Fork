#pragma once

#include <Arduino.h>

enum class LetterTransport { WIFI, SERIAL_PORT };

// How the letter reaches the station: HTTP POST over WiFi, or a JSON line on
// the USB serial (COM) port.
static const LetterTransport LETTER_TRANSPORT = LetterTransport::SERIAL_PORT;

// stationUrl is only used with LetterTransport::WIFI.
bool sendLetter(const String& stationUrl, char letter);
