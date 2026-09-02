#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define USE_HAL_DRIVER
#include "stm32n6xx_hal.h"

#include "stm32n6570_discovery_xspi.h"
#include "stm32n6_external_flash.h"

LOG_MODULE_REGISTER(stm32n6_external_flash, LOG_LEVEL_INF);

static bool external_flash_ready;
static bool external_flash_mmap_ready;
static bool external_flash_write_active;
K_MUTEX_DEFINE(external_flash_mutex);

/* Zephyr owns XSPI1 PSRAM for camera and LTDC buffers.  The NPU only needs
 * XSPI2 NOR, so do not reconfigure the live PSRAM controller from the BSP. */
#define STM32N6_EXTERNAL_FLASH_INIT_PSRAM 0

static uint32_t stm32n6_fnv1a32(const uint8_t *data, size_t len)
{
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0U; i < len; ++i) {
		hash ^= data[i];
		hash *= 16777619u;
	}

	return hash;
}

static int stm32n6_external_flash_ensure_ready(void)
{
	if (external_flash_ready) {
		return 0;
	}
	if (IS_ENABLED(CONFIG_XIP)) {
		/* XIP fetches instructions through this same memory-mapped XSPI2
		 * session; force-resetting it here (as init() does) hangs the CPU. */
		return -ENOTSUP;
	}

	return stm32n6_external_flash_init();
}

int stm32n6_external_flash_init(void)
{
	BSP_XSPI_NOR_Init_t flash = {
		.InterfaceMode = MX66UW1G45G_OPI_MODE,
		.TransferRate = MX66UW1G45G_DTR_TRANSFER,
	};
	int rc;

	LOG_DBG("external flash init begin");

	/* MCUboot (RAM_LOAD) hands off with the XSPI2 peripheral still busy
	 * servicing the memory-mapped session it used to copy this image out
	 * of flash. HAL_XSPI_Init() (called from MX_XSPI_NOR_Init) waits on
	 * the controller's BUSY flag before it can reconfigure anything, and
	 * that flag never clears on its own -- it just times out after ~5s,
	 * matching the observed BSP_ERROR_PERIPH_FAILURE (-4). Disabling
	 * memory-mapped mode first does not help here because hxspi_nor[0]
	 * is not populated yet (Init() has not run), so that HAL call is a
	 * no-op on an empty handle. Force-reset the XSPI2 peripheral itself
	 * at the RCC level instead -- this clears BUSY unconditionally,
	 * independent of HAL handle state.
	 */
	__HAL_RCC_XSPI2_FORCE_RESET();
	__HAL_RCC_XSPI2_RELEASE_RESET();

#if STM32N6_EXTERNAL_FLASH_INIT_PSRAM
	rc = BSP_XSPI_RAM_Init(0);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_RAM_Init failed: %d", rc);
		return rc;
	}

	rc = BSP_XSPI_RAM_EnableMemoryMappedMode(0);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_RAM_EnableMemoryMappedMode failed: %d", rc);
		return rc;
	}
#endif

	rc = BSP_XSPI_NOR_Init(0, &flash);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_Init failed: %d", rc);
		return rc;
	}

	rc = BSP_XSPI_NOR_EnableMemoryMappedMode(0);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_EnableMemoryMappedMode failed: %d", rc);
		return rc;
	}

	LOG_DBG("external NOR ready in SPI STR memory-mapped mode");
	external_flash_ready = true;
	external_flash_mmap_ready = true;
	return 0;
}

int stm32n6_external_flash_read(uint32_t abs_addr, uint8_t *buf, size_t len)
{
	uint32_t offset;
	int rc;

	if (buf == NULL || len == 0U) {
		return -EINVAL;
	}

	if (abs_addr < STM32N6_NOR_BASE_ADDR) {
		return -EINVAL;
	}

	rc = stm32n6_external_flash_ensure_ready();
	if (rc < 0) {
		return rc;
	}

	if (external_flash_mmap_ready) {
		memcpy(buf, (const void *)(uintptr_t)abs_addr, len);
		return 0;
	}

	offset = abs_addr - STM32N6_NOR_BASE_ADDR;
	rc = BSP_XSPI_NOR_Read(0, buf, offset, (uint32_t)len);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_Read failed: %d offset=0x%08x len=%u",
			rc, (unsigned int)offset, (unsigned int)len);
		return rc;
	}

	return 0;
}

int stm32n6_external_flash_begin_write(void)
{
	int rc;

	k_mutex_lock(&external_flash_mutex, K_FOREVER);
	if (external_flash_write_active) {
		k_mutex_unlock(&external_flash_mutex);
		return -EBUSY;
	}
	rc = stm32n6_external_flash_ensure_ready();
	if (rc != 0) {
		k_mutex_unlock(&external_flash_mutex);
		return rc;
	}
	if (external_flash_mmap_ready) {
		rc = BSP_XSPI_NOR_DisableMemoryMappedMode(0);
		if (rc != BSP_ERROR_NONE) {
			LOG_ERR("BSP_XSPI_NOR_DisableMemoryMappedMode failed: %d", rc);
			k_mutex_unlock(&external_flash_mutex);
			return rc;
		}
		external_flash_mmap_ready = false;
	}
	external_flash_write_active = true;
	return 0;
}

