#ifndef _HAL_PWM_H_
#define _HAL_PWM_H_

#include "gpio.h"
#include <stdint.h>

/**
 * @brief PWM channel handle type
 */
typedef uint8_t hal_pwm_channel_t;

#define HAL_PWM_INVALID_CHANNEL    0xFF

/**
 * @brief Initialize PWM subsystem
 * Must be called once before using any PWM functions
 */
void hal_pwm_init(void);

/**
 * @brief Configure and start a PWM channel on a GPIO pin
 * 
 * @param pin GPIO pin to use for PWM output
 * @param frequency_hz PWM frequency in Hz (typical: 1000-10000)
 * @param duty_percent Initial duty cycle (0-100)
 * @return PWM channel handle, or HAL_PWM_INVALID_CHANNEL on error
 */
hal_pwm_channel_t hal_pwm_start(hal_gpio_pin_t pin, uint32_t frequency_hz,
                                 uint8_t duty_percent);

/**
 * @brief Set PWM duty cycle
 * 
 * @param channel PWM channel handle returned by hal_pwm_start
 * @param duty_percent Duty cycle (0-100)
 */
void hal_pwm_set_duty(hal_pwm_channel_t channel, uint8_t duty_percent);

/**
 * @brief Stop PWM output on a channel
 * 
 * @param channel PWM channel handle
 */
void hal_pwm_stop(hal_pwm_channel_t channel);

#endif /* _HAL_PWM_H_ */
