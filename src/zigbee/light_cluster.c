#include "light_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "hal/printf_selector.h"
#include "hal/gpio.h"
#include "hal/zigbee.h"

#include <stdbool.h>
#include <string.h>

#define LIGHT_MAX_LEVEL    0xFE

static zigbee_light_cluster *light_cluster_by_endpoint[10];

static void light_cluster_set_pin(hal_gpio_pin_t pin, uint8_t on_high,
                                  uint8_t enabled) {
    if (pin == HAL_INVALID_PIN) {
        return;
    }
    uint8_t value = enabled ? (on_high ? 1 : 0) : (on_high ? 0 : 1);
    hal_gpio_write(pin, value);
}

static void light_cluster_gpio_output(zigbee_light_cluster *cluster) {
    if (!cluster) {
        return;
    }

    uint8_t active = cluster->on_off ? 1 : 0;
    if (cluster->supports_level) {
        active = active && cluster->level > 0 ? 1 : 0;
    }

    uint8_t use_rgb = 0;
    uint8_t use_ct  = 0;
    if (cluster->supports_color_xy &&
        cluster->color_mode == ZCL_COLOR_MODE_CURRENT_XY) {
        use_rgb = 1;
    } else if (cluster->supports_color_temp &&
               cluster->color_mode == ZCL_COLOR_MODE_COLOR_TEMPERATURE) {
        use_ct = 1;
    } else if (cluster->supports_color_xy && !cluster->supports_color_temp) {
        use_rgb = 1;
    } else if (cluster->supports_color_temp && !cluster->supports_color_xy) {
        use_ct = 1;
    }

    light_cluster_set_pin(cluster->pin_r, cluster->pin_r_on_high,
                          active && use_rgb);
    light_cluster_set_pin(cluster->pin_g, cluster->pin_g_on_high,
                          active && use_rgb);
    light_cluster_set_pin(cluster->pin_b, cluster->pin_b_on_high,
                          active && use_rgb);
    light_cluster_set_pin(cluster->pin_c, cluster->pin_c_on_high,
                          active && use_ct);
    light_cluster_set_pin(cluster->pin_w, cluster->pin_w_on_high,
                          active && use_ct);
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
