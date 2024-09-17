// Copyright (c) 2024 CannonKeys LLC
// SPDX-License-Identifier: MIT

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

// GPIO-based LED device and indices inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {
    DT_NODE_CHILD_IDX(DT_ALIAS(led_charge)),
    DT_NODE_CHILD_IDX(DT_ALIAS(led_blue))
};


extern void led_init_thread(void *d0, void *d1) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    led_on(led_dev, led_idx[0]);
    led_on(led_dev, led_idx[1]);
    k_sleep(K_MSEC(1000));
    led_off(led_dev, led_idx[1]);

}

// run init thread on boot
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);