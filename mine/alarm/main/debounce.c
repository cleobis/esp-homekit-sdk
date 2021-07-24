/* Derrived from iot_button.c from Espressif esp-homekit-sdk. Original license:
 *
 * ESPRSSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include "iot_debounce.h"

#define IOT_CHECK(tag, a, ret)  if(!(a)) {                                             \
        ESP_LOGE(tag,"%s:%d (%s)", __FILE__, __LINE__, __FUNCTION__);      \
        return (ret);                                                                   \
        }
#define ERR_ASSERT(tag, param)  IOT_CHECK(tag, (param) == ESP_OK, ESP_FAIL)
#define POINT_ASSERT(tag, param, ret)    IOT_CHECK(tag, (param) != NULL, (ret))

typedef enum {
    DEBOUNCE_STATE_IDLE = 0,
    DEBOUNCE_STATE_PUSH,
    DEBOUNCE_STATE_INIT,
} debounce_status_t;

typedef struct debounce_dev debounce_dev_t;
typedef struct btn_cb debounce_cb_t;

struct btn_cb{
    TickType_t interval;
    debounce_cb cb;
    void* arg;
    uint8_t on_press;
    TimerHandle_t tmr;
    debounce_dev_t *pbtn;
    debounce_cb_t *next_cb;
};

struct debounce_dev{
    uint8_t io_num;
    uint8_t active_level;
    uint32_t serial_thres_sec;
    debounce_status_t state;
    debounce_cb_t debounce_cb;
    debounce_cb_t tap_psh_cb;
    debounce_cb_t tap_rls_cb;
};

#define DEBOUNCE_GLITCH_FILTER_TIME_MS   50
static const char* TAG = "debounce";

static void debounce_debounce_cb(xTimerHandle tmr)
{
    debounce_cb_t* btn_cb = (debounce_cb_t*) pvTimerGetTimerID(tmr);
    debounce_dev_t* btn = btn_cb->pbtn;
    if (btn->active_level == gpio_get_level(btn->io_num)) {
        // True implies key is pressed
        if (btn->state != DEBOUNCE_STATE_PUSH) {
            btn->state = DEBOUNCE_STATE_PUSH;
            if (btn->tap_psh_cb.cb) {
                btn->tap_psh_cb.cb(btn->tap_psh_cb.arg);
            }
        }
    } else {
        if (btn->state != DEBOUNCE_STATE_IDLE) {
            btn->state = DEBOUNCE_STATE_IDLE;
            if (btn->tap_rls_cb.cb) {
                btn->tap_rls_cb.cb(btn->tap_rls_cb.arg);
            }
        }
    }
}

static void debounce_gpio_isr_handler(void* arg)
{
    debounce_dev_t* btn = (debounce_dev_t*) arg;
    portBASE_TYPE HPTaskAwoken = pdFALSE;
    if (btn->debounce_cb.tmr) {
        xTimerStopFromISR(btn->debounce_cb.tmr, &HPTaskAwoken);
        xTimerResetFromISR(btn->debounce_cb.tmr, &HPTaskAwoken);
    }
    if(HPTaskAwoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void debounce_free_tmr(xTimerHandle* tmr)
{
    if (tmr && *tmr) {
        xTimerStop(*tmr, portMAX_DELAY);
        xTimerDelete(*tmr, portMAX_DELAY);
        *tmr = NULL;
    }
}

esp_err_t iot_debounce_delete(debounce_handle_t btn_handle)
{
    POINT_ASSERT(TAG, btn_handle, ESP_ERR_INVALID_ARG);
    debounce_dev_t* btn = (debounce_dev_t*) btn_handle;
    gpio_set_intr_type(btn->io_num, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(btn->io_num);

    debounce_free_tmr(&btn->debounce_cb.tmr);

    free(btn);
    return ESP_OK;
}

debounce_handle_t iot_debounce_create(gpio_num_t gpio_num, debounce_active_t active_level)
{
    IOT_CHECK(TAG, gpio_num < GPIO_NUM_MAX, NULL);
    debounce_dev_t* btn = (debounce_dev_t*) calloc(1, sizeof(debounce_dev_t));
    POINT_ASSERT(TAG, btn, NULL);
    btn->active_level = active_level;
    btn->io_num = gpio_num;
    btn->state = DEBOUNCE_STATE_INIT;
    btn->debounce_cb.interval = DEBOUNCE_GLITCH_FILTER_TIME_MS / portTICK_PERIOD_MS;
    btn->debounce_cb.pbtn = btn;
    btn->debounce_cb.tmr = xTimerCreate("btn_debounce_tmr", btn->debounce_cb.interval, pdFALSE,
            &btn->debounce_cb, debounce_debounce_cb);
    btn->tap_psh_cb.arg = NULL;
    btn->tap_psh_cb.cb = NULL;
    btn->tap_rls_cb.arg = NULL;
    btn->tap_rls_cb.cb = NULL;
    gpio_config_t gpio_conf;
    gpio_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (1 << gpio_num);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&gpio_conf);
    return (debounce_handle_t) btn;
}

void iot_debounce_init(debounce_handle_t btn_handle) {
    debounce_dev_t* btn = (debounce_dev_t*)btn_handle;
    if (btn->state != DEBOUNCE_STATE_INIT) {
        // already initialized
        return;
    }
    gpio_install_isr_service(0);
    gpio_isr_handler_add(btn->io_num, debounce_gpio_isr_handler, btn);
    debounce_gpio_isr_handler(btn);
}

esp_err_t iot_debounce_rm_cb(debounce_handle_t btn_handle, debounce_cb_type_t type)
{
    debounce_dev_t* btn = (debounce_dev_t*) btn_handle;
    debounce_cb_t* btn_cb = NULL;
    if (type == DEBOUNCE_CB_PUSH) {
        btn_cb = &btn->tap_psh_cb;
    } else if (type == DEBOUNCE_CB_RELEASE) {
        btn_cb = &btn->tap_rls_cb;
    }
    btn_cb->cb = NULL;
    btn_cb->arg = NULL;
    btn_cb->pbtn = btn;
    debounce_free_tmr(&btn_cb->tmr);
    return ESP_OK;
}

esp_err_t iot_debounce_set_evt_cb(debounce_handle_t btn_handle, debounce_cb_type_t type, debounce_cb cb, void* arg)
{
    POINT_ASSERT(TAG, btn_handle, ESP_ERR_INVALID_ARG);
    debounce_dev_t* btn = (debounce_dev_t*) btn_handle;
    if (type == DEBOUNCE_CB_PUSH) {
        btn->tap_psh_cb.arg = arg;
        btn->tap_psh_cb.cb = cb;
    } else if (type == DEBOUNCE_CB_RELEASE) {
        btn->tap_rls_cb.arg = arg;
        btn->tap_rls_cb.cb = cb;
    }
    return ESP_OK;
}

esp_err_t iot_debounce_add_on_press_cb(debounce_handle_t btn_handle, debounce_cb cb, void* arg)
{
    POINT_ASSERT(TAG, btn_handle, ESP_ERR_INVALID_ARG);
    debounce_dev_t* btn = (debounce_dev_t*) btn_handle;
    btn->tap_psh_cb.cb = cb;
    btn->tap_psh_cb.arg = arg;
    return ESP_OK;
}

esp_err_t iot_debounce_add_on_release_cb(debounce_handle_t btn_handle, debounce_cb cb, void* arg)
{
    POINT_ASSERT(TAG, btn_handle, ESP_ERR_INVALID_ARG);
    debounce_dev_t* btn = (debounce_dev_t*) btn_handle;
    btn->tap_rls_cb.cb = cb;
    btn->tap_rls_cb.arg = arg;
    return ESP_OK;
}

