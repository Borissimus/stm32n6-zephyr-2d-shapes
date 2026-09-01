#ifndef STM32N6_SHAPES_UART_TRANSPORT_H_
#define STM32N6_SHAPES_UART_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

struct device;

typedef void (*shapes_uart_self_test_fn)(void);

int shapes_uart_transport_init(const struct device *uart, uint8_t *image_buffer,
			       size_t image_buffer_size, shapes_uart_self_test_fn self_test);
void shapes_uart_transport_run(void);

#endif /* STM32N6_SHAPES_UART_TRANSPORT_H_ */
