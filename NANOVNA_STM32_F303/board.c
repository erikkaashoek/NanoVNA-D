/*
    ChibiOS - Copyright (C) 2006..2015 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "hal.h"

#if HAL_USE_PAL || defined(__DOXYGEN__)
/**
 * @brief   PAL setup.
 * @details Digital I/O ports static configuration as defined in @p board.h.
 *          This variable is used by the HAL when initializing the PAL driver.
 */
const PALConfig pal_default_config = {
#if STM32_HAS_GPIOA
  {VAL_GPIOA_MODER, VAL_GPIOA_OTYPER, VAL_GPIOA_OSPEEDR, VAL_GPIOA_PUPDR,
   VAL_GPIOA_ODR,   VAL_GPIOA_AFRL,   VAL_GPIOA_AFRH},
#endif
#if STM32_HAS_GPIOB
  {VAL_GPIOB_MODER, VAL_GPIOB_OTYPER, VAL_GPIOB_OSPEEDR, VAL_GPIOB_PUPDR,
   VAL_GPIOB_ODR,   VAL_GPIOB_AFRL,   VAL_GPIOB_AFRH},
#endif
#if STM32_HAS_GPIOC
  {VAL_GPIOC_MODER, VAL_GPIOC_OTYPER, VAL_GPIOC_OSPEEDR, VAL_GPIOC_PUPDR,
   VAL_GPIOC_ODR,   VAL_GPIOC_AFRL,   VAL_GPIOC_AFRH},
#endif
#if STM32_HAS_GPIOD
  {VAL_GPIOD_MODER, VAL_GPIOD_OTYPER, VAL_GPIOD_OSPEEDR, VAL_GPIOD_PUPDR,
   VAL_GPIOD_ODR,   VAL_GPIOD_AFRL,   VAL_GPIOD_AFRH},
#endif
#if STM32_HAS_GPIOE
  {VAL_GPIOE_MODER, VAL_GPIOE_OTYPER, VAL_GPIOE_OSPEEDR, VAL_GPIOE_PUPDR,
   VAL_GPIOE_ODR,   VAL_GPIOE_AFRL,   VAL_GPIOE_AFRH},
#endif
#if STM32_HAS_GPIOF
  {VAL_GPIOF_MODER, VAL_GPIOF_OTYPER, VAL_GPIOF_OSPEEDR, VAL_GPIOF_PUPDR,
   VAL_GPIOF_ODR,   VAL_GPIOF_AFRL,   VAL_GPIOF_AFRH},
#endif
#if STM32_HAS_GPIOG
  {VAL_GPIOG_MODER, VAL_GPIOG_OTYPER, VAL_GPIOG_OSPEEDR, VAL_GPIOG_PUPDR,
   VAL_GPIOG_ODR,   VAL_GPIOG_AFRL,   VAL_GPIOG_AFRH},
#endif
#if STM32_HAS_GPIOH
  {VAL_GPIOH_MODER, VAL_GPIOH_OTYPER, VAL_GPIOH_OSPEEDR, VAL_GPIOH_PUPDR,
   VAL_GPIOH_ODR,   VAL_GPIOH_AFRL,   VAL_GPIOH_AFRH},
#endif
#if STM32_HAS_GPIOI
  {VAL_GPIOI_MODER, VAL_GPIOI_OTYPER, VAL_GPIOI_OSPEEDR, VAL_GPIOI_PUPDR,
   VAL_GPIOI_ODR,   VAL_GPIOI_AFRL,   VAL_GPIOI_AFRH}
#endif
};
#endif

/*
 * Early initialization code.
 * This initialization must be performed just after stack setup and before
 * any other initialization.
 */
void __early_init(void) {
  // Enter the ST system bootloader (DFU) when enter_dfu() left the magic word
  // in RAM and reset us.  This runs before .data/.bss init, and the magic
  // address sits above __ram0_free__ (the heap is disabled), so it survives.
  // Refer to these pages for how to start dfu from software
  // https://community.st.com/s/question/0D50X00009XkeeWSAR/stm32l476rg-jump-to-bootloader-from-software
  // https://stm32f4-discovery.net/2017/04/tutorial-jump-system-memory-software-stm32/
  // volatile: these are bare absolute addresses with no other reader in this
  // translation unit, so a non-volatile load/store is fair game for the
  // optimiser to fold away.
  volatile unsigned long *magic = (volatile unsigned long *)BOOT_FROM_SYTEM_MEMORY_MAGIC_ADDRESS;
  unsigned long seen = *magic;
  *((volatile unsigned long *)BOOT_MAGIC_SEEN_ADDRESS) = seen;   // for "dfu status"
  if ( seen == BOOT_FROM_SYTEM_MEMORY_MAGIC ) {
    // reset magic bytes so the next reset boots the application again
    *magic = 0;
    // Interrupts must be ENABLED, not masked: the ST bootloader services USB
    // from interrupts and assumes it starts from reset with PRIMASK clear.
    // Masking them leaves it sitting in the bootloader with no USB, which looks
    // exactly like "reset but never enumerated as DFU". The F072 path has always
    // done this - see the "require irq" comment in NANOVNA_STM32_F072/board.c.
    __enable_irq();
    // Remap system memory to 0x00000000. ChibiOS does not set VTOR until
    // chSysInit(), so at this point the core still fetches vectors from 0 and
    // the remap is what redirects them to the bootloader. SYSCFG is clock gated
    // out of reset, and without this the remap silently does nothing.
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) | SYSCFG_CFGR1_MEM_MODE_0;
    // Belt and braces on a Cortex-M4: point VTOR at the bootloader too, so it
    // does not matter whether anything already moved it.
    SCB->VTOR = STM32F303xC_SYSTEM_MEMORY;
    __DSB();
    __ISB();
    // Take the stack pointer and entry point from the system memory vector
    // table rather than hardcoding them; they differ between silicon revisions.
    __set_MSP(*((uint32_t *)(STM32F303xC_SYSTEM_MEMORY)));
    ( (void (*)(void)) (*((uint32_t *)(STM32F303xC_SYSTEM_MEMORY+4))) )();
    while(1);
  }

  stm32_clock_init();
}

/*
 * Board-specific initialization code.
 */
void boardInit(void) {
  // Speedup flash latency
  FLASH->ACR= FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_0;
}
