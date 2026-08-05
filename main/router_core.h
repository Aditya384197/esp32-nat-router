#ifndef ROUTER_CORE_H
#define ROUTER_CORE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t router_core_init(void);
void router_reconnect(void);
void router_stats_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif
