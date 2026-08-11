#include "drivers/sound/sb16.h"
#include "drivers/driver.h"
#include "devices/io.h"
#include "interrupts/idt.h"
#include "interrupts/pic.h"
#include "utilities/log.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include <stdint.h>
#include "utilities/string.h"

/* Port base and IRQ of the card we actually bound to. isa.c offers both
 * 0x220 and 0x240, so nothing here may assume the standard base. */
static uint16_t sb16_base = SB16_DEFAULT_IO;
static uint8_t  sb16_irq  = SB16_DEFAULT_IRQ;

static int sb16_is_playing = 0;
static int sb16_irq_hooked = 0;
static uint8_t *sb16_dma_buffer_virt;
static uint64_t sb16_dma_buffer_phys;

// Global state for the current audio stream
static uint8_t *current_audio_data = 0;  // Pointer to the raw WAV PCM data
static uint32_t current_audio_size = 0;  // Total size of the WAV data
static uint32_t audio_position = 0;      // How many bytes we've copied so far
static int      sb16_use_first_half = 1; // Tracks which half of the buffer to refill

void sb16_dsp_write(uint8_t command) {
    // Add a timeout to prevent infinite hangs if the DSP is stuck
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(sb16_base + SB16_REG_DSP_WSTAT) & 0x80)) {
            outb(sb16_base + SB16_REG_DSP_WRITE, command);
            return;
        }
    }
    log_write("SB16: DSP write timeout!", KERNEL, LOG_WARN);
}

void sb16_mixer_write(uint8_t index, uint8_t value) {
    outb(sb16_base + SB16_REG_MIXER_ADDR, index);
    outb(sb16_base + SB16_REG_MIXER_DATA, value);
}

void sb16_set_max_volume(void) {
    sb16_mixer_write(SB16_MIXER_MASTER_VOL, 0xFF);
    sb16_mixer_write(SB16_MIXER_VOICE_VOL, 0xFF);
}

/* Both volume registers keep the level in the top 4 bits of each nibble
 * (left in 7..4, right in 3..0), so the same 0..15 step goes in twice. */
void sb16_set_volume(int percent) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    uint8_t step = (uint8_t)((percent * 15) / 100);
    uint8_t level = (uint8_t)((step << 4) | step);

    sb16_mixer_write(SB16_MIXER_MASTER_VOL, level);
    sb16_mixer_write(SB16_MIXER_VOICE_VOL, level);
}

void sb16_reset_mixer(void) {
    sb16_mixer_write(0x00, 0x00);
}

/* Reading the 16-bit interrupt-status port drops the card's IRQ line. The
 * 8-bit path (read-buffer status) is acked too so a mode mismatch cannot
 * leave the line stuck asserted. */
void sb16_ack_irq(void) {
    (void)inb(sb16_base + SB16_REG_DSP_RINT16);
    (void)inb(sb16_base + SB16_REG_DSP_RSTAT);
}

/* Refill the half of the DMA buffer that just finished playing, while the
 * card plays the other one. The stream wraps inside the fill loop so a
 * chunk that straddles the end of the sample data still leaves the whole
 * half valid -- a short final copy would replay whatever the tail of the
 * previous pass left behind. */
void sb16_irq_handler(void) {
    sb16_ack_irq();

    /* The IDT sends the PIC EOI for us once this returns. */

    if (!sb16_is_playing || !current_audio_data || current_audio_size == 0)
        return;

    uint32_t half = SB16_DMA_BUFFER_SIZE / 2;
    uint8_t *target = sb16_dma_buffer_virt + (sb16_use_first_half ? 0 : half);

    uint32_t filled = 0;
    while (filled < half) {
        uint32_t chunk = half - filled;
        uint32_t remaining = current_audio_size - audio_position;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(target + filled, current_audio_data + audio_position, chunk);
        filled += chunk;
        audio_position += chunk;
        if (audio_position >= current_audio_size)
            audio_position = 0; /* loop the track */
    }

    sb16_use_first_half = !sb16_use_first_half;
}

