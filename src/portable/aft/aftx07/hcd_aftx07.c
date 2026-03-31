#include "pal.h"
#include "tusb_option.h"
#include "usb_pal.h"

#if CFG_TUH_ENABLED && CFG_TUSB_MCU == OPT_MCU_NONE

#include "host/hcd.h"
#include "host/usbh.h"

#include <stdint.h>

#define MMIO_PRINTER ((volatile uint32_t*)0xb0000000)

typedef struct {
  bool busy;
  uint8_t dev_addr;
  uint8_t ep_addr;// includes direction bit
  uint16_t total_len;
  uint8_t *buffer;
  bool in;// direction (true = IN)
} usb_xfer_ctx_t;

static volatile usb_xfer_ctx_t g_xfer;

// Data toggle tracking (for up to 8 devices and 16 endpoints)
static uint16_t g_toggle_in[8];
static uint16_t g_toggle_out[8];

// Helper unions to avoid type-punning warnings
typedef union {
  uint32_t val;
  usb_ctrl_bits_t bits;
} usb_ctrl_reg_t;

typedef union {
  uint32_t val;
  usb_status_bits_t bits;
} usb_status_reg_t;

typedef union {
  uint32_t val;
  usb_token_bits_t bits;
} usb_token_reg_t;

typedef union {
  uint32_t val;
  usb_rx_stat_bits_t bits;
} usb_rx_stat_reg_t;

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
  (void) rhport; (void) cfg_id; (void) cfg_param;
  return false;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void) rhport; (void) rh_init;

  usb_ctrl_reg_t ctrl;
  ctrl.val = 0;
  ctrl.bits.tx_flush = 1;
  ctrl.bits.phy_opmode = 0;
  ctrl.bits.phy_xcvrselect = 1;
  ctrl.bits.phy_termselect = 1;
  ctrl.bits.phy_dppulldown = 1;
  ctrl.bits.phy_dmpulldown = 1;
  ctrl.bits.enable_sof = 0;
  USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);

  // Clear pending interrupts
  uint32_t irq_sts = USB_HW_READ(USB_REG_OFS_IRQ_STS);
  USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, irq_sts);
  USB_HW_WRITE(USB_REG_OFS_IRQ_MASK, 0);

  // Reset toggles
  for (int i=0; i<8; i++) {
    g_toggle_in[i] = 0;
    g_toggle_out[i] = 0;
  }

  return true;
}

