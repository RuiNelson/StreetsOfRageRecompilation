/* Access to Genesis Plus GX internals that the libretro API does not expose:
 * the 68000 and Z80 RAM, the cartridge ROM and the audio resampler rate. */

#include "shared.h"

/* 68000 work RAM byte at $FF0000 + (addr & $FFFF). */
unsigned sor_gpgx_read_ram8(unsigned addr) {
    return READ_BYTE(work_ram, addr & 0xFFFFu);
}

void sor_gpgx_write_ram8(unsigned addr, unsigned value) {
    WRITE_BYTE(work_ram, addr & 0xFFFFu, value);
}

unsigned sor_gpgx_read_zram8(unsigned addr) {
    return zram[addr & 0x1FFFu];
}

unsigned sor_gpgx_read_rom8(unsigned addr) {
    return READ_BYTE(cart.rom, addr);
}

void sor_gpgx_write_rom8(unsigned addr, unsigned value) {
    WRITE_BYTE(cart.rom, addr, value);
}

/* Output `samplerate` samples per second with the emulated machine paced at
 * `framerate` frames per second (0: the console's own master clock). */
void sor_gpgx_set_audio_rate(int samplerate, double framerate) {
    audio_set_rate(samplerate, framerate);
    audio_reset();
}

void sor_gpgx_clear_audio(void) {
    audio_reset();
}
