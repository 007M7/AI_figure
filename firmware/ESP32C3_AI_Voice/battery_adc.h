#pragma once
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

struct BatterySample {
  int first = -1, last = -1, minimum = -1, maximum = -1;
  int mean = -1, pinMv = -1, batteryMv = -1;
  esp_err_t error = ESP_ERR_INVALID_STATE;
};

// Shared by the offline P self-test and the voice firmware. Keep the proven
// self-test conversion: 16 reads, rounded raw mean, curve calibration, x2.
class BatteryAdc {
 public:
  esp_err_t begin();
  bool read(BatterySample& result);
 private:
  adc_oneshot_unit_handle_t adc_ = nullptr;
  adc_cali_handle_t calibration_ = nullptr;
};
