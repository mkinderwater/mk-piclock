#define _GNU_SOURCE

#include "led_control.h"

#include <errno.h>
#include <gpiod.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LED_GPIO_CHIP "/dev/gpiochip0"
#define LED_PWM_PERIOD_NS (1000000000LL / MP_LED_PWM_HZ)
#define LED_TRANSITION_MS 350ULL
#define LED_WRITE_FAILURE_LIMIT 5u
#define LED_RT_PRIORITY 10

struct led_runtime {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    struct gpiod_chip *chip;
    struct gpiod_line_request *request;
    struct mp_led_profile profile;
    struct mp_led_global_settings settings;
    enum mp_led_scene scene;
    uint8_t colour_red;
    uint8_t colour_green;
    uint8_t colour_blue;
    uint8_t output_red;
    uint8_t output_green;
    uint8_t output_blue;
    uint8_t transition_colour_red;
    uint8_t transition_colour_green;
    uint8_t transition_colour_blue;
    uint8_t transition_output_red;
    uint8_t transition_output_green;
    uint8_t transition_output_blue;
    unsigned int write_errors;
    unsigned int consecutive_write_errors;
    uint64_t generation;
    uint64_t effect_started_ms;
    uint64_t transition_started_ms;
    int transition_active;
    int ready;
    int running;
    int thread_started;
};

static struct led_runtime g_led = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .chip = NULL,
    .request = NULL,
    .profile = {0},
    .settings = {1, 100, 100, 65, 80, 0, 15, 1, 60, 255, 255, 255},
    .scene = MP_LED_SCENE_DAYTIME,
    .ready = 0,
    .running = 0,
    .thread_started = 0
};

static uint64_t monotonic_millis_led(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static struct timespec timespec_add_ns(struct timespec base, int64_t ns) {
    base.tv_sec += (time_t)(ns / 1000000000LL);
    base.tv_nsec += (long)(ns % 1000000000LL);
    if (base.tv_nsec >= 1000000000L) {
        base.tv_sec++;
        base.tv_nsec -= 1000000000L;
    }
    return base;
}

static void sleep_until(struct timespec when) {
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &when, NULL) == EINTR) {}
}

static int gpio_set_values(int red, int green, int blue) {
    if (!g_led.request) return -1;
    const enum gpiod_line_value values[3] = {
        red ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        green ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE,
        blue ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE
    };
    int rc = gpiod_line_request_set_values(g_led.request, values);
    int disabled_now = 0;
    pthread_mutex_lock(&g_led.lock);
    if (rc == 0) {
        g_led.consecutive_write_errors = 0;
    } else {
        g_led.write_errors++;
        g_led.consecutive_write_errors++;
        if (g_led.consecutive_write_errors == LED_WRITE_FAILURE_LIMIT) {
            g_led.ready = 0;
            g_led.running = 0;
            disabled_now = 1;
            pthread_cond_broadcast(&g_led.changed);
        }
    }
    pthread_mutex_unlock(&g_led.lock);
    if (disabled_now) fprintf(stderr, "RGB LED disabled after repeated GPIO write failures\n");
    return rc;
}

static void all_off(void) {
    (void)gpio_set_values(0, 0, 0);
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint64_t amount, uint64_t scale) {
    if (scale == 0 || amount == 0) return a;
    if (amount >= scale) return b;
    uint64_t left = (uint64_t)a * (scale - amount);
    uint64_t right = (uint64_t)b * amount;
    return (uint8_t)((left + right + scale / 2ULL) / scale);
}

static void rainbow_wheel(uint8_t pos, uint8_t *red, uint8_t *green, uint8_t *blue) {
    if (pos < 85u) {
        *red = (uint8_t)(255u - pos * 3u);
        *green = (uint8_t)(pos * 3u);
        *blue = 0;
    } else if (pos < 170u) {
        pos = (uint8_t)(pos - 85u);
        *red = 0;
        *green = (uint8_t)(255u - pos * 3u);
        *blue = (uint8_t)(pos * 3u);
    } else {
        pos = (uint8_t)(pos - 170u);
        *red = (uint8_t)(pos * 3u);
        *green = 0;
        *blue = (uint8_t)(255u - pos * 3u);
    }
}

