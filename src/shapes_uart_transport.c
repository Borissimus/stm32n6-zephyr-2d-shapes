#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

#include "shapes_classifier.h"
#include "shapes_uart_transport.h"

#define SHAPES_PROTOCOL_VERSION 1U
#define SHAPES_CHUNK_SIZE 1024U
#define SHAPES_CHUNK_COUNT (SHAPES_IMAGE_BYTES / SHAPES_CHUNK_SIZE)
#define SHAPES_START_BYTES 14U
#define SHAPES_DATA_HEADER_BYTES 8U
#define SHAPES_DATA_BYTES (SHAPES_DATA_HEADER_BYTES + SHAPES_CHUNK_SIZE)
#define SHAPES_MAX_PACKET_BYTES SHAPES_DATA_BYTES
#define SHAPES_MAX_ENCODED_BYTES (SHAPES_MAX_PACKET_BYTES + (SHAPES_MAX_PACKET_BYTES / 254U) + 2U)
#define SHAPES_RX_RING_BYTES 2048U
#define SHAPES_SESSION_TIMEOUT_MS 1000

enum shapes_packet_type {
	SHAPES_PACKET_START = 1U,
	SHAPES_PACKET_DATA = 2U,
	SHAPES_PACKET_ABORT = 3U,
	SHAPES_PACKET_ACK_START = 0x81U,
	SHAPES_PACKET_ACK_DATA = 0x82U,
	SHAPES_PACKET_NACK = 0x83U,
	SHAPES_PACKET_RESULT = 0x84U,
};

enum shapes_nack_reason {
	SHAPES_NACK_FRAME = 1U,
	SHAPES_NACK_VERSION = 2U,
	SHAPES_NACK_START = 3U,
	SHAPES_NACK_SESSION = 4U,
	SHAPES_NACK_SEQUENCE = 5U,
	SHAPES_NACK_TIMEOUT = 6U,
	SHAPES_NACK_CRC = 7U,
	SHAPES_NACK_ABORTED = 8U,
};

struct shapes_session {
	bool active;
	uint32_t id;
	uint32_t expected_crc;
	uint16_t next_sequence;
	int64_t deadline_ms;
};

RING_BUF_DECLARE(g_rx_ring, SHAPES_RX_RING_BYTES);
K_SEM_DEFINE(g_rx_ready, 0, 1);

static const struct device *g_uart;
static uint8_t *g_image_buffer;
static shapes_uart_self_test_fn g_self_test;
static struct shapes_session g_session;
static uint8_t g_encoded_packet[SHAPES_MAX_ENCODED_BYTES];
static size_t g_encoded_length;
static bool g_drop_until_delimiter;
static volatile uint32_t g_rx_dropped;
static uint32_t g_rx_dropped_seen;

static size_t cobs_encode(const uint8_t *input, size_t input_length, uint8_t *output,
			  size_t output_capacity)
{
	size_t read = 0U;
	size_t write = 1U;
	size_t code_index = 0U;
	uint8_t code = 1U;

	if (output_capacity == 0U) {
		return 0U;
	}
	while (read < input_length) {
		if (write >= output_capacity) {
			return 0U;
		}
		if (input[read] == 0U) {
			output[code_index] = code;
			code_index = write++;
			code = 1U;
		} else {
			output[write++] = input[read];
			if (++code == 0xffU) {
				if (write >= output_capacity) {
					return 0U;
				}
				output[code_index] = code;
				code_index = write++;
				code = 1U;
			}
		}
		++read;
	}
	output[code_index] = code;
	return write;
}

static size_t cobs_decode(const uint8_t *input, size_t input_length, uint8_t *output,
			  size_t output_capacity)
{
	size_t read = 0U;
	size_t write = 0U;

	while (read < input_length) {
		uint8_t code = input[read++];

		if (code == 0U || (size_t)(code - 1U) > input_length - read) {
			return 0U;
		}
		if (write + (size_t)(code - 1U) > output_capacity) {
			return 0U;
		}
		for (uint8_t index = 1U; index < code; ++index) {
			output[write++] = input[read++];
		}
		if (code != 0xffU && read < input_length) {
			if (write >= output_capacity) {
				return 0U;
			}
			output[write++] = 0U;
		}
	}
	return write;
}

static void send_raw_packet(const uint8_t *packet, size_t packet_length)
{
	uint8_t encoded[64];
	size_t encoded_length = cobs_encode(packet, packet_length, encoded, sizeof(encoded));

	if (encoded_length == 0U) {
		return;
	}
	for (size_t index = 0U; index < encoded_length; ++index) {
		uart_poll_out(g_uart, encoded[index]);
	}
	uart_poll_out(g_uart, 0U);
}

