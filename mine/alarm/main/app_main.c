/* HomeKit device that monitors 4 digital inputs and exposes them as contact sensors. Can be
 * hooked up in parallel with a traditional home alarm system. 
 *
 * Based on example from Espressif HomeKit SDK. Original license:
 * 
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS products only, in which case,
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
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include <hap_apple_servs.h>
#include <hap_apple_chars.h>

#include <iot_button.h>

#include "iot_debounce.h"

#include <app_wifi.h>
#include <app_hap_setup_payload.h>

#include <hap.h>

static const char *TAG = "Alarm App";

#define BRIDGE_TASK_PRIORITY  1
#define BRIDGE_TASK_STACKSIZE 4 * 1024
#define BRIDGE_TASK_NAME      "hap_bridge"

#define NUM_BRIDGED_ACCESSORIES 4

/* Reset network credentials if button is pressed for more than 3 seconds and then released */
#define RESET_NETWORK_BUTTON_TIMEOUT        3

/* Reset to factory if button is pressed and held for more than 10 seconds */
#define RESET_TO_FACTORY_BUTTON_TIMEOUT     10

/* The button "Boot" will be used as the Reset button for the example */
#define RESET_GPIO  GPIO_NUM_0

struct contact_t {
    int i;
    gpio_num_t gpio;
    hap_serv_t *service;
    hap_char_t *characteristic;
    debounce_handle_t debounce;
} contacts[] = {
    { .i = 0, .gpio = GPIO_NUM_14},
    { .i = 1, .gpio = GPIO_NUM_27},
    { .i = 2, .gpio = GPIO_NUM_26},
    { .i = 3, .gpio = GPIO_NUM_25},
};

/**
 * @brief The network reset button callback handler.
 * Useful for testing the Wi-Fi re-configuration feature of WAC2
 */
static void reset_network_handler(void* arg)
{
    hap_reset_network();
}
/**
 * @brief The factory reset button callback handler.
 */
static void reset_to_factory_handler(void* arg)
{
    hap_reset_to_factory();
}

static void contact_trigger_handler(void* arg)
{
  struct contact_t *contact = arg;
  ESP_LOGE(TAG, "Contact %d triggered.", contact->i);
  hap_val_t val;
  val.i = true;
  hap_char_update_val(contact->characteristic, &val);
}

static void contact_clear_handler(void* arg)
{
  struct contact_t *contact = arg;
  ESP_LOGE(TAG, "Contact %d cleared.", contact->i);
  hap_val_t val; 
  val.i = false;
  hap_char_update_val(contact->characteristic, &val);
}

/**
 * The Reset button  GPIO initialisation function.
 * Same button will be used for resetting Wi-Fi network as well as for reset to factory based on
 * the time for which the button is pressed.
 */
static void reset_key_init(uint32_t key_gpio_pin)
{
    button_handle_t handle = iot_button_create(key_gpio_pin, BUTTON_ACTIVE_LOW);
    iot_button_add_on_release_cb(handle, RESET_NETWORK_BUTTON_TIMEOUT, reset_network_handler, NULL);
    iot_button_add_on_press_cb(handle, RESET_TO_FACTORY_BUTTON_TIMEOUT, reset_to_factory_handler, NULL);
}

/* Mandatory identify routine for the accessory (bridge)
 * In a real accessory, something like LED blink should be implemented
 * got visual identification
 */
static int bridge_identify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "Bridge identified");
    return HAP_SUCCESS;
}

/* Mandatory identify routine for the bridged accessory
 * In a real bridge, the actual accessory must be sent some request to
 * identify itself visually
 */
static int accessory_identify(hap_acc_t *ha)
{
    hap_serv_t *hs = hap_acc_get_serv_by_uuid(ha, HAP_SERV_UUID_ACCESSORY_INFORMATION);
    hap_char_t *hc = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_NAME);
    const hap_val_t *val = hap_char_get_val(hc);
    char *name = val->s;

    ESP_LOGI(TAG, "Bridged Accessory %s identified", name);
    return HAP_SUCCESS;
}

