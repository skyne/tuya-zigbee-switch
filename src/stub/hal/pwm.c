#include "hal/pwm.h"
#include "hal/gpio.h"
#include <stdio.h>

// Simple stub implementation for testing
// In stub mode, PWM just sets GPIO high/low based on duty cycle

#define MAX_PWM_CHANNELS 10

typedef struct {
    hal_gpio_pin_t pin;
    uint32_t frequency;
    uint8_t duty;
    uint8_t active;
} pwm_channel_state_t;

static pwm_channel_state_t pwm_channels[MAX_PWM_CHANNELS];
static uint8_t pwm_initialized = 0;

void hal_pwm_init(void) {
    for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
        pwm_channels[i].active = 0;
        pwm_channels[i].pin = HAL_INVALID_PIN;
    }
    pwm_initialized = 1;
}

hal_pwm_channel_t hal_pwm_start(hal_gpio_pin_t pin, uint32_t frequency_hz,
                                 uint8_t duty_percent) {
    if (!pwm_initialized) {
        hal_pwm_init();
    }

    if (pin == HAL_INVALID_PIN) {
        return HAL_PWM_INVALID_CHANNEL;
    }

    // Find free channel
    for (uint8_t i = 0; i < MAX_PWM_CHANNELS; i++) {
        if (!pwm_channels[i].active) {
            pwm_channels[i].pin = pin;
            pwm_channels[i].frequency = frequency_hz;
            pwm_channels[i].duty = duty_percent;
            pwm_channels[i].active = 1;
            
            // In stub mode, just set GPIO based on duty cycle threshold
            hal_gpio_write(pin, duty_percent > 50 ? 1 : 0);
            
            return i;
        }
    }

    return HAL_PWM_INVALID_CHANNEL;
}

void hal_pwm_set_duty(hal_pwm_channel_t channel, uint8_t duty_percent) {
    if (channel >= MAX_PWM_CHANNELS || !pwm_channels[channel].active) {
        return;
    }

    pwm_channels[channel].duty = duty_percent;
    
    // In stub mode, just set GPIO based on duty cycle threshold
    hal_gpio_write(pwm_channels[channel].pin, duty_percent > 50 ? 1 : 0);
}

void hal_pwm_stop(hal_pwm_channel_t channel) {
    if (channel >= MAX_PWM_CHANNELS || !pwm_channels[channel].active) {
        return;
    }

    // Turn off the pin
    hal_gpio_write(pwm_channels[channel].pin, 0);
    pwm_channels[channel].active = 0;
    pwm_channels[channel].pin = HAL_INVALID_PIN;
}
