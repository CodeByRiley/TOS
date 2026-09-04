#include <drivers/sound/sb16.h>
#include <drivers/driver.h>
#include <devices/io.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <utilities/log.h>
#include <memory/pmm.h>
#include <memory/hhdm.h>
#include <stdint.h>
#include <utilities/string.h>

/* Port base and IRQ of the card we actually bound to. isa.c offers both
 * 0x220 and 0x240, so nothing here may assume the standard base. */
static u16 sb16_base = SB16_DEFAULT_IO;
static u8  sb16_irq  = SB16_DEFAULT_IRQ;

static int sb16_is_playing = 0;
static int sb16_irq_hooked = 0;
static u8 *sb16_dma_buffer_virt;
static u64 sb16_dma_buffer_phys;

enum playback_kind {
    PLAYBACK_NONE,
    PLAYBACK_DMA_LOOP,
    PLAYBACK_SOURCE_LOOP,
    PLAYBACK_STREAM,
};

static enum playback_kind playback_kind = PLAYBACK_NONE;

/* Legacy kernel-only looping source used by sb16_play_wav(). */
static u8 *current_audio_data = 0;  // Pointer to the raw WAV PCM data
static u32 current_audio_size = 0;  // Total size of the WAV data
static u32 audio_position = 0;      // How many bytes we've copied so far
static int      sb16_use_first_half = 1; // Half completed by the next IRQ

/* Kernel-owned queue for userspace PCM. Keeping user pointers out of the IRQ
 * path is essential: the interrupted task may have a different CR3 from the
 * process that supplied the samples. */
static u8 stream_ring[SB16_STREAM_BUFFER_SIZE];
static u32 stream_read_pos;
static u32 stream_write_pos;
static u32 stream_used;
static u32 stream_dma_valid[2];
static u32 stream_dma_pending;
static u32 stream_rate = 44100;
static u32 stream_underruns;
static int stream_owner_pid;
static int stream_paused;
static int stream_draining;
static int stream_volume = 100;

_Static_assert((SB16_STREAM_BUFFER_SIZE & (SB16_STREAM_BUFFER_SIZE - 1)) == 0,
               "SB16 stream ring size must be a power of two");

static u64 irq_save(void) {
    u64 rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static void irq_restore(u64 rflags) {
    if (rflags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void sb16_dsp_write(u8 command) {
    // Add a timeout to prevent infinite hangs if the DSP is stuck
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(sb16_base + SB16_REG_DSP_WSTAT) & 0x80)) {
            outb(sb16_base + SB16_REG_DSP_WRITE, command);
            return;
        }
    }
    log_write("SB16: DSP write timeout!", KERNEL, LOG_WARN);
}

