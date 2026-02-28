/*
  * The MIT License (MIT)
  *
  * Copyright (c) 2023 Ha Thach (tinyusb.org)
  *
  * Permission is hereby granted, free of charge, to any person obtaining a copy
  * of this software and associated documentation files (the "Software"), to deal
  * in the Software without restriction, including without limitation the rights
  * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  * copies of the Software, and to permit persons to whom the Software is
  * furnished to do so, subject to the following conditions:
  *
  * The above copyright notice and this permission notice shall be included in
  * all copies or substantial portions of the Software.
  *
  * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  * THE SOFTWARE.
  *
  * This file is part of the TinyUSB stack.
  */

// HCD file following example: /src/portable/<vendor>/<chip_family>/hcd_<chip_family>.c
// /src/portable/aft/aftx07/hcd_aftx07.c

// usb register definitions in /hw/mcu/intel/de2_115/usb_pal.h
// board support package  in /hw/bsp/intel/de2_115/

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

// optional hcd configuration, called by tuh_configure()
bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void *cfg_param) {
  (void) rhport;
  (void) cfg_id;
  (void) cfg_param;
  return false;
}

// Initialize controller to host mode
bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void) rhport;
  (void) rh_init;

  // 1. Reset and Flush the internal FIFO buffer
  // Setting tx_flush ensures the 256-byte buffer is empty before we start
  USB_HOST->CTRL.bits.tx_flush = 1;

  // 2. Configure the UTMI PHY for Host Mode
  // - phy_opmode: 0 (Normal Operation)
  // - phy_xcvrselect: 1 (Full Speed)
  // - phy_termselect: 1 (Enable termination for FS host)
  // - phy_dppulldown/phy_dmpulldown: 1 (Enable pulldowns as required for a Host)
  USB_HOST->CTRL.bits.phy_opmode = 0;
  USB_HOST->CTRL.bits.phy_xcvrselect = 1;
  USB_HOST->CTRL.bits.phy_termselect = 1;
  USB_HOST->CTRL.bits.phy_dppulldown = 1;
  USB_HOST->CTRL.bits.phy_dmpulldown = 1;

  // 3. Disable SOF generation initially
  // We only want to start sending Start of Frame packets after a device is detected
  USB_HOST->CTRL.bits.enable_sof = 0;

  // 4. Clear any pending interrupts and mask them
  // We clear by writing the current status back to the ACK register
  USB_HOST->IRQ_ACK.val = USB_HOST->IRQ_STS.val;
  USB_HOST->IRQ_MASK.val = 0;// Keep interrupts disabled until hcd_int_enable is called

  return true;
}

// Interrupt Handler
void hcd_int_handler(uint8_t rhport, bool in_isr) {
  // 1. Read the masked interrupt status
  uint32_t status = USB_HOST->IRQ_STS.val & USB_HOST->IRQ_MASK.val;

  // 2. Handle Device Detection (Attach/Detach)
  if (status & USB_IRQ_DEVICE_DETECT) {
    uint32_t linestate = USB_HOST->STATUS.bits.linestate;
    
    if (linestate != 0) {
      // Device attached (D+ or D- is high)
      hcd_event_device_attach(rhport, in_isr);
    } else {
      // Device detached (Both lines low)
      hcd_event_device_remove(rhport, in_isr);
    }
    // W1C: Acknowledge the interrupt
    USB_HOST->IRQ_ACK.val = USB_IRQ_DEVICE_DETECT;
  }

  // 3. Handle Transfer Completion (DONE)
  if (status & USB_IRQ_DONE) {
    uint32_t rx_stat_val = USB_HOST->RX_STAT.val;
    xfer_result_t result = XFER_RESULT_SUCCESS;
    uint32_t actual_len = 0;

    // Check for hardware-level errors in RX_STAT
    if (rx_stat_val & (1u << 30)) { // CRC_ERR
      result = XFER_RESULT_FAILED;
    } else if (rx_stat_val & (1u << 29)) { // RESP_TIMEOUT
      result = XFER_RESULT_FAILED;
    } else {
      // Check PID for STALL
      uint8_t resp_pid = (uint8_t)((rx_stat_val >> 16) & 0xFF);
      if (resp_pid == 0x1E) { // STALL PID
        result = XFER_RESULT_STALLED;
      }
    }

    if (g_xfer.busy) {
      if (g_xfer.in) {
        // IN Transfer: Read bytes from FIFO into the software buffer
        actual_len = (uint32_t)(rx_stat_val & 0xFFFF); // COUNT bits
        for (uint32_t i = 0; i < actual_len; i++) {
          g_xfer.buffer[i] = (uint8_t)USB_HOST->DATA;
        }
      } else {
        // OUT/SETUP Transfer: Assume full length was sent on success
        actual_len = (result == XFER_RESULT_SUCCESS) ? g_xfer.total_len : 0;
      }

      // Notify TinyUSB stack of completion
      hcd_event_xfer_complete(g_xfer.dev_addr, g_xfer.ep_addr, actual_len, result, in_isr);
      g_xfer.busy = false;
    }

    // W1C: Acknowledge the interrupt
    USB_HOST->IRQ_ACK.val = USB_IRQ_DONE;
  }

  // 4. Handle General Hardware Errors
  if (status & USB_IRQ_ERR) {
    // Clear the internal FIFO to recover from the error state
    USB_HOST->CTRL.bits.tx_flush = 1;
    
    if (g_xfer.busy) {
      hcd_event_xfer_complete(g_xfer.dev_addr, g_xfer.ep_addr, 0, XFER_RESULT_FAILED, in_isr);
      g_xfer.busy = false;
    }
    
    // W1C: Acknowledge the interrupt
    USB_HOST->IRQ_ACK.val = USB_IRQ_ERR;
  }

  // 5. Handle SOF (Keep-alive) - Usually just cleared
  if (status & USB_IRQ_SOF) {
    USB_HOST->IRQ_ACK.val = USB_IRQ_SOF;
  }
}


