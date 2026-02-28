#ifndef USB_PAL_H_
#define USB_PAL_H_

#include <stdint.h>

#define __IO volatile
#define __I volatile const

#define USB_PLIC_SRC (17)

/* --- Bitfield Definitions --- */
typedef struct {
  uint32_t enable_sof : 1;     
  uint32_t phy_opmode : 2;     
  uint32_t phy_xcvrselect : 2; 
  uint32_t phy_termselect : 1; 
  uint32_t phy_dppulldown : 1; 
  uint32_t phy_dmpulldown : 1; 
  uint32_t tx_flush : 1;       
  uint32_t reserved : 23;
} usb_ctrl_bits_t;

typedef struct {
  uint32_t linestate : 2; 
  uint32_t rx_error : 1;  
  uint32_t reserved : 13;
  uint32_t sof_time : 16; 
} usb_status_bits_t;

typedef struct {
  uint32_t sof : 1;           
  uint32_t done : 1;          
  uint32_t err : 1;           
  uint32_t device_detect : 1; 
  uint32_t reserved : 28;
} usb_irq_bits_t;

typedef struct {
  uint32_t count : 16;   
  uint32_t resp_pid : 8; 
  uint32_t reserved : 4;
  uint32_t idle : 1;          
  uint32_t resp_timeout : 1;  
  uint32_t crc_err : 1;       
  uint32_t start_pending : 1; 
} usb_rx_stat_bits_t;

typedef struct {
  uint32_t reserved1 : 5;
  uint32_t ep_addr : 4;  
  uint32_t dev_addr : 7; 
  uint32_t pid_bits : 8; 
  uint32_t reserved2 : 4;
  uint32_t pid_datax : 1; 
  uint32_t ack : 1;       
  uint32_t in_xfer : 1;   
  uint32_t start : 1;     
} usb_token_bits_t;


/* --- Register Offsets --- */
#define USB_REG_OFS_CTRL       0x00
#define USB_REG_OFS_STATUS     0x04
#define USB_REG_OFS_IRQ_ACK    0x08
#define USB_REG_OFS_IRQ_STS    0x0C
#define USB_REG_OFS_IRQ_MASK   0x10
#define USB_REG_OFS_XFER_DATA  0x14
#define USB_REG_OFS_XFER_TOKEN 0x18
#define USB_REG_OFS_RX_STAT    0x1C
#define USB_REG_OFS_DATA       0x20

/* --- Hardware Access Macros --- */
#ifdef USB_TESTBENCH
  extern void usbhw_reg_write(uint32_t addr, uint32_t data);
  extern uint32_t usbhw_reg_read(uint32_t addr);

  // Directly pass the offset into Verilator
  #define USB_HW_WRITE(offset, data) usbhw_reg_write((offset), (data))
  #define USB_HW_READ(offset)        usbhw_reg_read((offset))

#else
  // Real CPU hardware address base
  #define USB_HOST_BASE ((uint32_t)0x90003000)

  // In hardware, write to the physical memory address
  #define USB_HW_WRITE(offset, data) (*(volatile uint32_t*)(USB_HOST_BASE + (offset)) = (data))
  #define USB_HW_READ(offset)        (*(volatile uint32_t*)(USB_HOST_BASE + (offset)))
#endif

/* --- Interrupt Masks --- */
#define USB_IRQ_SOF (1 << 0)
#define USB_IRQ_DONE (1 << 1)
#define USB_IRQ_ERR (1 << 2)
#define USB_IRQ_DEVICE_DETECT (1 << 3)

#define USB_PID_SETUP 0x2D
#define USB_PID_OUT 0xE1
#define USB_PID_IN 0x69

#endif