void sb16_mixer_write(u8 index, u8 value) {
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

    u8 step = (u8)((percent * 15) / 100);
    u8 level = (u8)((step << 4) | step);

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

static u32 stream_take(void *destination, u32 bytes) {
    u32 take = stream_used < bytes ? stream_used : bytes;
    u32 first = SB16_STREAM_BUFFER_SIZE - stream_read_pos;
    if (first > take)
        first = take;

    memcpy(destination, stream_ring + stream_read_pos, first);
    if (take > first)
        memcpy((u8 *)destination + first, stream_ring, take - first);

    stream_read_pos = (stream_read_pos + take) &
                      (SB16_STREAM_BUFFER_SIZE - 1);
    stream_used -= take;
    return take;
}

static u32 stream_fill_dma_half(int half_index) {
    u32 half_bytes = SB16_DMA_BUFFER_SIZE / 2;
    u8 *target = sb16_dma_buffer_virt + half_index * half_bytes;
    u32 copied = stream_take(target, half_bytes);

    if (copied < half_bytes) {
        memset(target + copied, 0, half_bytes - copied);
        if (!stream_draining)
            stream_underruns++;
    }
    stream_dma_valid[half_index] = copied;
    return copied;
}

/* Refill the half of the DMA buffer that just finished playing while the
 * card consumes the other half. The userspace stream copies only from the
 * kernel ring; the legacy source-loop path is kept for kernel callers. */
void sb16_irq_handler(void) {
    sb16_ack_irq();

    // The IDT sends the PIC EOI for us once this returns.
    if (!sb16_is_playing)
        return;

    u32 half = SB16_DMA_BUFFER_SIZE / 2;
    int half_index = sb16_use_first_half ? 0 : 1;
    u8 *target = sb16_dma_buffer_virt + half_index * half;

    if (playback_kind == PLAYBACK_STREAM) {
        if (stream_dma_pending >= stream_dma_valid[half_index])
            stream_dma_pending -= stream_dma_valid[half_index];
        else
            stream_dma_pending = 0;
        stream_dma_valid[half_index] = 0;
        stream_dma_pending += stream_fill_dma_half(half_index);
        sb16_use_first_half = !sb16_use_first_half;
        return;
    }

    if (playback_kind != PLAYBACK_SOURCE_LOOP || !current_audio_data ||
        current_audio_size == 0)
        return;

    u32 filled = 0;
    while (filled < half) {
        u32 chunk = half - filled;
        u32 remaining = current_audio_size - audio_position;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(target + filled, current_audio_data + audio_position, chunk);
        filled += chunk;
        audio_position += chunk;
        if (audio_position >= current_audio_size)
            audio_position = 0; // loop the track
    }

    sb16_use_first_half = !sb16_use_first_half;
}

void sb16_program_dma(void) {
		u16 count = (SB16_DMA_BUFFER_SIZE / 2) - 1;
    u16 addr_low = (sb16_dma_buffer_phys >> 1) & 0xFFFF;
    u8  addr_page = (sb16_dma_buffer_phys >> 16) & 0xFE;

    outb(DMA_SLAVE_MASK, DMA_MASK_CHAN5);
    outb(DMA_SLAVE_CLEAR_FF, 0xFF);
    /* single transfer | auto-init | read (mem->device) | channel 5.
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
    u64 last = sb16_dma_buffer_phys + SB16_DMA_BUFFER_SIZE - 1;
    if ((sb16_dma_buffer_phys / ISA_DMA16_BOUNDARY) != (last / ISA_DMA16_BOUNDARY)) {
        log_write_hex("SB16: DMA buffer crosses 128K boundary @",
                      sb16_dma_buffer_phys, KERNEL, LOG_ERROR);
        pmm_free_contiguous(sb16_dma_buffer_phys,
                            SB16_DMA_BUFFER_SIZE / FRAME_SIZE);
        sb16_dma_buffer_phys = 0;
        return -1;
    }

    sb16_dma_buffer_virt = (u8 *)phys_to_virt(sb16_dma_buffer_phys);
    memset(sb16_dma_buffer_virt, 0, SB16_DMA_BUFFER_SIZE);
    return 0;
}

void *sb16_dma_buffer(u32 *size_out) {
    if (size_out)
        *size_out = sb16_dma_buffer_phys ? SB16_DMA_BUFFER_SIZE : 0;
    return sb16_dma_buffer_phys ? sb16_dma_buffer_virt : 0;
}

void sb16_set_sample_rate(u16 rate) {
    /* Rate is big-endian here, unlike the block length below. */
    sb16_dsp_write(SB16_DSP_SET_RATE);
    sb16_dsp_write(rate >> 8);
    sb16_dsp_write(rate & 0xFF);
}

void sb16_speaker_on(void) {
    sb16_dsp_write(SB16_DSP_SPEAKER_ON);
}

void sb16_speaker_off(void) {
    sb16_dsp_write(SB16_DSP_SPEAKER_OFF);
}

/* Arm auto-init 16-bit signed stereo playback over the whole DMA buffer.
 *
 * Ordering matters: hook the IRQ and unmask the PIC before arming the DSP,
 * or the first block boundary can arrive with no handler to refill it. */
static void sb16_arm(u32 sample_rate) {
    if (!sb16_irq_hooked) {
        irq_install(sb16_irq, sb16_irq_handler);
        sb16_irq_hooked = 1;
    }
    sb16_ack_irq();
    pic_clear_mask(sb16_irq);

    sb16_program_dma();
    outb(DMA_SLAVE_MASK, DMA_UNMASK_CHAN5);

    sb16_set_sample_rate((u16)sample_rate);

    sb16_dsp_write(SB16_DSP_PLAY_16BIT);
    sb16_dsp_write(SB16_MODE_SIGNED_STEREO);

    /* Half the buffer in 16-bit samples: bytes / 2 (width) / 2 (halves),
     * minus one. Sets IRQ cadence, not total length , see sb16.h. */
    u16 count = (u16)((SB16_DMA_BUFFER_SIZE / 4) - 1);
    sb16_dsp_write(count & 0xFF);
    sb16_dsp_write((count >> 8) & 0xFF);

    sb16_is_playing = 1;
}


/* Loop whatever the DMA buffer already holds.
 * No refill occurs; the DMA+DSP auto-init will keep re-reading the
 * current contents of the DMA buffer until sb16_stop(). */
void sb16_play(void) {
    if (!sb16_dma_buffer_phys) {
        log_write("SB16: play with no DMA buffer", KERNEL, LOG_ERROR);
        return;
    }
    if (sb16_is_playing)
        return;

    current_audio_data = 0;
    current_audio_size = 0;
    playback_kind = PLAYBACK_DMA_LOOP;
    sb16_arm(44100);
}

/* Stream signed 16-bit stereo PCM at 44.1 kHz from `wav_data`, looping
 * at wraparound.
 *
 * `wav_data` must remain valid for the entire playback because the
 * IRQ handler refills the DMA buffer by copying from `wav_data`.
 * If the handler runs under a different address space, `wav_data`
 * must still be mapped there (or must be safe to access everywhere
 * the IRQ handler can execute).
 */
void sb16_play_wav(u8 *wav_data, u32 wav_size) {
    if (!sb16_dma_buffer_phys || !wav_data || wav_size == 0) {
        log_write("SB16: play_wav with no buffer or no data", KERNEL, LOG_ERROR);
        return;
    }
    if (sb16_is_playing)
        sb16_stop();

    current_audio_data = wav_data;
    current_audio_size = wav_size;
    audio_position = 0;

    /* The first IRQ indicates the “first half” has completed, so refill
     * that half first. */
    sb16_use_first_half = 1;

    /* Pre-fill the entire DMA buffer. The device will output the
     * second half before the first interrupt occurs. */
    u32 filled = 0;
    while (filled < SB16_DMA_BUFFER_SIZE) {
        u32 chunk = SB16_DMA_BUFFER_SIZE - filled;
        u32 remaining = wav_size - audio_position;

        if (chunk > remaining)
            chunk = remaining;

        memcpy(sb16_dma_buffer_virt + filled,
               wav_data + audio_position,
               chunk);

        filled += chunk;
        audio_position += chunk;

        if (audio_position >= wav_size)
            audio_position = 0;
    }

    playback_kind = PLAYBACK_SOURCE_LOOP;
    sb16_arm(44100);
}

static void sb16_stop_locked(void) {
    if (!sb16_is_playing)
        return;

    sb16_dsp_write(0xD5); // pause 16-bit DMA
    sb16_dsp_write(0xD9); // exit auto-init 16-bit DMA

    outb(DMA_SLAVE_MASK, DMA_MASK_CHAN5);
    pic_set_mask(sb16_irq);
    sb16_ack_irq();

    sb16_is_playing = 0;
    stream_paused = 0;
    playback_kind = PLAYBACK_NONE;
}

void sb16_stop(void) {
    u64 rflags = irq_save();
    sb16_stop_locked();
    current_audio_data = 0;
    current_audio_size = 0;
    stream_owner_pid = 0;
    stream_read_pos = stream_write_pos = stream_used = 0;
    stream_dma_valid[0] = stream_dma_valid[1] = 0;
    stream_dma_pending = 0;
    stream_draining = 0;
    irq_restore(rflags);
}

static void stream_reset_locked(void) {
    stream_read_pos = 0;
    stream_write_pos = 0;
    stream_used = 0;
    stream_dma_valid[0] = 0;
    stream_dma_valid[1] = 0;
    stream_dma_pending = 0;
    stream_paused = 0;
    stream_draining = 0;
    sb16_use_first_half = 1;
}

static void stream_start_locked(int force) {
    if (!stream_owner_pid || stream_paused || sb16_is_playing ||
        stream_used == 0)
        return;
    if (!force && stream_used < SB16_DMA_BUFFER_SIZE)
        return;

    memset(sb16_dma_buffer_virt, 0, SB16_DMA_BUFFER_SIZE);
    stream_dma_pending = stream_fill_dma_half(0);
    stream_dma_pending += stream_fill_dma_half(1);
    sb16_use_first_half = 1;
    playback_kind = PLAYBACK_STREAM;
    sb16_arm(stream_rate);
}

int sb16_stream_open(int owner_pid, u32 sample_rate,
                     u32 channels, u32 format) {
    if (owner_pid <= 0 || channels != SB16_STREAM_CHANNELS ||
        format != SB16_STREAM_FORMAT_S16_LE ||
        sample_rate < SB16_STREAM_RATE_MIN ||
        sample_rate > SB16_STREAM_RATE_MAX)
        return SB16_STREAM_ERR_INVALID;

    u64 rflags = irq_save();
    if (!sb16_dma_buffer_phys) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NO_DEVICE;
    }
    if (stream_owner_pid && stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_BUSY;
    }

    sb16_stop_locked();
    current_audio_data = 0;
    current_audio_size = 0;
    stream_reset_locked();
    stream_owner_pid = owner_pid;
    stream_rate = sample_rate;
    stream_underruns = 0;
    irq_restore(rflags);
    return 0;
}

