#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_version.h>
#include <zephyr/sys/printk.h>

#include "embedded_self_test_images.h"
#include "heartbeat.h"
#include "shapes_classifier.h"
#include "shapes_uart_transport.h"

static const struct device *const console_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static uint8_t image_bytes[SHAPES_IMAGE_BYTES] __aligned(32);

static uint8_t expand_rgb5(uint16_t value)
{
	return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand_rgb6(uint16_t value)
{
	return (uint8_t)((value << 2) | (value >> 4));
}

static void rgb565_to_rgb888_hwc(const uint16_t *rgb565, uint8_t *rgb888)
{
	for (size_t index = 0; index < SHAPES_IMAGE_WIDTH * SHAPES_IMAGE_HEIGHT; ++index) {
		uint16_t pixel = rgb565[index];
		size_t output = index * SHAPES_IMAGE_CHANNELS;

		rgb888[output] = expand_rgb5((pixel >> 11) & 0x1fU);
		rgb888[output + 1U] = expand_rgb6((pixel >> 5) & 0x3fU);
		rgb888[output + 2U] = expand_rgb5(pixel & 0x1fU);
	}
}

static void run_embedded_self_test(void)
{
	uint32_t correct = 0U;

	printk("SELFTEST begin count=%u\n", (unsigned int)EMBEDDED_SELF_TEST_CASE_COUNT);
	for (size_t index = 0; index < EMBEDDED_SELF_TEST_CASE_COUNT; ++index) {
		const embedded_self_test_case_t *test_case = &embedded_self_test_cases[index];
		struct shapes_result result;
		int rc;

		rgb565_to_rgb888_hwc(test_case->rgb565_data, image_bytes);
		rc = shapes_classifier_run_rgb888_hwc(image_bytes, &result);
		bool match = rc == 0 && strcmp(shapes_classifier_label(result.index), test_case->label) == 0;

		correct += match ? 1U : 0U;
		printk("SELFTEST case=%u expected=%s predicted=%s status=%s time_us=%u\n",
		       (unsigned int)index, test_case->label,
		       rc == 0 ? shapes_classifier_label(result.index) : "error",
		       match ? "ok" : "fail", rc == 0 ? result.inference_us : 0U);
	}
	printk("SELFTEST summary correct=%u total=%u\n", correct,
	       (unsigned int)EMBEDDED_SELF_TEST_CASE_COUNT);
}

int main(void)
{
	uint32_t version = sys_kernel_version_get();

	printk("stm32n6-zephyr-2d-shapes: boot\n");
	printk("kernel version: %u.%u.%u\n", SYS_KERNEL_VER_MAJOR(version),
	       SYS_KERNEL_VER_MINOR(version), SYS_KERNEL_VER_PATCHLEVEL(version));
	if (heartbeat_start() < 0 || shapes_classifier_init() < 0) {
		return -1;
	}
	if (shapes_uart_transport_init(console_uart, image_bytes, sizeof(image_bytes),
				      run_embedded_self_test) < 0) {
		return -1;
	}
	printk("Shapes2D ready: COBS v1, 27 x 1024 B, CRC32 frame; TST0 self-test\n");
	run_embedded_self_test();
	printk("Waiting for COBS image frames...\n");
	shapes_uart_transport_run();
	return 0;
}
