#include "light_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "device_config/nvm_items.h"
#include "hal/nvm.h"
#include "hal/printf_selector.h"
#include "hal/gpio.h"
#include "hal/pwm.h"
#include "hal/zigbee.h"

#include <stdbool.h>
#include <string.h>

#define LIGHT_MAX_LEVEL     0xFE
#define PWM_FREQUENCY_HZ    600  // 600Hz PWM for all channels

static zigbee_light_cluster *light_cluster_by_endpoint[10];

// PWM channel handles for each color channel
static hal_pwm_channel_t pwm_channels_r[10];
static hal_pwm_channel_t pwm_channels_g[10];
static hal_pwm_channel_t pwm_channels_b[10];
static hal_pwm_channel_t pwm_channels_c[10];
static hal_pwm_channel_t pwm_channels_w[10];

// Convert ZCL level (0-254) to percentage (0-100)
static uint8_t level_to_percent(uint8_t level) {
    if (level == 0) return 0;

    if (level >= LIGHT_MAX_LEVEL) return 100;

    return (uint8_t)((uint32_t)level * 100 / LIGHT_MAX_LEVEL);
}

// Convert color temperature (mired) to warm/cold percentages
// cold_temp_mired = 153 (6500K), warm_temp_mired = 500 (2000K)
static void color_temp_to_percentages(uint16_t color_temp_mired,
                                      uint8_t *cold_percent,
                                      uint8_t *warm_percent) {
    const uint16_t cold_mired = 153;
    const uint16_t warm_mired = 500;

    if (color_temp_mired <= cold_mired) {
        *cold_percent = 100;
        *warm_percent = 0;
    } else if (color_temp_mired >= warm_mired) {
        *cold_percent = 0;
        *warm_percent = 100;
    } else {
        // Linear interpolation
        uint32_t range  = warm_mired - cold_mired;
        uint32_t offset = color_temp_mired - cold_mired;
        *warm_percent = (uint8_t)((offset * 100) / range);
        *cold_percent = 100 - *warm_percent;
    }
}

// Convert HSV to RGB
static void hsv_to_rgb_percent(uint8_t hue, uint8_t sat, uint8_t val,
                               uint8_t *r_percent,
                               uint8_t *g_percent,
                               uint8_t *b_percent) {
    // Hue is 0-254 (360 degrees), Sat is 0-254 (100%), Val is 0-254 (100%)
    if (sat == 0) {
        // Grayscale
        *r_percent = val * 100 / 254;
        *g_percent = val * 100 / 254;
        *b_percent = val * 100 / 254;
        return;
    }

    uint16_t h = (uint16_t)hue * 360 / 254;  // Convert to 0-360
    uint16_t s = sat;
    uint16_t v = val;

    uint16_t region    = h / 60;
    uint16_t remainder = (h % 60) * 6;

    uint16_t p = (v * (254 - s)) / 254;
    uint16_t q = (v * (254 - ((s * remainder) / 360))) / 254;
    uint16_t t = (v * (254 - ((s * (360 - remainder)) / 360))) / 254;

    uint8_t r, g, b;
    switch (region) {
    case 0: r  = v; g = t; b = p; break;
    case 1: r  = q; g = v; b = p; break;
    case 2: r  = p; g = v; b = t; break;
    case 3: r  = p; g = q; b = v; break;
    case 4: r  = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }

    *r_percent = r * 100 / 254;
    *g_percent = g * 100 / 254;
    *b_percent = b * 100 / 254;
}