long sb16_stream_write(int owner_pid, const void *data, u32 bytes) {
    if (!data || bytes == 0 || (bytes & 3) != 0)
        return bytes == 0 ? 0 : SB16_STREAM_ERR_INVALID;

    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }

    u32 free_bytes = SB16_STREAM_BUFFER_SIZE - stream_used;
    u32 write_bytes = bytes < free_bytes ? bytes : free_bytes;
    write_bytes &= ~3U;

    u32 first = SB16_STREAM_BUFFER_SIZE - stream_write_pos;
    if (first > write_bytes)
        first = write_bytes;
    memcpy(stream_ring + stream_write_pos, data, first);
    if (write_bytes > first)
        memcpy(stream_ring, (const u8 *)data + first,
               write_bytes - first);

    stream_write_pos = (stream_write_pos + write_bytes) &
                       (SB16_STREAM_BUFFER_SIZE - 1);
    stream_used += write_bytes;
    stream_draining = 0;
    stream_start_locked(0);
    irq_restore(rflags);
    return (long)write_bytes;
}

int sb16_stream_status(struct sb16_stream_status *out) {
    if (!out)
        return SB16_STREAM_ERR_INVALID;

    u64 rflags = irq_save();
    *out = (struct sb16_stream_status){
        .available = sb16_dma_buffer_phys != 0,
        .playing = playback_kind == PLAYBACK_STREAM && sb16_is_playing &&
                   !stream_paused,
        .paused = stream_paused,
        .sample_rate = stream_rate,
        .channels = SB16_STREAM_CHANNELS,
        .format = SB16_STREAM_FORMAT_S16_LE,
        .ring_capacity = SB16_STREAM_BUFFER_SIZE,
        .ring_queued = stream_used,
        .device_queued = stream_dma_pending,
        .underruns = stream_underruns,
        .volume = (u32)stream_volume,
        .owner_pid = stream_owner_pid,
    };
    irq_restore(rflags);
    return 0;
}