void sb16_program_dma(void) {
		uint16_t count = (SB16_DMA_BUFFER_SIZE / 2) - 1;
    uint16_t addr_low = (sb16_dma_buffer_phys >> 1) & 0xFFFF;
    uint8_t  addr_page = (sb16_dma_buffer_phys >> 16) & 0xFE;

    outb(DMA_SLAVE_MASK, DMA_MASK_CHAN5);
    outb(DMA_SLAVE_CLEAR_FF, 0xFF);
    /* Mode: single transfer | auto-init | read (mem->device) | channel 5.
     * The low two bits are the channel select, and they are *relative* to
     * the slave controller: 01 = channel 5. Selecting 00 here would rewrite
     * channel 4's mode -- that is the master/slave cascade channel, which
     * must stay in cascade mode, and it would leave channel 5's own mode
     * register unprogrammed. */
    outb(DMA_SLAVE_MODE, DMA_MODE_SINGLE | DMA_MODE_AUTOINIT |
                         DMA_MODE_READ   | DMA_CHAN5_SELECT);
    outb(DMA_SLAVE_CHAN5_ADDR, addr_low & 0xFF);
    outb(DMA_SLAVE_CHAN5_ADDR, (addr_low >> 8) & 0xFF);
    outb(DMA_CHAN5_PAGE, addr_page);
    outb(DMA_SLAVE_CHAN5_COUNT, count & 0xFF);
    outb(DMA_SLAVE_CHAN5_COUNT, (count >> 8) & 0xFF);
}

int sb16_allocate_dma_buffer(void) {
    sb16_dma_buffer_phys = pmm_alloc_contiguous_below(SB16_DMA_LIMIT, SB16_DMA_BUFFER_SIZE / FRAME_SIZE);

    if (!sb16_dma_buffer_phys) {
        log_write("SB16: Failed to allocate DMA buffer!", KERNEL, LOG_ERROR);
        return -1;
    }

    /* The 8237 cannot carry into the page register mid-transfer, so a
     * buffer straddling a 128 KiB boundary would silently wrap back to
     * the start of its own 128 KiB block. */
    uint64_t last = sb16_dma_buffer_phys + SB16_DMA_BUFFER_SIZE - 1;
    if ((sb16_dma_buffer_phys / ISA_DMA16_BOUNDARY) != (last / ISA_DMA16_BOUNDARY)) {
        log_write_hex("SB16: DMA buffer crosses 128K boundary @",
                      sb16_dma_buffer_phys, KERNEL, LOG_ERROR);
        pmm_free_contiguous(sb16_dma_buffer_phys,
                            SB16_DMA_BUFFER_SIZE / FRAME_SIZE);
        sb16_dma_buffer_phys = 0;
        return -1;
    }

    sb16_dma_buffer_virt = (uint8_t *)phys_to_virt(sb16_dma_buffer_phys);
    memset(sb16_dma_buffer_virt, 0, SB16_DMA_BUFFER_SIZE);
    return 0;
}

void *sb16_dma_buffer(uint32_t *size_out) {
    if (size_out)
        *size_out = sb16_dma_buffer_phys ? SB16_DMA_BUFFER_SIZE : 0;
    return sb16_dma_buffer_phys ? sb16_dma_buffer_virt : 0;
}

void sb16_set_sample_rate(uint16_t rate) {
    sb16_dsp_write(0x41); /* set output sample rate, big-endian Hz */
    sb16_dsp_write(rate >> 8);
    sb16_dsp_write(rate & 0xFF);
}

void sb16_speaker_on(void) {
    sb16_dsp_write(0xD1);
}

void sb16_speaker_off(void) {
    sb16_dsp_write(0xD3);
}

/* Arm auto-init 16-bit signed stereo playback over the whole DMA buffer.
 *
 * The one number worth care here is the DSP block length. It counts 16-bit
 * samples, and it decides how often the card interrupts -- not how much it
 * plays, which the auto-init DMA already fixes at the full buffer. Setting
 * it to half the buffer is what makes the double buffer work: the IRQ
 * arrives as one half finishes, leaving the refill handler the whole of
 * the other half's playtime to work in. A full-buffer block length would
 * interrupt once per wrap, by which point both halves had already played
 * and half of every pass would be stale.
 *
 * Everything below has to be in this order: the handler must be live and
 * the channel unmasked before the DSP is armed, or the first block boundary
 * arrives with nothing to service it. */
static void sb16_arm(void) {
    if (!sb16_irq_hooked) {
        irq_install(sb16_irq, sb16_irq_handler);
        sb16_irq_hooked = 1;
    }
    sb16_ack_irq();
    pic_clear_mask(sb16_irq);

    sb16_program_dma();
    outb(DMA_SLAVE_MASK, DMA_UNMASK_CHAN5);

    sb16_set_sample_rate(44100);

    sb16_dsp_write(0xB6); /* 16-bit D/A, auto-init, FIFO */
    sb16_dsp_write(0x30); /* signed stereo */

    /* Half the buffer, in 16-bit samples: bytes / 2 (sample width) / 2. */
    uint16_t count = (uint16_t)((SB16_DMA_BUFFER_SIZE / 4) - 1);
    sb16_dsp_write(count & 0xFF);
    sb16_dsp_write((count >> 8) & 0xFF);

    sb16_is_playing = 1;
}

