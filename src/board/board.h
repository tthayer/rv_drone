#ifndef BOARD_H
#define BOARD_H
#if defined(BOARD_NANO)
#include "board_nano.h"
#elif defined(BOARD_QEMU)
#include "board_qemu.h"
#else
#error "Define BOARD_NANO or BOARD_QEMU (make BOARD=nano|qemu)"
#endif
#endif
