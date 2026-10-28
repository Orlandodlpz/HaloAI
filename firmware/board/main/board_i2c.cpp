#include "board_i2c.hpp"

#if defined(HALO_PANEL_WAVESHARE)
#include "board_pins.hpp"
#include "esp_log.h"

namespace halo::board {
namespace {
constexpr const char* kTag = "i2c";
i2c_master_bus_handle_t g_bus = nullptr;
bool g_tried = false;
}  // namespace

i2c_master_bus_handle_t i2c_bus() {
    if (g_tried) return g_bus;
    g_tried = true;

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = kI2cSda;
    cfg.scl_io_num = kI2cScl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&cfg, &g_bus) != ESP_OK) {
        g_bus = nullptr;
        ESP_LOGE(kTag, "I2C bus init failed on SDA %d / SCL %d: nothing on this bus "
                       "will answer, so the panel will not light and the microphone "
                       "will not record",
                 kI2cSda, kI2cScl);
        return nullptr;
    }
    ESP_LOGI(kTag, "I2C up on SDA %d / SCL %d", kI2cSda, kI2cScl);
    return g_bus;
}

i2c_master_dev_handle_t i2c_device(uint8_t address, uint32_t hz) {
    i2c_master_bus_handle_t bus = i2c_bus();
    if (bus == nullptr) return nullptr;

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = address;
    dev.scl_speed_hz = hz;

    i2c_master_dev_handle_t handle = nullptr;
    if (i2c_master_bus_add_device(bus, &dev, &handle) != ESP_OK) {
        ESP_LOGE(kTag, "could not add 0x%02x to the I2C bus", address);
        return nullptr;
    }
    return handle;
}

}  // namespace halo::board
#endif
