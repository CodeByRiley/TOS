/* kernel/drivers/sound/sb16.h — SoundBlaster 16 DSP + ISA DMA driver.
 *
 * The card is discovered through the legacy ISA table in kernel/isa/isa.c,
 * so its I/O base is whatever that entry says (0x220 or 0x240). All port
 * access therefore goes through the base recorded at probe time — see the
 * SB16_REG_* offsets below — rather than a compile-time constant.
 *
 * Implementation: kernel/drivers/sound/sb16.c.
 */
#ifndef SB16_H
#define SB16_H

#include <stdint.h>

#define SB16_DEFAULT_IO  0x220
#define SB16_DEFAULT_IRQ 5

/* Register offsets from the card's I/O base. */
#define SB16_REG_MIXER_ADDR 0x04
#define SB16_REG_MIXER_DATA 0x05
#define SB16_REG_DSP_RESET  0x06
#define SB16_REG_DSP_READ   0x0A
#define SB16_REG_DSP_WRITE  0x0C
#define SB16_REG_DSP_WSTAT  0x0C // Write-buffer status (bit 7 = busy)
#define SB16_REG_DSP_RSTAT  0x0E // Read-buffer status  (bit 7 = ready)
                                 // Reading it also acks an 8-bit IRQ.
#define SB16_REG_DSP_RINT16 0x0F // 16-bit IRQ acknowledge

// SB16 Mixer Values
#define SB16_MIXER_MASTER_VOL 0x22
#define SB16_MIXER_VOICE_VOL  0x23

// I/O Ports for the Slave DMA Controller (Channels 4-7)
#define DMA_SLAVE_MASK      0xD4
#define DMA_SLAVE_CLEAR_FF  0xD8
#define DMA_SLAVE_MODE      0xD6
#define DMA_SLAVE_CHAN5_ADDR 0xC4
#define DMA_SLAVE_CHAN5_COUNT 0xC6
#define DMA_CHAN5_PAGE      0x8B

// Slave 8237 mode-register fields (port 0xD6).
// Bits 1-0 select the channel *within* the slave controller, so channel 5
// is 01. Channel 4 (00) is the cascade channel and must never be reprogrammed.
#define DMA_CHAN5_SELECT    0x01
#define DMA_MODE_READ       0x08  // memory -> device
#define DMA_MODE_WRITE      0x04  // device -> memory
#define DMA_MODE_AUTOINIT   0x10
#define DMA_MODE_SINGLE     0x40

/* Single-mask register values for channel 5 (bit 2 = mask). */
#define DMA_MASK_CHAN5      (0x04 | DMA_CHAN5_SELECT)
#define DMA_UNMASK_CHAN5    (0x00 | DMA_CHAN5_SELECT)

#define SB16_DMA_BUFFER_SIZE 32768 // 32 KB
#define SB16_DMA_LIMIT       0x100000 // 1 MB limit (forces use of the low memory hole)

/* A 16-bit ISA DMA transfer may not cross a 128 KiB physical boundary:
 * the 8237 only increments the 16-bit word-address latch, and the page
 * register is not carried into. */
#define ISA_DMA16_BOUNDARY   0x20000

void sb16_driver_init(void);

void sb16_dsp_write(uint8_t command);
void sb16_mixer_write(uint8_t index, uint8_t value);
void sb16_program_dma(void);
int  sb16_allocate_dma_buffer(void);
void sb16_set_sample_rate(uint16_t rate);
void sb16_speaker_on(void);
void sb16_speaker_off(void);
void sb16_reset_mixer(void);
void sb16_set_max_volume(void);
void sb16_set_volume(int percent);

/* Acknowledge a pending DSP interrupt. Without this the card holds its
 * IRQ line asserted and will never raise another one. */
void sb16_ack_irq(void);

/* Start / stop auto-init playback out of the driver's DMA buffer. Writing
 * samples into that buffer is the caller's job; sb16_dma_buffer() hands
 * back its virtual address and size. Playback is not started by probe. */
void  sb16_play(void);
void  sb16_play_wav(uint8_t *data, uint32_t size);
void  sb16_stop(void);
void *sb16_dma_buffer(uint32_t *size_out);

#endif /* SB16_H */
