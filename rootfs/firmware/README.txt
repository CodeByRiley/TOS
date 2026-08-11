Place NVIDIA firmware files in this directory using their upstream names:

  gsp_ga10x.bin
  gsp_tu10x.bin
  ucodes_ga10x.bin
  ucodes_tu10x.bin

create_disk.sh copies files that exist into the flat FAT16 root using these
8.3 names:

  GSPGA10X.BIN
  GSPTU10X.BIN
  UCGA10X.BIN
  UCTU10X.BIN

The GSP image is required for native NVIDIA initialization. When present, the
optional ucode archive is staged as the GSP external-bindata radix image.

The driver currently validates and stages these files in system memory. It
does not yet reset or start the GSP, so the firmware boot framebuffer remains
active.
