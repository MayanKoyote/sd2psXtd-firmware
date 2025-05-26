#include "ps1_cardman.h"

#include <ctype.h>
#include <ps1/ps1_memory_card.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "card_config.h"

#include "ps1_mc_data_interface.h"
#include "sd.h"
#include "debug.h"
#include "settings.h"
#if WITH_PSRAM
#include <psram/psram.h>
#endif
#include "ps1_empty_card.h"
#include "util.h"

#include "game_db/game_db.h"

#include "hardware/timer.h"

#if LOG_LEVEL_PS1_CM == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS1_CM, level, fmt, ##x)
#endif


#define CARD_SIZE (128 * 1024)
#define BLOCK_SIZE 128
static uint8_t flushbuf[BLOCK_SIZE];
static int fd = -1;

#define IDX_MIN 1
#define CHAN_MIN 1
#define CHAN_MAX 8


static int card_idx;
static int card_chan;
static char folder_name[MAX_FOLDER_NAME_LENGTH];
static ps1_cardman_state_t cardman_state;
static bool needs_update;

static bool try_set_boot_card() {
    if (!settings_get_ps1_autoboot())
        return false;

    card_idx = PS1_CARD_IDX_SPECIAL;
    card_chan = settings_get_ps1_boot_channel();
    cardman_state = PS1_CM_STATE_BOOT;
    snprintf(folder_name, sizeof(folder_name), "BOOT");
    return true;
}

static void set_default_card() {
    card_idx = settings_get_ps1_card();
    card_chan = settings_get_ps1_channel();
    cardman_state = PS1_CM_STATE_NORMAL;
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

static bool try_set_game_id_card() {
    if (!settings_get_ps1_game_id())
        return false;

    char parent_id[MAX_GAME_ID_LENGTH] = {};

    (void)game_db_get_current_parent(parent_id);

    if (!parent_id[0])
        return false;

    card_idx = PS1_CARD_IDX_SPECIAL;
    card_chan = CHAN_MIN;
    cardman_state = PS1_CM_STATE_GAMEID;
    card_config_get_card_folder(parent_id, folder_name, sizeof(folder_name));

    snprintf(folder_name, sizeof(folder_name), "%s", parent_id);

    return true;
}

static bool try_set_next_named_card() {
    bool ret = false;
    if (cardman_state != PS1_CM_STATE_NAMED) {
        ret = try_set_named_card_folder("MemoryCards/PS1", 0, folder_name, sizeof(folder_name));
        if (ret)
            card_idx = 1;
    } else {
        ret = try_set_named_card_folder("MemoryCards/PS1", card_idx, folder_name, sizeof(folder_name));
        if (ret)
            card_idx++;
    }

    if (ret) {
        card_chan = CHAN_MIN;
        cardman_state = PS1_CM_STATE_NAMED;
    }

    return ret;
}

static bool try_set_prev_named_card() {
    bool ret = false;
    if (card_idx > 1) {
        ret = try_set_named_card_folder("MemoryCards/PS1", card_idx - 2, folder_name, sizeof(folder_name));
        if (ret) {
            card_idx--;
            card_chan = CHAN_MIN;
            cardman_state = PS1_CM_STATE_NAMED;
        }
    }
    return ret;
}

void ps1_cardman_init(void) {
    if (!try_set_game_id_card() && !try_set_boot_card())
        set_default_card();
}

int ps1_cardman_read_sector(int sector, void *buf128) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -2;

    if (sd_read(fd, buf128, BLOCK_SIZE) != BLOCK_SIZE)
        return -3;

    return 0;
}

