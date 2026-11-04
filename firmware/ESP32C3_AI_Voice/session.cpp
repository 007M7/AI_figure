#include "platform.h"
#include "session.h"
#include "config.h"
#include "secrets.h"
#include "audio_io.h"
#include "bubble.h"
#include "button.h"
#include "cue.h"
#include "led.h"
#include "net_wifi.h"
#include "net_ws.h"
#include "proto.h"
#include "ring.h"
#include "power.h"

// ---------------------------------------------------------------- 状态
static VoiceState s_st = VoiceState::ERROR_STATE;
static Button     s_btn;

static bool     s_ready      = false;   // 收到 {"t":"ready"} 了吗
static bool     s_waitRelease = false;  // A disconnected hold must end before a new turn.
static uint32_t s_helloAt    = 0;
static uint32_t s_wsRetryAt  = 0;
static uint32_t s_wsBackoff  = WS_RETRY_MIN_MS;

static int32_t  s_seq        = -1;      // 当前回合号(AGENT.md §3.3a)
static bool     s_audioEnded = false;   // 本回合的 audio_end 到了吗
static bool     s_playStarted = false;  // 预缓冲攒够、功放已开
static uint32_t s_waitStart  = 0;

// 服务器驱动的等待音效。onText 在 wsPoll 的回调里,不能在那里开播放
// (audioStartPlayback 要推静音、等功放唤醒,会阻塞)—— 攒个标记,回到 doWaiting 再开。
static bool     s_bubbleReq  = false;   // 收到 cue thinking,该开始冒泡了
static bool     s_waitFail   = false;   // WAITING 时服务器报错:不用干等 8 s 超时

// 上行组包:攒够 100 ms 才发一帧。
// 缓冲就是 net_ws 的上行帧 payload 区(wsAudioTxBuf):录音直接写进去,发送时原地加掩码,不再多拷一遍。
static int16_t *s_up16   = nullptr;     // sessionBegin 里取
static size_t   s_upFill = 0;
static uint32_t s_upDrops = 0, s_upDropStreak = 0;
static uint32_t s_recordSamples = 0;
static uint32_t s_underruns = 0;

// WAITING(冒泡)和 PLAYING 的一帧播放暂存。两个状态互斥,共用一块。
static int16_t  s_frame[FRAME_SAMPLES];

// cue 是阻塞的,不能在 wsPoll 的回调里放 —— 攒个标记,回到 sessionUpdate 再播
enum class PendingCue : uint8_t { NONE, LINK_UP, LINK_DOWN, ERR };
static PendingCue s_cue = PendingCue::NONE;
static char s_labPending[33] = {};
static char s_mobileVoice[33] = {};
static char s_interruptPending[33] = {}, s_volumePending[33] = {};
static int s_volumeValue = 0;
static char s_labIds[4][33] = {};
static const char *s_labStatuses[4] = {};
static const char *s_labCodes[4] = {};
static unsigned s_labNext = 0;
static uint32_t s_labStatusAt = 0;
static VoiceState s_labLastState = VoiceState::ERROR_STATE;
static int s_labLastVolume = -1;

