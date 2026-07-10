#ifndef MK_PICLOCK_LED_CONTROL_H
#define MK_PICLOCK_LED_CONTROL_H

#include <stdint.h>

#include "ipc_protocol.h"

#define MP_LED_GPIO_RED 5
#define MP_LED_GPIO_GREEN 6
#define MP_LED_GPIO_BLUE 13
#define MP_LED_PWM_HZ 200
#define MP_LED_PWM_LEVELS 32

struct mp_led_status {
    enum mp_led_scene scene;
    uint8_t colour_red;
    uint8_t colour_green;
    uint8_t colour_blue;
    uint8_t output_red;
    uint8_t output_green;
    uint8_t output_blue;
    unsigned int write_errors;
    int ready;
};

int mp_led_init(void);
void mp_led_shutdown(void);
int mp_led_ready(void);
void mp_led_set_global(const struct mp_led_global_settings *settings);
void mp_led_set(enum mp_led_scene scene, const struct mp_led_profile *profile);
void mp_led_snapshot(struct mp_led_status *status);
const char *mp_led_scene_name(enum mp_led_scene scene);
const char *mp_led_effect_name(enum mp_led_effect effect);

#endif