// Convert CIE XY to RGB with proper color space handling
static void xy_to_rgb_percent(uint16_t x, uint16_t y,
                              uint8_t *r_percent,
                              uint8_t *g_percent,
                              uint8_t *b_percent) {
    // X and Y are in range 0-65535 (representing 0.0-1.0)
    // Avoid division by zero
    if (y < 100) {
        *r_percent = 100;
        *g_percent = 100;
        *b_percent = 100;
        return;
    }

    // Convert to XYZ color space with Y=1 (maximum brightness)
    // x = X/(X+Y+Z), y = Y/(X+Y+Z), z = Z/(X+Y+Z)
    // Given x,y and assuming Y=1, we can solve: X = x/y, Z = (1-x-y)/y

    // Scale calculations to avoid overflow (use 16-bit math)
    // x and y are already 0-65535 representing 0.0-1.0
    int32_t x_norm = x;  // 0-65535
    int32_t y_norm = y;  // 0-65535

    // Calculate X and Z with Y=1 (scaled by 65535)
    int32_t X              = (x_norm * 65535) / y_norm;
    int32_t Y_val          = 65535;
    int32_t z_chromaticity = 65535 - x_norm - y_norm;
    if (z_chromaticity < 0) z_chromaticity = 0;
    int32_t Z = (z_chromaticity * 65535) / y_norm;

    // sRGB transformation matrix (scaled by 65536 for precision):
    // R =  3.2406*X - 1.5372*Y - 0.4986*Z
    // G = -0.9689*X + 1.8758*Y + 0.0415*Z
    // B =  0.0557*X - 0.2040*Y + 1.0570*Z

    // Integer coefficients scaled by 65536
    int32_t r = (X * 212336 - Y_val * 100763 - Z * 32681) / 65536;
    int32_t g = (-X * 63519 + Y_val * 122942 + Z * 2721) / 65536;
    int32_t b = (X * 3650 - Y_val * 13369 + Z * 69295) / 65536;

    // Clamp to valid range
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 65535) r = 65535;
    if (g > 65535) g = 65535;
    if (b > 65535) b = 65535;

    // Find max for normalization
    int32_t max = r;
    if (g > max) max = g;
    if (b > max) max = b;

    // Normalize to 0-100 range
    if (max > 0) {
        *r_percent = (uint8_t)((r * 100) / max);
        *g_percent = (uint8_t)((g * 100) / max);
        *b_percent = (uint8_t)((b * 100) / max);
    } else {
        *r_percent = 100;
        *g_percent = 100;
        *b_percent = 100;
    }
}

static void light_cluster_set_pin(hal_gpio_pin_t pin, uint8_t on_high,
                                  uint8_t enabled) {
    if (pin == HAL_INVALID_PIN) {
        return;
    }
    uint8_t value = enabled ? (on_high ? 1 : 0) : (on_high ? 0 : 1);
    hal_gpio_write(pin, value);
}

static void light_cluster_set_pwm_duty(hal_pwm_channel_t channel,
                                       hal_gpio_pin_t pin,
                                       uint8_t on_high,
                                       uint8_t duty_percent) {
    printf("set_pwm: ch=%d pin=%d duty=%d\r\n", channel, pin, duty_percent);

    if (pin == HAL_INVALID_PIN) {
        printf("Invalid pin!\r\n");
        return;
    }

    if (channel == HAL_PWM_INVALID_CHANNEL) {
        printf("Invalid channel!\r\n");
        // Fallback to simple on/off if PWM not available
        light_cluster_set_pin(pin, on_high, duty_percent > 50);
        return;
    }

    // Invert if active-low
    uint8_t actual_duty = on_high ? duty_percent : (100 - duty_percent);
    printf("Calling hal_pwm_set_duty(%d, %d)\r\n", channel, actual_duty);
    hal_pwm_set_duty(channel, actual_duty);
    printf("Done\r\n");
}