static bool labReply(const char *id, const char *status, const char *code) {
  char message[128];
  const int n = snprintf(message, sizeof(message),
      "{\"t\":\"lab_result\",\"id\":\"%s\",\"status\":\"%s\",\"code\":\"%s\"}", id, status, code);
  printf("[LAB] id=%s status=%s code=%s\n", id, status, code);
  return n > 0 && (size_t)n < sizeof(message) && wsSendText(message);
}
static void labFinish(const char *id, const char *status, const char *code) {
  memcpy(s_labIds[s_labNext], id, 33);
  s_labStatuses[s_labNext] = status;
  s_labCodes[s_labNext] = code;
  s_labNext = (s_labNext + 1) % 4;
  labReply(id, status, code);
}
static void mobileFinish(const char *status, const char *code) {
  if (!s_mobileVoice[0]) return;
  labFinish(s_mobileVoice, status, code);
  s_mobileVoice[0] = '\0';
}
static void labStatus() {
  const int volume=(getOutputGain()*100+128)/256;
  if (!s_ready || (s_st == s_labLastState && volume==s_labLastVolume && appMillis() - s_labStatusAt < 2000)) return;
  s_labStatusAt = appMillis();
  char message[160];
  const int n = snprintf(message, sizeof(message),
      "{\"t\":\"lab_status\",\"state\":\"%s\",\"usb\":%d,\"mv\":%d,\"rssi\":%ld,\"volume\":%d}",
      sessionStateName(), powerUsbPresent(), powerBatteryMv(), (long)netWifiRssi(),volume);
  if (n > 0 && (size_t)n < sizeof(message) && wsSendText(message)) {s_labLastState = s_st;s_labLastVolume=volume;}
}

uint32_t sessionUplinkDrops() { return s_upDrops; }
uint32_t sessionUnderruns()   { return s_underruns; }
VoiceState sessionState()     { return s_st; }
bool sessionServiceReady() { return s_ready && wsIsConnected() && netWifiIsUp(); }
void sessionSuppressButton() { s_waitRelease = true; s_btn.clearEvents(); }

const char *sessionStateName() {
  switch (s_st) {
    case VoiceState::IDLE:        return "IDLE";
    case VoiceState::RECORDING:   return "REC";
    case VoiceState::WAITING:     return "WAIT";
    case VoiceState::PLAYING:     return "PLAY";
    default:                      return "ERR";
  }
}

