#ifndef USB_PAL_H_
#define USB_PAL_H_

#include <stdint.h>

#define __IO volatile
#define __I  volatile const

/* --- Bitfield Definitions --- */

typedef struct {
    uint32_t enable_sof     : 1;  /* Bit 0: Enable SOF generation */
    uint32_t phy_opmode     : 2;  /* Bits 1-2: UTMI PHY Output Mode */
    uint32_t phy_xcvrselect : 2;  /* Bits 3-4: UTMI PHY Transceiver Select */
    uint32_t phy_termselect : 1;  /* Bit 5: UTMI PHY Termination Select */
    uint32_t phy_dppulldown : 1;  /* Bit 6: UTMI PHY D+ Pulldown Enable */
    uint32_t phy_dmpulldown : 1;  /* Bit 7: UTMI PHY D- Pulldown Enable */
    uint32_t tx_flush       : 1;  /* Bit 8: Flush Tx FIFO */
    uint32_t reserved       : 23;
} usb_ctrl_bits_t;

typedef struct {
    uint32_t linestate     : 2;  /* Bits 0-1: 1=D-, 0=D+ */
    uint32_t rx_error      : 1;  /* Bit 2: Rx error detected */
    uint32_t reserved      : 13;
    uint32_t sof_time      : 16; /* Bits 16-31: Current frame time */
} usb_status_bits_t;

typedef struct {
    uint32_t sof           : 1;  /* Bit 0: Start of Frame */
    uint32_t done          : 1;  /* Bit 1: Transfer completion */
    uint32_t err           : 1;  /* Bit 2: Error conditions */
    uint32_t device_detect : 1;  /* Bit 3: linestate != SE0 */
    uint32_t reserved      : 28;
} usb_irq_bits_t;

typedef struct {
    uint32_t count         : 16; /* Bits 0-15: Received data count */
    uint32_t resp_pid      : 8;  /* Bits 16-23: Received response PID */
    uint32_t reserved      : 4;
    uint32_t idle          : 1;  /* Bit 28: SIE idle */
    uint32_t resp_timeout  : 1;  /* Bit 29: Response timeout */
    uint32_t crc_err       : 1;  /* Bit 30: CRC error */
    uint32_t start_pending : 1;  /* Bit 31: Transfer start pending */
} usb_rx_stat_bits_t;

typedef struct {
    uint32_t reserved1      : 5;
    uint32_t ep_addr        : 4;  /* Bits 5-8 */
    uint32_t dev_addr       : 7;  /* Bits 9-15 */
    uint32_t pid_bits       : 8;  /* Bits 16-23: SETUP=0x2d, OUT=0xE1, IN=0x69 */
    uint32_t reserved2      : 4;
    uint32_t pid_datax      : 1;  /* Bit 28: DATA1 or DATA0 */
    uint32_t ack            : 1;  /* Bit 29: Send ACK for IN data */
    uint32_t in_xfer        : 1;  /* Bit 30: Direction */
    uint32_t start          : 1;  /* Bit 31: Start request */
} usb_token_bits_t;

/* --- Main Register Map Structure --- */

typedef struct {
    union {
        __IO uint32_t val;
        __IO usb_ctrl_bits_t bits;
    } CTRL;           /* 0x00 */

    union {
        __I  uint32_t val;
        __I  usb_status_bits_t bits;
    } STATUS;         /* 0x04 */

    union {
        __IO uint32_t val;
        __IO usb_irq_bits_t bits;
    } IRQ_ACK;        /* 0x08 - Write to Clear */

    union {
        __I  uint32_t val;
        __I  usb_irq_bits_t bits;
    } IRQ_STS;        /* 0x0C */

    union {
        __IO uint32_t val;
        __IO usb_irq_bits_t bits;
    } IRQ_MASK;       /* 0x10 */

    __IO uint32_t XFER_DATA; /* 0x14 - Bits 15:0 Tx Len */

    union {
        __IO uint32_t val;
        __IO usb_token_bits_t bits;
    } XFER_TOKEN;     /* 0x18 */

    union {
        __I  uint32_t val;
        __I  usb_rx_stat_bits_t bits;
    } RX_STAT;        /* 0x1C */

    __IO uint32_t DATA;      /* 0x20 - FIFO access point */
} USB_Host_Type;



#define USB_HOST_BASE   ((uint32_t)0x40001000) 
#define USB_HOST        ((USB_Host_Type *) USB_HOST_BASE)

/* --- Interrupt Masks --- */

#define USB_IRQ_SOF           (1 << 0)
#define USB_IRQ_DONE          (1 << 1)
#define USB_IRQ_ERR           (1 << 2)
#define USB_IRQ_DEVICE_DETECT (1 << 3)

/* Helper Constants for PID */
#define USB_PID_SETUP   0x2D
#define USB_PID_OUT     0xE1
#define USB_PID_IN      0x69

#endif