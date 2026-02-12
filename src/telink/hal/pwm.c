#include "hal/pwm.h"
#include "hal/gpio.h"
#pragma pack(push, 1)
#include "tl_common.h"
#pragma pack(pop)

#include <string.h>

// PWM configuration
#define PWM_MAX_CHANNELS        6
#define PWM_DEFAULT_FREQ_HZ     1000  // 1kHz default

typedef struct {
    hal_gpio_pin_t pin;
    uint8_t pwm_id;
    uint16_t cycle_ticks;
    uint8_t in_use;
    uint8_t inverted; // 1 if pin is active-low
} pwm_channel_info_t;

static pwm_channel_info_t pwm_channels[PWM_MAX_CHANNELS];
static uint8_t pwm_initialized = 0;

// Map GPIO pin to PWM ID based on TLSR8258 PWM pin capabilities
static uint8_t pin_to_pwm_id(hal_gpio_pin_t pin) {
    // TLSR8258 PWM channel mapping (from TLSR8258 datasheet DS-TLSR8258-E18):
    // PWM0: PC1, PC2, PC4_N
    // PWM1: PC3, PD3_N, PC1_N
    // PWM2: PD4_N, PC4
    // PWM3: PD2
    // PWM4: PB1, PB4, PC0_N, PD1_N
    // PWM5: PB5
    
    uint32_t port = (pin >> 8) & 0x07;
    uint32_t pin_num = 0;
    for (int i = 0; i < 8; i++) {
        if (pin & (1 << i)) {
            pin_num = i;
            break;
        }
    }
    
    // Map based on port (A=0, B=1, C=2, D=3, E=4) and pin number
    if (port == 1) { // Port B
        if (pin_num == 1) return 4; // PB1 -> PWM4
        if (pin_num == 4) return 4; // PB4 -> PWM4
        if (pin_num == 5) return 5; // PB5 -> PWM5
    } else if (port == 2) { // Port C
        if (pin_num == 1) return 0; // PC1 -> PWM0
        if (pin_num == 2) return 0; // PC2 -> PWM0
        if (pin_num == 3) return 1; // PC3 -> PWM1
        if (pin_num == 4) return 2; // PC4 -> PWM2
    } else if (port == 3) { // Port D
        if (pin_num == 2) return 3; // PD2 -> PWM3
    }
    
    return 0xFF; // No PWM support for this pin
}

void hal_pwm_init(void) {
    if (pwm_initialized) {
        return;
    }
    
    memset(pwm_channels, 0, sizeof(pwm_channels));
    for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
        pwm_channels[i].pin = HAL_INVALID_PIN;
        pwm_channels[i].pwm_id = 0xFF;
    }
    
    drv_pwm_init();
    pwm_initialized = 1;
}

