#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#define USE_HAL_DRIVER
#include "stm32n6xx_hal.h"

#include "stm32n6_external_flash.h"

LOG_MODULE_REGISTER(stm32n6_npu_preinit, LOG_LEVEL_INF);

/* The N6 clock and RAM gates need to settle before the next dependent access. */
#define PREINIT_TRACE(message) \
	do { \
		LOG_DBG("%s", message); \
	} while (0)

static bool g_preinit_done;
static bool g_xspi_preinit_done;

/* Required by the ST NPU runtime before HAL_CACHEAXI_Init(). */
void npu_cache_enable_clocks_and_reset(void)
{
	__HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
	__HAL_RCC_CACHEAXI_CLK_ENABLE();
	__HAL_RCC_CACHEAXI_FORCE_RESET();
	__HAL_RCC_CACHEAXI_RELEASE_RESET();
}

static uint32_t get_risaf_max_addr(RISAF_TypeDef *risaf)
{
	if ((risaf == RISAF2_S) || (risaf == RISAF2_NS)) {
		return RISAF2_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF3_S) || (risaf == RISAF3_NS)) {
		return RISAF3_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF4_S) || (risaf == RISAF4_NS)) {
		return RISAF4_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF5_S) || (risaf == RISAF5_NS)) {
		return RISAF5_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF6_S) || (risaf == RISAF6_NS)) {
		return RISAF6_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF7_S) || (risaf == RISAF7_NS)) {
		return RISAF7_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF8_S) || (risaf == RISAF8_NS)) {
		return RISAF8_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF11_S) || (risaf == RISAF11_NS)) {
		return RISAF11_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF12_S) || (risaf == RISAF12_NS)) {
		return RISAF12_LIMIT_ADDRESS_SPACE_SIZE;
	}
	if ((risaf == RISAF15_S) || (risaf == RISAF15_NS)) {
		return RISAF15_LIMIT_ADDRESS_SPACE_SIZE;
	}
	return 0U;
}

static void set_risaf_default(RISAF_TypeDef *risaf)
{
	RISAF_BaseRegionConfig_t risaf_conf = {0};

	risaf_conf.StartAddress = 0x0;
	risaf_conf.EndAddress = get_risaf_max_addr(risaf);
	risaf_conf.Filtering = RISAF_FILTER_ENABLE;
	risaf_conf.PrivWhitelist = RIF_CID_NONE;
	risaf_conf.ReadWhitelist = RIF_CID_MASK;
	risaf_conf.WriteWhitelist = RIF_CID_MASK;

	risaf_conf.Secure = RIF_ATTRIBUTE_SEC;
	HAL_RIF_RISAF_ConfigBaseRegion(risaf, 0, &risaf_conf);

	risaf_conf.Secure = RIF_ATTRIBUTE_NSEC;
	HAL_RIF_RISAF_ConfigBaseRegion(risaf, 1, &risaf_conf);
}

