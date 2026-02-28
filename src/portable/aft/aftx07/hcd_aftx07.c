#include "pal.h"
#include "tusb_option.h"
#include "usb_pal.h"

#if CFG_TUH_ENABLED && CFG_TUSB_MCU == OPT_MCU_NONE

#include "host/hcd.h"
#include "host/usbh.h"

#include <stdint.h>

typedef struct {
  bool busy;
  uint8_t dev_addr;
  uint8_t ep_addr;// includes direction bit
  uint16_t total_len;
  uint8_t *buffer;
  bool in;// direction (true = IN)
} usb_xfer_ctx_t;

static volatile usb_xfer_ctx_t g_xfer;

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
  (void) rhport; (void) cfg_id; (void) cfg_param;
  return false;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void) rhport; (void) rh_init;

  usb_ctrl_bits_t ctrl = {0};
  ctrl.tx_flush = 1;
  ctrl.phy_opmode = 0;
  ctrl.phy_xcvrselect = 1;
  ctrl.phy_termselect = 1;
  ctrl.phy_dppulldown = 1;
  ctrl.phy_dmpulldown = 1;
  ctrl.enable_sof = 0;
  USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)&ctrl);

  // Clear pending interrupts
  uint32_t irq_sts = USB_HW_READ(USB_REG_OFS_IRQ_STS);
  USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, irq_sts);
  USB_HW_WRITE(USB_REG_OFS_IRQ_MASK, 0);

  return true;
}

void hcd_int_handler(uint8_t rhport, bool in_isr) {
  uint32_t irq_sts = USB_HW_READ(USB_REG_OFS_IRQ_STS);
  uint32_t irq_mask = USB_HW_READ(USB_REG_OFS_IRQ_MASK);
  uint32_t status = irq_sts & irq_mask;

  if (status & USB_IRQ_DEVICE_DETECT) {
    uint32_t raw_status = USB_HW_READ(USB_REG_OFS_STATUS);
    usb_status_bits_t *hw_stat = (usb_status_bits_t*)&raw_status;
    
    if (hw_stat->linestate != 0) {
      hcd_event_device_attach(rhport, in_isr);
    } else {
      hcd_event_device_remove(rhport, in_isr);
    }
    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_DEVICE_DETECT);
  }

  if (status & USB_IRQ_DONE) {
    uint32_t rx_stat_val = USB_HW_READ(USB_REG_OFS_RX_STAT);
    xfer_result_t result = XFER_RESULT_SUCCESS;
    uint32_t actual_len = 0;

    if (rx_stat_val & (1u << 30)) { 
      result = XFER_RESULT_FAILED;
    } else if (rx_stat_val & (1u << 29)) { 
      result = XFER_RESULT_FAILED;
    } else {
      uint8_t resp_pid = (uint8_t)((rx_stat_val >> 16) & 0xFF);
      if (resp_pid == 0x1E) { 
        result = XFER_RESULT_STALLED;
      }
    }

    if (g_xfer.busy) {
      if (g_xfer.in) {
        actual_len = (uint32_t)(rx_stat_val & 0xFFFF);
        for (uint32_t i = 0; i < actual_len; i++) {
          g_xfer.buffer[i] = (uint8_t)USB_HW_READ(USB_REG_OFS_DATA);
        }
      } else {
        actual_len = (result == XFER_RESULT_SUCCESS) ? g_xfer.total_len : 0;
      }

      hcd_event_xfer_complete(g_xfer.dev_addr, g_xfer.ep_addr, actual_len, result, in_isr);
      g_xfer.busy = false;
    }

    USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, USB_IRQ_DONE);
  }

  if (status & USB_IRQ_ERR) {
    uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
    usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
    ctrl->tx_flush = 1;
    USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);
    
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
  uint32_t usb_mask = USB_IRQ_DEVICE_DETECT | USB_IRQ_DONE | USB_IRQ_ERR | USB_IRQ_SOF;
  USB_HW_WRITE(USB_REG_OFS_IRQ_ACK, usb_mask); 
  USB_HW_WRITE(USB_REG_OFS_IRQ_MASK, usb_mask);

