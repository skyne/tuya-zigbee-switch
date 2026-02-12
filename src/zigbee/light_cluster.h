#ifndef _LIGHT_CLUSTER_H_
#define _LIGHT_CLUSTER_H_

#include "hal/gpio.h"
#include "hal/zigbee.h"
#include <stdint.h>

typedef struct zigbee_light_cluster zigbee_light_cluster;

struct zigbee_light_cluster {
    uint8_t              endpoint;

    uint8_t              on_off;
    uint8_t              level;
    uint16_t             current_x;
    uint16_t             current_y;
    uint8_t              current_hue;
    uint8_t              current_saturation;
    uint16_t             color_temp;
    uint8_t              color_mode;
    uint8_t              startup_onoff;
    uint16_t             startup_color_temp;

    uint8_t              supports_level;
    uint8_t              supports_color_xy;
    uint8_t              supports_color_temp;

    hal_gpio_pin_t       pin_r;
    hal_gpio_pin_t       pin_g;
    hal_gpio_pin_t       pin_b;
    hal_gpio_pin_t       pin_c;
    hal_gpio_pin_t       pin_w;

    uint8_t              pin_r_on_high;
    uint8_t              pin_g_on_high;
    uint8_t              pin_b_on_high;
    uint8_t              pin_c_on_high;
    uint8_t              pin_w_on_high;

    hal_zigbee_attribute onoff_attr_infos[2];
    hal_zigbee_attribute level_attr_infos[1];
    hal_zigbee_attribute color_attr_infos[8];

    void                 (*output_callback)(zigbee_light_cluster *cluster);
};

void light_cluster_init(zigbee_light_cluster *cluster);
void light_cluster_set_output_callback(zigbee_light_cluster *cluster,
                                       void (*cb)(zigbee_light_cluster *cluster));
void light_cluster_add_to_endpoint(zigbee_light_cluster *cluster,
                                   hal_zigbee_endpoint *endpoint);

void light_cluster_callback_attr_write_trampoline(uint8_t endpoint,
                                                  uint16_t cluster_id,
                                                  uint16_t attribute_id);

void light_cluster_handle_startup_mode(zigbee_light_cluster *cluster);
void update_light_clusters();

#endif
