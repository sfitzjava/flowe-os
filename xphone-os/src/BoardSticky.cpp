#include "BoardSticky.h"

#include <BoardConfig.h>

#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY

#include <Arduino.h>
#include <driver/gpio.h>

void BoardSticky::powerHold() {
  // GPIO45/46 are both ESP32-S3 strapping pins — they are only sampled at
  // reset, so by the time app code runs they are free to drive. Do not attach
  // anything that pulls them at boot (see the ESP32-S3 strapping-pin docs).
  gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_45, 1);  // hold the rail
  gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_46, 1);
  gpio_set_level(GPIO_NUM_46, 0);  // pulse
  gpio_set_level(GPIO_NUM_46, 1);
}

#else

void BoardSticky::powerHold() {}

#endif