static void preinit_common_clocks_and_memory(void)
{
	LOG_DBG("pre-init: clocks");
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	__HAL_RCC_CRC_CLK_ENABLE();
	__HAL_RCC_CACHEAXI_CLK_ENABLE();
	__HAL_RCC_XSPIM_CLK_ENABLE();
	__HAL_RCC_XSPIPHYCOMP_CLK_ENABLE();
	__HAL_RCC_XSPIPHYCOMP_CLK_SLEEP_ENABLE();
	__HAL_RCC_XSPI2_CLK_ENABLE();
	__HAL_RCC_RIFSC_CLK_ENABLE();
	__HAL_RCC_RISAF_CLK_ENABLE();
	__HAL_RCC_IAC_CLK_ENABLE();
	__HAL_RCC_RIFSC_CLK_SLEEP_ENABLE();
	__HAL_RCC_RISAF_CLK_SLEEP_ENABLE();
	__HAL_RCC_IAC_CLK_SLEEP_ENABLE();
	__HAL_RCC_XSPIM_CLK_SLEEP_ENABLE();
	__HAL_RCC_XSPI2_CLK_SLEEP_ENABLE();
	__HAL_RCC_CACHEAXI_CLK_SLEEP_ENABLE();
	__HAL_RCC_NPU_CLK_SLEEP_ENABLE();
	__HAL_RCC_AXISRAM3_MEM_CLK_SLEEP_ENABLE();
	__HAL_RCC_AXISRAM4_MEM_CLK_SLEEP_ENABLE();
	__HAL_RCC_AXISRAM5_MEM_CLK_SLEEP_ENABLE();
	__HAL_RCC_AXISRAM6_MEM_CLK_SLEEP_ENABLE();
	__HAL_RCC_CACHEAXIRAM_MEM_CLK_SLEEP_ENABLE();

	LOG_DBG("pre-init: memenr");
	RCC->MEMENR |= RCC_MEMENR_AXISRAM3EN | RCC_MEMENR_AXISRAM4EN |
		       RCC_MEMENR_AXISRAM5EN | RCC_MEMENR_AXISRAM6EN;
	RCC->MEMENR |= RCC_MEMENR_CACHEAXIRAMEN;

	LOG_DBG("pre-init: ramcfg");
	RAMCFG_SRAM2_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;

	LOG_DBG("pre-init: memsysctl");
	MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_DCACTIVE_Msk | MEMSYSCTL_MSCR_ICACTIVE_Msk;
}

static void preinit_common_risaf(void)
{
	LOG_DBG("pre-init: risaf defaults");
	LOG_DBG("pre-init: risaf2");
	set_risaf_default(RISAF2_S);
	LOG_DBG("pre-init: risaf2 done");
	LOG_DBG("pre-init: risaf3");
	set_risaf_default(RISAF3_S);
	LOG_DBG("pre-init: risaf3 done");
	LOG_DBG("pre-init: risaf4");
	set_risaf_default(RISAF4_S);
	LOG_DBG("pre-init: risaf4 done");
	LOG_DBG("pre-init: risaf5");
	set_risaf_default(RISAF5_S);
	LOG_DBG("pre-init: risaf5 done");
	LOG_DBG("pre-init: risaf6");
	set_risaf_default(RISAF6_S);
	LOG_DBG("pre-init: risaf6 done");
	LOG_DBG("pre-init: risaf7");
	set_risaf_default(RISAF7_S);
	LOG_DBG("pre-init: risaf7 done");
	LOG_DBG("pre-init: risaf8");
	set_risaf_default(RISAF8_S);
	LOG_DBG("pre-init: risaf8 done");
	LOG_DBG("pre-init: risaf11/12 skipped");
}

int stm32n6_xspi_pre_init(void)
{
	if (g_xspi_preinit_done) {
		return 0;
	}

	LOG_DBG("xspi-pre-init begin");
	preinit_common_clocks_and_memory();
	LOG_DBG("xspi-pre-init: risaf skipped");

	LOG_DBG("xspi-pre-init: risc xspi");
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI2,
					      RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
	LOG_DBG("xspi-pre-init: risc xspi2 done");

	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPIM,
					      RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
	LOG_DBG("xspi-pre-init: risc xspim done");

	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_CACHEAXIRAM,
					      RIF_ATTRIBUTE_SEC);
	LOG_DBG("xspi-pre-init: risc cacheaxiram done");

	LOG_DBG("xspi-pre-init: external flash");
	if (stm32n6_external_flash_init() != 0) {
		LOG_ERR("xspi-pre-init: external flash init failed");
		return -1;
	}

	g_xspi_preinit_done = true;
	LOG_DBG("xspi-pre-init done");
	return 0;
}

