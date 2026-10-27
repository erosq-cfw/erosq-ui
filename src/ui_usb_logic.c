#include "ui_usb_logic.h"

const char *usb_xfer_text(enum usb_xfer_state s)
{
    switch (s) {
    case USB_XFER_CONNECTING:
        return "Connecting as USB drive...";
    case USB_XFER_CONNECTED:
        return "This player is now a USB drive.\n\n"
               "Copy files to it from your computer, eject it there, "
               "then press Back to return.";
    case USB_XFER_FAILED:
        return "Couldn't enter USB drive mode\n(is music still playing?).\n\n"
               "Press Back and try again.";
    case USB_XFER_EXITING:
        return "Returning to the player...";
    case USB_XFER_BUSY:
        return "Still connected to your computer.\n\n"
               "Eject \"EROS Q\" on the computer first, "
               "then press Back again.";
    }
    return "";
}

enum usb_xfer_state usb_xfer_after_enter(int rc)
{
    return rc == 0 ? USB_XFER_CONNECTED : USB_XFER_FAILED;
}

int usb_xfer_back_gated(enum usb_xfer_state s)
{
    return s == USB_XFER_CONNECTED || s == USB_XFER_BUSY;
}

int usb_xfer_exit_ok(int rc)
{
    return rc == 0;
}
