"""Compile real session/button/protocol and WS sources with fake hardware/transport.

Never reads secrets.h or opens a serial port. Requires native g++ on PATH.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++')
if not compiler:
    raise SystemExit('A native g++ compiler is required')

stubs = {
    'platform.h': '''#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
extern uint32_t nowMs;
inline uint32_t appMillis() { return nowMs; }
inline void appDelay(uint32_t n) { nowMs += n; }
''',
    'secrets.h': '''#pragma once
#define DEVICE_ID "test-device"
#define SERVER_HOST "test.invalid"
#define SERVER_PORT 8766
#define SERVER_WS_PATH "/"
#define SERVER_USE_TLS 0
''',
    'driver/gpio.h': '''#pragma once
#include <cstdint>
using gpio_num_t=int;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; };
constexpr int GPIO_MODE_INPUT=1, GPIO_PULLUP_DISABLE=0;
extern int level;
inline int gpio_get_level(gpio_num_t) { return level; }
inline int gpio_config(const gpio_config_t*) { return 0; }
''',
    'esp_err.h': '#pragma once\n#include <cassert>\n#define ESP_ERROR_CHECK(x) assert((x)==0)\n',
    'esp_netif_ip_addr.h': '#pragma once\nstruct esp_ip4_addr_t { unsigned addr; };\n',
    'esp_random.h': '#pragma once\ninline unsigned esp_random() { return 123456; }\n',
    'lwip/sockets.h': '''#pragma once
struct fd_set { int fd; };
struct timeval { long tv_sec, tv_usec; };
#define FD_ZERO(p) ((p)->fd=-1)
#define FD_SET(n,p) ((p)->fd=(n))
#define FD_ISSET(n,p) ((p)->fd==(n))
inline int select(int,fd_set*,fd_set*,fd_set*,timeval*) { return 1; }
''',
}

session_test = r'''
#include <cassert>
#include <algorithm>
#include <string>
#include <vector>
#include "session.cpp"
#include "driver/gpio.h"
uint32_t nowMs=0;
int level=1;
bool capturing=false, playing=false, up=true;
int cues=0, starts=0, ends=0, aborts=0;
int labCues=0;
uint16_t outputGain=200;
uint16_t getOutputGain(){return outputGain;}
void setOutputGain(uint16_t value){outputGain=value;}
bool identifyOK=true;
std::vector<std::string> labMessages;
std::string lastLabResult() {
  for(auto it=labMessages.rbegin();it!=labMessages.rend();++it) if(it->find("lab_result")!=std::string::npos)return *it;
  return "";
}
std::string lastLabStatus() {
  for(auto it=labMessages.rbegin();it!=labMessages.rend();++it) if(it->find("lab_status")!=std::string::npos)return *it;
  return "";
}
WsTextHandler callback=nullptr;
std::string incoming;
int16_t uplink[NET_CHUNK_SAMPLES];
void wsSetTextHandler(WsTextHandler h) { callback=h; }
void wsSetAudioAccept(bool) {}
int16_t* wsAudioTxBuf() { return uplink; }
bool wsIsConnected() { return up; }
bool wsConnect() { up=true; return true; }
void wsClose(const char*,WsDisconnect) { up=false; }
WsDisconnect wsLastDisconnectCode() { return WsDisconnect::TCP_LOST; }
void wsPoll() {
  if (!incoming.empty()) { auto message=incoming; incoming.clear(); callback(message.c_str(),message.size()); }
}
bool wsSendText(const char* s) {
  if (strstr(s,"lab_")) labMessages.emplace_back(s);
  starts += strcmp(s,PROTO_TURN_START)==0;
  ends += strcmp(s,PROTO_TURN_END)==0;
  aborts += strcmp(s,PROTO_ABORT)==0;
  assert(strlen(s)<=WS_TEXT_MAX); return true;
}
bool wsSendBinary(const uint8_t*,size_t) { return true; }
bool netWifiIsUp() { return true; }
int32_t netWifiRssi() { return -45; }
bool powerUsbPresent() { return true; }
int powerBatteryMv() { return 1500; }
void powerNoteActivity() {}
void netWifiForceRetry() {}
bool audioIsCapturing() { return capturing; }
bool audioIsPlaying() { return playing; }
bool audioStartCapture() { playing=false; capturing=true; return true; }
void audioStopCapture() { capturing=false; }
bool audioStartPlayback() { capturing=false; playing=true; return true; }
void audioStopPlayback() { playing=false; }
void audioCutPlayback() { playing=false; }
size_t audioRead(int16_t* dst,size_t n,uint32_t) {
  if (!capturing) return 0;
  n=std::min(n,size_t(FRAME_SAMPLES)); std::fill_n(dst,n,0); nowMs+=20; return n;
}
size_t audioWrite(const int16_t*,size_t n,uint32_t) { return n; }
uint32_t audioTxUnderruns() { return 0; }
void bubbleStart() {}
void bubbleStop() {}
bool bubbleActive() { return false; }
bool bubbleInGap() { return true; }
void bubbleFill(int16_t*,size_t) {}
void ledSet(LedMode) {}
void ledUpdate() {}
void ringReset() {}
size_t ringUsed() { return 0; }
size_t ringRead(uint8_t*,size_t) { return 0; }
void cueLinkUp() { ++cues; audioStartPlayback(); audioStopPlayback(); }
void cueLinkDown() { ++cues; audioStartPlayback(); audioStopPlayback(); }
void cueError() { ++cues; audioStartPlayback(); audioStopPlayback(); }
bool cueIdentify() { ++labCues; return identifyOK; }
void tick(int n=1) { while(n--) { nowMs+=2; sessionUpdate(); } }
void edge(int value) { level=value; tick(); nowMs+=26; tick(); }
void receive(const char* s) { incoming=s; tick(); }
int main() {
  sessionBegin(); receive("{\"t\":\"ready\",\"sr\":16000}");
  // Press begins while link-up cue is queued, before the 25ms debounce ends.
  edge(0); assert(cues==0); assert(capturing); assert(starts==1);
  tick(20); const auto samples=s_recordSamples;
  // Late aborted-turn errors previously queued a cue that stopped capture.
  receive("{\"t\":\"error\",\"code\":\"old_turn\"}"); tick();
  assert(cues==0 && capturing && s_recordSamples>samples);
  receive("{\"t\":\"ready\",\"sr\":16000}");
  assert(sessionState()==VoiceState::RECORDING && capturing);
  edge(1); assert(ends==1 && sessionState()==VoiceState::WAITING);
  // A fresh press interrupts waiting; stale error cannot stop the new turn.
  edge(0); tick(20); assert(starts==2 && capturing);
  receive("{\"t\":\"error\",\"code\":\"cancelled\"}"); tick();
  assert(capturing && cues==0);
  // Disconnect while held, reconnect and ready: no cue, no phantom recording.
  up=false; tick(); assert(!capturing); receive("{\"t\":\"ready\",\"sr\":16000}");
  tick(20); assert(cues==0 && starts==2 && !capturing);
  edge(1); edge(0); assert(starts==3 && capturing); tick(20); edge(1);
  assert(ends==2); // Recovery needs just a release and a normal new hold, no double-click.
  // A real error in WAITING still gets its idle cue and exits the failed turn.
  receive("{\"t\":\"error\",\"code\":\"asr_failed\"}"); tick();
  assert(cues==1 && sessionState()==VoiceState::IDLE);
  edge(0); edge(1); assert(aborts>=2 && sessionState()==VoiceState::IDLE);
  puts("PASS: recording cue isolation, stale errors/ready, reconnect held key, release rearm, short tap");
  receive("{\"t\":\"lab_identify\",\"id\":\"00000000000000000000000000000001\"}");
  assert(labCues==1);
  assert(lastLabResult().find("completed")!=std::string::npos);
  receive("{\"t\":\"lab_identify\",\"id\":\"00000000000000000000000000000001\"}");
  assert(labCues==1); // Duplicate ID acknowledges without playing twice.
  receive("{\"t\":\"lab_identify\",\"id\":\"bad\"}");
  assert(labCues==1);
  receive("{\"t\":\"lab_identify\",\"id\":\"000000000000000000000000000000001\"}");
  assert(labCues==1); // Never truncate a long ID into another command.
  edge(0);
  receive("{\"t\":\"lab_identify\",\"id\":\"00000000000000000000000000000002\"}");
  assert(labCues==1 && capturing);
  assert(lastLabResult().find("rejected")!=std::string::npos);
  edge(1);receive("{\"t\":\"error\",\"code\":\"test_end\"}");tick();
  identifyOK=false;
  receive("{\"t\":\"lab_identify\",\"id\":\"00000000000000000000000000000003\"}");
  assert(labCues==2 && lastLabResult().find("failed")!=std::string::npos);
  puts("PASS: identify receipt, duplicate suppression, malformed IDs, voice priority and audio failure");
  receive("{\"t\":\"lab_voice\",\"id\":\"00000000000000000000000000000011\"}");
  assert(sessionState()==VoiceState::WAITING && !capturing && s_mobileVoice[0]);
  assert(lastLabStatus().find("\"state\":\"WAIT\"")!=std::string::npos);
  const auto voiceAt=nowMs;
  receive("{\"t\":\"audio_begin\",\"seq\":55}");
  assert(sessionState()==VoiceState::PLAYING);
  receive("{\"t\":\"audio_end\",\"seq\":55}");tick(2);
  assert(sessionState()==VoiceState::IDLE && !s_mobileVoice[0]);
  assert(lastLabResult().find("completed")!=std::string::npos);
  assert(lastLabStatus().find("\"state\":\"IDLE\"")!=std::string::npos);
  assert(nowMs-voiceAt<2000); // End-of-play state is reported without heartbeat delay.
  receive("{\"t\":\"lab_voice\",\"id\":\"00000000000000000000000000000011\"}");
  assert(sessionState()==VoiceState::IDLE); // Completed voice is never replayed.
  receive("{\"t\":\"lab_voice\",\"id\":\"00000000000000000000000000000012\"}");
  edge(0); assert(capturing && !s_mobileVoice[0]); // Physical key has priority.
  edge(1); receive("{\"t\":\"error\",\"code\":\"end\"}");tick();
  receive("{\"t\":\"lab_voice\",\"id\":\"00000000000000000000000000000013\"}");
  nowMs+=WAITING_TIMEOUT_MS+1;tick();
  assert(!s_mobileVoice[0] && sessionState()==VoiceState::IDLE);
  puts("PASS: mobile playback admission, completion, deduplication, physical priority and timeout");

  s_st=VoiceState::PLAYING;s_playStarted=true;s_audioEnded=false;playing=true;
  receive("{\"t\":\"lab_volume\",\"id\":\"00000000000000000000000000000021\",\"volume\":65}");
  assert(outputGain==166 && playing && s_st==VoiceState::PLAYING);
  assert(lastLabResult().find("completed")!=std::string::npos);
  const int oldAborts=aborts;
  receive("{\"t\":\"lab_interrupt\",\"id\":\"00000000000000000000000000000022\"}");
  assert(!playing && s_st==VoiceState::IDLE && aborts==oldAborts+1);
  assert(lastLabResult().find("completed")!=std::string::npos);
  receive("{\"t\":\"lab_interrupt\",\"id\":\"00000000000000000000000000000022\"}");
  assert(aborts==oldAborts+1);
  receive("{\"t\":\"audio_begin\",\"seq\":999}");
  assert(s_st==VoiceState::IDLE && !playing);
  level=0;tick();
  receive("{\"t\":\"lab_interrupt\",\"id\":\"00000000000000000000000000000023\"}");
  assert(lastLabResult().find("device_busy")!=std::string::npos);
  level=1;tick();
  ProtoMsg bad;
  const char* malformed="{\"t\":\"lab_volume\",\"id\":\"00000000000000000000000000000024\",\"volume\":65.1}";
  protoParse(malformed,strlen(malformed),&bad);assert(bad.t==MsgType::UNKNOWN);
  for(const char *value : {"2147483648", "-2147483649", "999999999999999999999999", "1.2", "3e2"}) {
    std::string json=std::string("{\"t\":\"audio_begin\",\"seq\":")+value+"}";
    protoParse(json.c_str(),json.size(),&bad);assert(bad.seq==-1);
  }
  const char *maxSeq="{\"t\":\"audio_begin\",\"seq\":2147483647}";
  protoParse(maxSeq,strlen(maxSeq),&bad);assert(bad.seq==INT32_MAX);
  const char *minSeq="{\"t\":\"audio_begin\",\"seq\":-2147483648}";
  protoParse(minSeq,strlen(minSeq),&bad);assert(bad.seq==INT32_MIN);
  puts("PASS: live volume, interrupt playback cut, duplicate suppression, stale audio and physical recording priority");

}
'''

ws_test = r'''
#include <cassert>
#include <string>
#include "net_ws.cpp"
const char *deviceServerHost() { return "127.0.0.1"; }
uint16_t deviceServerPort() { return 8766; }
uint32_t nowMs=0;
bool connected=true;
int writeCount=0;
bool transportConnected() { return connected; }
void transportClose() { connected=false; }
bool transportConnect() { connected=true; return true; }
int transportFd() { return 1; }
int transportRead(uint8_t*,size_t) { return -1; }
size_t transportWrite(const uint8_t*,size_t n,uint32_t) {
  return writeCount<0 ? n : size_t(writeCount);
}
size_t ringFree() { return 16384; }
uint8_t storage[16384];
size_t ringWriteSpan(uint8_t** p) { *p=storage; return sizeof(storage); }
void ringCommit(size_t) {}
int main() {
  s_up=true;
  // A transient zero-byte write must not tear down a usable stream.
  assert(!wsSendBinary((const uint8_t*)"abcd",4)); assert(wsIsConnected());
  writeCount=-1; assert(wsSendBinary((const uint8_t*)"abcd",4));
  const std::string extendedHello(180,'x');
  assert(wsSendText(extendedHello.c_str())); // Real extended text frame, not mocked wsSendText.
  const std::string tooLong(WS_TEXT_MAX+1,'x');
  assert(!wsSendText(tooLong.c_str()) && wsIsConnected());
  assert(!sendFrame(0x9,(const uint8_t*)extendedHello.data(),126,100)); // RFC control bound stays 125.
  writeCount=2; assert(!wsSendBinary((const uint8_t*)"abcd",4));
  assert(!wsIsConnected() && wsLastDisconnectCode()==WsDisconnect::PARTIAL_WRITE);
  s_up=true; connected=false; writeCount=0;
  assert(!wsSendBinary((const uint8_t*)"abcd",4));
  assert(!wsIsConnected() && wsLastDisconnectCode()==WsDisconnect::TCP_LOST);
  puts("PASS: zero-byte congestion preserves WS; partial frame and dead TCP force reconnect");
}
'''

with tempfile.TemporaryDirectory(prefix='ai-figure-recovery-') as folder:
    root = Path(folder)
    for name in ('config.h', 'session.h', 'button.h', 'audio_io.h', 'bubble.h',
                 'cue.h', 'led.h', 'net_wifi.h', 'net_ws.h', 'proto.h', 'ring.h',
                 'transport.h', 'device_settings.h', 'power.h', 'session.cpp', 'button.cpp', 'proto.cpp', 'net_ws.cpp'):
        shutil.copyfile(project / name, root / name)
    for name, content in stubs.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')
    for name, harness, sources in (
        ('session', session_test, ['button.cpp', 'proto.cpp']), ('ws', ws_test, []),
    ):
        (root / 'test.cpp').write_text(harness, encoding='utf-8')
        executable = root / (name + '.exe')
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-static-libgcc', '-static-libstdc++', '-I', str(root),
                        str(root / 'test.cpp'), *(str(root / s) for s in sources),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