hal_pwm_channel_t hal_pwm_start(hal_gpio_pin_t pin, uint32_t frequency_hz,
                                 uint8_t duty_percent) {
    if (!pwm_initialized) {
        hal_pwm_init();
    }
    
    if (pin == HAL_INVALID_PIN || frequency_hz == 0) {
        return HAL_PWM_INVALID_CHANNEL;
    }
    
    // Find or allocate a channel
    hal_pwm_channel_t channel = HAL_PWM_INVALID_CHANNEL;
    for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (pwm_channels[i].pin == pin && pwm_channels[i].in_use) {
            channel = i;
            break;
        }
    }
    
    if (channel == HAL_PWM_INVALID_CHANNEL) {
        // Allocate new channel
        for (uint8_t i = 0; i < PWM_MAX_CHANNELS; i++) {
            if (!pwm_channels[i].in_use) {
                channel = i;
                break;
            }
        }
    }
    
    if (channel == HAL_PWM_INVALID_CHANNEL) {
        return HAL_PWM_INVALID_CHANNEL;
    }
    
    uint8_t pwm_id = pin_to_pwm_id(pin);
    if (pwm_id == 0xFF) {
        printf("PWM: pin %d has no PWM capability\r\n", pin);
        return HAL_PWM_INVALID_CHANNEL;
    }
    
    printf("PWM start: pin=%d pwm_id=%d freq=%d duty=%d\r\n", pin, pwm_id, frequency_hz, duty_percent);
    
    // Calculate PWM timing
    // cycle_ticks = CLOCK / freq
    // For TLSR8258 @ 24MHz: 1kHz = 24000 ticks
    uint32_t clock_hz = CLOCK_SYS_CLOCK_HZ;
    uint16_t cycle_ticks = (uint16_t)(clock_hz / frequency_hz);
    
    if (cycle_ticks < 100) {
        cycle_ticks = 100;  // Minimum for stable PWM
    }
    
    printf("PWM config: cycle_ticks=%d\r\n", cycle_ticks);
    
    // Configure GPIO as PWM
    // Map PWM ID to function
    GPIO_FuncTypeDef pwm_func;
    switch (pwm_id) {
    case 0: pwm_func = AS_PWM0; break;
    case 1: pwm_func = AS_PWM1; break;
    case 2: pwm_func = AS_PWM2; break;
    case 3: pwm_func = AS_PWM3; break;
    case 4: pwm_func = AS_PWM4; break;
    case 5: pwm_func = AS_PWM5; break;
    default: return HAL_PWM_INVALID_CHANNEL;
    }
    
    gpio_set_func((GPIO_PinTypeDef)pin, pwm_func);
    gpio_set_output_en((GPIO_PinTypeDef)pin, 1);
    gpio_set_input_en((GPIO_PinTypeDef)pin, 0);
    
    // Configure PWM channel
    uint16_t cmp_ticks = (uint16_t)((uint32_t)cycle_ticks * duty_percent / 100);
    printf("Calling drv_pwm_cfg(id=%d, cmp=%d, cycle=%d)\r\n", pwm_id, cmp_ticks, cycle_ticks);
    drv_pwm_cfg(pwm_id, cmp_ticks, cycle_ticks);
    
    // Start PWM
    printf("Calling drv_pwm_start(id=%d)\r\n", pwm_id);
    drv_pwm_start(pwm_id);
    
    // Store channel info
    pwm_channels[channel].pin = pin;
    pwm_channels[channel].pwm_id = pwm_id;
    pwm_channels[channel].cycle_ticks = cycle_ticks;
    pwm_channels[channel].in_use = 1;
    pwm_channels[channel].inverted = 0;
    
    return channel;
}

void hal_pwm_set_duty(hal_pwm_channel_t channel, uint8_t duty_percent) {
    if (channel >= PWM_MAX_CHANNELS || !pwm_channels[channel].in_use) {
        printf("hal_pwm_set_duty: invalid ch=%d\r\n", channel);
        return;
    }
    
    if (duty_percent > 100) {
        duty_percent = 100;
    }
    
    uint16_t cycle_ticks = pwm_channels[channel].cycle_ticks;
    uint16_t cmp_ticks = (uint16_t)((uint32_t)cycle_ticks * duty_percent / 100);
    
    if (pwm_channels[channel].inverted) {
        cmp_ticks = cycle_ticks - cmp_ticks;
    }
    
    uint8_t pwm_id = pwm_channels[channel].pwm_id;
    printf("hal_pwm_set_duty: ch=%d pwm_id=%d duty=%d cmp=%d cycle=%d\r\n", 
           channel, pwm_id, duty_percent, cmp_ticks, cycle_ticks);
    drv_pwm_cfg(pwm_channels[channel].pwm_id, cmp_ticks, cycle_ticks);
}

void hal_pwm_stop(hal_pwm_channel_t channel) {
    if (channel >= PWM_MAX_CHANNELS || !pwm_channels[channel].in_use) {
        return;
    }
    
    drv_pwm_stop(pwm_channels[channel].pwm_id);
    
    // Restore GPIO to normal output, set low
    gpio_set_func((GPIO_PinTypeDef)pwm_channels[channel].pin, AS_GPIO);
    gpio_write((GPIO_PinTypeDef)pwm_channels[channel].pin, 0);
    
    pwm_channels[channel].in_use = 0;
    pwm_channels[channel].pin = HAL_INVALID_PIN;
}
