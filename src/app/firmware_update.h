#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

/* In-place Nano firmware update over the UART0 console (the 'u' command).
 *
 * The host streams a firmware image; this stages it as /fip.new on the SD
 * card (which stays in the Nano), verifies its size and CRC-32, replaces the
 * boot image /fip.bin, and reboots. Blocks the caller. Returns negative on
 * error (the existing /fip.bin is left untouched); on success it does not
 * return. */
int firmware_update_receive(void);

#endif
