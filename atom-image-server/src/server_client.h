#pragma once
#include <Arduino.h>

using ServerImageHandler = bool (*)(const uint8_t* png, size_t length);
void setServerImageHandler(ServerImageHandler handler);

void initServerClient(const char* stationUrl);
void pollServer();
// Nonblocking handoff to the HTTP worker.
bool queueStationLetter(char letter);
int queueStationResult(int id, const char* result);
