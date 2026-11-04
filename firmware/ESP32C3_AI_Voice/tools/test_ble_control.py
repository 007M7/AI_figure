"""Exercise real BLE endpoint and main-loop command code with fake GPIO/radio/NVS.

No serial access, real credentials or network operations. Uses the pinned cJSON
source from the configured ESP-IDF build cache.
"""
from pathlib import Path
import argparse, hashlib, os, shutil, subprocess, tempfile

project=Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cjson-dir', type=Path, help='Directory containing the pinned cJSON.c and cJSON.h')
args = parser.parse_args()
suffix = hashlib.sha256(str(project).encode()).hexdigest().upper()[:12]
candidates = [args.cjson_dir, os.environ.get('CJSON_DIR'),
              project/'managed_components/espressif__cjson/cJSON',
              Path(tempfile.gettempdir())/f'ai-figure-idf61-{suffix}'/'managed_components/espressif__cjson/cJSON']
component = next((Path(p) for p in candidates if p and
                  (Path(p)/'cJSON.c').is_file() and (Path(p)/'cJSON.h').is_file()), None)
if component is None:
    raise SystemExit('Build the firmware first, or pass --cjson-dir pointing to espressif/cjson 1.7.19~2')
compiler=shutil.which('g++'); cc=shutil.which('gcc')
assert compiler and cc
headers={
'platform.h':r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
using esp_err_t=int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
extern uint32_t nowMs;
inline uint32_t appMillis(){return nowMs;}
inline size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){memcpy(d,s,l<n?l:n-1);d[l<n?l:n-1]=0;}return l;}
''',
'driver/gpio.h':'''#pragma once
using gpio_num_t=int;
extern bool buttonDown;
inline int gpio_get_level(int){return buttonDown?0:1;}
''',
'esp_mac.h':'''#pragma once
#define ESP_MAC_WIFI_STA 0
inline int esp_read_mac(unsigned char*p,int){memset(p,0,6);return 0;}
''',
'freertos/semphr.h':'''#pragma once
using SemaphoreHandle_t=int;
#define portMAX_DELAY 100
inline int xSemaphoreCreateMutex(){return 1;}
inline int xSemaphoreTake(int,int){return 1;}
inline int xSemaphoreGive(int){return 1;}
''',
'nvs.h':'''#pragma once
#include "platform.h"
using nvs_handle_t=int;
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char*,int,int*);
int nvs_get_str(int,const char*,char*,size_t*);
int nvs_set_str(int,const char*,const char*);
int nvs_get_blob(int,const char*,void*,size_t*);
int nvs_set_blob(int,const char*,const void*,size_t);
int nvs_commit(int);
inline void nvs_close(int){}
''',
'protocomm.h':'''#pragma once
#include "platform.h"
#include <sys/types.h>
struct protocomm_t{};
inline protocomm_t*protocomm_new(){static protocomm_t p;return &p;}
inline void protocomm_delete(protocomm_t*){}
inline int protocomm_set_version(protocomm_t*,const char*,const char*){return 0;}
inline int protocomm_set_security(protocomm_t*,const char*,const void*,void*){return 0;}
using Handler=int(*)(uint32_t,const uint8_t*,ssize_t,uint8_t**,ssize_t*,void*);
inline int protocomm_add_endpoint(protocomm_t*,const char*,Handler,void*){return 0;}
''',
'protocomm_ble.h':'''#pragma once
#include "protocomm.h"
struct protocomm_ble_name_uuid_t{const char*name;unsigned uuid;};
struct protocomm_ble_config_t{char device_name[30];unsigned char service_uuid[16];int nu_lookup_count;protocomm_ble_name_uuid_t*nu_lookup;};
inline int protocomm_ble_start(protocomm_t*,void*){return 0;}
inline void protocomm_ble_stop(protocomm_t*){}
''',
'protocomm_security1.h':'#pragma once\nstatic int protocomm_security1;\n',
'esp_netif_ip_addr.h':'''#pragma once
struct esp_ip4_addr_t{unsigned char bytes[4];};
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (p)->bytes[0],(p)->bytes[1],(p)->bytes[2],(p)->bytes[3]
''',
'secrets.h':'#pragma once\n#define SERVER_HOST "192.168.137.1"\n#define SERVER_PORT 8766\n',
'ble_gateway_private.h':'#pragma once\n#define BLE_GATEWAY_CREDENTIAL "test-only-bootstrap-not-a-real-secret"\n',
}
harness=r'''
#include <cassert>
#include <string>
#include "ble_control.cpp"
uint32_t nowMs=1000;bool buttonDown=false,wifiUp=false,serverReady=false;
VoiceState state=VoiceState::IDLE;
int identifies=0,restores=0,saves=0,gain=200,suppressed=0;
std::string storedOwner;
int nvs_open(const char*,int,int*n){*n=1;return 0;}
int nvs_get_str(int,const char*,char*d,size_t*n){if(storedOwner.empty())return -1;strlcpy(d,storedOwner.c_str(),*n);return 0;}
int nvs_set_str(int,const char*,const char*s){storedOwner=s;return 0;}
int nvs_get_blob(int,const char*,void*,size_t*){return -1;}
int nvs_set_blob(int,const char*,const void*,size_t){return 0;}
int nvs_commit(int){return 0;}
bool audioIsPlaying(){return false;} bool audioIsCapturing(){return false;}
uint16_t getOutputGain(){return gain;}void setOutputGain(uint16_t g){gain=g;}
bool cueIdentify(){++identifies;return true;}
VoiceState sessionState(){return state;}
const char*sessionStateName(){return state==VoiceState::WAITING?"WAIT":"IDLE";}
bool sessionServiceReady(){return serverReady;}
void sessionSuppressButton(){++suppressed;}
bool powerUsbPresent(){return true;}void powerNoteActivity(){}
bool netWifiIsUp(){return wifiUp;}
bool netWifiSsid(char*out,size_t cap){strlcpy(out,wifiUp?"old-network":"",cap);return wifiUp;}
esp_ip4_addr_t netWifiIp(){return {{192,168,137,159}};}
bool netWifiSaveCreds(const char*,const char*){++saves;return true;}
void netWifiBegin(){++restores;wifiUp=false;}
void netWifiStop(){wifiUp=false;}
bool netWifiTryCredentials(const char*,const char*){wifiUp=false;return true;}
void wsClose(const char*,WsDisconnect){}
std::string request(const std::string&op,const std::string&key,const std::string&id="",const std::string&extra=""){
 std::string text="{\"o\":\""+op+"\",\"k\":\""+key+"\"";
 if(!id.empty())text+=",\"i\":\""+id+"\"";
 text+=extra+"}";uint8_t*out=nullptr;ssize_t len=0;
 assert(handle(1,(const uint8_t*)text.data(),text.size(),&out,&len,nullptr)==0);
 std::string response((char*)out,len);free(out);return response;
}
bool has(const std::string&text,const std::string&token){return text.find(token)!=std::string::npos;}
int main(){
 assert(bleControlBegin());bleControlUpdate();
 std::string k(64,'a'),other(64,'b'),id(32,'1');
 assert(has(request("identify",k,id),"not_owner"));assert(identifies==0);
 assert(has(request("gateway",k),"not_owner"));
 assert(has(request("auth",k),"confirm"));bleControlUpdate();
 assert(storedOwner.empty());buttonDown=true;bleControlUpdate();buttonDown=false;
 assert(storedOwner==k&&suppressed>0);assert(has(request("auth",k),"completed"));
 assert(has(request("auth",other),"not_owner"));
 assert(has(request("status",k),"\"wifi\":false"));
 assert(!has(request("status",k),"credential"));
 assert(has(request("gateway",k),"test-only-bootstrap"));
 assert(has(request("identify",k,id),"accepted"));
 assert(has(request("identify",k,id),"running"));bleControlUpdate();
 assert(identifies==1);assert(has(request("identify",k,id),"completed"));assert(identifies==1);
 id=std::string(32,'2');request("volume",k,id,",\"n\":90");bleControlUpdate();assert(gain==230);
 state=VoiceState::WAITING;id=std::string(32,'3');request("identify",k,id);bleControlUpdate();
 assert(has(request("result",k,id),"device_busy"));assert(identifies==1);state=VoiceState::IDLE;
 id=std::string(32,'4');assert(has(request("server",k,id,",\"s\":\"8.8.8.8\",\"n\":8766"),"invalid_request"));
 assert(!devicePrivateHost("127.0.0.1")&&!devicePrivateHost("192.168.1.2junk")&&!devicePrivateHost("192.168.001.2"));
 assert(devicePrivateHost("192.168.1.2")&&devicePrivateHost("172.20.10.2"));
 request("server",k,id,",\"s\":\"192.168.1.2\",\"n\":8766");bleControlUpdate();assert(!strcmp(deviceServerHost(),"192.168.1.2"));
 id=std::string(32,'5');request("wifi",k,id,",\"s\":\"new\",\"p\":\"12345678\"");bleControlUpdate();
 assert(saves==0);wifiUp=true;bleControlUpdate();assert(saves==1);assert(has(request("result",k,id),"completed"));
 id=std::string(32,'6');request("wifi",k,id,",\"s\":\"bad\",\"p\":\"12345678\"");bleControlUpdate();
 nowMs+=35001;bleControlUpdate();assert(restores==1&&saves==1);assert(has(request("result",k,id),"wifi_failed_restored"));
 uint8_t*out=nullptr;ssize_t len;std::string nested="{\"a\":{\"a\":{}}}";
 assert(handle(1,(const uint8_t*)nested.data(),nested.size(),&out,&len,nullptr)==ESP_ERR_INVALID_ARG);
 puts("PASS: real BLE authorization/physical confirmation, offline controls, deduplication, busy rejection, private server settings and WiFi save/rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='ble-control-') as folder:
    root=Path(folder)
    for name in ('ble_control.cpp','ble_control.h','device_settings.cpp','device_settings.h','config.h','session.h','audio_io.h','cue.h','power.h','net_wifi.h','net_ws.h'):
        shutil.copyfile(project/name,root/name)
    for name,text in headers.items():
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    shutil.copyfile(component/'cJSON.h',root/'cJSON.h')
    (root/'main.cpp').write_text(harness,encoding='utf-8')
    subprocess.run([cc,'-c',str(component/'cJSON.c'),'-o',str(root/'json.o')],check=True)
    exe=root/'ble-tests.exe'
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(root/'main.cpp'),str(root/'device_settings.cpp'),str(root/'json.o'),'-static-libgcc','-static-libstdc++','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
