/* AHCI adapter for ordinary virtual-buffer sector I/O. */
#ifndef AHCI_BLOCK_H
#define AHCI_BLOCK_H
#include "ahci.h"
#include "block.h"
#include <sync/spinlock.h>

struct ahci_block_device {
    struct block_device device;
    struct AHCI_DEVICE_DATA *controller;
    int port;
    uint64_t bounce_phys;
    unsigned char *bounce;
    struct spinlock lock;
};
/* The adapter must outlive every mount using device. Do not reopen it live. */
int ahci_block_open(struct ahci_block_device *disk, struct AHCI_DEVICE_DATA *controller, int port);
void ahci_block_close(struct ahci_block_device *disk);
#endif