static uint8_t gamma_u8(uint8_t value) {
    return (uint8_t)(((unsigned int)value * value + 127u) / 255u);
}

static uint8_t scale_output(uint8_t linear, uint8_t brightness, uint8_t maximum,
                            uint8_t gain, int enabled) {
    if (!enabled || linear == 0 || brightness == 0 || maximum == 0 || gain == 0) return 0;
    unsigned int effective = brightness < maximum ? brightness : maximum;
    unsigned int output = (unsigned int)linear * effective;
    output = (output + 50u) / 100u;
    output = (output * gain + 50u) / 100u;
    return (uint8_t)(output > 255u ? 255u : output);
}

static uint64_t effect_cycle_ms(const struct mp_led_profile *profile) {
    unsigned int seconds = profile->cycle_seconds;
    if (seconds < 2u) seconds = profile->effect == MP_LED_EFFECT_RAINBOW ? 12u : 8u;
    if (seconds > 60u) seconds = 60u;
    return (uint64_t)seconds * 1000ULL;
}

static void profile_rgb(const struct mp_led_profile *profile,
                        const struct mp_led_global_settings *settings,
                        uint64_t effect_ms, uint8_t *colour_red, uint8_t *colour_green,
                        uint8_t *colour_blue, uint8_t *output_red, uint8_t *output_green,
                        uint8_t *output_blue) {
    uint8_t raw_r = profile->red1;
    uint8_t raw_g = profile->green1;
    uint8_t raw_b = profile->blue1;
    uint8_t linear_r = gamma_u8(raw_r);
    uint8_t linear_g = gamma_u8(raw_g);
    uint8_t linear_b = gamma_u8(raw_b);
    uint64_t cycle_ms = effect_cycle_ms(profile);

    switch ((enum mp_led_effect)profile->effect) {
        case MP_LED_EFFECT_FADE: {
            uint64_t phase = effect_ms % cycle_ms;
            uint64_t half = cycle_ms / 2ULL;
            uint64_t amount = phase <= half ? phase : cycle_ms - phase;
            raw_r = lerp_u8(profile->red1, profile->red2, amount, half);
            raw_g = lerp_u8(profile->green1, profile->green2, amount, half);
            raw_b = lerp_u8(profile->blue1, profile->blue2, amount, half);
            linear_r = lerp_u8(gamma_u8(profile->red1), gamma_u8(profile->red2), amount, half);
            linear_g = lerp_u8(gamma_u8(profile->green1), gamma_u8(profile->green2), amount, half);
            linear_b = lerp_u8(gamma_u8(profile->blue1), gamma_u8(profile->blue2), amount, half);
            break;
        }
        case MP_LED_EFFECT_RAINBOW: {
            uint8_t wheel = (uint8_t)(((effect_ms % cycle_ms) * 255ULL) / cycle_ms);
            rainbow_wheel(wheel, &raw_r, &raw_g, &raw_b);
            linear_r = gamma_u8(raw_r);
            linear_g = gamma_u8(raw_g);
            linear_b = gamma_u8(raw_b);
            break;
        }
        case MP_LED_EFFECT_SOLID:
        default:
            break;
    }

    *colour_red = raw_r;
    *colour_green = raw_g;
    *colour_blue = raw_b;
    *output_red = scale_output(linear_r, profile->brightness, settings->max_brightness,
                               settings->red_gain, settings->enabled);
    *output_green = scale_output(linear_g, profile->brightness, settings->max_brightness,
                                 settings->green_gain, settings->enabled);
    *output_blue = scale_output(linear_b, profile->brightness, settings->max_brightness,
                                settings->blue_gain, settings->enabled);
}

