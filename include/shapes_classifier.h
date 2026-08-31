#ifndef STM32N6_SHAPES_CLASSIFIER_H_
#define STM32N6_SHAPES_CLASSIFIER_H_

#include <stdint.h>

#define SHAPES_IMAGE_WIDTH 96U
#define SHAPES_IMAGE_HEIGHT 96U
#define SHAPES_IMAGE_CHANNELS 3U
#define SHAPES_IMAGE_BYTES (SHAPES_IMAGE_WIDTH * SHAPES_IMAGE_HEIGHT * SHAPES_IMAGE_CHANNELS)
#define SHAPES_CLASS_COUNT 3U

struct shapes_result {
	uint32_t index;
	float scores[SHAPES_CLASS_COUNT];
	uint32_t preprocess_us;
	uint32_t inference_us;
};

int shapes_classifier_init(void);
int shapes_classifier_run_rgb888_hwc(const uint8_t *rgb, struct shapes_result *result);
const char *shapes_classifier_label(uint32_t index);

#endif /* STM32N6_SHAPES_CLASSIFIER_H_ */