/*The main thread for handling the Bridge Accessory */
static void bridge_thread_entry(void *p)
{
    hap_acc_t *accessory;
    hap_serv_t *service;

    /* Initialize the HAP core */
    hap_init(HAP_TRANSPORT_WIFI);

    /* Initialise the mandatory parameters for Accessory which will be added as
     * the mandatory services internally
     */
    hap_acc_cfg_t cfg = {
        .name = "ESP32-Alarm",
        .manufacturer = "Me",
        .model = "alarm01",
        .serial_num = "00000001",
        .fw_rev = "1.0.0",
        .hw_rev = NULL,
        .pv = "1.0.0",
        .identify_routine = bridge_identify,
        .cid = HAP_CID_SECURITY_SYSTEM,
    };
    /* Create accessory object */
    accessory = hap_acc_create(&cfg);

    /* Add a dummy Product Data */
    uint8_t product_data[] = {'E','S','P','A','l','a','r','m'};
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));

    /* Add the Accessory to the HomeKit Database */
    hap_add_accessory(accessory);

    /* Create and add the Accessory to the Bridge object*/
    for (uint8_t i = 0; i < NUM_BRIDGED_ACCESSORIES; i++) {
        char accessory_name[16] = {0};
        sprintf(accessory_name, "Alarm Contact %d", i);

        hap_acc_cfg_t bridge_cfg = {
            .name = accessory_name,
            .manufacturer = "Me",
            .model = "Alarm Contact",
            .serial_num = "00000001",
            .fw_rev = "1.0.0",
            .hw_rev = NULL,
            .pv = "1.0.0",
            .identify_routine = accessory_identify,
            .cid = HAP_CID_SECURITY_SYSTEM,
        };
        /* Create accessory object */
        accessory = hap_acc_create(&bridge_cfg);

        /* Create button before service so we can read the gpio state */
        contacts[i].debounce = iot_debounce_create(contacts[i].gpio, DEBOUNCE_ACTIVE_HIGH);
        
        /* Create the Fan Service. Include the "name" since this is a user visible service  */
        contacts[i].service = service = hap_serv_contact_sensor_create(i%2);
        contacts[i].characteristic = hap_serv_get_first_char(service);
        hap_serv_add_char(service, hap_char_name_create(accessory_name));

        /* Set the Accessory name as the Private data for the service,
         * so that the correct accessory can be identified in the
         * write callback
         */
        hap_serv_set_priv(service, strdup(accessory_name));

        /* Add the Fan Service to the Accessory Object */
        hap_acc_add_serv(accessory, service);

        /* Add the Accessory to the HomeKit Database */
        hap_add_bridged_accessory(accessory, hap_get_unique_aid(accessory_name));
        
        /* Attach to the hardware debounce events */
        iot_debounce_add_on_release_cb(contacts[i].debounce, contact_clear_handler, contacts+i);
        iot_debounce_add_on_press_cb(contacts[i].debounce, contact_trigger_handler, contacts+i);
    }

    /* Register a common button for reset Wi-Fi network and reset to factory.
     */
    reset_key_init(RESET_GPIO);

    /* For production accessories, the setup code shouldn't be programmed on to
     * the device. Instead, the setup info, derived from the setup code must
     * be used. Use the factory_nvs_gen utility to generate this data and then
     * flash it into the factory NVS partition.
     *
     * By default, the setup ID and setup info will be read from the factory_nvs
     * Flash partition and so, is not required to set here explicitly.
     *
     * However, for testing purpose, this can be overridden by using hap_set_setup_code()
     * and hap_set_setup_id() APIs, as has been done here.
     */
#ifdef CONFIG_EXAMPLE_USE_HARDCODED_SETUP_CODE
    /* Unique Setup code of the format xxx-xx-xxx. Default: 111-22-333 */
    hap_set_setup_code(CONFIG_EXAMPLE_SETUP_CODE);
    /* Unique four character Setup Id. Default: ES32 */
    hap_set_setup_id(CONFIG_EXAMPLE_SETUP_ID);
#ifdef CONFIG_APP_WIFI_USE_WAC_PROVISIONING
    app_hap_setup_payload(CONFIG_EXAMPLE_SETUP_CODE, CONFIG_EXAMPLE_SETUP_ID, true, cfg.cid);
#else
    app_hap_setup_payload(CONFIG_EXAMPLE_SETUP_CODE, CONFIG_EXAMPLE_SETUP_ID, false, cfg.cid);
#endif
#endif

    /* Enable Hardware MFi authentication (applicable only for MFi variant of SDK) */
    hap_enable_mfi_auth(HAP_MFI_AUTH_HW);

    /* Initialize Wi-Fi */
    app_wifi_init_with_hostname("esp32-alarm");
    

    /* After all the initializations are done, start the HAP core */
    hap_start();
    
    /* Start Wi-Fi */
    app_wifi_start(portMAX_DELAY);

    /* Start the contact sensors monitoring. Will raise an event for the current state. */
    for (uint8_t i = 0; i < NUM_BRIDGED_ACCESSORIES; i++) {
        iot_debounce_init(contacts[i].debounce);
    }

    /* The task ends here. The read/write callbacks will be invoked by the HAP Framework */
    vTaskDelete(NULL);
}

void app_main()
{
    xTaskCreate(bridge_thread_entry, BRIDGE_TASK_NAME, BRIDGE_TASK_STACKSIZE, NULL, BRIDGE_TASK_PRIORITY, NULL);
}

