#pragma once

void initServerClient(const char* stationUrl);
void pollServer();
// Nonblocking handoff to the HTTP worker.
bool queueStationLetter(char letter);
int queueStationResult(int id, const char* result);
