/* 93C56 serial EEPROM in 8-bit mode (256 cells, 9 command-address bits),
 * following MAME 0.264 eeprom_serial_base_device / eeprom_serial_93cxx_device
 * (src/devices/machine/eepromser.cpp, eeprom.cpp): no streaming, DO driven
 * from the shift register in READING_DATA, READY reported on DO while
 * waiting for a start bit, 1.75 ms write / 1 ms erase / 8 ms write-all and
 * erase-all completion times. */
#include "db/board.h"

#include <string.h>

enum { ST_IN_RESET, ST_WAIT_FOR_START_BIT, ST_WAIT_FOR_COMMAND, ST_READING_DATA, ST_WAIT_FOR_DATA, ST_WAIT_FOR_COMPLETION };
enum { CMD_INVALID, CMD_READ, CMD_WRITE, CMD_ERASE, CMD_LOCK, CMD_UNLOCK, CMD_WRITEALL, CMD_ERASEALL };
enum { EV_CS_RISING = 1, EV_CS_FALLING = 2, EV_CLK_RISING = 4, EV_CLK_FALLING = 8 };

#define ADDRESS_BITS 8
#define COMMAND_ADDRESS_BITS 9
#define DATA_BITS 8
/* first CPU cycle at or after x microseconds (MAME compares attotimes) */
#define ATTOS_PER_CYCLE (1000000000000000000ull / DB_CPU_CLOCK)
#define USEC(x) (((uint64_t)(x) * 1000000000000ull + ATTOS_PER_CYCLE - 1) / ATTOS_PER_CYCLE)

void db_eeprom_reset(db_eeprom *e)
{
    /* device_start + device_reset */
    e->state = ST_IN_RESET;
    e->locked = 1;
    e->bits_accum = 0;
    e->command_address_accum = 0;
    e->command = CMD_INVALID;
    e->address = 0;
    e->shift_register = 0;
    e->cs = e->clk = e->di = 0;
    e->completion_time = 0;
    e->last_cs_rising_edge = 0;
}

static int ready(const db_eeprom *e, uint64_t now) { return now >= e->completion_time; }

static void op_write(db_eeprom *e, uint64_t now, unsigned address, uint32_t data)
{
    e->data[address & 0xff] = (uint8_t)data;
    e->completion_time = now + USEC(1750);
    e->dirty = 1;
}

static void op_write_all(db_eeprom *e, uint64_t now, uint32_t data)
{
    for (unsigned a = 0; a < DB_EEPROM_SIZE; a++) e->data[a] = (uint8_t)(e->data[a] & data);
    e->completion_time = now + USEC(8000);
    e->dirty = 1;
}

static void op_erase(db_eeprom *e, uint64_t now, unsigned address)
{
    e->data[address & 0xff] = 0xff;
    e->completion_time = now + USEC(1000);
    e->dirty = 1;
}

static void op_erase_all(db_eeprom *e, uint64_t now)
{
    memset(e->data, 0xff, sizeof(e->data));
    e->completion_time = now + USEC(8000);
    e->dirty = 1;
}

static void parse_command_and_address(db_eeprom *e)
{
    e->command = CMD_INVALID;
    e->address = e->command_address_accum & ((1u << COMMAND_ADDRESS_BITS) - 1);
    switch (e->command_address_accum >> COMMAND_ADDRESS_BITS) {
    case 0:
        switch (e->address >> (COMMAND_ADDRESS_BITS - 2)) {
        case 0: e->command = CMD_LOCK; break;
        case 1: e->command = CMD_WRITEALL; break;
        case 2: e->command = CMD_ERASEALL; break;
        case 3: e->command = CMD_UNLOCK; break;
        }
        e->address = 0;
        break;
    case 1: e->command = CMD_WRITE; break;
    case 2: e->command = CMD_READ; break;
    case 3: e->command = CMD_ERASE; break;
    }
}