int stm32n6_external_flash_erase_64k(uint32_t abs_addr)
{
	int rc;

	if (!external_flash_write_active || abs_addr < STM32N6_NOR_BASE_ADDR ||
	    ((abs_addr - STM32N6_NOR_BASE_ADDR) % STM32N6_NOR_TEST_BLOCK_LEN) != 0U) {
		return -EINVAL;
	}
	rc = BSP_XSPI_NOR_Erase_Block(0, abs_addr - STM32N6_NOR_BASE_ADDR,
				      BSP_XSPI_NOR_ERASE_64K);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_Erase_Block failed: %d address=0x%08x", rc,
			(unsigned int)abs_addr);
	}
	return rc;
}

int stm32n6_external_flash_write(uint32_t abs_addr, const uint8_t *buf, size_t len)
{
	int rc;

	if (!external_flash_write_active || buf == NULL || len == 0U ||
	    abs_addr < STM32N6_NOR_BASE_ADDR || len > UINT32_MAX) {
		return -EINVAL;
	}
	rc = BSP_XSPI_NOR_Write(0, buf, abs_addr - STM32N6_NOR_BASE_ADDR, (uint32_t)len);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_Write failed: %d address=0x%08x length=%u", rc,
			(unsigned int)abs_addr, (unsigned int)len);
	}
	return rc;
}

int stm32n6_external_flash_read_indirect(uint32_t abs_addr, uint8_t *buf, size_t len)
{
	int rc;

	if (!external_flash_write_active || buf == NULL || len == 0U ||
	    abs_addr < STM32N6_NOR_BASE_ADDR || len > UINT32_MAX) {
		return -EINVAL;
	}
	rc = BSP_XSPI_NOR_Read(0, buf, abs_addr - STM32N6_NOR_BASE_ADDR, (uint32_t)len);
	return rc == BSP_ERROR_NONE ? 0 : rc;
}

int stm32n6_external_flash_end_write(void)
{
	int rc = 0;

	if (!external_flash_write_active) {
		return -EINVAL;
	}
	rc = BSP_XSPI_NOR_EnableMemoryMappedMode(0);
	if (rc != BSP_ERROR_NONE) {
		LOG_ERR("BSP_XSPI_NOR_EnableMemoryMappedMode failed: %d", rc);
	} else {
		external_flash_mmap_ready = true;
	}
	external_flash_write_active = false;
	k_mutex_unlock(&external_flash_mutex);
	return rc;
}

int stm32n6_external_flash_nor_test(struct stm32n6_nor_test_result *result)
{
	uint8_t tx[STM32N6_NOR_TEST_DATA_LEN];
	uint8_t rx[STM32N6_NOR_TEST_DATA_LEN];
	BSP_XSPI_NOR_Diag_t diag;
	uint32_t offset;
	uint32_t i;
	int rc;

	if (result == NULL) {
		return -EINVAL;
	}

	memset(result, 0, sizeof(*result));
	result->abs_addr = STM32N6_NOR_TEST_ABS_ADDR;
	result->offset = STM32N6_NOR_TEST_ABS_ADDR - STM32N6_NOR_BASE_ADDR;
	result->size = STM32N6_NOR_TEST_DATA_LEN;
	result->first_bad_offset = UINT32_MAX;

	for (i = 0U; i < STM32N6_NOR_TEST_DATA_LEN; ++i) {
		tx[i] = (uint8_t)((i * 37U) + 11U);
	}
	result->expected_crc32 = stm32n6_fnv1a32(tx, sizeof(tx));

	rc = stm32n6_external_flash_ensure_ready();
	if (rc < 0) {
		result->erase_rc = rc;
		return rc;
	}

	if (external_flash_mmap_ready) {
		return -EBUSY;
	}

	offset = result->offset;
	rc = BSP_XSPI_NOR_TestIO(0, offset, BSP_XSPI_NOR_ERASE_64K, tx, rx, sizeof(tx), &diag);
	result->erase_rc = rc;
	result->auto_poll_rc = diag.auto_poll_rc;
	result->write_enable_erase_rc = diag.write_enable_erase_rc;
	result->block_erase_rc = diag.block_erase_rc;
	result->auto_poll_after_erase_rc = diag.auto_poll_after_erase_rc;
	result->write_enable_program_rc = diag.write_enable_program_rc;
	result->page_program_rc = diag.page_program_rc;
	result->auto_poll_after_program_rc = diag.auto_poll_after_program_rc;
	result->direct_read_rc = diag.read_rc;
	if (rc != BSP_ERROR_NONE) {
		return rc;
	}
	result->write_rc = diag.page_program_rc == MX66UW1G45G_OK ? BSP_ERROR_NONE : BSP_ERROR_COMPONENT_FAILURE;
	result->read_rc = diag.read_rc == MX66UW1G45G_OK ? BSP_ERROR_NONE : BSP_ERROR_COMPONENT_FAILURE;

	result->actual_crc32 = stm32n6_fnv1a32(rx, sizeof(rx));

	for (i = 0U; i < STM32N6_NOR_TEST_DATA_LEN; ++i) {
		if (rx[i] != tx[i]) {
			++result->mismatch_count;
			if (result->first_bad_offset == UINT32_MAX) {
				result->first_bad_offset = i;
			}
		}
	}

	if (result->first_bad_offset == UINT32_MAX) {
		result->first_bad_offset = 0U;
	}

	return (result->mismatch_count == 0U) ? 0 : -EIO;
}

/* BSP_XSPI_NOR_DisableMemoryMappedMode() -> HAL_XSPI_Abort() references this
 * even though our XSPI2 NOR usage is polling-only (no DMA configured). */
__weak HAL_StatusTypeDef HAL_DMA_Abort(DMA_HandleTypeDef *const hdma)
{
	ARG_UNUSED(hdma);
	return HAL_OK;
}
