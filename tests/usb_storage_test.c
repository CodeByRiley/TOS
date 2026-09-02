/* Wire-level BOT/SCSI fake: checks framing, endian conversion, and recovery. */
#include <drivers/usb/storage/usb_storage.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum fault { NONE, BAD_TAG, BAD_SIGNATURE, BAD_RESIDUE, SHORT_CSW, PHASE_ERROR,
             COMMAND_FAILED, CSW_STALL, DATA_STALL, TIMEOUT };
struct fake_usb {
    unsigned char disk[512 * 512], cdb[16];
    uint32_t tag, length, residue;
    int stage, input, resets, clears, commands, maxlun_stall, reset_fail;
    int capacity_4k;
    enum fault fault;
    unsigned char recovery[3];
};
static uint32_t le32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const unsigned char *p) {
    return p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static void put32(unsigned char *p, uint32_t n) {
    p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24;
}
static int control(void *ctx, const struct usb_setup_packet *setup, void *data) {
    struct fake_usb *f = ctx;
    if (setup->bRequest == 0xfe) {
        assert(setup->bmRequestType == 0xa1 && setup->wIndex == 3 && setup->wLength == 1);
        if (f->maxlun_stall) return USB_STORAGE_STALL;
        *(unsigned char *)data = 0;
        return 1;
    }
    if (setup->bRequest == 0xff) {
        assert(setup->bmRequestType == 0x21 && setup->wIndex == 3 && !setup->wLength);
        f->resets++; f->stage = 0; f->recovery[0] = 0xff;
        return f->reset_fail ? -1 : 0;
    }
    assert(setup->bRequest == 1 && setup->bmRequestType == 2 && !setup->wValue);
    assert(setup->wIndex == 0x81 || setup->wIndex == 2);
    f->clears++;
    f->recovery[setup->wIndex == 0x81 ? 1 : 2] = setup->wIndex;
    return 0;
}
static int bulk(void *ctx, uint8_t ep, uint16_t packet, uint8_t *toggle,
                void *buffer, uint32_t length, uint32_t *actual) {
    struct fake_usb *f = ctx;
    unsigned char *p = buffer;
    assert(packet == 512);
    *actual = 0;
    if (f->fault == TIMEOUT) { f->fault = NONE; return -1; }
    if (f->stage == 0) {
        assert(ep == 2 && length == 31 && le32(p) == 0x43425355);
        assert(p[13] == 0 && p[14] >= 6 && p[14] <= 16);
        f->commands++; f->tag = le32(p + 4); f->length = le32(p + 8);
        f->input = p[12] == 0x80; f->residue = 0;
        memcpy(f->cdb, p + 15, 16);
        f->stage = f->length ? 1 : 2;
    } else if (f->stage == 1) {
        assert(ep == (f->input ? 0x81 : 2) && length == f->length);
        f->stage = 2;
        if (f->fault == DATA_STALL) {
            f->fault = COMMAND_FAILED; f->residue = length;
            return USB_STORAGE_STALL;
        }
        if (f->input) memset(p, 0, length);
        switch (f->cdb[0]) {
        case 0x12: assert(length == 36); break;
        case 0x25:
            assert(length == 8);
            p[2] = 1; p[3] = 0xff; /* 512 sectors */
            p[6] = f->capacity_4k ? 16 : 2;
            break;
        case 0x28: case 0x2a: {
            uint32_t lba = be32(f->cdb + 2);
            unsigned count = (unsigned)f->cdb[7] << 8 | f->cdb[8];
            assert(length == count * 512 && lba + count <= 512);
            if (f->input) memcpy(p, f->disk + lba * 512, length);
            else memcpy(f->disk + lba * 512, p, length);
            break;
        }
        default: assert(0);
        }
    } else {
        assert(ep == 0x81 && length == 13);
        if (f->fault == CSW_STALL) { f->fault = NONE; return USB_STORAGE_STALL; }
        memset(p, 0, length);
        put32(p, f->fault == BAD_SIGNATURE ? 0 : 0x53425355);
        put32(p + 4, f->tag + (f->fault == BAD_TAG));
        put32(p + 8, f->fault == BAD_RESIDUE ? f->length + 1 : f->residue);
        p[12] = f->fault == PHASE_ERROR ? 2 : f->fault == COMMAND_FAILED;
        if (f->fault == SHORT_CSW) length--;
        f->fault = NONE; f->stage = 0;
    }
    *actual = length;
    *toggle ^= ((length + 511) / 512) & 1;
    return 0;
}

int main(void) {
    static struct fake_usb f;
    unsigned char config[] = {
        9,2,32,0,1,1,0,0x80,50,
        9,4,3,0,2,8,6,0x50,0,
        7,5,0x81,2,0,2,0,
        7,5,2,2,0,2,0,
    };
    struct usb_storage_transport transport = {&f, control, bulk};
    assert(usb_storage_probe(&transport, config, sizeof(config) - 1) < 0);
    config[23] = 0;
    assert(usb_storage_probe(&transport, config, sizeof(config)) < 0);
    config[23] = 2;
    config[16] = 0x62; /* UAS is not BOT */
    assert(usb_storage_probe(&transport, config, sizeof(config)) < 0);
    config[16] = 0x50;
    f.capacity_4k = 1;
    assert(usb_storage_probe(&transport, config, sizeof(config)) < 0);
    f.capacity_4k = 0; f.maxlun_stall = 1;
    assert(usb_storage_probe(&transport, config, sizeof(config)) == 0);
    assert(usb_storage_count() == 1 && !usb_storage_device(1));
    const struct block_device *block = usb_storage_device(0);
    assert(block->sectors == 512);
    static unsigned char in[130 * 512], out[130 * 512];
    for (size_t i = 0; i < sizeof(in); i++) in[i] = i * 17 + (i >> 9);
    assert(block_write(block, 257, 130, in) == 0);
    assert(block_read(block, 257, 130, out) == 0 && !memcmp(in, out, sizeof(in)));
    assert(block_flush(block) == 0);
    int commands = f.commands;
    assert(block_read(block, 511, 2, out) < 0 && f.commands == commands);

    struct usb_storage *disk = block->context;
    const unsigned char ready[6] = {0};
    for (enum fault fault = BAD_TAG; fault <= PHASE_ERROR; fault++) {
        int resets = f.resets;
        f.fault = fault;
        assert(usb_bot_command(disk, ready, 6, 0, 0, 0, 0) < 0);
        assert(f.resets == resets + 1 && !disk->in_toggle && !disk->out_toggle);
        assert(f.recovery[0] == 0xff && f.recovery[1] == 0x81 && f.recovery[2] == 2);
        assert(block_flush(block) == 0);
    }
    int resets = f.resets;
    f.fault = COMMAND_FAILED;
    assert(usb_bot_command(disk, ready, 6, 0, 0, 0, 0) == 1 && f.resets == resets);
    f.fault = CSW_STALL;
    assert(block_flush(block) == 0 && f.resets == resets);
    f.fault = DATA_STALL;
    assert(block_read(block, 0, 1, out) != 0 && f.resets == resets);
    assert(block_read(block, 0, 1, out) == 0);
    f.fault = TIMEOUT; f.reset_fail = 1;
    assert(block_write(block, 0, 1, in) < 0 && disk->offline);
    commands = f.commands;
    assert(block_read(block, 0, 1, out) < 0 && f.commands == commands);
    puts("USB storage: BOT framing/recovery, SCSI read/write/flush, and probe checks passed");
    return 0;
}
