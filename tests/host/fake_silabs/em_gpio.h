/* Minimal em_gpio surface for host tests that compile production BG22 sources.
 * Pin writes are recorded, not performed; see fake_silabs_gpio_writes. */
#ifndef OD_TEST_FAKE_SILABS_EM_GPIO_H
#define OD_TEST_FAKE_SILABS_EM_GPIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    gpioPortA = 0,
    gpioPortB = 1,
    gpioPortC = 2,
    gpioPortD = 3
} GPIO_Port_TypeDef;

#define GPIO_PORT_MAX 3

typedef enum {
    gpioModeDisabled = 0,
    gpioModePushPull = 1
} GPIO_Mode_TypeDef;

void GPIO_PinModeSet(GPIO_Port_TypeDef port, uint8_t pin, GPIO_Mode_TypeDef mode, unsigned out);
void GPIO_PinOutSet(GPIO_Port_TypeDef port, uint8_t pin);
void GPIO_PinOutClear(GPIO_Port_TypeDef port, uint8_t pin);

#endif /* OD_TEST_FAKE_SILABS_EM_GPIO_H */
