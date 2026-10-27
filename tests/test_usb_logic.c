#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_usb_logic.h"

static void test_text_one_per_state(void) {
    const enum usb_xfer_state all[] = { USB_XFER_CONNECTING, USB_XFER_CONNECTED,
        USB_XFER_FAILED, USB_XFER_EXITING, USB_XFER_BUSY };
    size_t n = sizeof all / sizeof all[0];
    for (size_t i = 0; i < n; i++) {
        const char *t = usb_xfer_text(all[i]);
        assert(t && *t);
        for (size_t j = 0; j < i; j++)          /* every state distinct */
            assert(strcmp(t, usb_xfer_text(all[j])) != 0);
    }
    /* the failure text must never claim we're connecting (the reported bug) */
    assert(!strstr(usb_xfer_text(USB_XFER_FAILED), "Connecting"));
    assert(strstr(usb_xfer_text(USB_XFER_FAILED), "Couldn't"));
    assert(strstr(usb_xfer_text(USB_XFER_CONNECTED), "now a USB drive"));
    assert(strstr(usb_xfer_text(USB_XFER_BUSY), "Eject"));
    assert(strstr(usb_xfer_text(USB_XFER_CONNECTING), "Connecting"));
    assert(strcmp(usb_xfer_text((enum usb_xfer_state)99), "") == 0);
}

static void test_enter(void) {
    assert(usb_xfer_after_enter(0) == USB_XFER_CONNECTED);
    assert(usb_xfer_after_enter(1 << 8) == USB_XFER_FAILED);   /* exit 1 */
    assert(usb_xfer_after_enter(127 << 8) == USB_XFER_FAILED); /* not found (host) */
    assert(usb_xfer_after_enter(-1) == USB_XFER_FAILED);       /* fork failed */
}

static void test_back_gate(void) {
    assert(usb_xfer_back_gated(USB_XFER_CONNECTED));
    assert(usb_xfer_back_gated(USB_XFER_BUSY));
    assert(!usb_xfer_back_gated(USB_XFER_FAILED));     /* never trap on failure */
    assert(!usb_xfer_back_gated(USB_XFER_CONNECTING));
    assert(!usb_xfer_back_gated(USB_XFER_EXITING));
}

static void test_exit(void) {
    assert(usb_xfer_exit_ok(0));
    assert(!usb_xfer_exit_ok(2 << 8));                 /* eject BUSY */
    assert(!usb_xfer_exit_ok(1 << 8));
    assert(!usb_xfer_exit_ok(-1));
}

int main(void) {
    test_text_one_per_state();
    test_enter();
    test_back_gate();
    test_exit();
    printf("usb_logic: all tests passed\n");
    return 0;
}