static void send_ack(uint8_t type, uint32_t session_id, uint16_t sequence)
{
	uint8_t packet[8] = { SHAPES_PROTOCOL_VERSION, type };

	sys_put_le32(session_id, &packet[2]);
	if (type == SHAPES_PACKET_ACK_DATA) {
		sys_put_le16(sequence, &packet[6]);
		send_raw_packet(packet, sizeof(packet));
	} else {
		send_raw_packet(packet, 6U);
	}
}

static void send_nack(uint32_t session_id, uint8_t reason, uint16_t expected_sequence)
{
	uint8_t packet[10] = { SHAPES_PROTOCOL_VERSION, SHAPES_PACKET_NACK };

	sys_put_le32(session_id, &packet[2]);
	packet[6] = reason;
	sys_put_le16(expected_sequence, &packet[7]);
	send_raw_packet(packet, 9U);
}

static void send_result(uint32_t session_id, const struct shapes_result *result, int rc)
{
	uint8_t packet[19] = { SHAPES_PROTOCOL_VERSION, SHAPES_PACKET_RESULT };

	sys_put_le32(session_id, &packet[2]);
	packet[6] = rc == 0 ? 0U : 1U;
	packet[7] = rc == 0 ? (uint8_t)result->index : 0xffU;
	sys_put_le32(rc == 0 ? result->inference_us : 0U, &packet[8]);
	sys_put_le32(rc == 0 ? result->preprocess_us : 0U, &packet[12]);
	if (rc == 0) {
		for (size_t index = 0U; index < SHAPES_CLASS_COUNT; ++index) {
			packet[16U + index] = (uint8_t)((int8_t)(result->scores[index] / 0.0954789146780968f));
		}
	}
	send_raw_packet(packet, sizeof(packet));
}

static void reset_session(void)
{
	memset(&g_session, 0, sizeof(g_session));
}

static void refresh_session_deadline(void)
{
	g_session.deadline_ms = k_uptime_get() + SHAPES_SESSION_TIMEOUT_MS;
}

static void handle_start(const uint8_t *packet, size_t packet_length)
{
	uint32_t session_id;
	uint32_t total_size;

	if (packet_length != SHAPES_START_BYTES) {
		send_nack(0U, SHAPES_NACK_START, 0U);
		return;
	}
	session_id = sys_get_le32(&packet[2]);
	total_size = sys_get_le32(&packet[6]);
	if (session_id == 0U || total_size != SHAPES_IMAGE_BYTES) {
		send_nack(session_id, SHAPES_NACK_START, 0U);
		return;
	}
	if (g_session.active && g_session.id == session_id && g_session.next_sequence == 0U) {
		send_ack(SHAPES_PACKET_ACK_START, session_id, 0U);
		return;
	}
	g_session.active = true;
	g_session.id = session_id;
	g_session.expected_crc = sys_get_le32(&packet[10]);
	g_session.next_sequence = 0U;
	refresh_session_deadline();
	send_ack(SHAPES_PACKET_ACK_START, session_id, 0U);
}

static void complete_image(void)
{
	struct shapes_result result;
	uint32_t session_id = g_session.id;
	int rc;

	if (crc32_ieee(g_image_buffer, SHAPES_IMAGE_BYTES) != g_session.expected_crc) {
		send_nack(session_id, SHAPES_NACK_CRC, SHAPES_CHUNK_COUNT);
		reset_session();
		return;
	}
	rc = shapes_classifier_run_rgb888_hwc(g_image_buffer, &result);
	reset_session();
	send_result(session_id, &result, rc);
}

static void handle_data(const uint8_t *packet, size_t packet_length)
{
	uint32_t session_id;
	uint16_t sequence;

	if (packet_length != SHAPES_DATA_BYTES) {
		send_nack(g_session.active ? g_session.id : 0U, SHAPES_NACK_FRAME,
			  g_session.next_sequence);
		return;
	}
	session_id = sys_get_le32(&packet[2]);
	sequence = sys_get_le16(&packet[6]);
	if (!g_session.active || session_id != g_session.id) {
		send_nack(session_id, SHAPES_NACK_SESSION,
			  g_session.active ? g_session.next_sequence : 0U);
		return;
	}
	if (sequence + 1U == g_session.next_sequence) {
		send_ack(SHAPES_PACKET_ACK_DATA, session_id, sequence);
		return;
	}
	if (sequence != g_session.next_sequence || sequence >= SHAPES_CHUNK_COUNT) {
		send_nack(session_id, SHAPES_NACK_SEQUENCE, g_session.next_sequence);
		return;
	}
	memcpy(&g_image_buffer[(size_t)sequence * SHAPES_CHUNK_SIZE],
	       &packet[SHAPES_DATA_HEADER_BYTES], SHAPES_CHUNK_SIZE);
	++g_session.next_sequence;
	refresh_session_deadline();
	if (g_session.next_sequence == SHAPES_CHUNK_COUNT) {
		complete_image();
		return;
	}
	send_ack(SHAPES_PACKET_ACK_DATA, session_id, sequence);
}

