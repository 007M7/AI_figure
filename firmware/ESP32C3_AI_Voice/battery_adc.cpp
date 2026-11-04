#include "battery_adc.h"
#include "config.h"
#include "esp_adc/adc_cali_scheme.h"

esp_err_t BatteryAdc::begin() {
  static_assert(PIN_BAT_ADC == 1, "The self-test samples GPIO1 / ADC1 channel1");
  adc_oneshot_unit_init_cfg_t unit = {};
  unit.unit_id = ADC_UNIT_1;
  esp_err_t error = adc_oneshot_new_unit(&unit, &adc_);
  if (error != ESP_OK) return error;
  adc_oneshot_chan_cfg_t channel = {};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_DEFAULT;
  error = adc_oneshot_config_channel(adc_, ADC_CHANNEL_1, &channel);
  if (error != ESP_OK) return error;
  adc_cali_curve_fitting_config_t calibration = {};
  calibration.unit_id = ADC_UNIT_1;
  calibration.chan = ADC_CHANNEL_1;
  calibration.atten = channel.atten;
  calibration.bitwidth = channel.bitwidth;
  return adc_cali_create_scheme_curve_fitting(&calibration, &calibration_);
}

bool BatteryAdc::read(BatterySample& result) {
  result = BatterySample{};
  if (!adc_ || !calibration_) return false;
  int raw = 0, total = 0, mv = 0;
  for (int i = 0; i < 16; ++i) {
    result.error = adc_oneshot_read(adc_, ADC_CHANNEL_1, &raw);
    if (result.error != ESP_OK) return false;
    if (i == 0) result.first = result.minimum = result.maximum = raw;
    result.last = raw;
    if (raw < result.minimum) result.minimum = raw;
    if (raw > result.maximum) result.maximum = raw;
    total += raw;
  }
  result.mean = (total + 8) / 16;
  result.error = adc_cali_raw_to_voltage(calibration_, result.mean, &mv);
  if (result.error != ESP_OK) return false;
  result.pinMv = mv;
  result.batteryMv = mv * 2;
  return true;
}