int stm32n6_npu_pre_init(void)
{
	RIMC_MasterConfig_t master_conf = {0};

	if (g_preinit_done) {
		return 0;
	}

	/* This runs before camera/LTDC DMA starts and establishes the NPU access domain. */
	PREINIT_TRACE("pre-init: NPU, xSPI, and AXISRAM access setup");
	preinit_common_clocks_and_memory();

	PREINIT_TRACE("pre-init: reset NPU");
	__HAL_RCC_NPU_CLK_ENABLE();
	__HAL_RCC_NPU_FORCE_RESET();
	__HAL_RCC_NPU_RELEASE_RESET();

	PREINIT_TRACE("pre-init: configure NPU master access");
	master_conf.MasterCID = RIF_CID_1;
	master_conf.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
	HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &master_conf);

	PREINIT_TRACE("pre-init: configure NPU and xSPI peripherals");
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU,
					      RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPI2,
					      RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_XSPIM,
					      RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_CACHEAXIRAM,
					      RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM0,
					      RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM1,
					      RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM2,
					      RIF_ATTRIBUTE_SEC);
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RCC_PERIPH_INDEX_NPURAM3,
					      RIF_ATTRIBUTE_SEC);

	/*
	 * The application executes directly from the xSPI2 NOR image. MCUboot has
	 * already configured the controller's memory-mapped mode; reinitializing it
	 * here would cut off instruction fetches from the running application.
	 */
	PREINIT_TRACE("pre-init: preserve MCUboot xSPI memory map");

	PREINIT_TRACE("pre-init: ensure sleep clocks remain available");
	PREINIT_TRACE("pre-init: enable XSPIM clock");
	__HAL_RCC_XSPIM_CLK_ENABLE();
	PREINIT_TRACE("pre-init: XSPIM clock enabled");
	PREINIT_TRACE("pre-init: enable XSPI2 clock");
	__HAL_RCC_XSPI2_CLK_ENABLE();
	PREINIT_TRACE("pre-init: XSPI2 clock enabled");
	PREINIT_TRACE("pre-init: enable XSPIM sleep clock");
	__HAL_RCC_XSPIM_CLK_SLEEP_ENABLE();
	PREINIT_TRACE("pre-init: XSPIM sleep clock enabled");
	PREINIT_TRACE("pre-init: enable XSPIPHYCOMP sleep clock");
	__HAL_RCC_XSPIPHYCOMP_CLK_SLEEP_ENABLE();
	PREINIT_TRACE("pre-init: XSPIPHYCOMP sleep clock enabled");
	PREINIT_TRACE("pre-init: enable XSPI2 sleep clock");
	__HAL_RCC_XSPI2_CLK_SLEEP_ENABLE();
	PREINIT_TRACE("pre-init: XSPI2 sleep clock enabled");

	/* Generated YuNet buffers occupy AXISRAM3 through AXISRAM6. */
	PREINIT_TRACE("pre-init: enable AXISRAM3-6 and CACHEAXIRAM");
	RCC->MEMENR |= RCC_MEMENR_AXISRAM3EN | RCC_MEMENR_AXISRAM4EN |
		       RCC_MEMENR_AXISRAM5EN | RCC_MEMENR_AXISRAM6EN |
		       RCC_MEMENR_CACHEAXIRAMEN;
	PREINIT_TRACE("pre-init: AXISRAM banks enabled");
	PREINIT_TRACE("pre-init: wake AXISRAM3");
	RAMCFG_SRAM3_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	PREINIT_TRACE("pre-init: wake AXISRAM4");
	RAMCFG_SRAM4_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	PREINIT_TRACE("pre-init: wake AXISRAM5");
	RAMCFG_SRAM5_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	PREINIT_TRACE("pre-init: wake AXISRAM6");
	RAMCFG_SRAM6_AXI->CR &= ~RAMCFG_CR_SRAMSD;
	PREINIT_TRACE("pre-init: AXISRAM3-6 awake");
	PREINIT_TRACE("pre-init: enable CACHEAXI");
	MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_DCACTIVE_Msk | MEMSYSCTL_MSCR_ICACTIVE_Msk;
	PREINIT_TRACE("pre-init: CACHEAXI enabled");

	g_preinit_done = true;
	PREINIT_TRACE("pre-init done");

	return 0;
}