static void light_cluster_gpio_output(zigbee_light_cluster *cluster) {
    if (!cluster) {
        return;
    }

    uint8_t endpoint           = cluster->endpoint;
    uint8_t active             = cluster->on_off ? 1 : 0;
    uint8_t brightness_percent = 100;

    if (cluster->supports_level) {
        active             = active && cluster->level > 0 ? 1 : 0;
        brightness_percent = level_to_percent(cluster->level);
    }

    // Determine mode based on color_mode attribute
    // XY/HS mode = use RGB, color_temp mode = use whites
    uint8_t use_rgb = (cluster->color_mode == ZCL_COLOR_MODE_CURRENT_XY ||
                       cluster->color_mode == ZCL_COLOR_MODE_HS);
    uint8_t use_ct = (cluster->color_mode == ZCL_COLOR_MODE_COLOR_TEMPERATURE);

    printf("Light: active=%d bright=%d rgb=%d ct=%d mode=%d x=%u y=%u h=%u s=%u\r\n",
           active, brightness_percent, use_rgb, use_ct, cluster->color_mode,
           cluster->current_x, cluster->current_y, cluster->current_hue,
           cluster->current_saturation);

    if (use_rgb && active) {
        // RGB mode - stop white PWM, start RGB PWM
        if (pwm_channels_c[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_c[endpoint]);
            pwm_channels_c[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_w[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_w[endpoint]);
            pwm_channels_w[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }

        // RGB mode - use color XY or HS based on mode
        uint8_t r_pct, g_pct, b_pct;

        // Use HS or XY based on color mode
        if (cluster->color_mode == ZCL_COLOR_MODE_HS) {
            // For HS mode, use current level as the V (value/brightness)
            hsv_to_rgb_percent(cluster->current_hue, cluster->current_saturation, cluster->level,
                               &r_pct, &g_pct, &b_pct);
            printf("HS RGB: ");
        } else {
            // For XY mode, get the color then apply brightness
            xy_to_rgb_percent(cluster->current_x, cluster->current_y,
                              &r_pct, &g_pct, &b_pct);
            // Apply brightness to XY colors
            r_pct = (uint8_t)((uint32_t)r_pct * brightness_percent / 100);
            g_pct = (uint8_t)((uint32_t)g_pct * brightness_percent / 100);
            b_pct = (uint8_t)((uint32_t)b_pct * brightness_percent / 100);
            printf("XY RGB: ");
        }

        printf("%d,%d,%d\r\n", r_pct, g_pct, b_pct);

        // Ensure at least 2% duty if channel is supposed to be on to avoid issues
        if (r_pct > 0 && r_pct < 2) r_pct = 2;
        if (g_pct > 0 && g_pct < 2) g_pct = 2;
        if (b_pct > 0 && b_pct < 2) b_pct = 2;

        printf("RGB adjusted: %d,%d,%d on_high=%d,%d,%d\r\n",
               r_pct, g_pct, b_pct,
               cluster->pin_r_on_high, cluster->pin_g_on_high, cluster->pin_b_on_high);


        // Start RGB PWM channels if not already started
        if (pwm_channels_r[endpoint] == HAL_PWM_INVALID_CHANNEL &&
            cluster->pin_r != HAL_INVALID_PIN) {
            pwm_channels_r[endpoint] = hal_pwm_start(cluster->pin_r, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_g[endpoint] == HAL_PWM_INVALID_CHANNEL &&
            cluster->pin_g != HAL_INVALID_PIN) {
            pwm_channels_g[endpoint] = hal_pwm_start(cluster->pin_g, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_b[endpoint] == HAL_PWM_INVALID_CHANNEL &&
            cluster->pin_b != HAL_INVALID_PIN) {
            pwm_channels_b[endpoint] = hal_pwm_start(cluster->pin_b, PWM_FREQUENCY_HZ, 0);
        }

        // Set RGB PWM duty cycles
        light_cluster_set_pwm_duty(pwm_channels_r[endpoint], cluster->pin_r,
                                   cluster->pin_r_on_high, r_pct);
        light_cluster_set_pwm_duty(pwm_channels_g[endpoint], cluster->pin_g,
                                   cluster->pin_g_on_high, g_pct);
        light_cluster_set_pwm_duty(pwm_channels_b[endpoint], cluster->pin_b,
                                   cluster->pin_b_on_high, b_pct);
    }else if (use_ct && active) {
        // Color temp mode - stop RGB, start white hardware PWM
        if (pwm_channels_r[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_r[endpoint]);
            pwm_channels_r[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_g[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_g[endpoint]);
            pwm_channels_g[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_b[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_b[endpoint]);
            pwm_channels_b[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }

        // Ensure white PWM is started
        if (pwm_channels_c[endpoint] == HAL_PWM_INVALID_CHANNEL &&
            cluster->pin_c != HAL_INVALID_PIN) {
            pwm_channels_c[endpoint] = hal_pwm_start(cluster->pin_c, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_w[endpoint] == HAL_PWM_INVALID_CHANNEL &&
            cluster->pin_w != HAL_INVALID_PIN) {
            pwm_channels_w[endpoint] = hal_pwm_start(cluster->pin_w, PWM_FREQUENCY_HZ, 0);
        }

        // Color temperature mode - use warm/cold white
        uint8_t cold_pct, warm_pct;
        color_temp_to_percentages(cluster->color_temp, &cold_pct, &warm_pct);

        // Apply brightness
        cold_pct = (uint8_t)((uint32_t)cold_pct * brightness_percent / 100);
        warm_pct = (uint8_t)((uint32_t)warm_pct * brightness_percent / 100);

        // Ensure at least 1% duty if channel is supposed to be on to avoid flicker
        if (cold_pct > 0 && cold_pct < 2) cold_pct = 2;
        if (warm_pct > 0 && warm_pct < 2) warm_pct = 2;

        light_cluster_set_pwm_duty(pwm_channels_c[endpoint], cluster->pin_c,
                                   cluster->pin_c_on_high, cold_pct);
        light_cluster_set_pwm_duty(pwm_channels_w[endpoint], cluster->pin_w,
                                   cluster->pin_w_on_high, warm_pct);
    }else {
        // Off - stop all PWM
        if (pwm_channels_r[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_r[endpoint]);
            pwm_channels_r[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_g[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_g[endpoint]);
            pwm_channels_g[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_b[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_b[endpoint]);
            pwm_channels_b[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_c[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_c[endpoint]);
            pwm_channels_c[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
        if (pwm_channels_w[endpoint] != HAL_PWM_INVALID_CHANNEL) {
            hal_pwm_stop(pwm_channels_w[endpoint]);
            pwm_channels_w[endpoint] = HAL_PWM_INVALID_CHANNEL;
        }
    }
}

static void light_cluster_notify(zigbee_light_cluster *cluster,
                                 uint16_t cluster_id,
                                 uint16_t attr_id) {
    if (!cluster) {
        return;
    }
    hal_zigbee_notify_attribute_changed(cluster->endpoint, cluster_id, attr_id);
}

static void light_cluster_apply_output(zigbee_light_cluster *cluster) {
    if (cluster->output_callback) {
        cluster->output_callback(cluster);
    }
}

static uint8_t clamp_level(uint8_t level) {
    if (level == 0xFF) {
        return LIGHT_MAX_LEVEL;
    }
    return level > LIGHT_MAX_LEVEL ? LIGHT_MAX_LEVEL : level;
}

static void light_cluster_set_onoff(zigbee_light_cluster *cluster,
                                    uint8_t on_off) {
    if (!cluster) {
        return;
    }
    cluster->on_off = on_off ? 1 : 0;
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_ON_OFF, ZCL_ATTR_ONOFF);
}

// NVM storage for light cluster state
#define NV_ITEM_LIGHT_CLUSTER_DATA(idx)    (0x0600 + (idx))

typedef struct {
    uint8_t  on_off;
    uint8_t  level;
    uint16_t current_x;
    uint16_t current_y;
    uint8_t  current_hue;
    uint8_t  current_saturation;
    uint16_t color_temp;
    uint8_t  color_mode;
    uint8_t  startup_onoff;
    uint16_t startup_color_temp;
} light_cluster_nv_data_t;

static light_cluster_nv_data_t nv_light_buffer;

static void light_cluster_store_to_nv(zigbee_light_cluster *cluster) {
    nv_light_buffer.on_off             = cluster->on_off;
    nv_light_buffer.level              = cluster->level;
    nv_light_buffer.current_x          = cluster->current_x;
    nv_light_buffer.current_y          = cluster->current_y;
    nv_light_buffer.current_hue        = cluster->current_hue;
    nv_light_buffer.current_saturation = cluster->current_saturation;
    nv_light_buffer.color_temp         = cluster->color_temp;
    nv_light_buffer.color_mode         = cluster->color_mode;
    nv_light_buffer.startup_onoff      = cluster->startup_onoff;
    nv_light_buffer.startup_color_temp = cluster->startup_color_temp;

    hal_nvm_write(NV_ITEM_LIGHT_CLUSTER_DATA(cluster->endpoint),
                  sizeof(light_cluster_nv_data_t), (uint8_t *)&nv_light_buffer);
}

static void light_cluster_restore_from_nv(zigbee_light_cluster *cluster) {
    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_LIGHT_CLUSTER_DATA(cluster->endpoint),
        sizeof(light_cluster_nv_data_t), (uint8_t *)&nv_light_buffer);

    if (st != HAL_NVM_SUCCESS)
        return;

    cluster->startup_onoff      = nv_light_buffer.startup_onoff;
    cluster->startup_color_temp = nv_light_buffer.startup_color_temp;
}

void light_cluster_handle_startup_mode(zigbee_light_cluster *cluster) {
    hal_nvm_status_t st = hal_nvm_read(
        NV_ITEM_LIGHT_CLUSTER_DATA(cluster->endpoint),
        sizeof(light_cluster_nv_data_t), (uint8_t *)&nv_light_buffer);

    if (st != HAL_NVM_SUCCESS)
        return;

    uint8_t prev_on = nv_light_buffer.on_off;

    switch (cluster->startup_onoff) {
    case ZCL_START_UP_ONOFF_SET_ONOFF_TO_OFF:
        light_cluster_set_onoff(cluster, 0);
        break;

    case ZCL_START_UP_ONOFF_SET_ONOFF_TO_ON:
        light_cluster_set_onoff(cluster, 1);
        break;

    case ZCL_START_UP_ONOFF_SET_ONOFF_TOGGLE:
        light_cluster_set_onoff(cluster, prev_on ? 0 : 1);
        break;

    case ZCL_START_UP_ONOFF_SET_ONOFF_TO_PREVIOUS:
        cluster->level              = nv_light_buffer.level;
        cluster->current_x          = nv_light_buffer.current_x;
        cluster->current_y          = nv_light_buffer.current_y;
        cluster->current_hue        = nv_light_buffer.current_hue;
        cluster->current_saturation = nv_light_buffer.current_saturation;
        cluster->color_temp         = nv_light_buffer.color_temp;
        cluster->color_mode         = nv_light_buffer.color_mode;
        light_cluster_set_onoff(cluster, prev_on);
        break;
    }

    // Apply startup color temperature if configured
    if (cluster->startup_color_temp != 0xFFFF && cluster->supports_color_temp) {
        cluster->color_temp = cluster->startup_color_temp;
        cluster->color_mode = ZCL_COLOR_MODE_COLOR_TEMPERATURE;
        light_cluster_apply_output(cluster);
    }
}

void update_light_clusters() {
    for (int i = 0; i < 10; i++) {
        if (light_cluster_by_endpoint[i] != NULL) {
            light_cluster_handle_startup_mode(light_cluster_by_endpoint[i]);
        }
    }
}

static void light_cluster_set_level(zigbee_light_cluster *cluster,
                                    uint8_t level, bool with_onoff) {
    if (!cluster || !cluster->supports_level) {
        return;
    }
    cluster->level = clamp_level(level);
    if (with_onoff) {
        cluster->on_off = cluster->level > 0 ? 1 : 0;
        light_cluster_notify(cluster, ZCL_CLUSTER_ON_OFF, ZCL_ATTR_ONOFF);
    }
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_LEVEL_CONTROL,
                         ZCL_ATTR_LEVEL_CURRENT_LEVEL);
}

static void light_cluster_set_color_xy(zigbee_light_cluster *cluster,
                                       uint16_t x, uint16_t y) {
    if (!cluster || !cluster->supports_color_xy) {
        return;
    }
    cluster->current_x  = x;
    cluster->current_y  = y;
    cluster->color_mode = ZCL_COLOR_MODE_CURRENT_XY;
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_X);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_Y);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_COLOR_MODE);
}

static void light_cluster_set_color_hs(zigbee_light_cluster *cluster,
                                       uint8_t hue, uint8_t sat) {
    if (!cluster || !cluster->supports_color_xy) {
        return;
    }
    cluster->current_hue        = hue;
    cluster->current_saturation = sat;
    cluster->color_mode         = ZCL_COLOR_MODE_HS;
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_COLOR_MODE);
}

static void light_cluster_set_color_temp(zigbee_light_cluster *cluster,
                                         uint16_t color_temp) {
    if (!cluster || !cluster->supports_color_temp) {
        return;
    }
    cluster->color_temp = color_temp;
    cluster->color_mode = ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_COLOR_MODE);
}

static hal_zigbee_cmd_result_t light_cluster_onoff_cmd_callback(
    uint8_t endpoint, uint16_t cluster_id, uint8_t command_id,
    void *cmd_payload) {
    zigbee_light_cluster *cluster = light_cluster_by_endpoint[endpoint];

    (void)cluster_id;
    (void)cmd_payload;

    if (!cluster) {
        return HAL_ZIGBEE_CMD_SKIPPED;
    }

    switch (command_id) {
    case ZCL_CMD_ONOFF_ON:
    case ZCL_CMD_ON_WITH_RECALL_GLOBAL_SCENE:
        light_cluster_set_onoff(cluster, 1);
        break;
    case ZCL_CMD_ONOFF_OFF:
    case ZCL_CMD_OFF_WITH_EFFECT:
        light_cluster_set_onoff(cluster, 0);
        break;
    case ZCL_CMD_ONOFF_TOGGLE:
        light_cluster_set_onoff(cluster, !cluster->on_off);
        break;
    default:
        printf("Unknown OnOff command: %d\r\n", command_id);
        break;
    }

    return HAL_ZIGBEE_CMD_PROCESSED;
}

static hal_zigbee_cmd_result_t light_cluster_level_cmd_callback(
    uint8_t endpoint, uint16_t cluster_id, uint8_t command_id,
    void *cmd_payload) {
    zigbee_light_cluster *cluster = light_cluster_by_endpoint[endpoint];
    uint8_t *payload = (uint8_t *)cmd_payload;

    (void)cluster_id;

    if (!cluster || !cluster->supports_level) {
        return HAL_ZIGBEE_CMD_SKIPPED;
    }

    switch (command_id) {
    case ZCL_CMD_LEVEL_MOVE_TO_LEVEL:
    case ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF:
        if (payload) {
            light_cluster_set_level(cluster, payload[0],
                                    command_id ==
                                    ZCL_CMD_LEVEL_MOVE_TO_LEVEL_WITH_ON_OFF);
        }
        break;
    case ZCL_CMD_LEVEL_MOVE:
    case ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF:
        if (payload) {
            uint8_t dir    = payload[0];
            uint8_t target = dir == ZCL_LEVEL_MOVE_DOWN ? 0 : LIGHT_MAX_LEVEL;
            light_cluster_set_level(cluster, target,
                                    command_id ==
                                    ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF);
        }
        break;
    case ZCL_CMD_LEVEL_STEP:
    case ZCL_CMD_LEVEL_STEP_WITH_ON_OFF:
        if (payload) {
            uint8_t dir   = payload[0];
            uint8_t step  = payload[1];
            int     level = cluster->level;
            if (dir == ZCL_LEVEL_MOVE_DOWN) {
                level = level > step ? level - step : 0;
            } else {
                level += step;
                if (level > LIGHT_MAX_LEVEL) {
                    level = LIGHT_MAX_LEVEL;
                }
            }
            light_cluster_set_level(cluster, (uint8_t)level,
                                    command_id ==
                                    ZCL_CMD_LEVEL_STEP_WITH_ON_OFF);
        }
        break;
    case ZCL_CMD_LEVEL_STOP:
    case ZCL_CMD_LEVEL_STOP_WITH_ON_OFF:
        break;
    default:
        printf("Unknown Level command: %d\r\n", command_id);
        break;
    }

    return HAL_ZIGBEE_CMD_PROCESSED;
}

static hal_zigbee_cmd_result_t light_cluster_color_cmd_callback(
    uint8_t endpoint, uint16_t cluster_id, uint8_t command_id,
    void *cmd_payload) {
    zigbee_light_cluster *cluster = light_cluster_by_endpoint[endpoint];
    uint8_t *payload = (uint8_t *)cmd_payload;

    (void)cluster_id;

    if (!cluster || (!cluster->supports_color_temp &&
                     !cluster->supports_color_xy)) {
        return HAL_ZIGBEE_CMD_SKIPPED;
    }

    switch (command_id) {
    case ZCL_CMD_COLOR_MOVE_TO_HUE_AND_SATURATION:
        if (payload && cluster->supports_color_xy) {
            uint8_t hue = payload[0];
            uint8_t sat = payload[1];
            light_cluster_set_color_hs(cluster, hue, sat);
            printf("Set HS: h=%d s=%d\r\n", hue, sat);
        }
        break;
    case ZCL_CMD_COLOR_MOVE_TO_COLOR:
        if (payload && cluster->supports_color_xy) {
            uint16_t x = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            uint16_t y = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            light_cluster_set_color_xy(cluster, x, y);
        }
        break;
    case ZCL_CMD_COLOR_MOVE_TO_COLOR_TEMPERATURE:
        if (payload && cluster->supports_color_temp) {
            uint16_t temp = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            light_cluster_set_color_temp(cluster, temp);
        }
        break;
    default:
        printf("Unknown Color command: %d\r\n", command_id);
        break;
    }

    return HAL_ZIGBEE_CMD_PROCESSED;
}

void light_cluster_init(zigbee_light_cluster *cluster) {
    if (!cluster) {
        return;
    }
    memset(cluster, 0, sizeof(*cluster));
    cluster->level              = LIGHT_MAX_LEVEL;
    cluster->color_temp         = 370;
    cluster->color_mode         = ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    cluster->startup_onoff      = ZCL_START_UP_ONOFF_SET_ONOFF_TO_PREVIOUS;
    cluster->startup_color_temp = 0xFFFF;  // Use previous color temp
    cluster->pin_r              = HAL_INVALID_PIN;
    cluster->pin_g              = HAL_INVALID_PIN;
    cluster->pin_b              = HAL_INVALID_PIN;
    cluster->pin_c              = HAL_INVALID_PIN;
    cluster->pin_w              = HAL_INVALID_PIN;
    cluster->pin_r_on_high      = 1;
    cluster->pin_g_on_high      = 1;
    cluster->pin_b_on_high      = 1;
    cluster->pin_c_on_high      = 1;
    cluster->pin_w_on_high      = 1;
    cluster->output_callback    = light_cluster_gpio_output;

    // Initialize PWM subsystem
    hal_pwm_init();
}

void light_cluster_set_output_callback(zigbee_light_cluster *cluster,
                                       void (*cb)(zigbee_light_cluster *cluster)) {
    if (!cluster) {
        return;
    }
    cluster->output_callback = cb;
}

void light_cluster_add_to_endpoint(zigbee_light_cluster *cluster,
                                   hal_zigbee_endpoint *endpoint) {
    if (!cluster || !endpoint) {
        return;
    }

    light_cluster_by_endpoint[endpoint->endpoint] = cluster;
    cluster->endpoint = endpoint->endpoint;

    // Restore startup mode from NVM
    light_cluster_restore_from_nv(cluster);

    if (!cluster->supports_level &&
        (cluster->supports_color_temp || cluster->supports_color_xy)) {
        cluster->supports_level = 1;
    }

    // Default to white mode if available, otherwise RGB
    if (cluster->supports_color_temp) {
        cluster->color_mode = ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    } else if (cluster->supports_color_xy) {
        cluster->color_mode = ZCL_COLOR_MODE_CURRENT_XY;
    }

    // Initialize PWM channel handles to invalid
    // They will be started dynamically based on color mode
    uint8_t ep = endpoint->endpoint;
    pwm_channels_r[ep] = HAL_PWM_INVALID_CHANNEL;
    pwm_channels_g[ep] = HAL_PWM_INVALID_CHANNEL;
    pwm_channels_b[ep] = HAL_PWM_INVALID_CHANNEL;
    pwm_channels_c[ep] = HAL_PWM_INVALID_CHANNEL;
    pwm_channels_w[ep] = HAL_PWM_INVALID_CHANNEL;

    SETUP_ATTR_FOR_TABLE(cluster->onoff_attr_infos, 0, ZCL_ATTR_ONOFF,
                         ZCL_DATA_TYPE_BOOLEAN, ATTR_WRITABLE,
                         cluster->on_off);
    SETUP_ATTR_FOR_TABLE(cluster->onoff_attr_infos, 1, ZCL_ATTR_START_UP_ONOFF,
                         ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE,
                         cluster->startup_onoff);

    endpoint->clusters[endpoint->cluster_count].cluster_id =
        ZCL_CLUSTER_ON_OFF;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 2;
    endpoint->clusters[endpoint->cluster_count].attributes      =
        cluster->onoff_attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server    = 1;
    endpoint->clusters[endpoint->cluster_count].cmd_callback =
        light_cluster_onoff_cmd_callback;
    endpoint->cluster_count++;

    if (cluster->supports_level) {
        SETUP_ATTR_FOR_TABLE(cluster->level_attr_infos, 0,
                             ZCL_ATTR_LEVEL_CURRENT_LEVEL,
                             ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE, cluster->level);

        endpoint->clusters[endpoint->cluster_count].cluster_id =
            ZCL_CLUSTER_LEVEL_CONTROL;
        endpoint->clusters[endpoint->cluster_count].attribute_count = 1;
        endpoint->clusters[endpoint->cluster_count].attributes      =
            cluster->level_attr_infos;
        endpoint->clusters[endpoint->cluster_count].is_server    = 1;
        endpoint->clusters[endpoint->cluster_count].cmd_callback =
            light_cluster_level_cmd_callback;
        endpoint->cluster_count++;
    }

    if (cluster->supports_color_temp || cluster->supports_color_xy) {
        uint8_t attr_index = 0;
        if (cluster->supports_color_xy) {
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE,
                                 ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE,
                                 cluster->current_hue);
            attr_index++;
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION,
                                 ZCL_DATA_TYPE_UINT8, ATTR_WRITABLE,
                                 cluster->current_saturation);
            attr_index++;
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_CURRENT_X,
                                 ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
                                 cluster->current_x);
            attr_index++;
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_CURRENT_Y,
                                 ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
                                 cluster->current_y);
            attr_index++;
        }
        if (cluster->supports_color_temp) {
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP,
                                 ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
                                 cluster->color_temp);
            attr_index++;
            SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                                 ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMP,
                                 ZCL_DATA_TYPE_UINT16, ATTR_WRITABLE,
                                 cluster->startup_color_temp);
            attr_index++;
        }
        SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                             ZCL_ATTR_COLOR_CONTROL_COLOR_MODE,
                             ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE,
                             cluster->color_mode);
        attr_index++;

        endpoint->clusters[endpoint->cluster_count].cluster_id =
            ZCL_CLUSTER_COLOR_CONTROL;
        endpoint->clusters[endpoint->cluster_count].attribute_count = attr_index;
        endpoint->clusters[endpoint->cluster_count].attributes      =
            cluster->color_attr_infos;
        endpoint->clusters[endpoint->cluster_count].is_server    = 1;
        endpoint->clusters[endpoint->cluster_count].cmd_callback =
            light_cluster_color_cmd_callback;
        endpoint->cluster_count++;
    }
}

