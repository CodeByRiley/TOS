/* The USB device model shared by the host controllers.
 *
 * Everything here used to exist twice, once inside uhci.c and once inside
 * ehci.c, where neither copy could be reached without a controller. Behind
 * struct usb_pipe it is ordinary logic over a fake transfer function, so the
 * setup packets it builds and the descriptor walking it does can be asserted
 * directly.
 */
#include "drivers/usb/usb_device.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

/* usb_device.c logs through four of the eight kernel log entry points, and
 * tests/host_kernel_stubs.c only stands in for two of them. */
void log_write(const char *m, uint8_t t, uint8_t l) { (void)m; (void)t; (void)l; }
void log_write_hex(const char *m, uint64_t v, uint8_t t, uint8_t l) {
    (void)m; (void)v; (void)t; (void)l;
}
void log_write_int(const char *m, int64_t v, uint8_t t, uint8_t l) {
    (void)m; (void)v; (void)t; (void)l;
}
void log_write_string(const char *m, const char *v, uint8_t t, uint8_t l) {
    (void)m; (void)v; (void)t; (void)l;
}

/* --- fake controller ---------------------------------------------------- */

#define MAX_CALLS 16

struct call {
    struct usb_setup_packet setup;
    u8 address;
    u16 maxpacket;
    int low_speed;
    u16 length;
    int in;
};

static struct call calls[MAX_CALLS];
static int call_count;
/* Bytes handed back for the next data-stage read, if any. */
static const u8 *reply;
static u16 reply_length;
static int fail_next;

static int fake_control(const struct usb_pipe *pipe,
                        const struct usb_setup_packet *setup, void *data,
                        u16 length, int in) {
    if (call_count < MAX_CALLS) {
        calls[call_count] = (struct call){.setup = *setup,
                                          .address = pipe->address,
                                          .maxpacket = pipe->maxpacket,
                                          .low_speed = pipe->low_speed,
                                          .length = length,
                                          .in = in};
    }
    call_count++;
    if (fail_next) {
        fail_next = 0;
        return -1;
    }
    u16 copied = 0;
    if (in && data && reply) {
        copied = length < reply_length ? length : reply_length;
        memcpy(data, reply, copied);
    }
    return in ? (int)copied : 0;
}

static struct usb_pipe make_pipe(void) {
    call_count = 0;
    reply = 0;
    reply_length = 0;
    fail_next = 0;
    struct usb_pipe pipe = {.hc = (void *)0x1234,
                            .control = fake_control,
                            .address = 7,
                            .maxpacket = 64,
                            .low_speed = 1,
                            .tag = "TEST"};
    return pipe;
}

/* --- standard requests --------------------------------------------------- */

static void test_get_descriptor_builds_the_request(void) {
    struct usb_pipe pipe = make_pipe();
    u8 out[18] = {0};
    static const u8 canned[18] = {18, 1, 0x00, 0x02};
    reply = canned;
    reply_length = sizeof(canned);

    expect(usb_get_descriptor(&pipe, USB_DESC_DEVICE, 0, out, sizeof(out)) ==
               (int)sizeof(out),
           "get_descriptor returns the byte count the transfer reported");
    expect(call_count == 1, "get_descriptor issues exactly one transfer");
    expect(calls[0].setup.bmRequestType == USB_REQTYPE_IN_STD_DEVICE,
           "get_descriptor is a device-to-host standard request");
    expect(calls[0].setup.bRequest == USB_REQ_GET_DESCRIPTOR,
           "get_descriptor sends GET_DESCRIPTOR");
    expect(calls[0].setup.wValue == ((USB_DESC_DEVICE << 8) | 0),
           "descriptor type and index are packed into wValue");
    expect(calls[0].setup.wLength == sizeof(out) && calls[0].in == 1,
           "the data stage is an IN of the requested length");
    expect(calls[0].address == 7 && calls[0].low_speed == 1,
           "the pipe's address and speed reach the controller");
    expect(memcmp(out, canned, sizeof(canned)) == 0,
           "the device's bytes arrive in the caller's buffer");
}

/* A device only adopts its new address once the status stage completes, so
 * the request itself has to go to address 0. */