// ERROR interrupt

// Enable USB interrupt
void hcd_int_enable(uint8_t rhport) {
  (void) rhport;

  // USB peripheral interrupt enabled
  // interrupt controller is configured (PLIC)
  // CPU global interrupts enabled
  uint32_t usb_mask = USB_IRQ_DEVICE_DETECT | USB_IRQ_DONE | USB_IRQ_ERR | USB_IRQ_SOF;
  USB_HOST->IRQ_ACK.val = usb_mask; // clear out interrupts
  USB_HOST->IRQ_MASK.val = usb_mask;// enable all interrupts

  // enable PLIC source
  uint32_t src = USB_PLIC_SRC;
  uint32_t ctx = 0;

  // make > 0 or it wont fire
  *PLIC_PRIORITY(PLIC_BASE, src) = 1;

  // enable bit (32 bit word covers 32 sources)
  volatile uint32_t *en = PLIC_ENABLE(PLIC_BASE, src, ctx);
  *en |= (1u << (src % 32));

  // allow priorities >= 1
  *PLIC_PRIORITY_THRESHOLD(PLIC_BASE, ctx) = 0;
}

// Disable USB interrupt
void hcd_int_disable(uint8_t rhport) {
  (void) rhport;
}

// Get frame number (1ms)
uint32_t hcd_frame_number(uint8_t rhport) {
  (void) rhport;
  return USB_HOST->STATUS.bits.sof_time;
}

//--------------------------------------------------------------------+
// Port API
//--------------------------------------------------------------------+

bool hcd_port_connect_status(uint8_t rhport) {
  (void) rhport;

  // Read the current line state from the hardware status register
  // linestate bits: 01 = D+ high (Full Speed device), 10 = D- high (Low Speed device)
  uint32_t line_state = USB_HOST->STATUS.bits.linestate;

  // A non-zero value means the lines are not in Single Ended Zero (SE0) state,
  // indicating that a device pull-up is present and detected.
  return (line_state != 0);
}

// Reset USB bus on the port. Return immediately, bus reset sequence may not be complete.
// Some port would require hcd_port_reset_end() to be invoked after 10ms to complete the reset sequence.
void hcd_port_reset(uint8_t rhport) {
  (void) rhport;

  // 1. Enter SE0 (Reset) mode
  // Based on hardware documentation, phy_opmode = 2 (binary 10) drives the lines to SE0.
  // We also ensure pulldowns are active and SOF is disabled during reset.
  USB_HOST->CTRL.bits.phy_opmode = 2;
  USB_HOST->CTRL.bits.enable_sof = 0;
  USB_HOST->CTRL.bits.phy_termselect = 0;
  USB_HOST->CTRL.bits.phy_xcvrselect = 0;
}

// Complete bus reset sequence, may be required by some controllers
void hcd_port_reset_end(uint8_t rhport) {
  (void) rhport;

  // 1. Return to Normal Operation mode
  // Setting phy_opmode back to 0 (binary 00) stops driving SE0.
  USB_HOST->CTRL.bits.phy_opmode = 0;

  // 2. Re-enable termination and select Full Speed for the root hub
  USB_HOST->CTRL.bits.phy_termselect = 1;
  USB_HOST->CTRL.bits.phy_xcvrselect = 1;

  // 3. Flush the FIFO to ensure we start with a clean state for enumeration
  USB_HOST->CTRL.bits.tx_flush = 1;

  // enable SOF packets on wire
  USB_HOST->CTRL.bits.enable_sof = 1;// enable SOF packet generation
}