static int profile_stays_dark(const struct mp_led_profile *profile,
                              const struct mp_led_global_settings *settings) {
    if (!settings->enabled || profile->brightness == 0 || settings->max_brightness == 0)
        return 1;
    if (profile->effect == MP_LED_EFFECT_RAINBOW)
        return settings->red_gain == 0 && settings->green_gain == 0 && settings->blue_gain == 0;

    uint8_t values[6] = {
        profile->red1, profile->green1, profile->blue1,
        profile->red2, profile->green2, profile->blue2
    };
    uint8_t gains[6] = {
        settings->red_gain, settings->green_gain, settings->blue_gain,
        settings->red_gain, settings->green_gain, settings->blue_gain
    };
    int count = profile->effect == MP_LED_EFFECT_FADE ? 6 : 3;
    for (int i = 0; i < count; i++) {
        if (scale_output(gamma_u8(values[i]), profile->brightness,
                         settings->max_brightness, gains[i], settings->enabled) > 0)
            return 0;
    }
    return 1;
}

struct pwm_edge {
    uint8_t channel;
    uint8_t start_level;
};

static void sort_edges(struct pwm_edge *edges, int count) {
    for (int i = 1; i < count; i++) {
        struct pwm_edge current = edges[i];
        int j = i - 1;
        while (j >= 0 && edges[j].start_level > current.start_level) {
            edges[j + 1] = edges[j];
            j--;
        }
        edges[j + 1] = current;
    }
}

/*
 * Keep each non-zero output at one stable PWM level. The prior temporal
 * dithering changed levels across periods and could become visible at the
 * very low bedroom brightness settings.
 */
static uint8_t quantize_level(uint8_t duty) {
    if (duty == 0u) return 0u;
    unsigned int level = ((unsigned int)duty * MP_LED_PWM_LEVELS + 127u) / 255u;
    if (level == 0u) level = 1u;
    if (level > MP_LED_PWM_LEVELS) level = MP_LED_PWM_LEVELS;
    return (uint8_t)level;
}

static int timespec_at_or_after(struct timespec a, struct timespec b) {
    return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec >= b.tv_nsec);
}

/*
 * Use trailing-edge PWM. Partial-duty channels remain off during the long
 * portion of each period and turn on only near the end. If Linux wakes the
 * thread too late, the pulse is skipped rather than started late at full
 * brightness. Full-duty channels remain continuously on.
 */
static int pwm_period(uint8_t red, uint8_t green, uint8_t blue,
                      struct timespec period_start) {
    const uint8_t duties[3] = {red, green, blue};
    uint8_t levels[3];
    struct pwm_edge edges[3];
    int edge_count = 0;
    int active[3] = {0, 0, 0};

    for (int i = 0; i < 3; i++) {
        levels[i] = quantize_level(duties[i]);
        if (levels[i] == MP_LED_PWM_LEVELS) {
            active[i] = 1;
        } else if (levels[i] > 0u) {
            edges[edge_count].channel = (uint8_t)i;
            edges[edge_count].start_level = (uint8_t)(MP_LED_PWM_LEVELS - levels[i]);
            edge_count++;
        }
    }

    if (gpio_set_values(active[0], active[1], active[2]) != 0) return -1;

    sort_edges(edges, edge_count);
    struct timespec period_end = timespec_add_ns(period_start, LED_PWM_PERIOD_NS);
    for (int i = 0; i < edge_count;) {
        uint8_t start_level = edges[i].start_level;
        int64_t offset_ns = (LED_PWM_PERIOD_NS * start_level) / MP_LED_PWM_LEVELS;
        sleep_until(timespec_add_ns(period_start, offset_ns));

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_at_or_after(now, period_end)) {
            /* Missed this pulse. Stay dark instead of producing a late flash. */
            return gpio_set_values(levels[0] == MP_LED_PWM_LEVELS,
                                   levels[1] == MP_LED_PWM_LEVELS,
                                   levels[2] == MP_LED_PWM_LEVELS);
        }

        while (i < edge_count && edges[i].start_level == start_level) {
            active[edges[i].channel] = 1;
            i++;
        }
        if (gpio_set_values(active[0], active[1], active[2]) != 0) return -1;
    }

    sleep_until(period_end);
    return gpio_set_values(levels[0] == MP_LED_PWM_LEVELS,
                           levels[1] == MP_LED_PWM_LEVELS,
                           levels[2] == MP_LED_PWM_LEVELS);
}