int ps1_cardman_write_sector(int sector, void *buf512) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -1;

    if (sd_write(fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

void ps1_cardman_flush(void) {
    if (fd >= 0)
        sd_flush(fd);
}

static void ensuredirs(void) {
    char cardpath[64];

    snprintf(cardpath, sizeof(cardpath), "MemoryCards/PS1/%s", folder_name);

    sd_mkdir("MemoryCards");
    sd_mkdir("MemoryCards/PS1");
    sd_mkdir(cardpath);

    if (!sd_exists("MemoryCards") || !sd_exists("MemoryCards/PS1") || !sd_exists(cardpath))
        fatal(ERR_CARDMAN, "error creating directories");
}

static void genblock(size_t pos, void *buf) {
    memset(buf, 0xFF, BLOCK_SIZE);

    if (pos < 0x2000)
        memcpy(buf, &ps1_empty_card[pos], BLOCK_SIZE);
}

void ps1_cardman_open(void) {
    char path[96];
    sd_init();
    ensuredirs();
    needs_update = false;

    switch (cardman_state) {
        case PS1_CM_STATE_BOOT:
            if (card_chan == 1) {
                snprintf(path, sizeof(path), "MemoryCards/PS1/%s/BootCard-%d.mcd", folder_name, card_chan);
                if (!sd_exists(path)) {
                    // before boot card channels, boot card was located at BOOT/BootCard.mcd, for backwards compatibility check if it exists
                    snprintf(path, sizeof(path), "MemoryCards/PS1/%s/BootCard.mcd", folder_name);
                    if (!sd_exists(path)) {
                        // go back to BootCard-1.mcd if it doesn't
                        snprintf(path, sizeof(path), "MemoryCards/PS1/%s/BootCard-%d.mcd", folder_name, card_chan);
                    }
                }
            } else {
                snprintf(path, sizeof(path), "MemoryCards/PS1/%s/BootCard-%d.mcd", folder_name, card_chan);
            }

            settings_set_ps1_boot_channel(card_chan);
            break;
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_GAMEID:
            snprintf(path, sizeof(path), "MemoryCards/PS1/%s/%s-%d.mcd", folder_name, folder_name, card_chan);
            break;
        case PS1_CM_STATE_NORMAL:
            snprintf(path, sizeof(path), "MemoryCards/PS1/%s/%s-%d.mcd", folder_name, folder_name, card_chan);

            /* this is ok to do on every boot because it wouldn't update if the value is the same as currently stored */
            settings_set_ps1_card(card_idx);
            settings_set_ps1_channel(card_chan);
            break;
    }

    log(LOG_INFO, "Switching to card path = %s\n", path);

    if (!sd_exists(path)) {
        fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);

        if (fd < 0)
            fatal(ERR_CARDMAN, "cannot open for creating new card");

        log(LOG_INFO, "create new image at %s... ", path);
        uint64_t cardprog_start = time_us_64();

        for (size_t pos = 0; pos < CARD_SIZE; pos += BLOCK_SIZE) {
            genblock(pos, flushbuf);
#if WITH_PSRAM
            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
#endif
            if (sd_write(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                fatal(ERR_CARDMAN, "cannot init memcard");
#if WITH_PSRAM
            psram_wait_for_dma();
#endif
        }
        sd_flush(fd);

        ps1_mc_data_interface_card_changed();

        uint64_t end = time_us_64();
        log(LOG_INFO, "OK!\n");


        log(LOG_INFO, "took = %.2f s; SD write speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
            1000000.0 * CARD_SIZE / (end - cardprog_start) / 1024);
    } else {
        fd = sd_open(path, O_RDWR);

        if (fd < 0)
            fatal(ERR_CARDMAN, "cannot open card");

        /* read 8 megs of card image */
        log(LOG_INFO, "reading card.... ");
        uint64_t cardprog_start = time_us_64();
#if WITH_PSRAM
        for (size_t pos = 0; pos < CARD_SIZE; pos += BLOCK_SIZE) {
            if (sd_read(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                fatal(ERR_CARDMAN, "cannot read memcard");

            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
            psram_wait_for_dma();
        }
#endif
        ps1_mc_data_interface_card_changed();
        uint64_t end = time_us_64();
        log(LOG_INFO, "OK!\n");

        log(LOG_INFO, "took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
            1000000.0 * CARD_SIZE / (end - cardprog_start) / 1024);
    }
}

bool ps1_cardman_needs_update(void) {
    return needs_update;
}

void ps1_cardman_close(void) {
    if (fd < 0)
        return;
    ps1_cardman_flush();
    sd_close(fd);
    fd = -1;
}

void ps1_cardman_next_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS1_CM_STATE_BOOT) ? "BootCard" : folder_name);
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
        case PS1_CM_STATE_GAMEID:
        case PS1_CM_STATE_NORMAL:
            card_chan += 1;
            if (card_chan > max_chan)
                card_chan = CHAN_MIN;
            break;
    }

    needs_update = true;
}

void ps1_cardman_prev_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS1_CM_STATE_BOOT) ? "BootCard" : folder_name);

    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
        case PS1_CM_STATE_GAMEID:
        case PS1_CM_STATE_NORMAL:
            card_chan -= 1;
            if (card_chan < CHAN_MIN)
                card_chan = max_chan;
            break;
    }
    needs_update = true;
}

