#include "light_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "hal/printf_selector.h"
#include "hal/gpio.h"
#include "hal/pwm.h"
#include "hal/zigbee.h"

#include <stdbool.h>
#include <string.h>

#define LIGHT_MAX_LEVEL    0xFE
#define PWM_FREQUENCY_HZ   1000  // 1kHz PWM for smooth dimming

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
        uint32_t range = warm_mired - cold_mired;
        uint32_t offset = color_temp_mired - cold_mired;
        *warm_percent = (uint8_t)((offset * 100) / range);
        *cold_percent = 100 - *warm_percent;
    }
}

// Convert CIE XY to RGB (simplified - proper conversion needs more complex math)
// This is a basic approximation
static void xy_to_rgb_percent(uint16_t x, uint16_t y,
                              uint8_t *r_percent,
                              uint8_t *g_percent,
                              uint8_t *b_percent) {
    // X and Y are in range 0-65535 (representing 0.0-1.0)
    // For now, use a simple mapping - proper XY to RGB conversion
    // requires color space transformations
    
    // Simple heuristic: 
    // Red corner: x=0.7, y=0.3  -> (45874, 19660)
    // Green corner: x=0.17, y=0.7 -> (11140, 45874)  
    // Blue corner: x=0.15, y=0.06 -> (9830, 3932)
    
    // For simplicity, use a basic interpretation:
    // High X, low Y -> Red
    // Low X, high Y -> Green  
    // Low X, low Y -> Blue
    
    if (x > 40000) {
        // Red-ish
        *r_percent = 100;
        *g_percent = (uint8_t)((uint32_t)y * 100 / 65535);
        *b_percent = 0;
    } else if (y > 40000) {
        // Green-ish
        *r_percent = (uint8_t)((uint32_t)x * 100 / 65535);
        *g_percent = 100;
        *b_percent = 0;
    } else {
        // Blue-ish or white
        *r_percent = (uint8_t)((uint32_t)x * 100 / 65535);
        *g_percent = (uint8_t)((uint32_t)y * 100 / 65535);
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
    if (pin == HAL_INVALID_PIN) {
        return;
    }
    
    if (channel == HAL_PWM_INVALID_CHANNEL) {
        // Fallback to simple on/off if PWM not available
        light_cluster_set_pin(pin, on_high, duty_percent > 50);
        return;
    }
    
    // Invert if active-low
    uint8_t actual_duty = on_high ? duty_percent : (100 - duty_percent);
    hal_pwm_set_duty(channel, actual_duty);
}

static void light_cluster_gpio_output(zigbee_light_cluster *cluster) {
    if (!cluster) {
        return;
    }

    uint8_t endpoint = cluster->endpoint;
    uint8_t active = cluster->on_off ? 1 : 0;
    uint8_t brightness_percent = 100;
    
    if (cluster->supports_level) {
        active = active && cluster->level > 0 ? 1 : 0;
        brightness_percent = level_to_percent(cluster->level);
    }

    // Determine mode based on color_mode attribute
    // XY mode = use RGB, color_temp mode = use whites
    uint8_t use_rgb = (cluster->color_mode == ZCL_COLOR_MODE_CURRENT_XY);
    uint8_t use_ct = (cluster->color_mode == ZCL_COLOR_MODE_COLOR_TEMPERATURE);

    printf("Light: active=%d rgb=%d ct=%d mode=%d\r\n", active, use_rgb, use_ct, cluster->color_mode);

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
        
        // Ensure RGB PWM is started
        if (pwm_channels_r[endpoint] == HAL_PWM_INVALID_CHANNEL && cluster->pin_r != HAL_INVALID_PIN) {
            pwm_channels_r[endpoint] = hal_pwm_start(cluster->pin_r, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_g[endpoint] == HAL_PWM_INVALID_CHANNEL && cluster->pin_g != HAL_INVALID_PIN) {
            pwm_channels_g[endpoint] = hal_pwm_start(cluster->pin_g, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_b[endpoint] == HAL_PWM_INVALID_CHANNEL && cluster->pin_b != HAL_INVALID_PIN) {
            pwm_channels_b[endpoint] = hal_pwm_start(cluster->pin_b, PWM_FREQUENCY_HZ, 0);
        }
        
        // RGB mode - use color XY
        uint8_t r_pct, g_pct, b_pct;
        xy_to_rgb_percent(cluster->current_x, cluster->current_y,
                          &r_pct, &g_pct, &b_pct);
        
        // Apply brightness
        r_pct = (uint8_t)((uint32_t)r_pct * brightness_percent / 100);
        g_pct = (uint8_t)((uint32_t)g_pct * brightness_percent / 100);
        b_pct = (uint8_t)((uint32_t)b_pct * brightness_percent / 100);
        
        light_cluster_set_pwm_duty(pwm_channels_r[endpoint], cluster->pin_r,
                                   cluster->pin_r_on_high, r_pct);
        light_cluster_set_pwm_duty(pwm_channels_g[endpoint], cluster->pin_g,
                                   cluster->pin_g_on_high, g_pct);
        light_cluster_set_pwm_duty(pwm_channels_b[endpoint], cluster->pin_b,
                                   cluster->pin_b_on_high, b_pct);
    } else if (use_ct && active) {
        // Color temp mode - stop RGB PWM, start white PWM
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
        if (pwm_channels_c[endpoint] == HAL_PWM_INVALID_CHANNEL && cluster->pin_c != HAL_INVALID_PIN) {
            pwm_channels_c[endpoint] = hal_pwm_start(cluster->pin_c, PWM_FREQUENCY_HZ, 0);
        }
        if (pwm_channels_w[endpoint] == HAL_PWM_INVALID_CHANNEL && cluster->pin_w != HAL_INVALID_PIN) {
            pwm_channels_w[endpoint] = hal_pwm_start(cluster->pin_w, PWM_FREQUENCY_HZ, 0);
        }
        
        // Color temperature mode - use warm/cold white
        uint8_t cold_pct, warm_pct;
        color_temp_to_percentages(cluster->color_temp, &cold_pct, &warm_pct);
        
        // Apply brightness
        cold_pct = (uint8_t)((uint32_t)cold_pct * brightness_percent / 100);
        warm_pct = (uint8_t)((uint32_t)warm_pct * brightness_percent / 100);
        
        light_cluster_set_pwm_duty(pwm_channels_c[endpoint], cluster->pin_c,
                                   cluster->pin_c_on_high, cold_pct);
        light_cluster_set_pwm_duty(pwm_channels_w[endpoint], cluster->pin_w,
                                   cluster->pin_w_on_high, warm_pct);
    } else {
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
    cluster->current_x = x;
    cluster->current_y = y;
    cluster->color_mode = ZCL_COLOR_MODE_CURRENT_XY;
    light_cluster_apply_output(cluster);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_X);
    light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                         ZCL_ATTR_COLOR_CONTROL_CURRENT_Y);
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
            uint8_t dir = payload[0];
            uint8_t target = dir == ZCL_LEVEL_MOVE_DOWN ? 0 : LIGHT_MAX_LEVEL;
            light_cluster_set_level(cluster, target,
                                    command_id ==
                                      ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF);
        }
        break;
    case ZCL_CMD_LEVEL_STEP:
    case ZCL_CMD_LEVEL_STEP_WITH_ON_OFF:
        if (payload) {
            uint8_t dir = payload[0];
            uint8_t step = payload[1];
            int level = cluster->level;
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
    cluster->level = LIGHT_MAX_LEVEL;
    cluster->color_temp = 370;
    cluster->color_mode = ZCL_COLOR_MODE_COLOR_TEMPERATURE;
    cluster->pin_r = HAL_INVALID_PIN;
    cluster->pin_g = HAL_INVALID_PIN;
    cluster->pin_b = HAL_INVALID_PIN;
    cluster->pin_c = HAL_INVALID_PIN;
    cluster->pin_w = HAL_INVALID_PIN;
    cluster->pin_r_on_high = 1;
    cluster->pin_g_on_high = 1;
    cluster->pin_b_on_high = 1;
    cluster->pin_c_on_high = 1;
    cluster->pin_w_on_high = 1;
    cluster->output_callback = light_cluster_gpio_output;
    
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

    if (!cluster->supports_level &&
        (cluster->supports_color_temp || cluster->supports_color_xy)) {
        cluster->supports_level = 1;
    }

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

    endpoint->clusters[endpoint->cluster_count].cluster_id      =
        ZCL_CLUSTER_ON_OFF;
    endpoint->clusters[endpoint->cluster_count].attribute_count = 1;
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
        endpoint->clusters[endpoint->cluster_count].attributes =
            cluster->level_attr_infos;
        endpoint->clusters[endpoint->cluster_count].is_server = 1;
        endpoint->clusters[endpoint->cluster_count].cmd_callback =
            light_cluster_level_cmd_callback;
        endpoint->cluster_count++;
    }

    if (cluster->supports_color_temp || cluster->supports_color_xy) {
        uint8_t attr_index = 0;
        if (cluster->supports_color_xy) {
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
        }
        SETUP_ATTR_FOR_TABLE(cluster->color_attr_infos, attr_index,
                             ZCL_ATTR_COLOR_CONTROL_COLOR_MODE,
                             ZCL_DATA_TYPE_ENUM8, ATTR_WRITABLE,
                             cluster->color_mode);
        attr_index++;

        endpoint->clusters[endpoint->cluster_count].cluster_id =
            ZCL_CLUSTER_COLOR_CONTROL;
        endpoint->clusters[endpoint->cluster_count].attribute_count = attr_index;
        endpoint->clusters[endpoint->cluster_count].attributes =
            cluster->color_attr_infos;
        endpoint->clusters[endpoint->cluster_count].is_server = 1;
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
        return;
    }

    if (cluster_id == ZCL_CLUSTER_LEVEL_CONTROL &&
        attribute_id == ZCL_ATTR_LEVEL_CURRENT_LEVEL) {
        light_cluster_set_level(cluster, cluster->level, true);
        return;
    }

    if (cluster_id == ZCL_CLUSTER_COLOR_CONTROL) {
        if (attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_X ||
            attribute_id == ZCL_ATTR_COLOR_CONTROL_CURRENT_Y) {
            light_cluster_set_color_xy(cluster, cluster->current_x,
                                       cluster->current_y);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP) {
            light_cluster_set_color_temp(cluster, cluster->color_temp);
        } else if (attribute_id == ZCL_ATTR_COLOR_CONTROL_COLOR_MODE) {
            light_cluster_apply_output(cluster);
            light_cluster_notify(cluster, ZCL_CLUSTER_COLOR_CONTROL,
                                 ZCL_ATTR_COLOR_CONTROL_COLOR_MODE);
        }
    }
}