int sb16_stream_begin_drain(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }

    stream_draining = 1;
    if (stream_paused && sb16_is_playing)
        sb16_dsp_write(0xD6); /* continue 16-bit DMA */
    stream_paused = 0;
    stream_start_locked(1);
    irq_restore(rflags);
    return 0;
}

u32 sb16_stream_pending(int owner_pid) {
    u64 rflags = irq_save();
    u32 pending = stream_owner_pid == owner_pid
                     ? stream_used + stream_dma_pending : 0;
    irq_restore(rflags);
    return pending;
}

int sb16_stream_finish_drain(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }
    if (stream_used || stream_dma_pending) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_BUSY;
    }

    sb16_stop_locked();
    stream_reset_locked();
    irq_restore(rflags);
    return 0;
}

int sb16_stream_pause(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }
    if (sb16_is_playing && !stream_paused)
        sb16_dsp_write(0xD5); /* pause 16-bit DMA */
    stream_paused = 1;
    irq_restore(rflags);
    return 0;
}

int sb16_stream_resume(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }
    if (stream_paused && sb16_is_playing)
        sb16_dsp_write(0xD6); /* continue 16-bit DMA */
    stream_paused = 0;
    stream_start_locked(0);
    irq_restore(rflags);
    return 0;
}

int sb16_stream_set_volume(int owner_pid, int percent) {
    if (percent < 0 || percent > 100)
        return SB16_STREAM_ERR_INVALID;

    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }
    sb16_set_volume(percent);
    stream_volume = percent;
    irq_restore(rflags);
    return 0;
}

