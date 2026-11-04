#pragma once
#include <stdint.h>
void deviceSettingsBegin();
const char *deviceServerHost();
uint16_t deviceServerPort();
bool deviceSetServer(const char *host, uint16_t port);
bool devicePrivateHost(const char *host);
