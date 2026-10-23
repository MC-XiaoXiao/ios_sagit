/* UARTDM v1.4 register interface. Clocks and pinmux are owned by firmware. */
#ifndef _PEXPERT_QCOM_UARTDM_H
#define _PEXPERT_QCOM_UARTDM_H
#define QUDM_MR1         0x00
#define QUDM_MR2         0x04
#define QUDM_SR          0x08
#define QUDM_CR          0x10
#define QUDM_IMR         0x14
#define QUDM_ISR         0x14
#define QUDM_IPR         0x18
#define QUDM_TFWR        0x1c
#define QUDM_RFWR        0x20
#define QUDM_DMRX        0x34
#define QUDM_RX_SNAP     0x38
#define QUDM_DMEN        0x3c
#define QUDM_NCF_TX      0x40
#define QUDM_RXFS        0x50
#define QUDM_FIFO        0x70
#define QUDM_RX_READY    (1U << 0)
#define QUDM_TX_READY    (1U << 2)
#define QUDM_TX_EMPTY    (1U << 3)
#define QUDM_OVERRUN     (1U << 4)
#define QUDM_RX_STALE    (1U << 3)
#define QUDM_RESET_RX    0x10
#define QUDM_RESET_TX    0x20
#define QUDM_RESET_ERR   0x30
#define QUDM_RESET_STALE 0x80
#define QUDM_FORCE_STALE 0x400
#define QUDM_STALE_OFF   0x600
#define QUDM_ENABLE      0x05
#define QUDM_DISABLE     0x0a
#define QUDM_8N1         0x34
#define QUDM_RX_LIMIT    0xffffff
#endif