// Only execute PLIC interrupt config on real RISC-V hardware, skip on Verilator
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
  uint32_t raw_status = USB_HW_READ(USB_REG_OFS_STATUS);
  usb_status_bits_t *stat = (usb_status_bits_t*)&raw_status;
  return stat->sof_time;
}

bool hcd_port_connect_status(uint8_t rhport) {
  (void) rhport;
  uint32_t raw_status = USB_HW_READ(USB_REG_OFS_STATUS);
  usb_status_bits_t *stat = (usb_status_bits_t*)&raw_status;
  return (stat->linestate != 0);
}

void hcd_port_reset(uint8_t rhport) {
  (void) rhport;
  uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
  usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
  ctrl->phy_opmode = 2;
  ctrl->enable_sof = 0;
  ctrl->phy_termselect = 0;
  ctrl->phy_xcvrselect = 0;
  USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);
}

void hcd_port_reset_end(uint8_t rhport) {
  (void) rhport;
  uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
  usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
  ctrl->phy_opmode = 0;
  ctrl->phy_termselect = 1;
  ctrl->phy_xcvrselect = 1;
  ctrl->tx_flush = 1;
  ctrl->enable_sof = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  (void) rhport;
  uint32_t raw_status = USB_HW_READ(USB_REG_OFS_STATUS);
  usb_status_bits_t *stat = (usb_status_bits_t*)&raw_status;
  if (stat->linestate & 0x01) return TUSB_SPEED_FULL;
  if (stat->linestate & 0x02) return TUSB_SPEED_LOW;
  return TUSB_SPEED_FULL;
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) { (void) rhport; (void) dev_addr; }

bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc) { return true; }
bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) { return false; }

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
  (void) rhport;
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr = ep_addr;
  g_xfer.total_len = buflen;
  g_xfer.in = (ep_addr & 0x80) ? true : false;
  g_xfer.busy = true;

  if (!g_xfer.in && buflen > 0) {
    uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
    usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
    ctrl->tx_flush = 1;
    USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);
    
    for (uint16_t i = 0; i < buflen; i++) {
      USB_HW_WRITE(USB_REG_OFS_DATA, buffer[i]);
    }
  }

  USB_HW_WRITE(USB_REG_OFS_XFER_DATA, buflen);

  usb_token_bits_t token = {0};
  token.dev_addr = dev_addr;
  token.ep_addr = ep_addr & 0x0F;
  token.in_xfer = g_xfer.in ? 1 : 0;
  token.ack = 1;
  token.pid_bits = g_xfer.in ? USB_PID_IN : USB_PID_OUT;
  token.pid_datax = 0; // Temporarily hardcoded to 0 to pass compilation
  token.start = 1;     

  USB_HW_WRITE(USB_REG_OFS_XFER_TOKEN, *(uint32_t*)&token);
  return true;
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport; (void) dev_addr; (void) ep_addr;
  uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
  usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
  ctrl->tx_flush = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);
  return true;
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]) {
  (void) rhport;
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr  = 0;
  g_xfer.total_len = 8;
  g_xfer.in       = false;
  g_xfer.busy     = true;

  uint32_t raw_ctrl = USB_HW_READ(USB_REG_OFS_CTRL);
  usb_ctrl_bits_t *ctrl = (usb_ctrl_bits_t*)&raw_ctrl;
  ctrl->tx_flush = 1;
  USB_HW_WRITE(USB_REG_OFS_CTRL, *(uint32_t*)ctrl);

  for (int i = 0; i < 8; i++) {
    USB_HW_WRITE(USB_REG_OFS_DATA, setup_packet[i]);
  }

  USB_HW_WRITE(USB_REG_OFS_XFER_DATA, 8);

  usb_token_bits_t token = {0};
  token.pid_bits = USB_PID_SETUP;
  token.dev_addr = dev_addr;
  token.ep_addr  = 0;
  token.pid_datax = 0; 
  token.ack       = 1;
  token.in_xfer   = 0;
  token.start     = 1;

  USB_HW_WRITE(USB_REG_OFS_XFER_TOKEN, *(uint32_t*)&token);
  return true;
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport; (void) dev_addr; (void) ep_addr;
  return true;
}

#endif