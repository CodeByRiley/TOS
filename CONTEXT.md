# Domain language

Terms this codebase uses in a specific way. Subsystem-level detail lives with
the subsystem , see `kernel/fs/README.md` for the filesystem object model.

## User address space

**Address space** , one `struct task_vm`: a page table, a table of
reservations, and two arenas. Kernel-only tasks have none. Owned by
`kernel/memory/uvm.{c,h}`.

**Reservation** , a range of user virtual addresses that is legal to touch,
carrying the page flags its pages get when they are materialised. A
reservation is not memory: it holds no frames until something faults.
`struct user_vma` is one record in the table.

**Materialise** (fault in) , turn one page of a reservation into a real
zeroed frame. The only two callers are the page-fault handler and
`uvm_buffer_ok`; both go through `uvm_fault_in`, so both enforce the
reservation's permissions identically. A write into a read-only reservation
is refused rather than mapped.

**Arena** , a bump-allocated region of the address space that hands out
auto-placed ranges. There are two: the mmap arena and the shmem arena. A
released mmap range goes back to the arena's **hole list** for reuse.

**Address-space map** , the fixed layout of the user half (image, framebuffer
window, mmap arena, shmem arena, stack). Constants in `kernel/loader/process.h`.

## Storage

**Block device** , the 512-byte sector interface in
`kernel/drivers/storage/block.h`: read, write, flush, capacity. Two adapters
today, AHCI and USB mass storage. Buffers are ordinary virtual addresses; the
transport owns DMA. Both filesystems mount through it, so any filesystem can
be read from any transport.

**Transport** , what carries sectors to a device (AHCI, USB BOT/SCSI). Distinct
from the filesystem that interprets them.

**Root search** , the boot-time pairing of transports with filesystems in
`kernel/fs/rootfs.c`. Not a fixed order of special cases: it walks the devices
the drivers found and offers each to every registered filesystem until one
recognises the volume. The Multiboot ramdisk is the fallback, not the first
choice, so a machine with a formatted disk boots from the disk.

**Write-through** , FAT's persistence model: every changed sector is written to
the device as it changes, rather than being tracked and written later. ext2
instead marks blocks dirty and writes them at sync. Both cache the whole
volume in RAM.

## Display

**Scanout** , the framebuffer the host or hardware actually displays. A backend
either owns scanout or does not; `kernel/display/framebuffer.c` tracks which.

**Damage** , the accumulated rectangles that changed since the last present.
Accumulated separately in libtos, in the `SYS_FB_PRESENT` ABI, and in the
kernel, with three different budgets.

## Userspace

**Winman** , the desktop compositor, `userspace/bin/winman/`. Clients talk to
it over IPC through **libwm** (`userspace/lib/wm.h`) and never see the wire
protocol.

**libtos** , the TOS-specific userspace library: windows, graphics, audio, IPC,
process inspection. Distinct from musl, which supplies the standard C library.
