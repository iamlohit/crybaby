#include "display.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "display";

static void backlight_init(uint8_t percent)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = 1023 * percent / 100, .hpoint = 0,
    };
    ledc_channel_config(&c);
}

lv_display_t *display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_SPI_MOSI, .miso_io_num = -1,
        .sclk_io_num = LCD_SPI_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, .spi_mode = 0,
        .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t p_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &p_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true)); /* ST7789 240x240 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_H_RES * 60,
        .double_buffer = true,
        .hres = LCD_H_RES, .vres = LCD_V_RES,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    backlight_init(80);
    ESP_LOGI(TAG, "ST7789 240x240 up");
    return disp;
}