// Get port link speed
tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  (void) rhport;

  // Read the linestate from the status register
  // Based on your pal: bit 0 is D+ and bit 1 is D-
  uint32_t line_state = USB_HOST->STATUS.bits.linestate;

  // Check bit 0 (D+). If high, it is a Full Speed device.
  if (line_state & 0x01) {
    return TUSB_SPEED_FULL;
  }

  // Check bit 1 (D-). If high, it is a Low Speed device.
  if (line_state & 0x02) {
    return TUSB_SPEED_LOW;
  }

  // Default to Full Speed if state is ambiguous
  return TUSB_SPEED_FULL;
}

// HCD closes all opened endpoints belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  (void) rhport;
  (void) dev_addr;
}

//--------------------------------------------------------------------+
// Endpoints API
//--------------------------------------------------------------------+

// Open an endpoint
bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const *ep_desc) {
  (void) rhport;
  (void) dev_addr;

  // NOTE: ep_desc is allocated on the stack when called from usbh_edpt_control_open()
  // You need to copy the data into a local variable who maintains the state of the endpoint and transfer.
  // Check _hcd_data in hcd_dwc2.c for example.

  return true;
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
  (void) rhport;
  (void) daddr;
  (void) ep_addr;
  return false;// TODO not implemented yet
}

// Submit a transfer, when complete hcd_event_xfer_complete() must be invoked
bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;
  (void) buffer;
  (void) buflen;

  // save transfer context (on others its done in hardware)
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr = ep_addr;
  g_xfer.total_len = buflen;
  g_xfer.in = (ep_addr & 0x80) ? true : false;
  g_xfer.busy = true;

  // program hardware registers here
  if (!g_xfer.in && buflen > 0) {
    USB_HOST->CTRL.bits.tx_flush = 1;
    for (uint16_t i = 0; i < buflen; i++) {
      USB_HOST->DATA = buffer[i];// Load OUT data into FIFO
    }
  }

  USB_HOST->XFER_DATA = buflen;// Set length for the hardware

  usb_token_bits_t token = {0};
  token.dev_addr = dev_addr;
  token.ep_addr = ep_addr & 0x0F;
  token.in_xfer = g_xfer.in ? 1 : 0;
  token.ack = 1;
  token.pid_bits = g_xfer.in ? USB_PID_IN : USB_PID_OUT;//
  token.pid_datax = usbh_get_toggle(dev_addr, ep_addr); // Helper needed to track DATA0/DATA1
  token.start = 1;                                      // Kick off state machine

  USB_HOST->XFER_TOKEN.bits = token;
  return true;
}

// Abort a queued transfer. Note: it can only abort transfer that has not been started
// Return true if a queued transfer is aborted, false if there is no transfer to abort
bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;

  // Aborting a transfer on this IP would involve flushing the FIFO
  // if the transfer hasn't started yet.
  USB_HOST->CTRL.bits.tx_flush = 1;
  return true;
}

// Submit a special transfer to send 8-byte Setup Packet, when complete hcd_event_xfer_complete() must be invoked
bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8]) {
  (void) rhport;
  // Initialize global transfer context for the interrupt handler
  g_xfer.dev_addr = dev_addr;
  g_xfer.ep_addr  = 0;
  g_xfer.total_len = 8;
  g_xfer.in       = false;
  g_xfer.busy     = true;

  USB_HOST->CTRL.bits.tx_flush = 1;
  for (int i = 0; i < 8; i++) {
    USB_HOST->DATA = setup_packet[i];
  }

  USB_HOST->XFER_DATA = 8;

  usb_token_bits_t token = {0};
  token.pid_bits = USB_PID_SETUP;
  token.dev_addr = dev_addr;
  token.ep_addr  = 0;
  token.pid_datax = 0; // Setup always uses DATA0
  token.ack       = 1;
  token.in_xfer   = 0;
  token.start     = 1;

  USB_HOST->XFER_TOKEN.bits = token;
  return true;
}

// clear stall, data toggle is also reset to DATA0
bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;

  // Since the hardware allows us to specify the toggle (DATA0/DATA1)
  // in every token, we don't need to reset a hardware-side toggle bit here.
  // The TinyUSB stack will handle resetting its internal toggle state
  // to DATA0 after this returns true.
  return true;
}

#endif