// ---------------------------------------------------------------- 下行控制消息
static void onText(const char *json, size_t len) {
  ProtoMsg m;
  protoParse(json, len, &m);

  switch (m.t) {
    case MsgType::LAB_INTERRUPT:
    case MsgType::LAB_VOLUME:
      for (unsigned i=0;i<4;i++) {
        if(s_labIds[i][0] && !strcmp(s_labIds[i],m.id)){labReply(m.id,s_labStatuses[i],s_labCodes[i]);return;}
      }
      if(!strcmp(s_interruptPending,m.id)||!strcmp(s_volumePending,m.id)) return;
      if(!s_ready || s_interruptPending[0] || s_volumePending[0] ||
         s_st==VoiceState::RECORDING || s_btn.isActive() || audioIsCapturing()) {
        labFinish(m.id,"rejected","device_busy");return;
      }
      if(m.t==MsgType::LAB_INTERRUPT) memcpy(s_interruptPending,m.id,33);
      else {memcpy(s_volumePending,m.id,33);s_volumeValue=m.volume;}
      break;
    case MsgType::LAB_VOICE:
      for (unsigned i = 0; i < 4; ++i) {
        if (s_labIds[i][0] && strcmp(s_labIds[i], m.id) == 0) {
          labReply(m.id, s_labStatuses[i], s_labCodes[i]); return;
        }
      }
      if (strcmp(s_mobileVoice, m.id) == 0) { labReply(m.id, "running", ""); return; }
      if (!s_ready || s_st != VoiceState::IDLE || s_btn.isActive() ||
          audioIsCapturing() || audioIsPlaying() || s_labPending[0]) {
        labFinish(m.id, "rejected", "device_busy"); return;
      }
      memcpy(s_mobileVoice, m.id, sizeof(s_mobileVoice));
      s_cue = PendingCue::NONE;
      s_st = VoiceState::WAITING;
      s_waitStart = appMillis(); s_bubbleReq = false; s_waitFail = false;
      powerNoteActivity();
      labReply(m.id, "running", "");
      break;
    case MsgType::LAB_IDENTIFY:
      for (unsigned i = 0; i < 4; ++i) {
        if (s_labIds[i][0] && strcmp(s_labIds[i], m.id) == 0) {
          labReply(m.id, s_labStatuses[i], s_labCodes[i]);
          return;
        }
      }
      if (strcmp(s_labPending, m.id) == 0) return;
      if (!s_ready || s_st != VoiceState::IDLE || s_btn.isActive() ||
          audioIsCapturing() || audioIsPlaying() || s_labPending[0]) {
        labFinish(m.id, "rejected", "device_busy");
        return;
      }
      memcpy(s_labPending, m.id, sizeof(s_labPending));
      break;
    case MsgType::READY:
      if (s_ready) break;             // A duplicate ready must not reset a live recording.
      // sr 只用于开机断言,不是用来切换配置的(AGENT.md §4)。
      if (m.sr != SAMPLE_RATE_HZ) {
        printf("[SESS] !! 服务器说采样率是 %ld,设备是 %d —— 固定 PCM 格式,"
                      "这个对不上就没法播\n", (long)m.sr, SAMPLE_RATE_HZ);
        wsClose("采样率不一致");
        return;
      }
      s_ready = true;
      s_waitRelease = s_waitRelease || s_btn.isActive();
      s_btn.clearEvents();
      s_st = VoiceState::IDLE;
      s_cue = PendingCue::LINK_UP;
      printf("[SESS] ready sr=%ld —— 可以按按键说话了\n", (long)m.sr);
      break;

    case MsgType::AUDIO_BEGIN:
      // 只有 WAITING 时才认。打断时序下服务器可能刚好把上一回合的 audio_begin
      // 发了出来,此刻设备已经进 RECORDING —— 认了它会把录音打断,而且播的是
      // 已经被 abort 掉的那一轮的声音。
      if (s_st != VoiceState::WAITING) {
        printf("[SESS] 忽略 %s 状态下的 audio_begin seq=%ld(过期的回合)\n",
                      sessionStateName(), (long)m.seq);
        break;
      }
      s_seq = m.seq;
      ringReset();
      s_audioEnded  = false;
      s_playStarted = false;
      wsSetAudioAccept(true);            // 从这一刻起 binary 帧才算数
      s_st = VoiceState::PLAYING;
      printf("[SESS] audio_begin seq=%ld\n", (long)m.seq);
      break;

    case MsgType::AUDIO_END:
      if (m.seq == s_seq) {
        s_audioEnded = true;
        printf("[SESS] audio_end seq=%ld,ring 里还剩 %u 字节\n",
                      (long)m.seq, (unsigned)ringUsed());
      }
      break;

    case MsgType::ASR:
    case MsgType::REPLY:
      printf("[SESS] %.*s\n", (int)len, json);   // 仅供串口调试
      break;

    case MsgType::CUE:
      // thinking = 服务器收到 turn_end 了,在识别/想/合成。只在 WAITING 时认 ——
      // 打断时序下晚到的 cue 不该在录音或播放中间冒泡。
      if (strcmp(m.name, "thinking") == 0) {
        if (s_st == VoiceState::WAITING) s_bubbleReq = true;
      } else {
        // 必须静默忽略不认识的 cue 名,好让服务器先行加新音效
        printf("[SESS] 不认识的 cue name=%s,忽略\n", m.name);
      }
      break;

    case MsgType::ERROR_MSG:
      printf("[SESS] 服务器报错 code=%s: %.*s\n", m.name, (int)len, json);
      // 还没开始播就报错(ASR 失败之类)= 这一回合不会有声音了,不用等超时
      // A late error from an aborted turn must never switch RX to TX in a new recording.
      if (s_st == VoiceState::WAITING) {
        s_cue = PendingCue::ERR;
        s_waitFail = true;
      }
      break;

    default:
      // 必须静默忽略未知消息,好让服务器先行升级(AGENT.md §3.2)
      break;
  }
}

