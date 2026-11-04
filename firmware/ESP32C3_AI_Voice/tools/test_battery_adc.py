"""Run the shared ADC driver against deterministic IDF stubs, not a voltage model.

Compare with the original offline self-test conversion; never opens hardware.
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
    'esp_adc/adc_oneshot.h': r'''#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=259;
constexpr int ADC_UNIT_1=1, ADC_CHANNEL_1=1, ADC_ATTEN_DB_12=12, ADC_BITWIDTH_DEFAULT=0;
using adc_oneshot_unit_handle_t=void*;
struct adc_oneshot_unit_init_cfg_t { int unit_id; };
struct adc_oneshot_chan_cfg_t { int atten; int bitwidth; };
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*, adc_oneshot_unit_handle_t*);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t,int,const adc_oneshot_chan_cfg_t*);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t,int,int*);
''',
    'esp_adc/adc_cali.h': r'''#pragma once
#include "adc_oneshot.h"
using adc_cali_handle_t=void*;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t,int,int*);
''',
    'esp_adc/adc_cali_scheme.h': r'''#pragma once
#include "adc_cali.h"
struct adc_cali_curve_fitting_config_t { int unit_id; int chan; int atten; int bitwidth; };
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*,adc_cali_handle_t*);
''',
}

harness = r'''
#include <cassert>
#include <cstdio>
#include <algorithm>
#include "battery_adc.h"
#include "esp_adc/adc_cali_scheme.h"
int values[16], reads=0, failRead=-1, calibrationInput=-1, initFailure=0;
bool failCalibration=false;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t* c,void** h) {
  assert(c->unit_id==ADC_UNIT_1);
  if(initFailure==1) return 100;
  *h=(void*)1; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(void* h,int channel,const adc_oneshot_chan_cfg_t* c) {
  assert(h && channel==ADC_CHANNEL_1 && c->atten==ADC_ATTEN_DB_12 && c->bitwidth==ADC_BITWIDTH_DEFAULT);
  return initFailure==2 ? 101 : ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t* c,void** h) {
  assert(c->unit_id==ADC_UNIT_1 && c->chan==ADC_CHANNEL_1);
  assert(c->atten==ADC_ATTEN_DB_12 && c->bitwidth==ADC_BITWIDTH_DEFAULT);
  if(initFailure==3) return 102;
  *h=(void*)2; return ESP_OK;
}
esp_err_t adc_oneshot_read(void* h,int channel,int* raw) {
  assert(h && channel==ADC_CHANNEL_1 && reads<16);
  if(reads==failRead) return 103;
  *raw=values[reads++]; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(void* h,int raw,int* mv) {
  assert(h); calibrationInput=raw;
  if(failCalibration) return 104;
  // Deliberately nonlinear stand-in: catch averaging calibrated samples instead of raw samples.
  *mv=raw*raw/8192; return ESP_OK;
}
// Original hwtest_main.cpp batteryMv(), with the original read/check/order retained.
int originalSelftest() {
  int raw=0,total=0,mv=0;
  for(int i=0;i<16;++i) {
    if(adc_oneshot_read((void*)1,ADC_CHANNEL_1,&raw)!=ESP_OK) return -1;
    total+=raw;
  }
  if(adc_cali_raw_to_voltage((void*)2,(total+8)/16,&mv)!=ESP_OK) return -1;
  return mv*2;
}
int main() {
  BatteryAdc driver; BatterySample sample;
  assert(!driver.read(sample) && sample.batteryMv==-1);
  assert(driver.begin()==ESP_OK);
  for(int trial=0;trial<64;++trial) {
    for(int i=0;i<16;++i) values[i]=(trial*67+i*i*13)%4096;
    reads=0; int expected=originalSelftest(), expectedMean=calibrationInput;
    reads=0; assert(driver.read(sample));
    assert(reads==16 && sample.error==ESP_OK && sample.batteryMv==expected);
    assert(sample.mean==expectedMean && sample.first==values[0] && sample.last==values[15]);
    assert(sample.minimum==*std::min_element(values,values+16));
    assert(sample.maximum==*std::max_element(values,values+16));
  }
  // An ADC/calibration error must never reuse the preceding successful voltage.
  reads=0; failRead=7; assert(!driver.read(sample));
  assert(sample.error==103 && sample.batteryMv==-1 && sample.pinMv==-1);
  reads=0; failRead=-1; failCalibration=true; assert(!driver.read(sample));
  assert(sample.error==104 && sample.batteryMv==-1);
  failCalibration=false;
  for(int i=1;i<=3;++i) {
    initFailure=i; BatteryAdc broken;
    assert(broken.begin()==99+i);
    assert(!broken.read(sample) && sample.batteryMv==-1);
  }
  puts("PASS: original self-test equivalence (64 sequences), 16 reads, ADC configuration, error isolation");
}
'''

with tempfile.TemporaryDirectory(prefix='aif-battery-adc-') as directory:
    root = Path(directory)
    for name, content in stubs.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8')
    (root / 'test.cpp').write_text(harness, encoding='utf-8')
    executable = root / 'test.exe'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), '-I', str(project),
                    str(root / 'test.cpp'), str(project / 'battery_adc.cpp'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
