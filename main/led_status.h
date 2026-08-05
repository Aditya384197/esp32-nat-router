#ifndef LED_STATUS_H
#define LED_STATUS_H

typedef enum {
    LED_OFF,
    LED_ON_SOLID,
    LED_BLINK_FAST,
    LED_BLINK_SLOW
} led_state_t;

void led_init(void);
void led_set(led_state_t state);

#endif
