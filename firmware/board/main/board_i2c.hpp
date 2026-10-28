// The one I2C bus on the badge, shared.
//
// The PMU, the audio codec, the RTC, the temperature sensor and the IMU all sit
// on SDA 41 / SCL 42. ESP-IDF's i2c_master driver allows exactly one master per
// port, so the first driver to want the bus cannot own it: the panel needs the
// AXP2101 before anything else runs, and the microphone needs the ES8311 later,
// and whichever called i2c_new_master_bus() second would fail.
//
// So nobody owns it. This hands out the one handle and creates it on first use.
#pragma once

#if defined(HALO_PANEL_WAVESHARE)
#include "driver/i2c_master.h"

namespace halo::board {

// Null only if the bus could not be created at all, which on this board means
// nothing on it will answer: the panel will not light and the microphone will
// not record.
i2c_master_bus_handle_t i2c_bus();

// A device on that bus, or null. Addresses are in board_pins.hpp.
i2c_master_dev_handle_t i2c_device(uint8_t address, uint32_t hz = 400000);

}  // namespace halo::board
#endif