void light_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                  uint16_t cluster_id,
                                                  uint16_t attribute_id) {
    zigbee_light_cluster *cluster = light_cluster_by_endpoint[endpoint];

    if (!cluster) {
        return;
    }

    if (cluster_id == ZCL_CLUSTER_ON_OFF &&
        attribute_id == ZCL_ATTR_ONOFF) {
        light_cluster_set_onoff(cluster, cluster->on_off);
        // Store state if using TOGGLE or PREVIOUS mode
        if (cluster->startup_onoff == ZCL_START_UP_ONOFF_SET_ONOFF_TOGGLE ||
            cluster->startup_onoff == ZCL_START_UP_ONOFF_SET_ONOFF_TO_PREVIOUS) {
            light_cluster_store_to_nv(cluster);
        }
        return;
    }

    if (cluster_id == ZCL_CLUSTER_ON_OFF &&
        attribute_id == ZCL_ATTR_START_UP_ONOFF) {
        // Startup mode changed - store to NVM
        light_cluster_store_to_nv(cluster);
        return;
    }

    if (cluster_id == ZCL_CLUSTER_LEVEL_CONTROL &&
        attribute_id == ZCL_ATTR_LEVEL_CURRENT_LEVEL) {
        light_cluster_set_level(cluster, cluster->level, true);
        // Store state if using PREVIOUS mode
        if (cluster->startup_onoff == ZCL_START_UP_ONOFF_SET_ONOFF_TO_PREVIOUS) {
            light_cluster_store_to_nv(cluster);
        }
        return;
    }

    if (cluster_id == ZCL_CLUSTER_COLOR_CONTROL) {
        if (attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE ||
            attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION) {
            light_cluster_set_color_hs(cluster, cluster->current_hue,
                                       cluster->current_saturation);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_X ||
                   attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_Y) {
            light_cluster_set_color_xy(cluster, cluster->current_x,
                                       cluster->current_y);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP) {
            light_cluster_set_color_temp(cluster, cluster->color_temp);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMP) {
            // Startup color temp changed - store to NVM
            light_cluster_store_to_nv(cluster);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_COLOR_MODE) {
            light_cluster_apply_output(cluster);
            light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                                 ZCL_ATTR_COLOR_CONTROL_COLOR_MODE);
        }
        // Store color state if using PREVIOUS mode
        if (cluster->startup_onoff == ZCL_START_UP_ONOFF_SET_ONOFF_TO_PREVIOUS) {
            light_cluster_store_to_nv(cluster);
        }
    }
}
