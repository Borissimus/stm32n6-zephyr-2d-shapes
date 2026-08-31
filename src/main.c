#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_version.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "embedded_self_test_images.h"
#include "heartbeat.h"
#include "shapes_classifier.h"

#define FRAME_MAGIC "IMG0"
#define VERIFIED_MAGIC "VHDR"
#define CHUNK_MAGIC "CHNK"
#define SELF_TEST_MAGIC "TST0"
#define VERIFIED_HEADER_BYTES 12U
#define VERIFIED_CHUNK_HEADER_BYTES 8U
#define VERIFIED_MAX_CHUNK_BYTES 1024U

static const struct device *const console_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static uint8_t image_bytes[SHAPES_IMAGE_BYTES] __aligned(32);
static uint8_t chunk_bytes[VERIFIED_MAX_CHUNK_BYTES] __aligned(4);

static void uart_read_exact(uint8_t *buffer, size_t size)
{
	for (size_t index = 0; index < size;) {
		unsigned char byte;

		if (uart_poll_in(console_uart, &byte) == 0) {
			buffer[index++] = byte;
		} else {
			k_sleep(K_MSEC(1));
		}
	}
}

static bool uart_read_magic(const char expected[4])
{
	uint8_t received[4];

	uart_read_exact(received, sizeof(received));
	return memcmp(received, expected, sizeof(received)) == 0;
}

static int run_image(const uint8_t *rgb, const char *expected)
{
	struct shapes_result result;
	int rc = shapes_classifier_run_rgb888_hwc(rgb, &result);

	if (rc < 0) {
		printk("RESULT status=error rc=%d\n", rc);
		return rc;
	}
	/* Scores are emitted as scaled integers in the first UART iteration. */
	printk("RESULT status=ok index=%u label=%s time_us=%u prep_us=%u expected=%s scores_q=%d,%d,%d\n",
	       result.index, shapes_classifier_label(result.index), result.inference_us,
	       result.preprocess_us, expected ? expected : "unknown",
	       (int)(result.scores[0] / 0.0954789146780968f),
	       (int)(result.scores[1] / 0.0954789146780968f),
	       (int)(result.scores[2] / 0.0954789146780968f));
	return 0;
}

static void run_embedded_self_test(void)
{
	uint32_t correct = 0U;

	printk("SELFTEST begin count=%u\n", (unsigned int)EMBEDDED_SELF_TEST_CASE_COUNT);
	for (size_t index = 0; index < EMBEDDED_SELF_TEST_CASE_COUNT; ++index) {
		const embedded_self_test_case_t *test_case = &embedded_self_test_cases[index];
		struct shapes_result result;
		int rc = shapes_classifier_run_rgb888_hwc(test_case->rgb_data, &result);
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

static bool receive_verified_image(void)
{
	uint8_t header[VERIFIED_HEADER_BYTES];
	uint32_t total_size;
	uint16_t chunk_size;
	uint16_t chunk_count;
	uint32_t expected_crc;
	uint32_t received = 0U;

	uart_read_exact(header, sizeof(header));
	total_size = sys_get_le32(&header[0]);
	chunk_size = sys_get_le16(&header[4]);
	chunk_count = sys_get_le16(&header[6]);
	expected_crc = sys_get_le32(&header[8]);
	if (total_size != SHAPES_IMAGE_BYTES || chunk_size == 0U ||
	    chunk_size > VERIFIED_MAX_CHUNK_BYTES ||
	    chunk_count != DIV_ROUND_UP(SHAPES_IMAGE_BYTES, chunk_size)) {
		printk("NACK START reason=invalid_header\n");
		return false;
	}

	printk("ACK START total=%u chunk=%u count=%u\n", total_size, chunk_size, chunk_count);
	for (uint16_t index = 0U; index < chunk_count; ++index) {
		uint8_t chunk_header[VERIFIED_CHUNK_HEADER_BYTES];
		uint16_t received_index;
		uint16_t received_size;
		uint32_t expected_chunk_crc;

		if (!uart_read_magic(CHUNK_MAGIC)) {
			printk("NACK CHUNK index=%u reason=magic\n", index);
			return false;
		}
		uart_read_exact(chunk_header, sizeof(chunk_header));
		received_index = sys_get_le16(&chunk_header[0]);
		received_size = sys_get_le16(&chunk_header[2]);
		expected_chunk_crc = sys_get_le32(&chunk_header[4]);
		if (received_index != index || received_size == 0U || received_size > chunk_size ||
		    received + received_size > SHAPES_IMAGE_BYTES) {
			printk("NACK CHUNK index=%u reason=header\n", index);
			return false;
		}
		uart_read_exact(chunk_bytes, received_size);
		if (crc32_ieee(chunk_bytes, received_size) != expected_chunk_crc) {
			printk("NACK CHUNK index=%u reason=crc\n", index);
			return false;
		}
		memcpy(&image_bytes[received], chunk_bytes, received_size);
		received += received_size;
		printk("ACK CHUNK index=%u\n", index);
	}
	if (crc32_ieee(image_bytes, received) != expected_crc) {
		printk("NACK IMAGE reason=crc\n");
		return false;
	}
	printk("ACK IMAGE crc=%08x\n", expected_crc);
	return true;
}

static void wait_for_frames(void)
{
	while (true) {
		uint8_t magic[4];

		uart_read_exact(magic, sizeof(magic));
		if (memcmp(magic, SELF_TEST_MAGIC, sizeof(magic)) == 0) {
			run_embedded_self_test();
			continue;
		}
		if (memcmp(magic, FRAME_MAGIC, sizeof(magic)) != 0) {
			continue;
		}

		uart_read_exact(magic, sizeof(magic));
		if (memcmp(magic, VERIFIED_MAGIC, sizeof(magic)) == 0) {
			if (receive_verified_image()) {
				(void)run_image(image_bytes, NULL);
			}
		} else {
			memcpy(image_bytes, magic, sizeof(magic));
			uart_read_exact(&image_bytes[sizeof(magic)], SHAPES_IMAGE_BYTES - sizeof(magic));
			(void)run_image(image_bytes, NULL);
		}
	}
}

int main(void)
{
	uint32_t version = sys_kernel_version_get();

	printk("stm32n6-zephyr-2d-shapes: boot\n");
	printk("kernel version: %u.%u.%u\n", SYS_KERNEL_VER_MAJOR(version),
	       SYS_KERNEL_VER_MINOR(version), SYS_KERNEL_VER_PATCHLEVEL(version));
	if (!device_is_ready(console_uart)) {
		return -1;
	}
	if (heartbeat_start() < 0 || shapes_classifier_init() < 0) {
		return -1;
	}
	printk("Shapes2D ready: UART 1000000 baud; IMG0/VHDR/CHNK, TST0 self-test\n");
	run_embedded_self_test();
	printk("Waiting for image frames...\n");
	wait_for_frames();
	return 0;
}
