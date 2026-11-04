#include "ble_control.h"
#include "platform.h"
#include "config.h"
#include "device_settings.h"
#include "audio_io.h"
#include "cue.h"
#include "session.h"
#include "power.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "nvs.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security1.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#if __has_include("ble_gateway_private.h")
#include "ble_gateway_private.h"
#endif
#ifndef BLE_GATEWAY_CREDENTIAL
#define BLE_GATEWAY_CREDENTIAL ""
#endif

// BLE callbacks only validate/enqueue/copy snapshots. GPIO/audio/NVS/WiFi belong to app_main.
static protocomm_t *pc;
static SemaphoreHandle_t lock;
struct Request { char id[33],op[16],a[65],b[65]; int number; };
struct Result { char id[33],state[16],code[32]; };
static Request pending={};
static char activeId[33]={};
static Result results[4]={}; static unsigned nextResult;
static char owner[65]={}, candidate[65]={};
static uint32_t confirmAt; static bool requireRelease;
struct Snapshot { bool wifi,ready,usb; int volume; char ssid[33],ip[16],state[8],host[64]; unsigned port; };
static Snapshot snapshot={};
static char wifiSsid[33]={},wifiPass[65]={},wifiId[33]={}; static uint32_t wifiAt;

static bool hex(const char *s,size_t n) {
  if(!s || strlen(s)!=n) return false;
  for(size_t i=0;i<n;i++) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
  return true;
}
static bool equalKey(const char *a,const char *b) {
  unsigned diff=0; for(unsigned i=0;i<64;i++) diff|=(unsigned char)a[i]^(unsigned char)b[i]; return diff==0;
}
static const char *str(cJSON *j,const char *name){ cJSON *v=cJSON_GetObjectItemCaseSensitive(j,name); return cJSON_IsString(v)?v->valuestring:""; }
static cJSON *response(const char *state,const char *code="") {
  cJSON *j=cJSON_CreateObject(); cJSON_AddNumberToObject(j,"v",1);
  cJSON_AddStringToObject(j,"result",state); cJSON_AddStringToObject(j,"code",code); return j;
}
static void finish(const char *id,const char *state,const char *code="") {
  xSemaphoreTake(lock,portMAX_DELAY); Result &r=results[nextResult++%4];
  strlcpy(r.id,id,sizeof(r.id)); strlcpy(r.state,state,sizeof(r.state)); strlcpy(r.code,code,sizeof(r.code));
  if(!strcmp(id,activeId)) activeId[0]=0;
  xSemaphoreGive(lock);
}
static esp_err_t handle(uint32_t,const uint8_t *in,ssize_t len,uint8_t **out,ssize_t *outLen,void*) {
  if(len<=0 || len>384) return ESP_ERR_INVALID_ARG;
  // Only a flat JSON object is accepted; reject nesting before calling a recursive parser.
  bool quoted=false,escaped=false; int depth=0;
  for(ssize_t i=0;i<len;i++) {
    char ch=(char)in[i];
    if(quoted){ if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false; }
    else if(ch=='"')quoted=true;
    else if(ch=='['||ch==']'||(ch=='{'&&++depth>1)||(ch=='}'&&--depth<0))return ESP_ERR_INVALID_ARG;
  }
  if(quoted||depth) return ESP_ERR_INVALID_ARG;
  cJSON *request=cJSON_ParseWithLength((const char*)in,len);
  if(!cJSON_IsObject(request)){ cJSON_Delete(request); return ESP_ERR_INVALID_ARG; }
  const char *op=str(request,"o"),*key=str(request,"k"),*id=str(request,"i");
  xSemaphoreTake(lock,portMAX_DELAY);
  const bool authorized=hex(key,64)&&owner[0]&&equalKey(key,owner);
  cJSON *reply=nullptr;
  if(!strcmp(op,"auth")) {
    if(!hex(key,64)) reply=response("rejected","invalid_key");
    else if(owner[0]) reply=response(authorized?"completed":"rejected",authorized?"":"not_owner");
    else if(candidate[0]&&!equalKey(key,candidate)) reply=response("rejected","pairing_busy");
    else if(snapshot.state[0] && strcmp(snapshot.state,"IDLE") && strcmp(snapshot.state,"ERR")) reply=response("rejected","device_busy");
    else { if(!candidate[0]) { memcpy(candidate,key,65); confirmAt=appMillis(); requireRelease=true; } reply=response("confirm","press_button"); }
  } else if(!authorized) reply=response("rejected","not_owner");
  else if(!strcmp(op,"status")) {
    reply=response("completed");
    cJSON_AddBoolToObject(reply,"wifi",snapshot.wifi); cJSON_AddBoolToObject(reply,"ready",snapshot.ready);
    cJSON_AddBoolToObject(reply,"usb",snapshot.usb); cJSON_AddNumberToObject(reply,"volume",snapshot.volume);
    cJSON_AddStringToObject(reply,"ssid",snapshot.ssid); cJSON_AddStringToObject(reply,"ip",snapshot.ip);
    cJSON_AddStringToObject(reply,"state",snapshot.state); cJSON_AddStringToObject(reply,"firmware",FW_VERSION);
    cJSON_AddStringToObject(reply,"host",snapshot.host); cJSON_AddNumberToObject(reply,"port",snapshot.port);
    cJSON_AddBoolToObject(reply,"switching",wifiId[0]!=0);
  } else if(!strcmp(op,"gateway")) {
    reply=response("completed"); char address[96]; snprintf(address,sizeof(address),"http://%s:8795",snapshot.host);
    cJSON_AddStringToObject(reply,"server",address);
    cJSON_AddStringToObject(reply,"credential",BLE_GATEWAY_CREDENTIAL);
  } else if(!strcmp(op,"result")) {
    reply=response("unknown");
    if(hex(id,32)) {
      if(!strcmp(id,pending.id) || !strcmp(id,activeId) || !strcmp(id,wifiId)) { cJSON_Delete(reply); reply=response("running"); }
      for(auto &r:results) if(!strcmp(id,r.id)) { cJSON_Delete(reply); reply=response(r.state,r.code); break; }
    }
  } else if(!hex(id,32)) reply=response("rejected","invalid_id");
  else {
    for(auto &r:results) if(!strcmp(id,r.id)) { reply=response(r.state,r.code); break; }
    if(!reply && (!strcmp(id,pending.id)||!strcmp(id,activeId)||!strcmp(id,wifiId))) reply=response("running");
    if(!reply && (pending.id[0]||activeId[0])) reply=response("rejected","device_busy");
    if(!reply) {
      Request command={}; strlcpy(command.id,id,sizeof(command.id)); strlcpy(command.op,op,sizeof(command.op));
      bool valid=!strcmp(op,"identify");
      cJSON *value=cJSON_GetObjectItemCaseSensitive(request,"n");
      if(!strcmp(op,"volume")) { valid=cJSON_IsNumber(value)&&value->valuedouble==value->valueint&&value->valueint>=0&&value->valueint<=100; if(valid) command.number=value->valueint; }
      if(!strcmp(op,"wifi")) {
        const char *ssid=str(request,"s"),*pass=str(request,"p");
        valid=ssid[0]&&strlen(ssid)<=32&&strlen(pass)<=63&&(!pass[0]||strlen(pass)>=8)&&!wifiId[0];
        if(valid){strlcpy(command.a,ssid,sizeof(command.a));strlcpy(command.b,pass,sizeof(command.b));}
      }
      if(!strcmp(op,"server")) {
        valid=devicePrivateHost(str(request,"s"))&&cJSON_IsNumber(value)&&value->valuedouble==value->valueint&&value->valueint>0&&value->valueint<=65535;
        if(valid){strlcpy(command.a,str(request,"s"),sizeof(command.a));command.number=value->valueint;}
      }
      if(valid) { pending=command; reply=response("accepted"); } else reply=response("rejected","invalid_request");
    }
  }
  xSemaphoreGive(lock); cJSON_Delete(request);
  char *json=cJSON_PrintUnformatted(reply); cJSON_Delete(reply);
  if(!json) return ESP_ERR_NO_MEM;
  *out=(uint8_t*)json; *outLen=strlen(json); return ESP_OK;
}
bool bleControlBegin() {
  lock=xSemaphoreCreateMutex(); if(!lock) return false;
  nvs_handle_t n;
  if(nvs_open("control",NVS_READONLY,&n)==ESP_OK) { size_t len=sizeof(owner); if(nvs_get_str(n,"owner",owner,&len)!=ESP_OK||!hex(owner,64)) owner[0]=0; nvs_close(n); }
  pc=protocomm_new(); if(!pc) return false;
  protocomm_ble_config_t cfg={};
  const uint8_t uuid[]={0x3f,0xdb,0x78,0xcc,0xd3,0x9c,0x94,0x85,0xa8,0x4c,0xaa,0xf2,0x9b,0x4c,0x62,0x10};
  memcpy(cfg.service_uuid,uuid,16);
  uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
  snprintf(cfg.device_name,sizeof(cfg.device_name),"AIFIG_%02X%02X%02X",mac[3],mac[4],mac[5]);
  static protocomm_ble_name_uuid_t endpoints[]={{"prov-session",0xFF51},{"proto-ver",0xFF53},{"device-control",0xFF54}};
  cfg.nu_lookup_count=3; cfg.nu_lookup=endpoints;
  if(protocomm_ble_start(pc,&cfg)!=ESP_OK) { protocomm_delete(pc); pc=nullptr; return false; }
  if(protocomm_set_version(pc,"proto-ver","{\"prov\":{\"sec_ver\":1},\"control\":1}")!=ESP_OK ||
     protocomm_set_security(pc,"prov-session",&protocomm_security1,nullptr)!=ESP_OK ||
     protocomm_add_endpoint(pc,"device-control",handle,nullptr)!=ESP_OK) { bleControlStop(); return false; }
  printf("[BLE] %s control=1 available offline\n",cfg.device_name); return true;
}
void bleControlStop(){ if(pc){ protocomm_ble_stop(pc); protocomm_delete(pc); pc=nullptr; } }
void bleControlUpdate() {
  if(!pc) return;
  Snapshot current={}; current.wifi=netWifiIsUp(); current.ready=sessionServiceReady(); current.usb=powerUsbPresent();
  current.volume=(getOutputGain()*100+128)/256; netWifiSsid(current.ssid,sizeof(current.ssid));
  auto ip=netWifiIp(); snprintf(current.ip,sizeof(current.ip),IPSTR,IP2STR(&ip));
  strlcpy(current.state,sessionStateName(),sizeof(current.state)); strlcpy(current.host,deviceServerHost(),sizeof(current.host)); current.port=deviceServerPort();
  Request command={}; char confirm[65]={};
  xSemaphoreTake(lock,portMAX_DELAY); snapshot=current;
  if(candidate[0]) {
    sessionSuppressButton();
    const bool down=gpio_get_level((gpio_num_t)PIN_PTT_BUTTON)==0;
    if(requireRelease&&!down) requireRelease=false;
    if(!requireRelease&&down) {memcpy(confirm,candidate,sizeof(confirm));candidate[0]=0;}
    else if(appMillis()-confirmAt>60000) {candidate[0]=0;}
  }
  if(pending.id[0]) {command=pending;strlcpy(activeId,command.id,sizeof(activeId));pending={};}
  xSemaphoreGive(lock);
  if(confirm[0]) {
    nvs_handle_t n; bool saved=false;
    if(nvs_open("control",NVS_READWRITE,&n)==ESP_OK){saved=nvs_set_str(n,"owner",confirm)==ESP_OK&&nvs_commit(n)==ESP_OK;nvs_close(n);}
    if(saved){xSemaphoreTake(lock,portMAX_DELAY);memcpy(owner,confirm,sizeof(owner));xSemaphoreGive(lock);powerNoteActivity();}
    memset(confirm,0,sizeof(confirm));
  }
  if(wifiId[0]) {
    bool done=false,ok=false; const char *code="";
    if(netWifiIsUp()){done=true;ok=netWifiSaveCreds(wifiSsid,wifiPass);code=ok?"":"storage_failed";}
    else if(appMillis()-wifiAt>35000){done=true;code="wifi_failed_restored";}
    if(done){ if(!ok) {netWifiStop();netWifiBegin();} finish(wifiId,ok?"completed":"failed",code); xSemaphoreTake(lock,portMAX_DELAY);wifiId[0]=0;xSemaphoreGive(lock);memset(wifiPass,0,sizeof(wifiPass)); }
  }
  if(!command.id[0]) return;
  powerNoteActivity();
  bool idle=(sessionState()==VoiceState::IDLE||sessionState()==VoiceState::ERROR_STATE)&&!audioIsPlaying()&&!audioIsCapturing()&&gpio_get_level((gpio_num_t)PIN_PTT_BUTTON)!=0;
  const bool liveVolume=!strcmp(command.op,"volume") && sessionState()!=VoiceState::RECORDING && !audioIsCapturing();
  if((!idle && !liveVolume) || wifiId[0]) { finish(command.id,"rejected","device_busy");return; }
  if(!strcmp(command.op,"identify")){ bool ok=cueIdentify();finish(command.id,ok?"completed":"failed",ok?"":"audio_failed"); }
  else if(!strcmp(command.op,"volume")){ setOutputGain((command.number*256+50)/100);finish(command.id,"completed"); }
  else if(!strcmp(command.op,"server")){bool ok=deviceSetServer(command.a,command.number);if(ok)wsClose("BLE service change");finish(command.id,ok?"completed":"failed",ok?"":"storage_failed");}
  else if(!strcmp(command.op,"wifi")) {
    wsClose("BLE WiFi change");
    if(netWifiTryCredentials(command.a,command.b)) {
      memcpy(wifiSsid,command.a,sizeof(wifiSsid));memcpy(wifiPass,command.b,sizeof(wifiPass));wifiAt=appMillis();
      xSemaphoreTake(lock,portMAX_DELAY);strlcpy(wifiId,command.id,sizeof(wifiId));xSemaphoreGive(lock);
      xSemaphoreTake(lock,portMAX_DELAY);activeId[0]=0;xSemaphoreGive(lock);
    } else {netWifiStop();netWifiBegin();finish(command.id,"failed","wifi_failed_restored");}
  }
  memset(command.b,0,sizeof(command.b));
}
