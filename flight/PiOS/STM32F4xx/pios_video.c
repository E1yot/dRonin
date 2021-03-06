/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_VIDEO Code for OSD video generator
 * @brief Output video (black & white pixels) over SPI
 * @{
 *
 * @file       pios_video.c
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013-2014
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010-2014.
 * @brief      OSD gen module, handles OSD draw. Parts from CL-OSD and SUPEROSD projects
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#include "pios_config.h"

#if defined(PIOS_INCLUDE_VIDEO)

#include "pios.h"
#include "pios_video.h"
#include "pios_semaphore.h"

// How many frames until we redraw
#define VSYNC_REDRAW_CNT 2

#ifndef PIOS_VIDEO_HSYNC_OFFSET
#define PIOS_VIDEO_HSYNC_OFFSET 0
#endif

extern struct pios_semaphore * onScreenDisplaySemaphore;

static const struct pios_video_type_boundary pios_video_type_boundary_ntsc = {
	.graphics_right  = 351,         // must be: graphics_width_real - 1
	.graphics_bottom = 239,         // must be: graphics_height_real - 1
};

static const struct pios_video_type_boundary pios_video_type_boundary_pal = {
	.graphics_right  = 359,         // must be: graphics_width_real - 1
	.graphics_bottom = 265,         // must be: graphics_height_real - 1
};

static const struct pios_video_type_cfg pios_video_type_cfg_ntsc = {
	.graphics_height_real   = 240,   // Real visible lines
	.graphics_column_start = 103,   // First visible OSD column (after Hsync)
	.graphics_line_start   = 19,    // First visible OSD line
	.dma_buffer_length     = 45,    // DMA buffer length in bytes (graphics_right / 8 + 1)
	.period = 24,
	.dc     = 12,
};

static const struct pios_video_type_cfg pios_video_type_cfg_pal = {
	.graphics_height_real   = 266,   // Real visible lines
	.graphics_column_start = 149,   // First visible OSD column (after Hsync)
	.graphics_line_start   = 28,    // First visible OSD line
	.dma_buffer_length     = 46,    // DMA buffer length in bytes ((graphics_right + 1) / 8 + 1)
	.period = 22,
	.dc     = 11,
};

// Allocate buffers.
// Must be allocated in one block, so it is in a struct.
struct _buffers {
	uint8_t buffer0_level[BUFFER_HEIGHT * BUFFER_WIDTH];
	uint8_t buffer0_mask[BUFFER_HEIGHT * BUFFER_WIDTH];
	uint8_t buffer1_level[BUFFER_HEIGHT * BUFFER_WIDTH];
	uint8_t buffer1_mask[BUFFER_HEIGHT * BUFFER_WIDTH];
} buffers;

// Remove the struct definition (makes it easier to write for).
#define buffer0_level (buffers.buffer0_level)
#define buffer0_mask  (buffers.buffer0_mask)
#define buffer1_level (buffers.buffer1_level)
#define buffer1_mask  (buffers.buffer1_mask)

// Pointers to each of these buffers.
uint8_t *draw_buffer_level;
uint8_t *draw_buffer_mask;
uint8_t *disp_buffer_level;
uint8_t *disp_buffer_mask;

volatile int16_t active_line = 10000; 

const struct pios_video_type_boundary *pios_video_type_boundary_act = &pios_video_type_boundary_pal;


// Private variables
static int8_t x_offset = 0;
static int8_t x_offset_new = 0;
static int8_t y_offset = 0;
static const struct pios_video_cfg *dev_cfg = NULL;
static uint16_t num_video_lines = 0;
static enum pios_video_system video_system_act = PIOS_VIDEO_SYSTEM_NONE;
static const struct pios_video_type_cfg *pios_video_type_cfg_act = &pios_video_type_cfg_pal;

static uint16_t line_int_flag;

// Private functions
static void swap_buffers();
static void prepare_line();
static void vid_disable_spis();

/**
 * @brief Vsync interrupt service routine
 */
bool PIOS_Vsync_ISR()
{
	static uint16_t Vsync_update = 0;

	// discard spurious vsync pulses (due to improper grounding), so we don't overload the CPU
	if (active_line < pios_video_type_cfg_ntsc.graphics_height_real - 10) {
		active_line = - (pios_video_type_cfg_act->graphics_line_start + y_offset);
		return false;
	}

	// Update the number of video lines
	num_video_lines = active_line + 
		(pios_video_type_cfg_act->graphics_line_start + y_offset);

	enum pios_video_system video_system_tmp;

	static uint8_t mode_hysteresis = 0;

	// check video type
	if (num_video_lines > VIDEO_TYPE_PAL_ROWS) {
		video_system_tmp = PIOS_VIDEO_SYSTEM_PAL;
	} else {
		video_system_tmp = PIOS_VIDEO_SYSTEM_NTSC;
	}

	// if video type has changed set new active values
	if ((video_system_act != video_system_tmp) && (mode_hysteresis++ > 10)) {
		video_system_act = video_system_tmp;
		if (video_system_act == PIOS_VIDEO_SYSTEM_NTSC) {
			pios_video_type_boundary_act = &pios_video_type_boundary_ntsc;
			pios_video_type_cfg_act = &pios_video_type_cfg_ntsc;
		} else {
			pios_video_type_boundary_act = &pios_video_type_boundary_pal;
			pios_video_type_cfg_act = &pios_video_type_cfg_pal;
		}

		if (dev_cfg->pixel_timer.timer == TIM9) { // XXX or other fast timers
			dev_cfg->pixel_timer.timer->CCR1 = pios_video_type_cfg_act->dc;
			dev_cfg->pixel_timer.timer->ARR  = pios_video_type_cfg_act->period - 1;
		} else {
			dev_cfg->pixel_timer.timer->CCR1 = pios_video_type_cfg_act->dc / 2;
			dev_cfg->pixel_timer.timer->ARR  = pios_video_type_cfg_act->period / 2 - 1;
		}

		x_offset = -100;	/* Force recalc */
	} else if (video_system_act == video_system_tmp) {
		mode_hysteresis = 0;
	}

	if (x_offset != x_offset_new)
	{
		x_offset = x_offset_new;
		dev_cfg->hsync_capture.timer->ARR = (pios_video_type_cfg_act->dc * (pios_video_type_cfg_act->graphics_column_start + x_offset)) / 2;
	}

	bool woken = false;

	// Every VSYNC_REDRAW_CNT field: swap buffers and trigger redraw
	if (++Vsync_update >= VSYNC_REDRAW_CNT) {
		Vsync_update = 0;
		swap_buffers();

		PIOS_Semaphore_Give_FromISR(onScreenDisplaySemaphore, &woken);
	}

	// Get ready for the first line
	active_line = - (pios_video_type_cfg_act->graphics_line_start + y_offset);

#ifdef PIOS_INCLUDE_WS2811
#ifdef SYSTEMMOD_RGBLED_VIDEO_HACK
	PIOS_WS2811_trigger_update(pios_ws2811);
#endif
#endif

	return woken;
}


void PIOS_Line_ISR(void);
void TIM2_IRQHandler(void) __attribute__((alias("PIOS_Line_ISR")));

/**
 * ISR Triggered by hsync cap, starts clocking out pixels for first visible OSD line
 */
void PIOS_Line_ISR(void)
{
	/* What this looks like:
	 * - Vsync int sets active line to a negative value for the number of
	 *   ignored lines.
	 * - Line ISR increments it each time we're called; when it reaches 0,
	 *   we disable ourselves and cue the DMA engine.  DMA engine is
	 *   responsible for counting lines.
	 * - When DMA engine is done, it re-enables us and we keep counting.
	 */
	if(TIM_GetITStatus(dev_cfg->hsync_capture.timer, line_int_flag))
	{
		TIM_ClearITPendingBit(dev_cfg->hsync_capture.timer,
				line_int_flag);

		if (active_line > 10000) {
			// Don't wrap.
			return;
		}

		if (active_line == 0) {
			// Prepare the first line
			prepare_line();

			return;
		}

		active_line++;
	}
}

void PIOS_VIDEO_DMA_Handler(void);
void DMA2_Stream3_IRQHandler(void) __attribute__((alias("PIOS_VIDEO_DMA_Handler")));
void DMA1_Stream4_IRQHandler(void) __attribute__((alias("PIOS_VIDEO_DMA_Handler")));

static void vid_disable_spis()
{
		// Disable the SPI, makes sure the pins are LOW
		dev_cfg->mask.regs->CR1 &= (uint16_t)~SPI_CR1_SPE;
		dev_cfg->level.regs->CR1 &= (uint16_t)~SPI_CR1_SPE;

		// Stop pixel timer
		dev_cfg->pixel_timer.timer->CR1  &= (uint16_t) ~TIM_CR1_CEN;
}

/**
 * DMA transfer complete interrupt handler
 * Note: This function is called for every line (~13k times / s), so we use direct register access for
 * efficiency
 */
void PIOS_VIDEO_DMA_Handler(void)
{	
	// Handle flags from DMA stream channel
	if ((dev_cfg->mask_dma->LISR & DMA_FLAG_TCIF3) && (dev_cfg->level_dma->HISR & DMA_FLAG_TCIF4)) {
		// Clear the DMA interrupt flags
		dev_cfg->mask_dma->LIFCR  |= DMA_FLAG_TCIF3;
		dev_cfg->level_dma->HIFCR |= DMA_FLAG_TCIF4;

		dev_cfg->mask.dma.tx.channel->CR  &= ~(uint32_t)DMA_SxCR_EN;
		dev_cfg->level.dma.tx.channel->CR &= ~(uint32_t)DMA_SxCR_EN;

		/* Wait for SPIs to be done */
		while ((dev_cfg->level.regs->SR & SPI_I2S_FLAG_TXE) == 0);
		while (dev_cfg->level.regs->SR & SPI_I2S_FLAG_BSY);

		// Disable SSEL to control phase
		dev_cfg->level.regs->CR1 |=   SPI_CR1_SSI;

		while ((dev_cfg->mask.regs->SR & SPI_I2S_FLAG_TXE) == 0);
		while (dev_cfg->mask.regs->SR & SPI_I2S_FLAG_BSY);

		dev_cfg->mask.regs->CR1  |=   SPI_CR1_SSI;

		vid_disable_spis();

		if (active_line < pios_video_type_cfg_act->graphics_height_real) { // lines existing
			prepare_line();
		} else { // last line completed
			TIM_ITConfig(dev_cfg->hsync_capture.timer, line_int_flag, ENABLE);
			// Disable the pixel timer slave mode configuration
			dev_cfg->pixel_timer.timer->SMCR &= (uint16_t) ~TIM_SMCR_SMS;
		}
	}
}

/**
 * Prepare the system to watch for a Hsync pulse to trigger the pixel clock and clock out the next line
 * Note: This function is called for every line (~13k times / s), so we use direct register access for
 * efficiency
 */
static inline void prepare_line()
{
	TIM_ITConfig(dev_cfg->hsync_capture.timer, line_int_flag, DISABLE);

	uint32_t buf_offset = active_line * BUFFER_WIDTH;

	// Set initial value
	dev_cfg->pixel_timer.timer->CNT   = 0;

	// Reset the SMS bits
	dev_cfg->pixel_timer.timer->SMCR &= (uint16_t) ~TIM_SMCR_SMS;
	dev_cfg->pixel_timer.timer->SMCR |= TIM_SlaveMode_Trigger;

	// Load new line
	dev_cfg->mask.dma.tx.channel->M0AR  = (uint32_t)&disp_buffer_mask[buf_offset];
	dev_cfg->level.dma.tx.channel->M0AR = (uint32_t)&disp_buffer_level[buf_offset];
	// Set length
	dev_cfg->mask.dma.tx.channel->NDTR  = (uint16_t)pios_video_type_cfg_act->dma_buffer_length;
	dev_cfg->level.dma.tx.channel->NDTR = (uint16_t)pios_video_type_cfg_act->dma_buffer_length;

	// Enable SPI
	dev_cfg->mask.regs->CR1  |= SPI_CR1_SPE;
	dev_cfg->level.regs->CR1 |= SPI_CR1_SPE;

	// Enable DMA
	dev_cfg->mask.dma.tx.channel->CR  |= (uint32_t)DMA_SxCR_EN;
	dev_cfg->level.dma.tx.channel->CR |= (uint32_t)DMA_SxCR_EN;

	// Advance line counter
	active_line++;

	dev_cfg->mask.regs->CR1  &= (uint16_t) ~ SPI_CR1_SSI;
	dev_cfg->level.regs->CR1 &= (uint16_t) ~ SPI_CR1_SSI;
}


/**
 * swap_buffers: Swaps the two buffers. Contents in the display
 * buffer is seen on the output and the display buffer becomes
 * the new draw buffer.
 */
static void swap_buffers()
{
	// While we could use XOR swap this is more reliable and
	// dependable and it's only called a few times per second.
	// Many compilers should optimize these to EXCH instructions.
	uint8_t *tmp;

	SWAP_BUFFS(tmp, disp_buffer_mask, draw_buffer_mask);
	SWAP_BUFFS(tmp, disp_buffer_level, draw_buffer_level);
}

/**
 * Init
 */
void PIOS_Video_Init(const struct pios_video_cfg *cfg)
{
	TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
	dev_cfg = cfg; // store config before enabling interrupt

	// This code currently only works with SPI1 and SPI2, catch misconfigurations here
	if ((cfg->mask.regs != SPI1) && (cfg->mask.regs != SPI2))
		PIOS_Assert(0);

	if ((cfg->level.regs != SPI1) && (cfg->level.regs != SPI2))
		PIOS_Assert(0);

	// SPI for maskbuffer
	GPIO_Init(cfg->mask.sclk.gpio, (GPIO_InitTypeDef *)&(cfg->mask.sclk.init));
	GPIO_Init(cfg->mask.miso.gpio, (GPIO_InitTypeDef *)&(cfg->mask.miso.init));
	if (cfg->mask.remap) {
		GPIO_PinAFConfig(cfg->mask.sclk.gpio, __builtin_ctz(cfg->mask.sclk.init.GPIO_Pin), cfg->mask.remap);
		GPIO_PinAFConfig(cfg->mask.miso.gpio, __builtin_ctz(cfg->mask.miso.init.GPIO_Pin), cfg->mask.remap);
	}

	// SPI for levelbuffer
	GPIO_Init(cfg->level.sclk.gpio, (GPIO_InitTypeDef *)&(cfg->level.sclk.init));
	GPIO_Init(cfg->level.miso.gpio, (GPIO_InitTypeDef *)&(cfg->level.miso.init));
	if (cfg->level.remap) {
		GPIO_PinAFConfig(cfg->level.sclk.gpio, __builtin_ctz(cfg->level.sclk.init.GPIO_Pin), cfg->level.remap);
		GPIO_PinAFConfig(cfg->level.miso.gpio, __builtin_ctz(cfg->level.miso.init.GPIO_Pin), cfg->level.remap);
	}

	// HSYNC captrue timer: Start counting at HSYNC and start pixel timer after at correct x-position
	GPIO_Init(cfg->hsync_capture.pin.gpio, (GPIO_InitTypeDef *)&(cfg->hsync_capture.pin.init));
	if (cfg->hsync_capture.remap) {
		GPIO_PinAFConfig(cfg->hsync_capture.pin.gpio, __builtin_ctz(cfg->hsync_capture.pin.init.GPIO_Pin), cfg->hsync_capture.remap);
	}

	TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
	TIM_TimeBaseStructure.TIM_Period = pios_video_type_cfg_act->dc * (pios_video_type_cfg_act->graphics_column_start + x_offset) / 2;
	TIM_TimeBaseStructure.TIM_Prescaler = 0;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(cfg->hsync_capture.timer, &TIM_TimeBaseStructure);

	TIM_SelectOnePulseMode(cfg->hsync_capture.timer, TIM_OPMode_Single);
	TIM_SelectSlaveMode(cfg->hsync_capture.timer, TIM_SlaveMode_Trigger);

	uint16_t tmpccer = cfg->hsync_capture.timer->CCER;

#ifdef PIOS_VIDEO_HSYNC_FALLING_EDGE
	/* Unfortunately not really a stdperiph function for this. */
	if (cfg->hsync_capture.timer_chan == TIM_Channel_1) {
		tmpccer &= (uint16_t)~(TIM_CCER_CC1NP);
		tmpccer |= (uint16_t)(TIM_CCER_CC1P);
	} else if (cfg->hsync_capture.timer_chan == TIM_Channel_2) {
		tmpccer &= (uint16_t)~(TIM_CCER_CC2NP);
		tmpccer |= (uint16_t)(TIM_CCER_CC2P);
	}
#endif

#ifdef PIOS_VIDEO_INPUT_FILTER
	if (cfg->hsync_capture.timer_chan == TIM_Channel_1) {
		tmpccer &= ((uint16_t)~TIM_CCMR1_IC1F);
		tmpccer |= 8 << 4;
		/* 8 = Fdts/8, N=6.  APB1=42MHz, so the prescaled clock input
		 * should be double that (84 MHz).
		 *
		 * 84MHz / 8 = 10.5Mhz... 6 / 10.5MHz = 0.57us
		 * require a steady value, different from the previous value
		 * for ~half a microsecond before accepting a hsync clock
		 * trigger edge. 
		 */
	} else if (cfg->hsync_capture.timer_chan == TIM_Channel_2) {
		tmpccer &= ((uint16_t)~TIM_CCMR1_IC2F);
		tmpccer |= 8 << 12;
	}
#endif

	cfg->hsync_capture.timer->CCER = tmpccer;

	if (cfg->hsync_capture.timer_chan == TIM_Channel_1) {
		TIM_SelectInputTrigger(cfg->hsync_capture.timer, TIM_TS_TI1FP1);

		line_int_flag = TIM_IT_CC1;
	} else if (cfg->hsync_capture.timer_chan == TIM_Channel_2) {
		TIM_SelectInputTrigger(cfg->hsync_capture.timer, TIM_TS_TI2FP2);

		line_int_flag = TIM_IT_CC2;
	} else {
		PIOS_Assert(0);
	}

	TIM_SelectMasterSlaveMode(cfg->hsync_capture.timer, TIM_MasterSlaveMode_Enable);
	TIM_SelectOutputTrigger(cfg->hsync_capture.timer, TIM_TRGOSource_Update);

	// Pixel timer: Outputs clock for SPI
	GPIO_Init(cfg->pixel_timer.pin.gpio, (GPIO_InitTypeDef *)&(cfg->pixel_timer.pin.init));
	if (cfg->pixel_timer.remap) {
		GPIO_PinAFConfig(cfg->pixel_timer.pin.gpio, __builtin_ctz(cfg->pixel_timer.pin.init.GPIO_Pin), cfg->pixel_timer.remap);
	}

	TIM_OC1Init(cfg->pixel_timer.timer, (TIM_OCInitTypeDef*)&cfg->tim_oc_init);
	TIM_OC1PreloadConfig(cfg->pixel_timer.timer, TIM_OCPreload_Enable);
	if (dev_cfg->pixel_timer.timer == TIM9) { // XXX or other fast timers
		TIM_SetCompare1(cfg->pixel_timer.timer, pios_video_type_cfg_act->dc);
		TIM_SetAutoreload(cfg->pixel_timer.timer, pios_video_type_cfg_act->period - 1);
	} else {
		TIM_SetCompare1(cfg->pixel_timer.timer, pios_video_type_cfg_act->dc / 2);
		TIM_SetAutoreload(cfg->pixel_timer.timer, pios_video_type_cfg_act->period / 2 - 1);
	}
	TIM_ARRPreloadConfig(cfg->pixel_timer.timer, ENABLE);
	TIM_CtrlPWMOutputs(cfg->pixel_timer.timer, ENABLE);

	if ((cfg->hsync_capture.timer == TIM2) && (cfg->pixel_timer.timer == TIM3)) {
		TIM_SelectInputTrigger(cfg->pixel_timer.timer, TIM_TS_ITR1);
	} else if ((cfg->hsync_capture.timer == TIM2) && (cfg->pixel_timer.timer == TIM9)) {
		TIM_SelectInputTrigger(cfg->pixel_timer.timer, TIM_TS_ITR0);
	} else {
		PIOS_Assert(0);
	}

	/* Enable the hsync cap global Interrupt */
	NVIC_InitTypeDef NVIC_InitStructure;

	if (cfg->hsync_capture.timer == TIM2)
		NVIC_InitStructure.NVIC_IRQChannel = TIM2_IRQn;
	else
		PIOS_Assert(0);

	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_HIGHEST;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	/* Initialize the SPI block */
	SPI_Init(cfg->level.regs, (SPI_InitTypeDef *)&(cfg->level.init));
	SPI_Init(cfg->mask.regs, (SPI_InitTypeDef *)&(cfg->mask.init));

	/* Configure DMA for SPI*/
	DMA_Init(cfg->mask.dma.tx.channel, (DMA_InitTypeDef *)&(cfg->mask.dma.tx.init));
	DMA_Init(cfg->level.dma.tx.channel, (DMA_InitTypeDef *)&(cfg->level.dma.tx.init));

	/* Trigger interrupt when transfer complete */
	DMA_ITConfig(cfg->level.dma.tx.channel, DMA_IT_TC, ENABLE);
	DMA_ITConfig(cfg->mask.dma.tx.channel, DMA_IT_TC, ENABLE);

	/* Configure and clear buffers */
	draw_buffer_level = buffer0_level;
	draw_buffer_mask  = buffer0_mask;
	disp_buffer_level = buffer1_level;
	disp_buffer_mask  = buffer1_mask;
	memset(disp_buffer_mask, 0, BUFFER_HEIGHT * BUFFER_WIDTH);
	memset(disp_buffer_level, 0, BUFFER_HEIGHT * BUFFER_WIDTH);
	memset(draw_buffer_mask, 0, BUFFER_HEIGHT * BUFFER_WIDTH);
	memset(draw_buffer_level, 0, BUFFER_HEIGHT * BUFFER_WIDTH);

	/* Configure DMA interrupt */
	NVIC_Init((NVIC_InitTypeDef*)&cfg->level.dma.irq.init);
	NVIC_Init((NVIC_InitTypeDef*)&cfg->mask.dma.irq.init);

	/* Enable SPI interrupts to DMA */
	SPI_I2S_DMACmd(cfg->mask.regs, SPI_I2S_DMAReq_Tx, ENABLE);
	SPI_I2S_DMACmd(cfg->level.regs, SPI_I2S_DMAReq_Tx, ENABLE);

	// Enable interrupts
	PIOS_EXTI_Init(cfg->vsync);
	TIM_ITConfig(cfg->hsync_capture.timer, line_int_flag, ENABLE);

	// Enable the capture timer
	TIM_Cmd(cfg->hsync_capture.timer, ENABLE);
}

/**
 *
 */
uint16_t PIOS_Video_GetLines(void)
{
	return num_video_lines;
}

/**
 *
 */
enum pios_video_system PIOS_Video_GetSystem(void)
{
	return video_system_act;
}

/**
*  Set the black and white levels
*/
void PIOS_Video_SetLevels(uint8_t black, uint8_t white)
{
	if (dev_cfg->set_bw_levels) {
		dev_cfg->set_bw_levels(black, white);
	}
}

/**
*  Set the offset in x direction
*/
void PIOS_Video_SetXOffset(int8_t x_offset_in)
{
	if (x_offset_in > 50)
		x_offset_in = 50;
	if (x_offset_in < -50)
		x_offset_in = -50;

	x_offset_new = x_offset_in + PIOS_VIDEO_HSYNC_OFFSET;
}

/**
*  Set the offset in y direction
*/
void PIOS_Video_SetYOffset(int8_t y_offset_in)
{
	if (y_offset_in > 20)
		y_offset_in = 20;
	if (y_offset_in < -20)
		y_offset_in = -20;
	y_offset = y_offset_in;
}

/**
*  Set the x scale
*/
void PIOS_Video_SetXScale(uint8_t x_scale)
{
	// Not supported by this driver
	return;
}

/**
*  Set the 3D mode configuration
*/
void PIOS_Video_Set3DConfig(enum pios_video_3d_mode mode, uint8_t right_eye_x_shift)
{
	// Not supported by this driver
	return;
}
#endif /* PIOS_INCLUDE_VIDEO */
