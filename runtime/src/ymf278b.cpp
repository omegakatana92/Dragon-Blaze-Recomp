// YMF278B (OPL4) for the Psikyo PS5 board: the ymfm core (Aaron Giles,
// BSD-3-Clause, the same code MAME 0.264 uses) with glue equivalent to MAME's
// ymfm_device_base / ymf278b_device (src/devices/sound/ymfm_mame.h,
// ymopl.cpp): timers and the busy flag run on the board's clock, the IRQ goes
// to the SH-2, PCM data is read from the "ymf" ROM region.
#include "db/ymf278b.h"

#include "ymfm_opl.h"

#include <cstring>
#include <new>
#include <vector>

namespace {

class ymf_glue : public ymfm::ymfm_interface
{
public:
    ymf_glue(const uint8_t *rom, uint32_t size, const db_ymf_host &host) :
        m_rom(rom), m_rom_size(size), m_host(host), m_busy_end(0), m_chip(*this)
    {
    }

    ymfm::ymf278b &chip() { return m_chip; }
    void timer_expired(uint32_t tnum) { m_engine->engine_timer_expired(tnum); }
    uint64_t busy_end() const { return m_busy_end; }
    void set_busy_end(uint64_t v) { m_busy_end = v; }

protected:
    // MAME synchronises these through the scheduler; with one CPU that means
    // "after the current instruction", which is when the board calls us.
    void ymfm_sync_mode_write(uint8_t data) override { m_engine->engine_mode_write(data); }
    void ymfm_sync_check_interrupts() override { m_engine->engine_check_interrupts(); }

    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
    {
        m_host.set_timer(m_host.user, (int)tnum, duration_in_clocks >= 0
            ? (int64_t)(m_host.now(m_host.user) + (uint64_t)duration_in_clocks) : -1);
    }

    void ymfm_set_busy_end(uint32_t clocks) override { m_busy_end = m_host.now(m_host.user) + clocks; }
    bool ymfm_is_busy() override { return m_host.now(m_host.user) < m_busy_end; }
    void ymfm_update_irq(bool asserted) override { m_host.irq(m_host.user, asserted ? 1 : 0); }

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        if (type != ymfm::ACCESS_PCM)
            return 0;
        address &= 0x3fffff;           // device_rom_interface<22>
        return address < m_rom_size ? m_rom[address] : 0;
    }

    void ymfm_external_write(ymfm::access_class, uint32_t, uint8_t) override {}

private:
    const uint8_t *m_rom;
    uint32_t m_rom_size;
    db_ymf_host m_host;
    uint64_t m_busy_end;
    ymfm::ymf278b m_chip;
};

} // namespace

struct db_ymf {
    ymf_glue glue;
    db_ymf(const uint8_t *rom, uint32_t size, const db_ymf_host &host) : glue(rom, size, host) {}
};

extern "C" {

db_ymf *db_ymf_create(const uint8_t *rom, uint32_t rom_size, const db_ymf_host *host)
{
    db_ymf *y = new (std::nothrow) db_ymf(rom, rom_size, *host);
    if (y)
        y->glue.chip().reset();
    return y;
}

void db_ymf_destroy(db_ymf *y) { delete y; }

void db_ymf_reset(db_ymf *y) { y->glue.chip().reset(); }

uint32_t db_ymf_sample_rate(uint32_t clock) { return clock / 768; }

uint8_t db_ymf_read(db_ymf *y, unsigned offset) { return y->glue.chip().read(offset & 7); }

void db_ymf_write(db_ymf *y, unsigned offset, uint8_t data) { y->glue.chip().write(offset & 7, data); }

void db_ymf_timer_expired(db_ymf *y, int tnum) { y->glue.timer_expired((uint32_t)tnum); }

// One mono speaker fed by all six outputs at gain 1.0 (MAME psikyo3v1:
// ymf.add_route(ALL_OUTPUTS, "mono", 1.0)); each output is a 16-bit value.
void db_ymf_generate(db_ymf *y, int32_t *mono, int samples)
{
    ymfm::ymf278b::output_data out[256];
    while (samples > 0) {
        int n = samples > 256 ? 256 : samples;
        y->glue.chip().generate(out, (uint32_t)n);
        for (int i = 0; i < n; i++) {
            int32_t s = 0;
            for (int k = 0; k < 6; k++)
                s += out[i].data[k];
            mono[i] = s;
        }
        mono += n;
        samples -= n;
    }
}

size_t db_ymf_save(db_ymf *y, uint8_t *buffer, size_t capacity)
{
    std::vector<uint8_t> blob;
    ymfm::ymfm_saved_state state(blob, true);
    y->glue.chip().save_restore(state);
    uint64_t busy = y->glue.busy_end();
    size_t need = blob.size() + 8;
    if (buffer && capacity >= need) {
        for (int i = 0; i < 8; i++)
            buffer[i] = (uint8_t)(busy >> (8 * i));
        std::memcpy(buffer + 8, blob.data(), blob.size());
    }
    return need;
}

int db_ymf_load(db_ymf *y, const uint8_t *buffer, size_t size)
{
    std::vector<uint8_t> probe;
    {
        ymfm::ymfm_saved_state state(probe, true);
        y->glue.chip().save_restore(state);
    }
    if (size != probe.size() + 8)
        return 0;
    uint64_t busy = 0;
    for (int i = 0; i < 8; i++)
        busy |= (uint64_t)buffer[i] << (8 * i);
    std::vector<uint8_t> blob(buffer + 8, buffer + size);
    ymfm::ymfm_saved_state state(blob, false);
    y->glue.chip().save_restore(state);
    y->glue.set_busy_end(busy);
    return 1;
}

}
