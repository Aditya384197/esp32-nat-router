#include "led_status.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define LED_GPIO GPIO_NUM_2

static QueueHandle_t led_queue = NULL;
static led_state_t current_state = LED_OFF;

static void led_blink_task(void *arg) {
    led_state_t new_state;
    while (1) {
        if (xQueueReceive(led_queue, &new_state, portMAX_DELAY) == pdTRUE) {
            current_state = new_state;
        }
        switch (current_state) {
            case LED_OFF:
                gpio_set_level(LED_GPIO, 0);
                break;
            case LED_ON_SOLID:
                gpio_set_level(LED_GPIO, 1);
                break;
            case LED_BLINK_FAST:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_BLINK_SLOW:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
}

void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, 0);

    led_queue = xQueueCreate(1, sizeof(led_state_t));
    xTaskCreatePinnedToCore(led_blink_task, "led_task", 2048, NULL, 1, NULL, 1);
}

void led_set(led_state_t state) {
    if (led_queue) {
        xQueueOverwrite(led_queue, &state);
    }
}
