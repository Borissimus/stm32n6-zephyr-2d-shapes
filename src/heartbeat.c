#include <stdbool.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "heartbeat.h"

LOG_MODULE_REGISTER(heartbeat, LOG_LEVEL_INF);

#define HEARTBEAT_STACK_SIZE 768
#define HEARTBEAT_PRIORITY 7
#define HEARTBEAT_PERIOD K_MSEC(500)

static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct k_thread heartbeat_thread_data;
static K_THREAD_STACK_DEFINE(heartbeat_thread_stack, HEARTBEAT_STACK_SIZE);
static struct k_sem heartbeat_start_sem;

static void heartbeat_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	k_sem_take(&heartbeat_start_sem, K_FOREVER);

	LOG_DBG("heartbeat thread running on %s", heartbeat_led.port->name);

	while (1) {
		gpio_pin_toggle_dt(&heartbeat_led);
		k_sleep(HEARTBEAT_PERIOD);
	}
}

int heartbeat_start(void)
{
	static bool initialized;
	static bool started;
	int ret;

	if (!gpio_is_ready_dt(&heartbeat_led)) {
		LOG_ERR("heartbeat LED device is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&heartbeat_led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("failed to configure heartbeat LED: %d", ret);
		return ret;
	}

	if (!initialized) {
		k_sem_init(&heartbeat_start_sem, 0, 1);

		k_thread_create(&heartbeat_thread_data,
			       heartbeat_thread_stack,
			       K_THREAD_STACK_SIZEOF(heartbeat_thread_stack),
			       heartbeat_thread,
			       NULL, NULL, NULL,
			       HEARTBEAT_PRIORITY,
			       0,
			       K_NO_WAIT);
		k_thread_name_set(&heartbeat_thread_data, "heartbeat");
		initialized = true;
	}

	if (started) {
		return 0;
	}

	k_sem_give(&heartbeat_start_sem);
	started = true;
	LOG_DBG("heartbeat started using led0");

	return 0;
}
