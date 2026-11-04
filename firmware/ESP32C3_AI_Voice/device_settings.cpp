#include "device_settings.h"
#include "secrets.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
struct ServerProfile { char host[64]; uint16_t port; };
static ServerProfile profile = {SERVER_HOST, SERVER_PORT};
bool devicePrivateHost(const char *host) {
  unsigned a,b,c,d; char tail;
  if (!host || sscanf(host,"%u.%u.%u.%u%c",&a,&b,&c,&d,&tail)!=4 || a>255 || b>255 || c>255 || d>255) return false;
  char canonical[16]; snprintf(canonical,sizeof(canonical),"%u.%u.%u.%u",a,b,c,d);
  return strcmp(canonical,host)==0 && (a==10 || (a==172 && b>=16 && b<=31) || (a==192 && b==168));
}
void deviceSettingsBegin() {
  nvs_handle_t n;
  if(nvs_open("control",NVS_READONLY,&n)!=ESP_OK) return;
  ServerProfile saved={}; size_t len=sizeof(saved);
  if(nvs_get_blob(n,"server",&saved,&len)==ESP_OK && len==sizeof(saved) &&
     memchr(saved.host,0,sizeof(saved.host)) && devicePrivateHost(saved.host) && saved.port) profile=saved;
  nvs_close(n);
}
const char *deviceServerHost(){ return profile.host; }
uint16_t deviceServerPort(){ return profile.port; }
bool deviceSetServer(const char *host,uint16_t port) {
  if(!devicePrivateHost(host) || !port) return false;
  ServerProfile candidate={}; strlcpy(candidate.host,host,sizeof(candidate.host)); candidate.port=port;
  nvs_handle_t n;
  if(nvs_open("control",NVS_READWRITE,&n)!=ESP_OK) return false;
  bool ok=nvs_set_blob(n,"server",&candidate,sizeof(candidate))==ESP_OK && nvs_commit(n)==ESP_OK;
  nvs_close(n); if(ok) profile=candidate; return ok;
}