// ---------------------------------------------------------------- 内部动作
// cut=true:打断用,不保护尾音、不阻塞(audioCutPlayback);其余情况照常补静音收尾。
static void stopAudioAll(bool cut = false) {
  bubbleStop();
  s_bubbleReq = false;
  s_waitFail  = false;
  if (audioIsCapturing()) audioStopCapture();
  if (audioIsPlaying()) {
    if (cut) audioCutPlayback();
    else     audioStopPlayback();
  }
  ringReset();
  wsSetAudioAccept(false);
  s_upFill = 0;
  s_audioEnded = false;
  s_playStarted = false;
}

static void enterError(const char *why) {
  s_labLastState = VoiceState::ERROR_STATE;
  mobileFinish("failed", "connection_lost");
  if(s_interruptPending[0]){labFinish(s_interruptPending,"failed","connection_lost");s_interruptPending[0]=0;}
  if(s_volumePending[0]){labFinish(s_volumePending,"failed","connection_lost");s_volumePending[0]=0;}
  if (s_labPending[0]) {
    labFinish(s_labPending, "rejected", "connection_lost");
    s_labPending[0] = '\0';
  }
  if (s_st == VoiceState::ERROR_STATE) return;
  printf("[SESS] -> ERROR (%s)\n", why);
  stopAudioAll();
  s_waitRelease = s_waitRelease || s_btn.isActive();
  s_ready = false;
  s_st = VoiceState::ERROR_STATE;
  s_cue = PendingCue::LINK_DOWN;
}

static void startRecording() {
  s_btn.clearEvents();                  // New gesture owns only future release events.
  s_cue = PendingCue::NONE;
  if (!wsSendText(PROTO_TURN_START)) { enterError("turn_start 发不出去"); return; }
  // Board bring-up (2026-10-25): GPIO8 LED on during capture caused subsequent
  // playback to fail. Apply OFF immediately, including when barging in from playback.
  ledSet(LedMode::OFF);
  ledUpdate();
  if (!audioStartCapture())          { enterError("录音启动失败");        return; }
  s_upFill = 0;
  s_recordSamples = 0;
  s_upDropStreak = 0;
  s_st = VoiceState::RECORDING;
  printf("%s\n", "[SESS] turn_start —— 录音中");
}

// 打断。AGENT.md §5:**先发 abort 再关功放** ——
// abort 是上行,不受下行缓冲拥塞影响,越早发服务器越早停止烧 token。
// 功放直接掐(cut),不等被打断的回复播完尾巴:紧接着就要开麦,用户已经在说话了。
static void bargeIn() {
  mobileFinish("failed", "physical_interrupted");
  wsSendText(PROTO_ABORT);
  stopAudioAll(true);
  s_seq = -1;                 // 到下一个 audio_begin 之前的 binary 帧全丢
  printf("%s\n", "[SESS] abort(打断)");
}

