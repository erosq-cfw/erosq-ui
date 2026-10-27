#ifndef EROSQ_UI_USB_LOGIC_H
#define EROSQ_UI_USB_LOGIC_H

/* USB File Transfer screen state machine. Pure + host-tested (no LVGL, no
 * syscalls): the screen shows exactly ONE message per state, and the rc of each
 * `erosq-usbgadget` call picks the next state.
 *
 *   CONNECTING --storage rc==0--> CONNECTED --Back--> EXITING --rc==0--> (leave)
 *       |                             ^                  |
 *       |                             +----- BUSY <------+ rc!=0 (host holds drive)
 *       +--storage rc!=0--> FAILED --Back--> (leave; no handoff gate)
 */
enum usb_xfer_state {
    USB_XFER_CONNECTING = 0,  /* `storage` running (blocking) */
    USB_XFER_CONNECTED,       /* SD exposed to the host */
    USB_XFER_FAILED,          /* storage-on aborted; player never left network mode */
    USB_XFER_EXITING,         /* `network` running (blocking) */
    USB_XFER_BUSY,            /* eject refused: host still has the drive */
};

/* The single on-screen message for a state (never NULL). */
const char *usb_xfer_text(enum usb_xfer_state s);

/* State after `erosq-usbgadget storage` returned rc (system() status). */
enum usb_xfer_state usb_xfer_after_enter(int rc);

/* Must Back/Home first hand the SD back (and stay on refusal)? Only while the
 * host may own the card: CONNECTED / BUSY. FAILED never entered storage mode,
 * so gating it would trap the user on the failure screen. */
int usb_xfer_back_gated(enum usb_xfer_state s);

/* After `erosq-usbgadget network` returned rc on a gated Back: 1 = leave the
 * screen, 0 = stay (state becomes USB_XFER_BUSY). */
int usb_xfer_exit_ok(int rc);

#endif
