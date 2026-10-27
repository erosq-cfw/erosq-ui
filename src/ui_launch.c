#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include "lvgl.h"
#include "ui_launch.h"
#include "ui_screens.h"
#include "ui_input.h"
#include "ui_power.h"
#include "ui_bt.h"
#ifndef EROSQ_HOST
#include <xf86drm.h>
#endif

static void copyfield(char *dst, size_t n, const char *s, const char *e)
{
    size_t len = (size_t)(e - s); if (len >= n) len = n - 1;
    memcpy(dst, s, len); dst[len] = 0;
}

int ui_manifest_parse(const char *text, ui_app_t *out, int max)
{
    int n = 0; const char *p = text;
    while (*p && n < max) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        if (*p != '#' && *p != '\n' && p != eol) {
            const char *t1 = memchr(p, '\t', eol - p);
            if (t1) {
                const char *t2 = memchr(t1 + 1, '\t', eol - (t1 + 1));
                if (t2) {
                    copyfield(out[n].name, sizeof out[n].name, p, t1);
                    copyfield(out[n].exec, sizeof out[n].exec, t1 + 1, t2);
                    copyfield(out[n].cat,  sizeof out[n].cat,  t2 + 1, eol);
                    n++;
                }
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return n;
}

#ifndef EROSQ_HOST
/* Wait for the game. The UI loop is stopped meanwhile, so this is where the
 * critical-battery check keeps its 10 s cadence: a pidfd turns "child exited"
 * into a pollable fd, so exit is still noticed instantly. On a critical battery
 * the whole process group (the SD wrapper script + the emulator) gets SIGTERM,
 * then SIGKILL 3 s later. Returns 1 iff the game was stopped for the battery.
 * No pidfd (pre-5.3 kernel) -> plain blocking wait, as before. */
static int wait_game(pid_t pid)
{
    int killed = 0, st;
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd >= 0) {
        struct pollfd p = { .fd = pfd, .events = POLLIN };
        for (;;) {
            int r = poll(&p, 1, killed ? 3000 : 10000);
            if (r > 0) break;
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (killed) { kill(-pid, SIGKILL); break; }
            if (ui_power_critbatt_due()) { kill(-pid, SIGTERM); killed = 1; }
        }
        close(pfd);
    }
    waitpid(pid, &st, 0);
    return killed;
}
#endif

void ui_launch_run(struct ui_ctx *ctx, const char *exec_path)
{
#ifdef EROSQ_HOST
    LV_LOG_USER("launch (host stub): %s", exec_path);   /* no handoff on host */
    (void)ctx; return;
#else
    /* Hand the panel to a child via DRM master, keeping our display intact.
     *
     * We must NOT close()/delete our DRM fd: once the userspace master fd goes
     * away, the kernel's in-kernel fbdev client grabs master, and then neither
     * the child nor a recreated display can take it (EACCES -> frozen). So keep
     * the fd open and just drmDropMaster: that yields the master slot so the
     * child (drmtest/emulator) auto-masters on open, and lets us drmSetMaster it
     * back afterward. Display/input/nav all stay intact (no re-init). */
    int fd = lv_linux_drm_get_fd(ctx->disp);
    if (fd >= 0) drmDropMaster(fd);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);   /* own group, so a battery stop reaches the emulator too */
        execl(exec_path, exec_path, (char *)NULL);
        _exit(127);
    }
    int battery_stop = pid > 0 && wait_game(pid);   /* UI paused while child runs */

    /* pcsx/picoarch menus write the DAC directly; adopt + save their level, or
     * the wheel jumps from the stale one and a reboot restores the pre-game one.
     * Not on BT: then s_vol is the headphone level, not the DAC's. */
    if (!ui_bt_audio_active()) { ui_volume_sync(); ui_volume_persist(); }

    ui_input_flush();   /* drop in-game keypresses; the quit-confirm press would
                         * otherwise replay and relaunch the focused entry */

    if (fd >= 0 && drmSetMaster(fd) != 0)
        fprintf(stderr, "erosq-ui: drmSetMaster failed reclaiming panel\n");

    /* The child reprogrammed + left the CRTC disabled (backlight off, DMA
     * stopped). Force a full modeset on the next flush so erosq-drm's pipe
     * .enable re-runs (panel init + backlight + first DMA frame), then repaint
     * the current screen. */
    lv_linux_drm_force_modeset(ctx->disp);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(ctx->disp);

    if (battery_stop) {
        /* The wrapper died before its "restart mpd" tail. Bring mpd back from
         * its tmpfs state_file so mpd-persist (on the power-off path) has a
         * live mpd to snapshot into /data, exactly as after a normal exit. */
        if (system("pidof mpd >/dev/null || mpd") == -1) { /* ignore */ }
        ui_power_critbatt_warn();
    }
#endif
}