static void test_set_address_is_sent_to_address_zero(void) {
    struct usb_pipe pipe = make_pipe();
    expect(usb_set_address(&pipe, 9) == 0, "set_address succeeds");
    expect(calls[0].address == 0,
           "SET_ADDRESS is addressed to 0 even when the pipe already has one");
    expect(pipe.address == 7, "set_address does not mutate the caller's pipe");
    expect(calls[0].setup.bRequest == USB_REQ_SET_ADDRESS &&
               calls[0].setup.wValue == 9,
           "the new address travels in wValue");
    expect(calls[0].setup.wLength == 0 && calls[0].in == 0,
           "SET_ADDRESS has no data stage");
}

static void test_set_configuration(void) {
    struct usb_pipe pipe = make_pipe();
    expect(usb_set_configuration(&pipe, 3) == 0, "set_configuration succeeds");
    expect(calls[0].setup.bmRequestType == USB_REQTYPE_OUT_STD_DEVICE &&
               calls[0].setup.bRequest == USB_REQ_SET_CONFIGURATION &&
               calls[0].setup.wValue == 3,
           "set_configuration sends the configuration value");
    expect(calls[0].address == 7,
           "set_configuration goes to the addressed device");
}

static void test_hid_requests(void) {
    struct usb_pipe pipe = make_pipe();
    expect(usb_hid_set_idle(&pipe, 2) == 0, "set_idle succeeds");
    expect(calls[0].setup.bmRequestType == USB_REQTYPE_OUT_CLASS_INTERFACE &&
               calls[0].setup.bRequest == USB_REQ_SET_IDLE &&
               calls[0].setup.wIndex == 2,
           "SET_IDLE is a class request aimed at the interface");

    pipe = make_pipe();
    expect(usb_hid_set_boot_protocol(&pipe, 5) == 0, "set_protocol succeeds");
    expect(calls[0].setup.bRequest == USB_REQ_SET_PROTOCOL &&
               calls[0].setup.wValue == 0 && calls[0].setup.wIndex == 5,
           "boot protocol is protocol 0 on the given interface");
}

static void test_a_failed_transfer_is_reported(void) {
    struct usb_pipe pipe = make_pipe();
    fail_next = 1;
    expect(usb_set_configuration(&pipe, 1) < 0,
           "a controller failure propagates out of the request");

    struct usb_pipe broken = make_pipe();
    broken.control = 0;
    expect(usb_set_configuration(&broken, 1) < 0,
           "a pipe with no transfer function fails rather than dereferencing it");
}

/* --- configuration blocks ------------------------------------------------ */

/* header: 9 bytes, wTotalLength at offset 2 */
static u8 config_block[64];
static void build_config(u16 total) {
    memset(config_block, 0, sizeof(config_block));
    config_block[0] = 9;
    config_block[1] = USB_DESC_CONFIGURATION;
    config_block[2] = (u8)(total & 0xFF);
    config_block[3] = (u8)(total >> 8);
    config_block[5] = 1; /* bConfigurationValue */
}

static void test_read_config_is_a_two_step_read(void) {
    struct usb_pipe pipe = make_pipe();
    build_config(32);
    reply = config_block;
    reply_length = sizeof(config_block);
    u8 buffer[64];

    expect(usb_read_config(&pipe, 0, buffer, sizeof(buffer)) == 32,
           "read_config returns wTotalLength bytes");
    expect(call_count == 2, "read_config reads the header, then the block");
    expect(calls[0].setup.wLength == 9,
           "the first read asks for just the 9-byte header");
    expect(calls[1].setup.wLength == 32,
           "the second read asks for the length the header declared");
}

static void test_read_config_truncates_rather_than_overruns(void) {
    struct usb_pipe pipe = make_pipe();
    build_config(4096); /* a device claiming far more than we can hold */
    reply = config_block;
    reply_length = sizeof(config_block);
    u8 buffer[32];

    int got = usb_read_config(&pipe, 0, buffer, sizeof(buffer));
    expect(got == (int)sizeof(buffer),
           "an oversized configuration is truncated to the buffer");
    expect(calls[1].setup.wLength == sizeof(buffer),
           "and the device is only asked for what fits");
}