// 把攒着的样本发出去。返回 false 表示链路坏了。
static bool flushUplink() {
  if (s_upFill == 0) return true;
  const bool ok = wsSendBinary((const uint8_t *)s_up16, s_upFill * sizeof(int16_t));
  s_upFill = 0;

  if (ok) { s_upDropStreak = 0; return true; }

  // 整帧丢弃而不是发半帧 —— WS 帧发一半会破坏帧边界。
  // ASR 丢 100 ms 比整条链路卡死强(AGENT.md §3.4)。
  s_upDrops++;
  if (++s_upDropStreak >= WS_UPLINK_DROP_LIMIT) {
    printf("[SESS] 连续丢了 %u 个上行帧,链路不行了\n", (unsigned)s_upDropStreak);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 各状态
static void doRecording() {
  // audioRead 阻塞 ~20 ms,这就是 RECORDING 状态的节拍器。
  // 直接读进上行包的空位里;maxSamples 限到剩余空位,所以永远不会写过 100 ms 的包尾。
  const size_t captured = audioRead(s_up16 + s_upFill, NET_CHUNK_SAMPLES - s_upFill, 25);
  s_upFill += captured;
  s_recordSamples += captured;
  if (s_upFill >= NET_CHUNK_SAMPLES) {
    if (!flushUplink()) { wsClose("上行持续失败"); return; }
  }

  if (s_btn.tookRelease()) {
    if (s_recordSamples < (SAMPLE_RATE_HZ * MIN_RECORDING_MS / 1000)) {
      // A tap is not a spoken turn. Cancel any already uploaded frames rather
      // than asking ASR to decode 20ms and playing a misleading not-heard reply.
      wsSendText(PROTO_ABORT);
      stopAudioAll(true);
      s_seq = -1;
      s_st = VoiceState::IDLE;
      s_btn.clearEvents();
      printf("[SESS] short tap ignored (%lu samples)\n", (unsigned long)s_recordSamples);
      return;
    }
    flushUplink();                                   // 把不足 100 ms 的尾巴也发掉
    audioStopCapture();
    if (!wsSendText(PROTO_TURN_END)) { enterError("turn_end 发不出去"); return; }
    s_st = VoiceState::WAITING;
    s_waitStart = appMillis();
    s_bubbleReq = false;
    s_waitFail  = false;
    ledSet(LedMode::BLINK_FAST);
    printf("%s\n", "[SESS] turn_end —— 等服务器");
  }
}

static void doWaiting() {
  if (s_waitFail) {
    mobileFinish("failed", "reply_failed");
    printf("%s\n", "[SESS] 服务器报错,这一回合没有回复了");
    stopAudioAll();
    s_st = VoiceState::IDLE;               // 错误音 onText 已经挂上了
    return;
  }
  if (appMillis() - s_waitStart > WAITING_TIMEOUT_MS) {
    mobileFinish("failed", "reply_timeout");
    printf("[SESS] 等了 %d ms 没等到 audio_begin,放弃\n", WAITING_TIMEOUT_MS);
    wsSendText(PROTO_ABORT);               // 先发 abort(§8),再停本地声音
    stopAudioAll();
    s_st = VoiceState::IDLE;
    s_cue = PendingCue::ERR;
    return;
  }

  // ---- 等待音效----
  if (s_bubbleReq && !bubbleActive()) {
    s_bubbleReq = false;
    if (audioStartPlayback()) bubbleStart();
  }
  if (bubbleActive()) {
    // 和 PLAYING 一样:audioWrite 阻塞到 DMA 腾出空间(~20 ms),这就是节拍器;
    // 两次之间 loop 照常 wsPoll,audio_begin 来了立刻能收到。
    bubbleFill(s_frame, FRAME_SAMPLES);
    audioWrite(s_frame, FRAME_SAMPLES, 60);
  }
}

static void doPlaying() {
  if (!s_playStarted) {
    // 攒够预缓冲再起播(AGENT.md §4)。回复很短、没攒够就收到 audio_end 的话,
    // 也得起播,否则会一直等一个永远不来的字节。
    const bool ready = ringUsed() >= PREBUFFER_BYTES || s_audioEnded;

    // 等待音效还在响:预缓冲攒够之前**接着冒泡**,不留一段死寂;
    // 攒够了也要等到两个气泡之间的空隙再切,否则在气泡中间硬切会"啪"一声。
    // 功放和 I2S 一直开着,正式回复是无缝接上去的(audioStartPlayback 已在播时直接返回)。
    if (bubbleActive()) {
      if (ready && bubbleInGap()) {
        bubbleStop();
      } else {
        bubbleFill(s_frame, FRAME_SAMPLES);
        audioWrite(s_frame, FRAME_SAMPLES, 60);
        return;
      }
    }

    if (ready) {
      if (!audioStartPlayback()) { enterError("播放启动失败"); return; }
      s_playStarted = true;
      ledSet(LedMode::ON);
      printf("[SESS] 起播,预缓冲 %u 字节 (%u ms)\n",
                    (unsigned)ringUsed(),
                    (unsigned)(ringUsed() * 1000 / (SAMPLE_RATE_HZ * 2)));
    } else {
      // 没冒泡、预缓冲也没攒够:这时没有 audioWrite 当节拍器,loop 会全速空转
      // (PLAYING 下 loop 不 delay)。歇 1 ms,wsPoll 照样每毫秒收一次。
      appDelay(1);
    }
    return;
  }

  size_t n = ringRead((uint8_t *)s_frame, sizeof(s_frame));
  if (n > 0) {
    // audioWrite 阻塞到 DMA 腾出空间,这是 PLAYING 状态的节拍器。
    audioWrite(s_frame, n / sizeof(int16_t), 60);
  } else if (s_audioEnded) {
    audioStopPlayback();                  // 内含补静音 -> drain -> 关功放
    wsSetAudioAccept(false);
    s_st = VoiceState::IDLE;
    printf("[SESS] 本回合结束。TX 欠载 %lu 次,ring 见底 %lu 次\n",
                  (unsigned long)audioTxUnderruns(), (unsigned long)s_underruns);
    mobileFinish("completed", "");
  } else {
    // ring 见底但服务器还没说完 —— 网络跟不上。
    // 不用喂静音:auto_clear 已经在硬件层面输出零了(AGENT.md §4)。
    s_underruns++;
    appDelay(2);
  }
}

// ---------------------------------------------------------------- 对外
void sessionBegin() {
  s_btn.begin(PIN_PTT_BUTTON, BUTTON_DEBOUNCE_MS);
  s_up16 = wsAudioTxBuf();
  wsSetTextHandler(onText);
  s_st = VoiceState::ERROR_STATE;
  s_wsRetryAt = 0;
}

void sessionUpdate() {
  s_btn.update();

  // Cues start playback, which stops capture. Drop stale cues while speaking,
  // including the first raw LOW before debounce; never defer them into a later turn.
  if (s_cue != PendingCue::NONE) {
    PendingCue c = s_cue;
    s_cue = PendingCue::NONE;
    if (!s_btn.isActive() && !audioIsCapturing() && !audioIsPlaying() &&
        (s_st == VoiceState::IDLE || s_st == VoiceState::ERROR_STATE)) {
      bubbleStop();
      if      (c == PendingCue::LINK_UP)   cueLinkUp();
      else if (c == PendingCue::LINK_DOWN) cueLinkDown();
      else                                 cueError();
    }
  }

  // ---- WiFi ----
  if (!netWifiIsUp()) {
    enterError("WiFi 未连接");
    s_waitRelease = s_waitRelease || s_btn.isActive();
    ledSet(LedMode::BLINK_SLOW);
    if (s_btn.tookPress()) netWifiForceRetry();      // AGENT.md §5
    s_btn.clearEvents();
    return;
  }

  // ---- WS ----
  if (!wsIsConnected()) {
    enterError("WS 未连接");
    s_waitRelease = s_waitRelease || s_btn.isActive();
    ledSet(LedMode::BLINK_SLOW);

    if (s_btn.tookPress()) { s_wsRetryAt = appMillis(); s_wsBackoff = WS_RETRY_MIN_MS; }
    s_btn.clearEvents();

    if ((int32_t)(appMillis() - s_wsRetryAt) >= 0) {
      if (wsConnect()) {
        char hello[256];
        size_t n = protoBuildHello(hello, sizeof(hello), DEVICE_ID, FW_VERSION);
        if (n) {
          const int extra = snprintf(hello + n - 1, sizeof(hello) - n + 1,
              ",\"lab\":1,\"mobile_voice\":1,\"desktop_standby\":1,\"mobile_interrupt\":1,\"volume_control\":1,\"up_ms\":%lu,\"dc\":%u,\"ud\":%lu}", (unsigned long)appMillis(),
              (unsigned)wsLastDisconnectCode(), (unsigned long)s_upDrops);
          if (extra <= 0 || (size_t)extra >= sizeof(hello) - n + 1) n = 0;
        }
        if (n && wsSendText(hello)) {
          s_helloAt = appMillis();
          s_wsBackoff = WS_RETRY_MIN_MS;
          ledSet(LedMode::BLINK_FAST);
        } else {
          wsClose("hello 发不出去");
          s_wsRetryAt = appMillis() + s_wsBackoff;
        }
      } else {
        s_wsRetryAt = appMillis() + s_wsBackoff;
        s_wsBackoff *= 2;
        if (s_wsBackoff > WS_RETRY_MAX_MS) s_wsBackoff = WS_RETRY_MAX_MS;
      }
    }
    return;
  }

  wsPoll();
  if (!wsIsConnected()) {
    enterError("WS 接收断开");
    ledSet(LedMode::BLINK_SLOW);
    return;
  }

  // ---- 等 ready ----
  if (!s_ready) {
    // 握手期间按的键不算数,否则一收到 ready 就会莫名其妙开始录音
    s_btn.clearEvents();
    if (appMillis() - s_helloAt > WS_HANDSHAKE_TIMEOUT_MS) {
      wsClose("发了 hello 但服务器没回 ready");
      s_wsRetryAt = appMillis() + s_wsBackoff;
    }
    return;
  }

  if(s_interruptPending[0]) {
    if(s_btn.isActive()||s_st==VoiceState::RECORDING||audioIsCapturing()) labFinish(s_interruptPending,"rejected","device_busy");
    else {
      const bool sent=wsSendText(PROTO_ABORT);
      stopAudioAll(true); s_cue=PendingCue::NONE;
      mobileFinish("failed","mobile_interrupted"); s_st=VoiceState::IDLE;
      powerNoteActivity();
      labFinish(s_interruptPending,sent?"completed":"failed",sent?"":"connection_lost");
    }
    s_interruptPending[0]=0;
  }
  if(s_volumePending[0]) {
    if(s_btn.isActive()||s_st==VoiceState::RECORDING||audioIsCapturing())labFinish(s_volumePending,"rejected","device_busy");
    else {setOutputGain((s_volumeValue*256+50)/100);powerNoteActivity();labFinish(s_volumePending,"completed","");}
    s_volumePending[0]=0;
  }

  // ---- 打断:任意非 IDLE 状态按下按钮(AGENT.md §5) ----
  if (s_waitRelease) {
    if (s_labPending[0]) {
      labFinish(s_labPending, "rejected", "device_busy");
      s_labPending[0] = '\0';
    }
    s_btn.clearEvents();
    if (!s_btn.isActive()) s_waitRelease = false;
    return;
  }
  if (s_btn.tookPress() && s_btn.isPressed()) {
    if (s_st == VoiceState::IDLE) {
      startRecording();
    } else if (s_st != VoiceState::RECORDING) {
      bargeIn();
      startRecording();
    }
  }

  if (s_labPending[0]) {
    if (s_st != VoiceState::IDLE || s_btn.isActive() || audioIsCapturing() || audioIsPlaying()) {
      labFinish(s_labPending, "rejected", "device_busy");
    } else if (labReply(s_labPending, "running", "")) {
      powerNoteActivity();
      const bool success = cueIdentify();
      labFinish(s_labPending, success ? "completed" : "failed", success ? "" : "audio_failed");
    } else {
      labFinish(s_labPending, "failed", "connection_lost");
    }
    s_labPending[0] = '\0';
  }
  labStatus();

  switch (s_st) {
    case VoiceState::IDLE:      ledSet(LedMode::HEARTBEAT); break;
    case VoiceState::RECORDING: doRecording(); break;
    case VoiceState::WAITING:   doWaiting();   break;
    case VoiceState::PLAYING:   doPlaying();   break;
    default: break;
  }
  // Capture transitions inside doRecording/doWaiting/doPlaying in this loop,
  // rather than waiting for the next heartbeat (including final playback drain).
  labStatus();
}