static void execute_command(db_eeprom *e, uint64_t now)
{
    parse_command_and_address(e);
    e->bits_accum = 0;
    switch (e->command) {
    case CMD_READ: e->shift_register = 0; e->state = ST_READING_DATA; break;
    case CMD_WRITE: case CMD_WRITEALL: e->shift_register = 0; e->state = ST_WAIT_FOR_DATA; break;
    case CMD_ERASE:
        if (e->locked) { e->state = ST_IN_RESET; break; }
        op_erase(e, now, e->address);
        e->state = ST_WAIT_FOR_COMPLETION;
        break;
    case CMD_LOCK: e->locked = 1; e->state = ST_IN_RESET; break;
    case CMD_UNLOCK: e->locked = 0; e->state = ST_IN_RESET; break;
    case CMD_ERASEALL:
        if (e->locked) { e->state = ST_IN_RESET; break; }
        op_erase_all(e, now);
        e->state = ST_WAIT_FOR_COMPLETION;
        break;
    default: e->state = ST_IN_RESET; break;
    }
}

static void execute_write_command(db_eeprom *e, uint64_t now)
{
    switch (e->command) {
    case CMD_WRITE:
        if (e->locked) { e->state = ST_IN_RESET; break; }
        op_write(e, now, e->address, e->shift_register);
        e->state = ST_WAIT_FOR_COMPLETION;
        break;
    case CMD_WRITEALL:
        if (e->locked) { e->state = ST_IN_RESET; break; }
        op_write_all(e, now, e->shift_register);
        e->state = ST_WAIT_FOR_COMPLETION;
        break;
    default: e->state = ST_IN_RESET; break;
    }
}

static void handle_event(db_eeprom *e, uint64_t now, int event)
{
    switch (e->state) {
    case ST_IN_RESET:
        if (event == EV_CS_RISING) e->state = ST_WAIT_FOR_START_BIT;
        break;
    case ST_WAIT_FOR_START_BIT:
        if (event == EV_CLK_RISING && e->di && ready(e, now) && now > e->last_cs_rising_edge) {
            e->command_address_accum = e->bits_accum = 0;
            e->state = ST_WAIT_FOR_COMMAND;
        } else if (event == EV_CS_FALLING) {
            e->state = ST_IN_RESET;
        }
        break;
    case ST_WAIT_FOR_COMMAND:
        if (event == EV_CLK_RISING) {
            e->command_address_accum = (e->command_address_accum << 1) | e->di;
            if (++e->bits_accum == 2 + COMMAND_ADDRESS_BITS) execute_command(e, now);
        } else if (event == EV_CS_FALLING) {
            e->state = ST_IN_RESET;
        }
        break;
    case ST_READING_DATA:
        if (event == EV_CLK_RISING) {
            uint32_t bit_index = e->bits_accum++;
            if (bit_index % DATA_BITS == 0 && bit_index == 0)
                e->shift_register = (uint32_t)e->data[(e->address + e->bits_accum / DATA_BITS) & ((1u << ADDRESS_BITS) - 1)]
                                    << (32 - DATA_BITS);
            else
                e->shift_register = (e->shift_register << 1) | 1;
        } else if (event == EV_CS_FALLING) {
            e->state = ST_IN_RESET;
        }
        break;
    case ST_WAIT_FOR_DATA:
        if (event == EV_CLK_RISING) {
            e->shift_register = (e->shift_register << 1) | e->di;
            if (++e->bits_accum == DATA_BITS) execute_write_command(e, now);
        } else if (event == EV_CS_FALLING) {
            e->state = ST_IN_RESET;
        }
        break;
    case ST_WAIT_FOR_COMPLETION:
        if (event == EV_CS_FALLING) e->state = ST_IN_RESET;
        break;
    }
}

/* psikyosh_state::eeprom_w: di_write, cs_write, clk_write in that order */
void db_eeprom_write_lines(db_eeprom *e, uint64_t now, int di, int cs, int clk)
{
    e->di = (uint8_t)(di & 1);
    cs &= 1;
    if (cs != e->cs) {
        e->cs = (uint8_t)cs;
        if (cs) e->last_cs_rising_edge = now;
        handle_event(e, now, cs ? EV_CS_RISING : EV_CS_FALLING);
    }
    clk &= 1;
    if (clk != e->clk) {
        e->clk = (uint8_t)clk;
        handle_event(e, now, clk ? EV_CLK_RISING : EV_CLK_FALLING);
    }
}

/* eeprom_serial_93cxx_device::do_read */
int db_eeprom_do_read(const db_eeprom *e, uint64_t now)
{
    int out = (e->state == ST_READING_DATA && (e->shift_register & 0x80000000u) == 0) ? 0 : 1;
    if (e->state == ST_WAIT_FOR_START_BIT) out &= ready(e, now) ? 1 : 0;
    return out;
}