static void test_read_config_rejects_a_nonsense_header(void) {
    struct usb_pipe pipe = make_pipe();
    build_config(4); /* smaller than the header itself */
    reply = config_block;
    reply_length = sizeof(config_block);
    u8 buffer[64];
    expect(usb_read_config(&pipe, 0, buffer, sizeof(buffer)) < 0,
           "a wTotalLength below the header size is rejected");

    pipe = make_pipe();
    u8 tiny[4];
    expect(usb_read_config(&pipe, 0, tiny, sizeof(tiny)) < 0,
           "a buffer too small for a header is rejected before any transfer");
    expect(call_count == 0, "and no transfer was issued");
}

/* --- HID pointer discovery ----------------------------------------------- */

/* Build a configuration block: interface descriptor, optional HID descriptor,
 * then one endpoint descriptor. */
static int build_hid_config(u8 *out, u8 interface_class, u8 subclass,
                            u8 protocol, int with_hid_desc, u16 report_length,
                            u8 endpoint_attributes, u8 endpoint_address,
                            u16 max_packet) {
    int at = 0;
    out[at++] = 9;
    out[at++] = USB_DESC_INTERFACE;
    out[at++] = 3; /* bInterfaceNumber */
    out[at++] = 0;
    out[at++] = 1; /* bNumEndpoints */
    out[at++] = interface_class;
    out[at++] = subclass;
    out[at++] = protocol;
    out[at++] = 0;
    if (with_hid_desc) {
        out[at++] = 9;
        out[at++] = USB_DESC_HID;
        out[at++] = 0; out[at++] = 0; out[at++] = 0; out[at++] = 0;
        out[at++] = 0;
        out[at++] = (u8)(report_length & 0xFF);
        out[at++] = (u8)(report_length >> 8);
    }
    out[at++] = 7;
    out[at++] = USB_DESC_ENDPOINT;
    out[at++] = endpoint_address;
    out[at++] = endpoint_attributes;
    out[at++] = (u8)(max_packet & 0xFF);
    out[at++] = (u8)(max_packet >> 8);
    out[at++] = 10; /* bInterval */
    return at;
}

static void test_finds_a_boot_mouse(void) {
    u8 block[64];
    int length = build_hid_config(block, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT,
                                  USB_HID_PROTOCOL_MOUSE, 0, 0,
                                  USB_EP_XFER_INTERRUPT,
                                  USB_EP_ADDR_DIR_IN | 1, 8);
    struct usb_hid_pointer pointer;
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 1,
           "a boot-protocol mouse is recognised");
    expect(pointer.kind == USB_INT_KIND_HID_BOOT_MOUSE, "and classed as one");
    expect(pointer.interface_number == 3, "the owning interface is reported");
    expect(pointer.endpoint.wMaxPacketSize == 8,
           "the endpoint's packet size is masked to 11 bits and kept");
}

static void test_tablet_is_matched_only_by_its_report_length(void) {
    u8 block[64];
    struct usb_hid_pointer pointer;

    int length = build_hid_config(block, USB_CLASS_HID, 0, 0, 1, 74,
                                  USB_EP_XFER_INTERRUPT,
                                  USB_EP_ADDR_DIR_IN | 1, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 1 &&
               pointer.kind == USB_INT_KIND_HID_TABLET,
           "a 74-byte HID report descriptor is treated as a tablet");

    /* Any other vendor HID device must not be guessed into a mouse. */
    length = build_hid_config(block, USB_CLASS_HID, 0, 0, 1, 52,
                              USB_EP_XFER_INTERRUPT, USB_EP_ADDR_DIR_IN | 1, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 0,
           "a HID device with any other report length is not a pointer");

    /* A tablet's packets must be big enough to carry absolute coordinates. */
    length = build_hid_config(block, USB_CLASS_HID, 0, 0, 1, 74,
                              USB_EP_XFER_INTERRUPT, USB_EP_ADDR_DIR_IN | 1, 4);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 0,
           "a tablet endpoint smaller than 5 bytes is rejected");
}

