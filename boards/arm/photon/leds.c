// Copyright (c) 2023 Cem Aksoylar, 2024 CannonKeys LLC
// SPDX-License-Identifier: MIT

// Adapted from https://github.com/caksoylar/zmk-rgbled-widget

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

#define SHOW_LAYER_CHANGE                                                                          \
    (IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE)) &&                                        \
        (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_red)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_blue)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

// GPIO-based LED device and indices of red/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t rgb_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(led_red)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_blue))};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// color values as specified by an R + B bitfield
enum color_t {
    LED_BLACK,   // 0b00
    LED_RED,     // 0b01
    LED_BLUE,    // 0b10
    LED_MAGENTA,  // 0b11
};

// a blink work item as specified by the color and duration
struct blink_item {
    enum color_t color;
    uint16_t duration_ms;
    bool first_item;
    uint16_t sleep_ms;
};

// define message queue of blink work items, that will be processed by a
// separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

#if IS_ENABLED(CONFIG_ZMK_BLE)
static void output_blink(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_OUTPUT_BLINK_MS};

    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected, slow blink blue", profile_index);
        blink.color = LED_BLUE;
        blink.duration_ms = 5000;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking blue", profile_index);
        blink.color = LED_BLUE;
    } else {
        LOG_INF("Profile %d not connected, blinking red", profile_index);
        blink.color = LED_RED;
    }


    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        output_blink();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
// run led_output_listener_cb on BLE profile change (on central)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // check if we are in critical battery levels at state change, blink if we are
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        LOG_INF("Battery level %d, blinking red for critical", battery_level);

        struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                                   .color = LED_RED};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)


extern void led_process_thread(void *d0, void *d1) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);

    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        LOG_DBG("Got a blink item from msgq, color %d, duration %d", blink.color,
                blink.duration_ms);

        // turn appropriate LEDs on
        for (uint8_t pos = 0; pos < 2; pos++) {
            if (BIT(pos) & blink.color) {
                led_on(led_dev, rgb_idx[pos]);
            }
        }

        // wait for blink duration
        k_sleep(K_MSEC(blink.duration_ms));

        // turn appropriate LEDs off
        for (uint8_t pos = 0; pos < 2; pos++) {
            if (BIT(pos) & blink.color) {
                led_off(led_dev, rgb_idx[pos]);
            }
        }

        // wait interval before processing another blink
        if (blink.sleep_ms > 0) {
            k_sleep(K_MSEC(blink.sleep_ms));
        } else {
            k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_INTERVAL_MS));
        }
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after
// boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

extern void led_init_thread(void *d0, void *d1) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    // check and indicate battery level on thread start
    LOG_INF("Indicating initial battery status");

    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                               .first_item = true};
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking magenta");
        blink.color = LED_MAGENTA;
    } else if (battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW){
        LOG_INF("Battery level %d, blinking red", battery_level);
        blink.color = LED_RED;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);

    // wait until blink should be displayed for further checks
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    output_blink();
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);