static void configure_led_thread(void) {
#ifdef __linux__
    (void)pthread_setname_np(pthread_self(), "mk-led-pwm");
#endif
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = LED_RT_PRIORITY;
    int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if (rc != 0) {
        fprintf(stderr,
                "RGB LED real-time scheduling unavailable: %s; continuing with normal scheduling\n",
                strerror(rc));
    }
}

static void *led_thread_main(void *arg) {
    (void)arg;
    configure_led_thread();
    struct timespec period_start;
    clock_gettime(CLOCK_MONOTONIC, &period_start);

    for (;;) {
        struct mp_led_profile profile;
        struct mp_led_global_settings settings;
        enum mp_led_scene scene;
        uint64_t effect_started_ms;
        uint64_t transition_started_ms;
        uint64_t generation;
        uint8_t from_cr, from_cg, from_cb, from_or, from_og, from_ob;
        int transition_active;
        int running;

        pthread_mutex_lock(&g_led.lock);
        running = g_led.running;
        profile = g_led.profile;
        settings = g_led.settings;
        scene = g_led.scene;
        effect_started_ms = g_led.effect_started_ms;
        transition_started_ms = g_led.transition_started_ms;
        generation = g_led.generation;
        transition_active = g_led.transition_active;
        from_cr = g_led.transition_colour_red;
        from_cg = g_led.transition_colour_green;
        from_cb = g_led.transition_colour_blue;
        from_or = g_led.transition_output_red;
        from_og = g_led.transition_output_green;
        from_ob = g_led.transition_output_blue;
        pthread_mutex_unlock(&g_led.lock);
        if (!running) break;

        uint64_t now_ms = monotonic_millis_led();
        uint64_t effect_ms = now_ms >= effect_started_ms ? now_ms - effect_started_ms : 0;
        uint8_t cr, cg, cb, red, green, blue;
        profile_rgb(&profile, &settings, effect_ms, &cr, &cg, &cb, &red, &green, &blue);

        if (transition_active && scene != MP_LED_SCENE_ALARM) {
            uint64_t elapsed = now_ms >= transition_started_ms ? now_ms - transition_started_ms : 0;
            if (elapsed < LED_TRANSITION_MS) {
                cr = lerp_u8(from_cr, cr, elapsed, LED_TRANSITION_MS);
                cg = lerp_u8(from_cg, cg, elapsed, LED_TRANSITION_MS);
                cb = lerp_u8(from_cb, cb, elapsed, LED_TRANSITION_MS);
                red = lerp_u8(from_or, red, elapsed, LED_TRANSITION_MS);
                green = lerp_u8(from_og, green, elapsed, LED_TRANSITION_MS);
                blue = lerp_u8(from_ob, blue, elapsed, LED_TRANSITION_MS);
            } else {
                pthread_mutex_lock(&g_led.lock);
                if (g_led.transition_started_ms == transition_started_ms)
                    g_led.transition_active = 0;
                pthread_mutex_unlock(&g_led.lock);
                transition_active = 0;
            }
        }

        pthread_mutex_lock(&g_led.lock);
        g_led.colour_red = cr;
        g_led.colour_green = cg;
        g_led.colour_blue = cb;
        g_led.output_red = red;
        g_led.output_green = green;
        g_led.output_blue = blue;
        running = g_led.running;
        pthread_mutex_unlock(&g_led.lock);
        if (!running) break;

        if (red == 0 && green == 0 && blue == 0 && !transition_active &&
            profile_stays_dark(&profile, &settings)) {
            all_off();
            pthread_mutex_lock(&g_led.lock);
            while (g_led.running && g_led.generation == generation)
                pthread_cond_wait(&g_led.changed, &g_led.lock);
            running = g_led.running;
            pthread_mutex_unlock(&g_led.lock);
            if (!running) break;
            clock_gettime(CLOCK_MONOTONIC, &period_start);
            continue;
        }

        if (pwm_period(red, green, blue, period_start) != 0 && !mp_led_ready()) break;

        period_start = timespec_add_ns(period_start, LED_PWM_PERIOD_NS);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > period_start.tv_sec ||
            (now.tv_sec == period_start.tv_sec && now.tv_nsec > period_start.tv_nsec))
            period_start = now;
    }

    if (g_led.request) all_off();
    pthread_mutex_lock(&g_led.lock);
    g_led.output_red = g_led.output_green = g_led.output_blue = 0;
    g_led.running = 0;
    pthread_mutex_unlock(&g_led.lock);
    return NULL;
}