void hcd_int_handler(uint8_t rhport, bool in_isr) {
  uint32_t irq_sts = USB_HW_READ(USB_REG_OFS_IRQ_STS);
  uint32_t irq_mask = USB_HW_READ(USB_REG_OFS_IRQ_MASK);
  uint32_t status = irq_sts & irq_mask;

  if (status & USB_IRQ_DEVICE_DETECT) {
    usb_status_reg_t hw_stat;
    hw_stat.val = USB_HW_READ(USB_REG_OFS_STATUS);
    
    if (hw_stat.bits.linestate != 0) {
      *MMIO_PRINTER = 'A'; // Attach
      hcd_event_device_attach(rhport, in_isr);
    } else {
      *MMIO_PRINTER = 'D'; // Detach
      hcd_event_device_remove(rhport, in_isr);
    }
    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_DEVICE_DETECT);
  }

  if (status & USB_IRQ_DONE) {
    usb_rx_stat_reg_t rx_stat;
    rx_stat.val = USB_HW_READ(USB_REG_OFS_RX_STAT);
    xfer_result_t result = XFER_RESULT_SUCCESS;
    uint32_t actual_len = 0;

    if (rx_stat.bits.resp_timeout) {
      *MMIO_PRINTER = 'T'; // Timeout
      result = XFER_RESULT_TIMEOUT;
    } else if (rx_stat.bits.crc_err) {
      *MMIO_PRINTER = 'C'; // CRC Error
      result = XFER_RESULT_FAILED;
    } else {
      actual_len = rx_stat.bits.count;
      *MMIO_PRINTER = 'K'; // OK (Done)
      
      if (g_xfer.in && actual_len > 0) {
        for (uint16_t i=0; i<actual_len; i++) {
          g_xfer.buffer[i] = (uint8_t)USB_HW_READ(USB_REG_OFS_DATA);
        }
      }
    }

    if (g_xfer.busy) {
      if (result == XFER_RESULT_SUCCESS) {
        // Successful transfer, update toggle
        uint8_t ep_num = g_xfer.ep_addr & 0x0F;
        if (g_xfer.dev_addr < 8) {
          if (g_xfer.in) g_toggle_in[g_xfer.dev_addr] ^= (1 << ep_num);
          else           g_toggle_out[g_xfer.dev_addr] ^= (1 << ep_num);
        }
      }

      hcd_event_xfer_complete(g_xfer.dev_addr, g_xfer.ep_addr, actual_len, result, in_isr);
      g_xfer.busy = false;
    }

    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_DONE);
  }

  if (status & USB_IRQ_ERR) {
    *MMIO_PRINTER = 'E'; // Error IRQ
    
    usb_ctrl_reg_t ctrl;
    ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
    ctrl.bits.tx_flush = 1;
    USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);
    
    if (g_xfer.busy) {
      hcd_event_xfer_complete(g_xfer.dev_addr, g_xfer.ep_addr, 0, XFER_RESULT_FAILED, in_isr);
      g_xfer.busy = false;
    }
    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_ERR);
  }

  if (status & USB_IRQ_SOF) {
    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_SOF);
  }
}

void hcd_int_enable(uint8_t rhport) {
  (void) rhport;
  uint32_t usb_mask = USB_IRQ_DEVICE_DETECT | USB_IRQ_DONE | USB_IRQ_ERR;
  USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, usb_mask); 
  USB_HW_WRITE(USB_REG_OFS_IRQ_MASK, usb_mask);

#ifndef USB_TESTBENCH
  uint32_t src = USB_PLIC_SRC;
  uint32_t ctx = 0;
  *PLIC_PRIORITY(PLIC_BASE, src) = 1;
  volatile uint32_t *en = PLIC_ENABLE(PLIC_BASE, src, ctx);
  *en |= (1u << (src % 32));
  *PLIC_PRIORITY_THRESHOLD(PLIC_BASE, ctx) = 0;
#endif
}

void hcd_int_disable(uint8_t rhport) { (void) rhport; }

uint32_t hcd_frame_number(uint8_t rhport) {
  (void) rhport;
  usb_status_reg_t stat;
  stat.val = USB_HW_READ(USB_REG_OFS_STATUS);
  return stat.bits.sof_time;
}

bool hcd_port_connect_status(uint8_t rhport) {
  (void) rhport;
  usb_status_reg_t stat;
  stat.val = USB_HW_READ(USB_REG_OFS_STATUS);
  return (stat.bits.linestate != 0);
}

void hcd_port_reset(uint8_t rhport) {
  (void) rhport;
  usb_ctrl_reg_t ctrl;
  ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
  ctrl.bits.phy_opmode = 2;
  ctrl.bits.enable_sof = 0;
  ctrl.bits.phy_termselect = 0;
  ctrl.bits.phy_xcvrselect = 0;
  USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);
}

void hcd_port_reset_end(uint8_t rhport) {
  (void) rhport;
  usb_ctrl_reg_t ctrl;
  ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
  ctrl.bits.phy_opmode = 0;
  ctrl.bits.phy_termselect = 1;
  ctrl.bits.phy_xcvrselect = 1;
  ctrl.bits.tx_flush = 1;
  ctrl.bits.enable_sof = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  (void) rhport;
  usb_status_reg_t stat;
  stat.val = USB_HW_READ(USB_REG_OFS_STATUS);
  if (stat.bits.linestate & 0x01) return TUSB_SPEED_FULL;
  if (stat.bits.linestate & 0x02) return TUSB_SPEED_LOW;
  return TUSB_SPEED_FULL;
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) { (void) rhport; (void) dev_addr; }

bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc) { 
  (void) rhport;
  uint8_t const ep_num = tu_edpt_number(ep_desc->bEndpointAddress);
  bool const in = tu_edpt_dir(ep_desc->bEndpointAddress) == TUSB_DIR_IN;

  // Initialize toggle to 0 for new endpoint
  if (dev_addr < 8) {
    if (in) g_toggle_in[dev_addr] &= ~(1 << ep_num);
    else    g_toggle_out[dev_addr] &= ~(1 << ep_num);
  }
  return true; 
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) { 
  (void) rhport; (void) daddr; (void) ep_addr;
  return true; 
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
  (void) rhport;
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr = ep_addr;
  g_xfer.total_len = buflen;
  g_xfer.in = (ep_addr & 0x80) ? true : false;
  g_xfer.busy = true;
  g_xfer.buffer = buffer;

  if (!g_xfer.in && buflen > 0) {
    usb_ctrl_reg_t ctrl;
    ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
    ctrl.bits.tx_flush = 1;
    USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);
    
    for (uint16_t i = 0; i < buflen; i++) {
      USB_HW_WRITE(USB_REG_OFS_DATA, buffer[i]);
    }
  }

  USB_HW_WRITE(USB_REG_OFS_XFER_DATA, buflen);

  uint8_t ep_num = ep_addr & 0x0F;
  uint8_t toggle = 0;
  if (dev_addr < 8) {
    if (g_xfer.in) toggle = (g_toggle_in[dev_addr] >> ep_num) & 0x01;
    else           toggle = (g_toggle_out[dev_addr] >> ep_num) & 0x01;
  }

  usb_token_reg_t token;
  token.val = 0;
  token.bits.dev_addr = dev_addr;
  token.bits.ep_addr = ep_num;
  token.bits.in_xfer = g_xfer.in ? 1 : 0;
  token.bits.ack = 1;
  token.bits.pid_bits = g_xfer.in ? USB_PID_IN : USB_PID_OUT;
  token.bits.pid_datax = toggle;
  token.bits.start = 1;     

  USB_HW_WRITE(USB_REG_OFS_XFER_TOKEN, token.val);
  return true;
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport; (void) dev_addr; (void) ep_addr;
  usb_ctrl_reg_t ctrl;
  ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
  ctrl.bits.tx_flush = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);
  return true;
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]) {
  (void) rhport;
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr  = 0;
  g_xfer.total_len = 8;
  g_xfer.in       = false;
  g_xfer.busy     = true;
  g_xfer.buffer   = NULL;

  // SETUP always resets toggles for EP0 to DATA1 for the next stage
  if (dev_addr < 8) {
    g_toggle_in[dev_addr] |= 1;  // Set next IN to DATA1
    g_toggle_out[dev_addr] |= 1; // Set next OUT to DATA1
  }

  usb_ctrl_reg_t ctrl;
  ctrl.val = USB_HW_READ(USB_REG_OFS_CTRL);
  ctrl.bits.tx_flush = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, ctrl.val);

  for (int i = 0; i < 8; i++) {
    USB_HW_WRITE(USB_REG_OFS_DATA, setup_packet[i]);
  }

  USB_HW_WRITE(USB_REG_OFS_XFER_DATA, 8);

  usb_token_reg_t token;
  token.val = 0;
  token.bits.pid_bits = USB_PID_SETUP;
  token.bits.dev_addr = dev_addr;
  token.bits.ep_addr  = 0;
  token.bits.pid_datax = 0; // SETUP is always DATA0
  token.bits.ack       = 1;
  token.bits.in_xfer   = 0;
  token.bits.start     = 1;

  USB_HW_WRITE(USB_REG_OFS_XFER_TOKEN, token.val);
  return true;
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport; (void) dev_addr; (void) ep_addr;
  return true;
}

#endif