void ps1_cardman_next_idx(void) {
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
            if (!try_set_prev_named_card() && !try_set_boot_card() && !try_set_game_id_card())
                set_default_card();
            break;
        case PS1_CM_STATE_BOOT:
            if (!try_set_game_id_card())
                set_default_card();
            break;
        case PS1_CM_STATE_GAMEID:
            set_default_card();
            break;
        case PS1_CM_STATE_NORMAL:
            card_idx += 1;
            card_chan = CHAN_MIN;
            if (card_idx > UINT16_MAX)
                card_idx = UINT16_MAX;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            break;
    }
    needs_update = true;
}

void ps1_cardman_prev_idx(void) {
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
            if (!try_set_next_named_card())
                set_default_card();
            break;
        case PS1_CM_STATE_GAMEID:
            if (!try_set_boot_card())
                if (!try_set_next_named_card())
                    set_default_card();
            break;
        case PS1_CM_STATE_NORMAL:
            card_idx -= 1;
            card_chan = CHAN_MIN;
            if (card_idx <= PS1_CARD_IDX_SPECIAL) {
                if (!try_set_game_id_card() && !try_set_boot_card() && !try_set_next_named_card())
                    set_default_card();
            } else {
                snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            }
            break;
    }
    needs_update = true;
}

int ps1_cardman_get_idx(void) {
    return card_idx;
}

int ps1_cardman_get_channel(void) {
    return card_chan;
}

void ps1_cardman_set_game_id(const char* card_game_id) {
    if (!settings_get_ps1_game_id())
        return;

    char new_folder_name[MAX_FOLDER_NAME_LENGTH] = {};
    if (card_game_id[0]) {
        card_config_get_card_folder(card_game_id, new_folder_name, sizeof(new_folder_name));
        if (new_folder_name[0] == 0x00)
            snprintf(new_folder_name, sizeof(new_folder_name), "%s", card_game_id);
        if ((strcmp(new_folder_name, folder_name) != 0) || (PS1_CM_STATE_GAMEID != cardman_state)) {
            card_idx = PS1_CARD_IDX_SPECIAL;
            cardman_state = PS1_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            memcpy(folder_name, new_folder_name, sizeof(folder_name));
            needs_update = true;
        }
    }
}
//TEMP
void ps1_cardman_switch_bootcard(void) {
    if (try_set_boot_card())
        needs_update = true;
}

void ps1_cardman_switch_default(void) {
    if (PS1_CARD_IDX_SPECIAL == card_idx) {
        set_default_card();
        needs_update = true;
    }
}


const char* ps1_cardman_get_folder_name(void) {
    return folder_name;
}

ps1_cardman_state_t ps1_cardman_get_state(void) {
    return cardman_state;
}
