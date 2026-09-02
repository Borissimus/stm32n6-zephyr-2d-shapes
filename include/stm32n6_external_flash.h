#ifndef STM32N6_EXTERNAL_FLASH_H
#define STM32N6_EXTERNAL_FLASH_H

#include <stddef.h>
#include <stdint.h>

#define STM32N6_NOR_BASE_ADDR      0x70000000UL
#define STM32N6_NOR_TEST_ABS_ADDR  0x71800000UL
#define STM32N6_NOR_TEST_BLOCK_LEN (64U * 1024U)
#define STM32N6_NOR_TEST_DATA_LEN  256U

struct stm32n6_nor_test_result {
	uint32_t abs_addr;
	uint32_t offset;
	uint32_t size;
	int erase_rc;
	int write_rc;
	int read_rc;
	int auto_poll_rc;
	int write_enable_erase_rc;
	int block_erase_rc;
	int auto_poll_after_erase_rc;
	int write_enable_program_rc;
	int page_program_rc;
	int auto_poll_after_program_rc;
	int direct_read_rc;
	uint32_t expected_crc32;
	uint32_t actual_crc32;
	uint32_t mismatch_count;
	uint32_t first_bad_offset;
};

int stm32n6_external_flash_init(void);
int stm32n6_external_flash_read(uint32_t abs_addr, uint8_t *buf, size_t len);
/* A write session temporarily switches XSPI2 from memory-mapped to indirect
 * mode. The caller must ensure no NPU inference is using NOR-backed weights. */
int stm32n6_external_flash_begin_write(void);
int stm32n6_external_flash_erase_64k(uint32_t abs_addr);
int stm32n6_external_flash_write(uint32_t abs_addr, const uint8_t *buf, size_t len);
int stm32n6_external_flash_read_indirect(uint32_t abs_addr, uint8_t *buf, size_t len);
int stm32n6_external_flash_end_write(void);
int stm32n6_external_flash_nor_test(struct stm32n6_nor_test_result *result);

#endif
