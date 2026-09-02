#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "stai.h"
#include "stai_shapes_mobilenet_v2.h"
#include "ll_aton_NN_interface.h"

#include "shapes_classifier.h"
#include "stm32n6_npu_preinit.h"

LOG_MODULE_REGISTER(shapes_classifier, LOG_LEVEL_INF);

#define SHAPES_INPUT_SCALE_NUM 127
#define SHAPES_INPUT_SCALE_DEN 255
#define SHAPES_OUTPUT_SCALE 0.0954789146780968f

STAI_NETWORK_CONTEXT_DECLARE(g_network_ctx, STAI_SHAPES_MOBILENET_V2_CONTEXT_SIZE);

static int8_t *g_model_input;
static int8_t *g_model_output;
static bool g_ready;

static const char *const g_labels[SHAPES_CLASS_COUNT] = {
	"circle",
	"square",
	"triangle",
};

static int8_t quantize_rgb_component(uint8_t value)
{
	/* QDQ input: ((rgb / 255.0) - 0.5) / 0.5, scale=1/127, zero=0. */
	int32_t quantized = ((int32_t)value * 2 * SHAPES_INPUT_SCALE_NUM +
			     SHAPES_INPUT_SCALE_DEN / 2) /
			    SHAPES_INPUT_SCALE_DEN - SHAPES_INPUT_SCALE_NUM;

	if (quantized > INT8_MAX) {
		return INT8_MAX;
	}
	if (quantized < INT8_MIN) {
		return INT8_MIN;
	}
	return (int8_t)quantized;
}

int shapes_classifier_init(void)
{
	stai_ptr inputs[STAI_SHAPES_MOBILENET_V2_IN_NUM];
	stai_ptr outputs[STAI_SHAPES_MOBILENET_V2_OUT_NUM];
	stai_size input_count = 0U;
	stai_size output_count = 0U;
	stai_return_code rc;

	if (g_ready) {
		return 0;
	}

	/* RAM-load images do not inherit MCUboot's XSPI2 mapping. */
	if (!IS_ENABLED(CONFIG_XIP) && stm32n6_xspi_pre_init() != 0) {
		LOG_ERR("xSPI NOR memory-map init failed");
		return -EIO;
	}
	if (stm32n6_npu_pre_init() != 0) {
		LOG_ERR("NPU platform pre-init failed");
		return -EIO;
	}
	if (stai_runtime_init() != STAI_SUCCESS) {
		LOG_ERR("STAI runtime init failed");
		return -EIO;
	}

	rc = stai_shapes_mobilenet_v2_init(g_network_ctx);
	if (rc != STAI_SUCCESS) {
		LOG_ERR("Shape model init failed: 0x%x", rc);
		return -EIO;
	}
	rc = stai_shapes_mobilenet_v2_get_inputs(g_network_ctx, inputs, &input_count);
	if (rc != STAI_SUCCESS || input_count != STAI_SHAPES_MOBILENET_V2_IN_NUM) {
		LOG_ERR("Shape model input query failed: rc=0x%x count=%u", rc,
			(unsigned int)input_count);
		return -EIO;
	}
	rc = stai_shapes_mobilenet_v2_get_outputs(g_network_ctx, outputs, &output_count);
	if (rc != STAI_SUCCESS || output_count != STAI_SHAPES_MOBILENET_V2_OUT_NUM) {
		LOG_ERR("Shape model output query failed: rc=0x%x count=%u", rc,
			(unsigned int)output_count);
		return -EIO;
	}

	g_model_input = (int8_t *)inputs[0];
	g_model_output = (int8_t *)outputs[0];
	g_ready = true;
	LOG_INF("Shapes model ready: input=%u bytes output=%u bytes weights=0x71000000",
		(unsigned int)SHAPES_IMAGE_BYTES, (unsigned int)SHAPES_CLASS_COUNT);
	return 0;
}

int shapes_classifier_run_rgb888_hwc(const uint8_t *rgb, struct shapes_result *result)
{
	const size_t plane_size = SHAPES_IMAGE_WIDTH * SHAPES_IMAGE_HEIGHT;
	uint32_t start_cycles;
	stai_return_code rc;

	if (!g_ready || rgb == NULL || result == NULL) {
		return -EINVAL;
	}

	start_cycles = k_cycle_get_32();
	for (size_t index = 0; index < plane_size; ++index) {
		const size_t source = index * SHAPES_IMAGE_CHANNELS;

		g_model_input[index] = quantize_rgb_component(rgb[source]);
		g_model_input[plane_size + index] = quantize_rgb_component(rgb[source + 1U]);
		g_model_input[(2U * plane_size) + index] = quantize_rgb_component(rgb[source + 2U]);
	}
	result->preprocess_us = k_cyc_to_us_floor32(k_cycle_get_32() - start_cycles);

	start_cycles = k_cycle_get_32();
	rc = stai_shapes_mobilenet_v2_run(g_network_ctx, STAI_MODE_SYNC);
	result->inference_us = k_cyc_to_us_floor32(k_cycle_get_32() - start_cycles);
	if (rc != STAI_SUCCESS) {
		LOG_ERR("Shape inference failed: 0x%x", rc);
		return -EIO;
	}

	result->index = 0U;
	for (size_t index = 0; index < SHAPES_CLASS_COUNT; ++index) {
		result->scores[index] = (float)g_model_output[index] * SHAPES_OUTPUT_SCALE;
		if (result->scores[index] > result->scores[result->index]) {
			result->index = index;
		}
	}
	return 0;
}

const char *shapes_classifier_label(uint32_t index)
{
	return index < SHAPES_CLASS_COUNT ? g_labels[index] : "invalid";
}