/* Loop whatever the DMA buffer already holds. No refill, so the buffer
 * plays unchanged until sb16_stop(). */
void sb16_play(void) {
    if (!sb16_dma_buffer_phys) {
        log_write("SB16: play with no DMA buffer", KERNEL, LOG_ERROR);
        return;
    }
    if (sb16_is_playing)
        return;

    current_audio_data = 0;
    current_audio_size = 0;
    sb16_arm();
}

/* Stream raw PCM (signed 16-bit stereo, 44.1 kHz) out of `wav_data`,
 * looping at the end. The caller keeps ownership of the buffer and must
 * keep it mapped in every address space -- the refill runs from an
 * interrupt, under whatever page tables happen to be current. */
void sb16_play_wav(uint8_t *wav_data, uint32_t wav_size) {
    if (!sb16_dma_buffer_phys || !wav_data || wav_size == 0) {
        log_write("SB16: play_wav with no buffer or no data", KERNEL, LOG_ERROR);
        return;
    }
    if (sb16_is_playing)
        sb16_stop();

    current_audio_data = wav_data;
    current_audio_size = wav_size;
    audio_position = 0;
    /* The first interrupt means "the first half finished", so that is the
     * half the handler refills first. */
    sb16_use_first_half = 1;

    /* Prime both halves: the card plays through the second one before the
     * first interrupt ever arrives. */
    uint32_t filled = 0;
    while (filled < SB16_DMA_BUFFER_SIZE) {
        uint32_t chunk = SB16_DMA_BUFFER_SIZE - filled;
        uint32_t remaining = wav_size - audio_position;
        if (chunk > remaining)
            chunk = remaining;
        memcpy(sb16_dma_buffer_virt + filled, wav_data + audio_position, chunk);
        filled += chunk;
        audio_position += chunk;
        if (audio_position >= wav_size)
            audio_position = 0;
    }

    sb16_arm();
}

void sb16_stop(void) {
    if (!sb16_is_playing)
        return;

    sb16_dsp_write(0xD5); /* pause 16-bit DMA */
    sb16_dsp_write(0xD9); /* exit auto-init 16-bit DMA */

    outb(DMA_SLAVE_MASK, DMA_MASK_CHAN5);
    pic_set_mask(sb16_irq);
    sb16_ack_irq();

    sb16_is_playing = 0;
}

// Only match ISA devices at standard SB16 ports!
static int sb16_match(const struct device *device) {
    if (device->bus != DEVICE_BUS_ISA) return 0;
    const struct isa_device *isa = &device->bus_info.isa;
    return (isa->io_base == 0x220 || isa->io_base == 0x240);
}

static int sb16_probe(struct device *device) {
    struct isa_device *isa = &device->bus_info.isa;
    uint16_t io_base = isa->io_base;

    outb(io_base + SB16_REG_DSP_RESET, 0x01);
    for (volatile int i = 0; i < 10000; i++);
    outb(io_base + SB16_REG_DSP_RESET, 0x00);

    for (int i = 0; i < 100000; i++) {
        if (inb(io_base + SB16_REG_DSP_RSTAT) & 0x80) {
            if (inb(io_base + SB16_REG_DSP_READ) == 0xAA) {
                log_write("SB16: SoundBlaster 16 detected!", KERNEL, LOG_INFO);

                /* Latch the base before any DSP/mixer access: everything
                 * below (and every later caller) drives this card, which
                 * is not necessarily the one at 0x220. */
                sb16_base = io_base;
                sb16_irq = (uint8_t)(isa->irq ? isa->irq : SB16_DEFAULT_IRQ);

                irq_install(5, sb16_irq_handler);
                pic_clear_mask(5);
                // Give the DSP a moment to stabilize after reset
                for (volatile int i = 0; i < 10000; i++);

                // Initialize hardware
                sb16_speaker_on();
                sb16_set_sample_rate(44100);
                sb16_reset_mixer();
                sb16_set_max_volume();

                /* Allocate + program the DMA channel, but leave it masked
                 * and the DSP idle. Probing a device must not start an
                 * unbounded auto-init transfer -- callers use sb16_play(). */
                if (sb16_allocate_dma_buffer() == 0)
                    sb16_program_dma();

                return 0;
            }
        }
    }
    return -1;
}

static struct driver sb16_driver = {
    .name = "SoundBlaster 16",
    .bus = DEVICE_BUS_ISA,
    .match = sb16_match,
    .probe = sb16_probe
};

void sb16_driver_init(void) {
    log_write("SB16: initializing", KERNEL, LOG_INFO);
    driver_register(&sb16_driver);
}
