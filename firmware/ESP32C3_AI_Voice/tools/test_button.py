"""Run the actual button.cpp against fake GPIO/time; no board or secrets needed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

project=Path(__file__).resolve().parents[1]
compiler=shutil.which('g++')
if not compiler:raise SystemExit('A native g++ compiler is required')
stubs={
    'esp_timer.h':'#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'freertos/FreeRTOS.h':'#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n',
    'freertos/task.h':'#pragma once\nvoid taskYIELD();\nvoid vTaskDelay(unsigned);\n',
    'driver/gpio.h':'''#pragma once
#include <cstdint>
using gpio_num_t=int;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; };
constexpr int GPIO_MODE_INPUT=1, GPIO_PULLUP_DISABLE=0;
int gpio_get_level(gpio_num_t);
int gpio_config(const gpio_config_t*);
''',
    'esp_err.h':'#pragma once\n#include <cassert>\n#define ESP_ERROR_CHECK(x) assert((x)==0)\n',
}
harness=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "button.h"
#include "driver/gpio.h"
static uint32_t nowMs=0;
static int level=1;
int64_t esp_timer_get_time() { return int64_t(nowMs)*1000; }
void taskYIELD() {}
void vTaskDelay(unsigned n) { nowMs+=n; }
int gpio_get_level(gpio_num_t) { return level; }
int gpio_config(const gpio_config_t*) { return 0; }
static void edge(Button& b,int value) {
  level=value;b.update();nowMs+=26;b.update();
}
int main() {
  Button b;b.begin(3,25);
  edge(b,0);assert(b.tookPress());
  edge(b,1); // A release while idle is deliberately left unconsumed.
  edge(b,0);assert(b.tookPress());
  assert(!b.tookRelease()); // This reproduced the immediate 20ms recording bug.
  for(int i=0;i<20;++i) { nowMs+=20;b.update();assert(!b.tookRelease()); }
  edge(b,1);assert(b.tookRelease());assert(!b.tookRelease());
  edge(b,0);edge(b,1); // Both edges can accumulate while another action blocks.
  assert(b.tookPress());assert(!b.isPressed()); // Session must not start stale taps.
  assert(b.tookRelease());
  // Bounces shorter than the debounce interval must not start a new press.
  level=0;b.update();nowMs+=5;level=1;b.update();nowMs+=30;b.update();
  assert(!b.tookPress());
  std::puts("PASS: idle release cannot end next held press; real release and debounce work");
}
'''
with tempfile.TemporaryDirectory(prefix='ai-figure-button-') as folder:
    root=Path(folder)
    for name,content in stubs.items():
        path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content,encoding='utf-8')
    (root/'test.cpp').write_text(harness,encoding='utf-8')
    executable=root/'test.exe'
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-static-libgcc','-static-libstdc++',
                    '-I',str(root),'-I',str(project),str(project/'button.cpp'),str(root/'test.cpp'),
                    '-o',str(executable)],check=True)
    result=subprocess.run([str(executable)])
    raise SystemExit(result.returncode)