int sb16_stream_close(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid != owner_pid) {
        irq_restore(rflags);
        return SB16_STREAM_ERR_NOT_OWNER;
    }

    sb16_stop_locked();
    stream_reset_locked();
    stream_owner_pid = 0;
    irq_restore(rflags);
    return 0;
}

void sb16_stream_release(int owner_pid) {
    u64 rflags = irq_save();
    if (stream_owner_pid == owner_pid) {
        sb16_stop_locked();
        stream_reset_locked();
        stream_owner_pid = 0;
    }
    irq_restore(rflags);
}

/* Only matches ISA devices with standard SB16 ports (0x220 or 0x240) */
static int sb16_match(const struct device *device) {
    if (device->bus != DEVICE_BUS_ISA) return 0;
    const struct isa_device *isa = &device->bus_info.isa;
    return (isa->io_base == 0x220 || isa->io_base == 0x240);
}

static int sb16_probe(struct device *device) {
    struct isa_device *isa = &device->bus_info.isa;
    u16 io_base = isa->io_base;

    outb(io_base + SB16_REG_DSP_RESET, 0x01);
    for (volatile int i = 0; i < 10000; i++);
    outb(io_base + SB16_REG_DSP_RESET, 0x00);

    for (int i = 0; i < 100000; i++) {
        if (inb(io_base + SB16_REG_DSP_RSTAT) & 0x80) {
            if (inb(io_base + SB16_REG_DSP_READ) == 0xAA) {
                log_write("SB16: SoundBlaster 16 detected!", KERNEL, LOG_INFO);

                /* Initialize SB16 (PCI/ISA-independent) before any DSP/mixer I/O.
                 * All subsequent callers assume this card’s state, which may not
                 * correspond to the device at I/O 0x220. */
                sb16_base = io_base;
                sb16_irq = (u8)(isa->irq ? isa->irq : SB16_DEFAULT_IRQ);

                irq_install(sb16_irq, sb16_irq_handler);
                sb16_irq_hooked = 1;
                sb16_ack_irq();
                pic_set_mask(sb16_irq);
                // Give the DSP a moment to stabilize after reset
                for (volatile int i = 0; i < 10000; i++);

                sb16_speaker_on();
                sb16_set_sample_rate(44100);
                sb16_reset_mixer();
                sb16_set_max_volume();

                /* Allocate the DMA resources, then program the channel while
                * keeping it masked and the DSP quiescent.
                * Probing must not trigger an uncontrolled auto-DMA transfer;
                * playback begins via sb16_play(). */
                if (sb16_allocate_dma_buffer() != 0)
                    return -1;
                sb16_program_dma();

                log_write("SB16: initialised", KERNEL, LOG_INFO);
                return 0;
            }
        }
    }
    return -1;
}

const struct driver sb16_driver = {
    .name = "SoundBlaster 16",
    .bus = DEVICE_BUS_ISA,
    .match = sb16_match,
    .probe = sb16_probe
};
