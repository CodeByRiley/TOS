#!/usr/bin/env bash
cd "$(dirname "$0")/.."
qemu-system-x86_64 -cdrom dist/x86_64/kernel.iso -cpu qemu64 -smp 4 -serial stdio -m 2048M
