#pragma once

#include <stdint.h>
#include "hardware/flash.h"
#include "wear_leveling/wear_leveling_rp2040_flash_config.h"

extern uint flash_capacity;
extern uint flash_border;

/* application, including rp2040 bootloader */
#define FLASH_OFF_APP (0x0)
#define CIV_SIZE    (0x1000)
#define SPLASH_SIZE (0x1000)

/* 16k space before 8MB boundary */
#define FLASH_OFF_EEPROM    (flash_border - WEAR_LEVELING_BACKING_SIZE)

/* 4k before eeprom starts */
#define FLASH_OFF_CIV       (FLASH_OFF_EEPROM - CIV_SIZE)

/* 4K before CIV starts */
#define FLASH_OFF_SPLASH    (FLASH_OFF_CIV  - SPLASH_SIZE)

#define FLASH_OFF_SPLASH_LEGACY (0x800000)



static inline void update_flash_capacity() {
    uint8_t txbuf[4] = {0x9f};
    uint8_t rxbuf[4] = {0};
    flash_do_cmd(txbuf, rxbuf, 4);
    flash_capacity = 1 << rxbuf[3];
    if (flash_capacity == 0x1000000) {
        flash_border = 0x800000;
    } else {
        flash_border = flash_capacity;
    }
}
