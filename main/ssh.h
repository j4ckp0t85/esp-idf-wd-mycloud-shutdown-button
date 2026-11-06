#ifndef SSH_H
#define SSH_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define SSH_FINISH_BIT (1U << 4)
#define SSH_ACCEPTED_BIT (1U << 5)
#define SSH_FAILED_BIT (1U << 6)
#define SSH_UNKNOWN_BIT (1U << 7)
#define SSH_RESULT_BITS (SSH_ACCEPTED_BIT | SSH_FAILED_BIT | SSH_UNKNOWN_BIT)

extern EventGroupHandle_t xEventGroup;

void ssh_task(void *pvParameters);

#endif