int mp_led_init(void) {
    unsigned int offsets[3] = {MP_LED_GPIO_RED, MP_LED_GPIO_GREEN, MP_LED_GPIO_BLUE};
    struct gpiod_line_settings *line_settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_request_config *req_cfg = NULL;
    int rc = -1;

    g_led.chip = gpiod_chip_open(LED_GPIO_CHIP);
    if (!g_led.chip) {
        perror("LED gpiod_chip_open");
        return -1;
    }

    line_settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!line_settings || !line_cfg || !req_cfg) goto done;

    gpiod_line_settings_set_direction(line_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(line_settings, GPIOD_LINE_VALUE_INACTIVE);
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 3, line_settings) != 0) goto done;
    gpiod_request_config_set_consumer(req_cfg, "mk-piclock-led");
    g_led.request = gpiod_chip_request_lines(g_led.chip, req_cfg, line_cfg);
    if (!g_led.request) {
        perror("LED gpiod_chip_request_lines");
        goto done;
    }

    pthread_mutex_lock(&g_led.lock);
    g_led.running = 1;
    g_led.ready = 1;
    g_led.write_errors = 0;
    g_led.consecutive_write_errors = 0;
    pthread_mutex_unlock(&g_led.lock);
    if (pthread_create(&g_led.thread, NULL, led_thread_main, NULL) != 0) {
        pthread_mutex_lock(&g_led.lock);
        g_led.running = 0;
        g_led.ready = 0;
        pthread_mutex_unlock(&g_led.lock);
        goto done;
    }
    g_led.thread_started = 1;
    rc = 0;

done:
    if (line_settings) gpiod_line_settings_free(line_settings);
    if (line_cfg) gpiod_line_config_free(line_cfg);
    if (req_cfg) gpiod_request_config_free(req_cfg);
    if (rc != 0) {
        if (g_led.request) gpiod_line_request_release(g_led.request);
        g_led.request = NULL;
        if (g_led.chip) gpiod_chip_close(g_led.chip);
        g_led.chip = NULL;
    }
    return rc;
}

void mp_led_shutdown(void) {
    pthread_mutex_lock(&g_led.lock);
    g_led.running = 0;
    pthread_cond_broadcast(&g_led.changed);
    pthread_mutex_unlock(&g_led.lock);
    if (g_led.thread_started) pthread_join(g_led.thread, NULL);
    g_led.thread_started = 0;
    if (g_led.request) (void)gpio_set_values(0, 0, 0);
    if (g_led.request) gpiod_line_request_release(g_led.request);
    g_led.request = NULL;
    if (g_led.chip) gpiod_chip_close(g_led.chip);
    g_led.chip = NULL;
    pthread_mutex_lock(&g_led.lock);
    g_led.ready = 0;
    pthread_mutex_unlock(&g_led.lock);
}

int mp_led_ready(void) {
    pthread_mutex_lock(&g_led.lock);
    int ready = g_led.ready;
    pthread_mutex_unlock(&g_led.lock);
    return ready;
}

void mp_led_set_global(const struct mp_led_global_settings *settings) {
    if (!settings) return;
    pthread_mutex_lock(&g_led.lock);
    if (memcmp(&g_led.settings, settings, sizeof(*settings)) != 0) {
        g_led.transition_colour_red = g_led.colour_red;
        g_led.transition_colour_green = g_led.colour_green;
        g_led.transition_colour_blue = g_led.colour_blue;
        g_led.transition_output_red = g_led.output_red;
        g_led.transition_output_green = g_led.output_green;
        g_led.transition_output_blue = g_led.output_blue;
        g_led.transition_started_ms = monotonic_millis_led();
        g_led.transition_active = settings->enabled ? 1 : 0;
        g_led.settings = *settings;
        g_led.generation++;
        pthread_cond_broadcast(&g_led.changed);
    }
    pthread_mutex_unlock(&g_led.lock);
}

void mp_led_set(enum mp_led_scene scene, const struct mp_led_profile *profile) {
    if (!profile || scene < 0 || scene >= MP_LED_SCENE_COUNT) return;
    pthread_mutex_lock(&g_led.lock);
    if (g_led.scene != scene || memcmp(&g_led.profile, profile, sizeof(*profile)) != 0) {
        uint64_t now = monotonic_millis_led();
        g_led.transition_colour_red = g_led.colour_red;
        g_led.transition_colour_green = g_led.colour_green;
        g_led.transition_colour_blue = g_led.colour_blue;
        g_led.transition_output_red = g_led.output_red;
        g_led.transition_output_green = g_led.output_green;
        g_led.transition_output_blue = g_led.output_blue;
        g_led.transition_started_ms = now;
        g_led.transition_active = scene == MP_LED_SCENE_ALARM ? 0 : 1;
        g_led.effect_started_ms = now;
        g_led.scene = scene;
        g_led.profile = *profile;
        g_led.generation++;
        pthread_cond_broadcast(&g_led.changed);
    }
    pthread_mutex_unlock(&g_led.lock);
}

void mp_led_snapshot(struct mp_led_status *status) {
    if (!status) return;
    pthread_mutex_lock(&g_led.lock);
    status->scene = g_led.scene;
    status->colour_red = g_led.colour_red;
    status->colour_green = g_led.colour_green;
    status->colour_blue = g_led.colour_blue;
    status->output_red = g_led.output_red;
    status->output_green = g_led.output_green;
    status->output_blue = g_led.output_blue;
    status->write_errors = g_led.write_errors;
    status->ready = g_led.ready;
    pthread_mutex_unlock(&g_led.lock);
}

const char *mp_led_scene_name(enum mp_led_scene scene) {
    switch (scene) {
        case MP_LED_SCENE_ALARM: return "alarm";
        case MP_LED_SCENE_BEDTIME: return "bedtime";
        case MP_LED_SCENE_MESSAGE: return "message";
        case MP_LED_SCENE_MUSIC: return "music";
        case MP_LED_SCENE_DAYTIME: return "daytime";
        case MP_LED_SCENE_STORIES: return "stories";
        case MP_LED_SCENE_TOUCH: return "touch";
        default: return "daytime";
    }
}

const char *mp_led_effect_name(enum mp_led_effect effect) {
    switch (effect) {
        case MP_LED_EFFECT_FADE: return "fade";
        case MP_LED_EFFECT_RAINBOW: return "rainbow";
        case MP_LED_EFFECT_SOLID:
        default: return "solid";
    }
}