static void handle_packet(const uint8_t *packet, size_t packet_length)
{
	if (packet_length < 2U || packet[0] != SHAPES_PROTOCOL_VERSION) {
		send_nack(0U, packet_length >= 1U ? SHAPES_NACK_VERSION : SHAPES_NACK_FRAME, 0U);
		return;
	}
	switch (packet[1]) {
	case SHAPES_PACKET_START:
		handle_start(packet, packet_length);
		break;
	case SHAPES_PACKET_DATA:
		handle_data(packet, packet_length);
		break;
	case SHAPES_PACKET_ABORT:
		if (packet_length == 6U && g_session.active &&
		    sys_get_le32(&packet[2]) == g_session.id) {
			send_nack(g_session.id, SHAPES_NACK_ABORTED, g_session.next_sequence);
			reset_session();
		}
		break;
	default:
		send_nack(g_session.active ? g_session.id : 0U, SHAPES_NACK_FRAME,
			  g_session.next_sequence);
		break;
	}
}

static void process_byte(uint8_t byte)
{
	if (byte == 0U) {
		if (!g_drop_until_delimiter && g_encoded_length > 0U) {
			uint8_t decoded[SHAPES_MAX_PACKET_BYTES];
			size_t decoded_length = cobs_decode(g_encoded_packet, g_encoded_length, decoded,
						     sizeof(decoded));

			if (decoded_length == 0U) {
				send_nack(g_session.active ? g_session.id : 0U, SHAPES_NACK_FRAME,
					  g_session.next_sequence);
			} else {
				handle_packet(decoded, decoded_length);
			}
		}
		g_encoded_length = 0U;
		g_drop_until_delimiter = false;
		return;
	}

	if (!g_session.active && g_encoded_length == 4U &&
	    memcmp(g_encoded_packet, "TST0", 4U) == 0 && (byte == '\r' || byte == '\n')) {
		g_encoded_length = 0U;
		if (g_self_test != NULL) {
			g_self_test();
		}
		return;
	}
	if (g_drop_until_delimiter) {
		return;
	}
	if (g_encoded_length >= sizeof(g_encoded_packet)) {
		g_drop_until_delimiter = true;
		return;
	}
	g_encoded_packet[g_encoded_length++] = byte;
}

static void uart_rx_isr(const struct device *uart, void *user_data)
{
	uint8_t fifo[64];

	ARG_UNUSED(user_data);
	while (uart_irq_update(uart) != 0 && uart_irq_rx_ready(uart)) {
		int received = uart_fifo_read(uart, fifo, sizeof(fifo));
		uint32_t stored;

		if (received <= 0) {
			break;
		}
		stored = ring_buf_put(&g_rx_ring, fifo, (uint32_t)received);
		g_rx_dropped += (uint32_t)received - stored;
		if (stored > 0U) {
			k_sem_give(&g_rx_ready);
		}
	}
}

int shapes_uart_transport_init(const struct device *uart, uint8_t *image_buffer,
			       size_t image_buffer_size, shapes_uart_self_test_fn self_test)
{
	if (uart == NULL || image_buffer == NULL || image_buffer_size != SHAPES_IMAGE_BYTES ||
	    !device_is_ready(uart)) {
		return -EINVAL;
	}
	g_uart = uart;
	g_image_buffer = image_buffer;
	g_self_test = self_test;
	reset_session();
	uart_irq_callback_user_data_set(g_uart, uart_rx_isr, NULL);
	uart_irq_rx_enable(g_uart);
	return 0;
}

void shapes_uart_transport_run(void)
{
	while (true) {
		uint8_t bytes[64];
		uint32_t count = ring_buf_get(&g_rx_ring, bytes, sizeof(bytes));

		for (uint32_t index = 0U; index < count; ++index) {
			process_byte(bytes[index]);
		}
		if (g_rx_dropped != g_rx_dropped_seen) {
			g_rx_dropped_seen = g_rx_dropped;
			if (g_session.active) {
				send_nack(g_session.id, SHAPES_NACK_FRAME, g_session.next_sequence);
				reset_session();
			}
			g_encoded_length = 0U;
			g_drop_until_delimiter = true;
		}
		if (g_session.active && k_uptime_get() >= g_session.deadline_ms) {
			send_nack(g_session.id, SHAPES_NACK_TIMEOUT, g_session.next_sequence);
			reset_session();
		}
		if (count == 0U) {
			(void)k_sem_take(&g_rx_ready, K_MSEC(20));
		}
	}
}