static void test_endpoint_must_be_interrupt_in(void) {
    u8 block[64];
    struct usb_hid_pointer pointer;

    int length = build_hid_config(block, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT,
                                  USB_HID_PROTOCOL_MOUSE, 0, 0,
                                  USB_EP_XFER_BULK, USB_EP_ADDR_DIR_IN | 1, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 0,
           "a bulk endpoint is not polled as a pointer");

    length = build_hid_config(block, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT,
                              USB_HID_PROTOCOL_MOUSE, 0, 0,
                              USB_EP_XFER_INTERRUPT, 1 /* OUT */, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 0,
           "an OUT endpoint is not polled as a pointer");

    length = build_hid_config(block, 0x08 /* mass storage */, 0, 0, 0, 0,
                              USB_EP_XFER_INTERRUPT, USB_EP_ADDR_DIR_IN | 1, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 0,
           "a non-HID interface is not a pointer");
}

/* The one place the two controllers genuinely differ: EHCI has a fixed
 * interrupt buffer, UHCI does not. */
static void test_max_packet_limit_is_the_callers_choice(void) {
    u8 block[64];
    int length = build_hid_config(block, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT,
                                  USB_HID_PROTOCOL_MOUSE, 0, 0,
                                  USB_EP_XFER_INTERRUPT,
                                  USB_EP_ADDR_DIR_IN | 1, 128);
    struct usb_hid_pointer pointer;

    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 64, "TEST", &pointer) == 0,
           "an endpoint above the caller's limit is rejected");

    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 0, "TEST", &pointer) == 1,
           "the same endpoint is accepted when the caller sets no limit");

    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(block, length, 128, "TEST", &pointer) == 1,
           "the limit is inclusive");
}

static void test_malformed_blocks_terminate(void) {
    u8 block[64];
    struct usb_hid_pointer pointer;
    memset(&pointer, 0, sizeof(pointer));

    /* A zero-length descriptor would advance the cursor by nothing. Stopping
     * is the only safe answer, so anything behind it stays unreachable , and
     * asserting that is what distinguishes stopping from skipping over it. */
    u8 poisoned[80];
    memset(poisoned, 0, sizeof(poisoned));
    poisoned[0] = 0; /* zero length */
    poisoned[1] = USB_DESC_INTERFACE;
    int tail = build_hid_config(poisoned + 2, USB_CLASS_HID,
                                USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE,
                                0, 0, USB_EP_XFER_INTERRUPT,
                                USB_EP_ADDR_DIR_IN | 1, 8);
    memset(&pointer, 0, sizeof(pointer));
    expect(usb_find_hid_pointer(poisoned, tail + 2, 0, "TEST", &pointer) == 0,
           "a zero-length descriptor stops the walk, hiding what follows it");

    memset(block, 0, sizeof(block));
    block[0] = 1; /* shorter than a descriptor header */
    block[1] = USB_DESC_INTERFACE;
    expect(usb_find_hid_pointer(block, 16, 0, "TEST", &pointer) == 0,
           "a one-byte descriptor stops the walk too");

    /* A descriptor claiming to run past the end of the block. */
    memset(block, 0, sizeof(block));
    block[0] = 40;
    block[1] = USB_DESC_INTERFACE;
    expect(usb_find_hid_pointer(block, 8, 0, "TEST", &pointer) == 0,
           "a descriptor overrunning the block stops the walk");

    expect(usb_find_hid_pointer(0, 32, 0, "TEST", &pointer) == 0,
           "a null block is not walked");
    expect(usb_find_hid_pointer(block, 0, 0, "TEST", &pointer) == 0,
           "an empty block finds nothing");
}

int main(void) {
    test_get_descriptor_builds_the_request();
    test_set_address_is_sent_to_address_zero();
    test_set_configuration();
    test_hid_requests();
    test_a_failed_transfer_is_reported();

    test_read_config_is_a_two_step_read();
    test_read_config_truncates_rather_than_overruns();
    test_read_config_rejects_a_nonsense_header();

    test_finds_a_boot_mouse();
    test_tablet_is_matched_only_by_its_report_length();
    test_endpoint_must_be_interrupt_in();
    test_max_packet_limit_is_the_callers_choice();
    test_malformed_blocks_terminate();

    if (failures) {
        fprintf(stderr, "usb_device_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("usb_device_test: all checks passed\n");
    return 0;
}
