/* Podcasts: feed list + paged episode picker.
 *
 * Episodes live OUTSIDE music_directory (/media/sd/podcasts, not
 * /media/sd/audio), so MPD never indexes them and they cannot leak into
 * Shuffle Songs / Songs / Artists / Albums / Genres / Folders. That means this
 * screen is the only way in, and playback goes by absolute path.
 *
 * Absolute-path playback relies on MPD's Client::AllowFile short-circuit: the
 * client's uid equals mpd's euid (both run as root here), so local files are
 * permitted. The SD card is vfat, so episodes are world-readable and would pass
 * the fallback mode check too. If mpd is ever dropped to an `mpd` user AND the
 * card is mounted restrictively, `add` will start failing with a permission
 * ACK -- that is the tripwire.
 *
 * Long operations are backgrounded scripts polled on an lv_timer; there is no
 * pthread anywhere in erosq-ui and this must not introduce the first one. */
#include "ui.h"
#include "ui_podcast.h"
#include "ui_podcast_logic.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_mpd.h"
#include "pod_name.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define POD_ROOT   "/media/sd/podcasts"
#define POD_FEEDS  "/media/sd/podcasts.txt"
#define POD_STATUS "/run/podfetch.status"
/* podfetch.sh's mkdir mutex, with the owning pid inside. Checked BEFORE
 * launching a run: the script's own lock-failure path deliberately does not
 * touch the status file (a live run owns it) and we background it with '&', so
 * system() cannot see its exit 1 -- without this check, pressing "Download
 * selected" during a refresh did nothing at all, visibly. */
#define POD_LOCK   "/run/podfetch.lock"
#define WANTED_MAX 8192

static up_feed_t s_feeds[UP_FEEDS_MAX];
static int       s_nfeeds;
static int       s_feed;              /* index into s_feeds for scr_pod_feed */
static int       s_page;              /* current picker page */

/* Sticky window for the "Busy" refusal message (see refuse_if_busy / poll_cb
 * below). s_busy_sticky is reset in attach_status(), which runs exactly once
 * per screen construction, so a freshly built podcast screen never inherits
 * a stale sticky window left over from a previous visit -- there is nothing
 * else that could leak it forward, since these are file-scope statics shared
 * by every podcast screen instance. */
#define BUSY_STICKY_MS 3500
static bool     s_busy_sticky;
static uint32_t s_busy_since;

/* ---------- small file helpers ---------- */

/* Returns the number of bytes read (buf is always NUL-terminated), or -1 if
 * either the file didn't fit the buffer or the read itself failed. A
 * truncated-but-clean read is a harmless approximation for callers that
 * only display or scan the buffer; the one caller that writes it back (the
 * wanted-list toggle in ep_click_cb) MUST treat -1 as "do not touch this
 * file" -- silently re-saving a truncated wanted.txt would drop every
 * queued episode past the cutoff with no warning (Finding 6). Fit is
 * detected by trying to read one byte past the buffer's capacity: if fread
 * stopped exactly at buflen-1 AND there is still a byte waiting, the file
 * didn't fit. A short read caused by a genuine SD I/O error (the card is
 * removable and this device has SD failure history) looks identical to a
 * short *file* unless ferror() is checked -- without that check, a
 * hardware read glitch would silently masquerade as "read this much, all
 * good" and the toggle path would write the resulting partial buffer back,
 * corrupting wanted.txt through a different door than Finding 6 closed
 * (Finding 7). ferror() is checked before the truncation peek so a real
 * I/O error is reported as -1 regardless of where in the file it hit. */
static int read_file(const char *path, char *buf, size_t buflen)
{
    FILE *f = fopen(path, "r");
    if (!f) { if (buflen) buf[0] = 0; return 0; }
    size_t n = fread(buf, 1, buflen - 1, f);
    int io_err = ferror(f);
    int truncated = !io_err && (n == buflen - 1) && (fgetc(f) != EOF);
    fclose(f);
    buf[n] = 0;
    return (io_err || truncated) ? -1 : (int)n;
}

/* Write-then-rename, with the data actually pushed to the card before the
 * rename. The target is a REMOVABLE vfat card with no journal: without the
 * fflush+fsync the rename can be durable while the file's data is not, and
 * wanted.txt comes back zero-length after a card pull or a power loss -- the
 * whole queue silently gone. The directory fsync afterwards covers the other
 * half (the rename itself). This device has SD failure history, so neither is
 * theoretical. Failures are checked, never assumed: a full or dying card is the
 * expected case, not the exception. */
static int write_file(const char *path, const char *buf)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    size_t len = strlen(buf);
    int ok = fwrite(buf, 1, len, f) == len;
    if (ok && fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    if (rename(tmp, path) != 0) return 0;
    /* `tmp` still holds "<path>.new", so cutting it at the last '/' gives the
     * containing directory without a second snprintf (and without a second
     * -Wformat-truncation site). A failed directory fsync is not worth
     * reporting: the rename already succeeded and the caller's state is
     * correct either way. */
    char *slash = strrchr(tmp, '/');
    if (slash) {
        *slash = 0;
        int dfd = open(tmp, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { if (fsync(dfd) != 0) { /* best effort */ } close(dfd); }
    }
    return 1;
}

/* Is a podfetch run actually in progress? Ground truth, not the status file:
 * the status file can be left frozen mid-download by a SIGKILL, which would
 * wedge the UI into refusing every later press forever. The lock's pid is
 * checkable against /proc, and podfetch.sh's take_lock() applies the IDENTICAL
 * rule (a lock whose pid is gone from /proc is stale and gets taken over), so
 * both sides agree on what "busy" means. */
static int run_active(void)
{
    char buf[32], proc[64];
    struct stat st;
    int pid;
    if (read_file(POD_LOCK "/pid", buf, sizeof buf) <= 0) return 0;
    pid = up_parse_pid(buf);
    if (pid <= 0) return 0;                 /* no owner recorded -> stale */
    snprintf(proc, sizeof proc, "/proc/%d", pid);
    return stat(proc, &st) == 0;
}

static void feed_dir(int i, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/%s", POD_ROOT, s_feeds[i].name);
}

static int load_feeds(void)
{
    char buf[4096];
    read_file(POD_FEEDS, buf, sizeof buf);
    s_nfeeds = up_parse_feeds(buf, s_feeds, UP_FEEDS_MAX);
    return s_nfeeds;
}

static int on_disk(const char *dir, const char *file)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, file);
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* An episode, as opposed to our own control files -- index.tsv and wanted.txt
 * both satisfy pod_name_ok(), so a bare readdir count would include them. */
static int is_media(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcmp(dot, ".mp3") == 0 || strcmp(dot, ".m4a") == 0 ||
           strcmp(dot, ".ogg") == 0 || strcmp(dot, ".opus") == 0;
}

/* Same idiom as ui_screens.c's file-scope lbl_set() (not exported, so
 * re-implemented here to the same shape): lv_label_set_text() invalidates
 * even when the text is unchanged, so a periodic poll that re-sets a label
 * every tick marks it dirty forever and forces a redraw (and SLCD DMA)
 * every second even while idle. Compare first. */
static void lbl_set(lv_obj_t *l, const char *txt)
{
    if (l && strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

/* ---------- status row + poll timer ----------
 *
 * No shared/global label or timer pointer. Each podcast screen's status
 * label lives in ITS OWN body's user_data slot (ui_screen_body(scr) is a
 * stable per-screen object; nothing else in this file uses its user_data),
 * and each screen gets its OWN lv_timer_t tied 1:1 to its OWN label via the
 * timer's user_data. The timer is deleted from the label's own
 * LV_EVENT_DELETE, so its lifetime is exactly the label's lifetime -- no
 * "current screen" bookkeeping to get wrong.
 *
 * This replaces an earlier draft that kept one shared "current" label
 * pointer, guarded by comparing identities on delete. That guard fixed the
 * PUSH direction (podcasts -> feed) but not the POP direction: ui_back()
 * only pops the nav stack and reloads the screen already sitting below (its
 * label object was never touched, since attach_status() only runs once, at
 * initial build), then defers the popped screen's lv_obj_delete() to
 * lv_async_call (ui_screens.c's screen_dispose_cb). By the time that
 * deferred delete fires, the shared pointer still pointed at the popped
 * screen's label (nothing had reinstalled the surviving screen's label as
 * "current" on the way back), so the identity check matched and nulled it
 * anyway -- leaving the screen the user is actually looking at with a dead
 * status row and a poll_cb that silently no-ops from then on. Per-screen
 * ownership with no shared "current" pointer sidesteps the whole class of
 * bug: there is nothing to reinstall on the way back, because each screen's
 * own label was never touched by any other screen's teardown. */
static lv_obj_t *status_of(lv_obj_t *from)
{
    return lv_obj_get_user_data(ui_screen_body(lv_obj_get_screen(from)));
}

static void poll_cb(lv_timer_t *t)
{
    lv_obj_t *label = lv_timer_get_user_data(t);
    /* Every other periodic poll in this codebase gates on ui_blank_active()
     * (ui_screens.c: playstate_update, np_refresh, batt_update); this is the
     * pocket-download case the feature exists for -- without the gate,
     * leaving Podcasts up during a download does a 1Hz status-file read plus
     * a label invalidate plus an SLCD DMA with the backlight off. */
    if (ui_blank_active()) return;

    /* Sticky "Busy" refusal (see refuse_if_busy): hold the message on screen
     * for BUSY_STICKY_MS so this 1Hz poll doesn't blow it away within the
     * same second it was written. Only the busy refusal is sticky -- the
     * optimistic "Refreshing..."/"Downloading..." messages set at click time
     * fall straight through to the read below and get replaced by real
     * progress on the very next poll, same as always. */
    if (s_busy_sticky) {
        if (lv_tick_elaps(s_busy_since) < BUSY_STICKY_MS) return;
        s_busy_sticky = false;
    }

    char line[256];
    read_file(POD_STATUS, line, sizeof line);
    up_status_t st;
    if (!up_parse_status(line, &st)) return;      /* keep the last good text */

    /* The script cannot clear its own status after it exits, and /run is only
     * wiped at boot, so a `done` line sits there for the rest of the session --
     * entering Podcasts hours later showed "Finished" as the CURRENT state.
     * Age the terminal states out instead; a live run rewrites the file every
     * episode, so nothing in progress is ever judged stale. */
    struct stat sst;
    long age = 0;
    if (stat(POD_STATUS, &sst) == 0) age = (long)(time(NULL) - sst.st_mtime);
    if (up_status_stale(st.state, age)) { lbl_set(label, " "); return; }

    char msg[128];
    if (strcmp(st.state, "download") == 0)
        snprintf(msg, sizeof msg, "Downloading %d/%d: %s", st.ei, st.en, st.text);
    else if (strcmp(st.state, "refresh") == 0)
        snprintf(msg, sizeof msg, "Refreshing %d/%d: %s", st.fi, st.fn, st.text);
    else if (strcmp(st.state, "wifi") == 0)
        snprintf(msg, sizeof msg, "Connecting WiFi...");
    else if (strcmp(st.state, "done") == 0) {
        /* The script puts podindex's skipped-episode count here. It used to go
         * only into an index.err nothing ever read, so episodes with an
         * unusable enclosure URL were dropped in total silence. */
        if (st.text[0] && strcmp(st.text, "finished") != 0)
            snprintf(msg, sizeof msg, "Finished - %s", st.text);
        else
            snprintf(msg, sizeof msg, "Finished");
    }
    else if (strcmp(st.state, "error") == 0)
        snprintf(msg, sizeof msg, "Error: %s", st.text);
    else
        snprintf(msg, sizeof msg, " ");
    lbl_set(label, msg);
}

static void status_timer_del_cb(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

/* Creates and positions the status label as the last row of `body`, and
 * starts its dedicated poll timer. The 320x196 list above it deliberately
 * leaves 22px at the bottom of the 218-high body for exactly this row.
 *
 * LV_LABEL_LONG_DOT clamps against the label object's CURRENT height, not
 * an implicit line limit -- with height left at LV_SIZE_CONTENT (the
 * default), a message wider than ~320px wraps to a second line, DOT-ends
 * THAT line, and the label's self-size grows to fit both lines. Aligned
 * BOTTOM_LEFT, that growth extends upward over the list's last row instead
 * of clipping. lv_font_ipod_14 has line_height 16, so a fixed 22px height
 * (matching the 22px strip this row already lives in) both clamps DOT to a
 * single line and comfortably fits that one line without clipping. */
static void attach_status(lv_obj_t *body)
{
    /* Fresh screen, fresh sticky state -- see the s_busy_sticky comment at
     * its declaration. Without this, a busy refusal shown on one visit
     * could still be "open" (by tick count) on a screen built moments later,
     * silently swallowing that screen's first real poll update. */
    s_busy_sticky = false;

    lv_obj_t *label = lv_label_create(body);
    lv_label_set_text(label, " ");
    lv_obj_set_size(label, lv_pct(100), 22);   /* body width */
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);   /* never overflow the row */
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_user_data(body, label);

    lv_timer_t *t = lv_timer_create(poll_cb, 1000, label);
    lv_obj_add_event_cb(label, status_timer_del_cb, LV_EVENT_DELETE, t);
}

/* ---------- actions ---------- */

/* Both actions refuse while a run is in progress rather than launching a second
 * invocation that dies on the lock without a word. The refusal lives HERE, not
 * in the script's lock-failure path, because that path must not write the
 * status file -- the live run owns it, and overwriting it with "already
 * running" would corrupt the status the poll is showing for a perfectly healthy
 * run. Refresh-then-Download is the natural flow and the two rows are adjacent,
 * so this is the common case, not a corner. */
static int refuse_if_busy(lv_obj_t *status)
{
    if (!run_active()) return 0;
    lbl_set(status, "Busy - wait for the current run");
    s_busy_sticky = true;
    s_busy_since = lv_tick_get();
    return 1;
}

static void refresh_click_cb(lv_event_t *e)
{
    lv_obj_t *status = status_of(lv_event_get_target(e));
    if (refuse_if_busy(status)) return;
    if (system("/usr/bin/podfetch.sh refresh >/dev/null 2>&1 &") == -1) { /* ignore */ }
    lbl_set(status, "Refreshing...");
}

static void download_click_cb(lv_event_t *e)
{
    lv_obj_t *status = status_of(lv_event_get_target(e));
    if (refuse_if_busy(status)) return;
    if (system("/usr/bin/podfetch.sh download >/dev/null 2>&1 &") == -1) { /* ignore */ }
    lbl_set(status, "Downloading...");
}

static void feed_click_cb(lv_event_t *e)
{
    s_feed = (int)(intptr_t)lv_event_get_user_data(e);
    s_page = 0;
    ui_goto(scr_pod_feed);
}

static void page_click_cb(lv_event_t *e)
{
    s_page += (int)(intptr_t)lv_event_get_user_data(e);
    if (s_page < 0) s_page = 0;
    /* A page turn genuinely needs a rebuild (different rows), so unlike the
     * toggle path this pops and re-pushes: back first so ESC from any page
     * returns to the feed list instead of unwinding one page at a time.
     *
     * ui_back() from a row's CLICKED handler does not free anything
     * synchronously: it only pops the nav stack and defers the actual
     * lv_obj_delete() of the old screen to lv_async_call (ui_screens.c,
     * screen_dispose_cb) specifically because deleting the object mid-event
     * -dispatch would be a use-after-free on the very row whose callback is
     * running. ui_goto() then runs scr_pod_feed() synchronously to build the
     * replacement before this handler returns. This is the same ordering
     * ui_screens.c already relies on for WiFi country/add-network flows
     * (e.g. country_open_cb / addnet path: `ui_back(); ui_goto(scr_wifi);`),
     * so no new pattern is introduced here. (ui_goto() itself has exactly
     * one genuinely SYNCHRONOUS delete path -- the nav-stack-full branch at
     * ui_screens.c:436-441, which deletes an untracked screen immediately
     * because it was never pushed and nothing can be showing it. It is
     * unreachable at the depth Podcasts sits at (stack max 8; Podcasts is
     * 2-4 deep), so it doesn't apply here, but it does mean "ui_back()/
     * ui_goto() never delete synchronously" is not quite true in general.) */
    ui_back();
    ui_goto(scr_pod_feed);
}

/* Toggle wanted, or play if already downloaded. user_data = row index into the
 * page's episode array, which is owned by the screen (freed on DELETE). */
static up_ep_t *s_page_eps;          /* current page's rows, screen-owned */
static int      s_page_n;

static void ep_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_page_eps || i < 0 || i >= s_page_n) return;
    const up_ep_t *ep = &s_page_eps[i];
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *status = status_of(btn);

    char dir[256];
    feed_dir(s_feed, dir, sizeof dir);

    if (on_disk(dir, ep->file)) {
        /* Play by absolute path. mpd_quote() escapes " and \; pod_name_ok()
         * already excluded both, so this is belt and braces. */
        char path[512], quoted[600], cmd[700], r[256];
        snprintf(path, sizeof path, "%s/%s", dir, ep->file);
        if (!pod_name_ok(ep->file)) return;         /* card is removable */
        mpd_quote(path, quoted, sizeof quoted);
        mpd_cmd("clear", r, sizeof r);
        snprintf(cmd, sizeof cmd, "add %s", quoted);
        mpd_cmd(cmd, r, sizeof r);
        /* random 0: otherwise an episode inherits shuffle from a previous
         * Shuffle Songs and jumps mid-listen. */
        mpd_cmd("random 0", r, sizeof r);
        mpd_cmd("play", r, sizeof r);
        ui_goto(scr_now_playing);
        return;
    }

    char wanted[WANTED_MAX], wpath[300];
    snprintf(wpath, sizeof wpath, "%s/wanted.txt", dir);
    if (read_file(wpath, wanted, sizeof wanted) < 0) {
        /* wanted.txt is bigger than our buffer: refuse to touch it rather
         * than silently writing back a truncated copy, which would drop
         * every queued entry past the cutoff with no warning. */
        lbl_set(status, "Wanted list too large to edit");
        return;
    }
    int rc = up_wanted_toggle(wanted, sizeof wanted, ep->file);
    if (rc < 0) {
        lbl_set(status, "Too many selected");
        return;
    }
    if (!write_file(wpath, wanted)) {
        lbl_set(status, "Cannot write to card");
        return;
    }
    /* Update this row's label IN PLACE rather than rebuilding the screen.
     * Deleting the screen from inside its own row's CLICKED handler is the
     * use-after-free shape that bit the BT screen; it also loses focus and
     * re-reads the index. Use the list widget's own text-setter (it walks
     * the button's children for the lv_label_class child rather than
     * assuming a fixed index) instead of reaching for child 0 directly. */
    char label[96];
    up_row_label(ep, rc == 1 ? UP_ST_WANTED : UP_ST_AVAIL, label, sizeof label);
    lv_list_set_button_text(lv_obj_get_parent(btn), btn, label);
}

/* ---------- long-press Delete ----------
 *
 * Manual selection implies manual deletion: the spec dropped automatic
 * retention pruning precisely because a prune rule would delete the episode the
 * user deliberately picked, which makes this Delete the ONLY bound on disk use
 * besides the df floor. Without it the branch ships with no disk-use bound at
 * all.
 *
 * Shape copied from the BT forget flow (ui_screens.c bt_dev_longpress_cb /
 * scr_bt_forget) rather than invented: long press pushes a confirm screen whose
 * buttons listen for SHORT_CLICKED, because the long press that OPENED the
 * popup is still held and its release delivers CLICKED to the newly focused
 * button -- which would instantly dismiss (or worse, confirm) the popup.
 * SHORT_CLICKED is suppressed after a long press, so that carried-over release
 * is ignored. The episode rows themselves use SHORT_CLICKED for the same
 * reason: CLICKED also fires on release after a long press, which would toggle
 * or play the very episode just deleted.
 *
 * Path and name are SNAPSHOTTED here, not held as a pointer into the page
 * array: the array belongs to the picker screen and a rebuild frees it. */
static char s_del_dir[256];
static char s_del_file[UP_FILE_MAX];

static lv_obj_t *scr_pod_delete(void);

static void del_yes_cb(lv_event_t *e)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", s_del_dir, s_del_file);
    if (remove(path) != 0) {
        /* Removable, possibly read-only or dying card: say so and stay put
         * rather than navigating away as if it had worked. */
        lv_obj_t *q = lv_event_get_user_data(e);
        if (q) lv_label_set_text(q, "Delete failed");
        return;
    }
    /* Pop the confirm, then pop and rebuild the picker so the deleted row is
     * gone. Both ui_back()s only pop the nav stack and defer the actual
     * lv_obj_delete() to lv_async_call, so nothing is freed underneath this
     * running handler -- the same ordering page_click_cb relies on. The
     * deferred free_eps_cb for the old page compares its pointer against
     * s_page_eps, so the array the rebuild just installed is not disturbed. */
    ui_back();
    ui_back();
    ui_goto(scr_pod_feed);
}

static lv_obj_t *scr_pod_delete(void)
{
    char q[UP_FILE_MAX + 16];
    snprintf(q, sizeof q, "Delete %s?", s_del_file);
    return ui_confirm_screen("Delete Episode", q, "Delete", "Cancel", del_yes_cb, 0);
}

static void ep_longpress_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_page_eps || i < 0 || i >= s_page_n) return;
    const up_ep_t *ep = &s_page_eps[i];
    char dir[256];
    if (!pod_name_ok(ep->file)) return;        /* the card is removable */
    if (!is_media(ep->file)) return;           /* never index.tsv/wanted.txt,
                                                 * even if a feed crafts a URL
                                                 * whose filename collides with
                                                 * one of our control files */
    feed_dir(s_feed, dir, sizeof dir);
    if (!on_disk(dir, ep->file)) return;       /* nothing downloaded to delete */
    snprintf(s_del_dir, sizeof s_del_dir, "%s", dir);
    snprintf(s_del_file, sizeof s_del_file, "%s", ep->file);
    ui_goto(scr_pod_delete);
}

/* ---------- on-disk episodes the feed has aged out ----------
 *
 * The feed's index only carries what the publisher still lists (most feeds cap
 * at 50-300 items). An episode that has fallen off the end is still on the card
 * but has no index row, so before this it was invisible in the picker AND
 * undeletable -- and Folders cannot see /media/sd/podcasts either, by design.
 * Permanently orphaned card space with no on-device remedy.
 *
 * Cost control: this readdir + second index pass runs ONLY on the last page
 * (where these rows belong -- the spec orders episodes by index position and
 * falls back to mtime for files no longer in the index), never on every page
 * turn. At most UP_ORPHAN_ROWS are appended, so the per-page row budget stays
 * bounded; anything beyond that is reported in a note row and becomes reachable
 * as the user deletes the ones shown.
 *
 * Returns the number of up_ep_t written to `out`; *more receives the count that
 * did not fit. */
static int collect_orphans(const char *dir, const char *ipath,
                           up_ep_t *out, int max, int *more)
{
    up_disk_t *d;
    DIR *dp;
    struct dirent *de;
    FILE *f;
    int n = 0, w = 0;

    *more = 0;
    if (max <= 0) return 0;
    d = malloc(sizeof *d * UP_DISK_MAX);
    if (!d) return 0;

    dp = opendir(dir);
    if (dp) {
        while ((de = readdir(dp)) && n < UP_DISK_MAX) {
            char p[512];
            struct stat st;
            size_t nl;
            if (!pod_name_ok(de->d_name) || !is_media(de->d_name)) continue;
            /* A name that does not FIT cannot be one podindex derived (it caps
             * at PI_NAME_MAX == UP_FILE_MAX), so skipping it loses nothing --
             * and copying by measured length keeps this off the
             * format-truncation list, where a silent cut would produce a
             * filename that matches nothing and deletes nothing. */
            nl = strlen(de->d_name);
            if (nl >= sizeof d[n].file) continue;
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            memcpy(d[n].file, de->d_name, nl + 1);
            d[n].mtime = (long)st.st_mtime;
            d[n].in_index = 0;
            n++;
        }
        closedir(dp);
    }

    /* A missing/unreadable index means every downloaded file is an orphan --
     * which is exactly right: without an index the picker has nothing else to
     * show, and those files must still be playable and deletable. */
    f = fopen(ipath, "r");
    if (f) { up_disk_mark_indexed(f, d, n); fclose(f); }

    n = up_disk_keep_orphans(d, n);
    up_disk_sort_mtime(d, n);                  /* newest first */
    for (w = 0; w < n && w < max; w++) {
        /* The filename IS the only title available -- the feed no longer lists
         * this episode, so there is no pubDate and no publisher title. The
         * title field is one byte shorter than the filename field, so the copy
         * is bounded explicitly rather than left to snprintf (display-only, and
         * out[w].file keeps the exact name the delete path needs). */
        size_t tl = strlen(d[w].file);
        snprintf(out[w].file, sizeof out[w].file, "%s", d[w].file);
        out[w].date[0] = 0;
        if (tl > sizeof out[w].title - 1) tl = sizeof out[w].title - 1;
        memcpy(out[w].title, d[w].file, tl);
        out[w].title[tl] = 0;
    }
    *more = n - w;
    free(d);
    return w;
}

static void free_eps_cb(lv_event_t *e)
{
    void *p = lv_event_get_user_data(e);
    if (p == s_page_eps) { s_page_eps = NULL; s_page_n = 0; }
    free(p);
}

/* ---------- screens ---------- */

static lv_obj_t *add_row_ev(lv_obj_t *list, const char *txt,
                            lv_event_cb_t cb, void *ud, lv_event_code_t code)
{
    lv_obj_t *b = lv_list_add_button(list, NULL, txt);
    ui_focus_add(b);
    ui_apply_focus_style(b);
    lv_obj_add_event_cb(b, cb, code, ud);
    lv_obj_add_event_cb(b, group_key_cb, LV_EVENT_KEY, NULL);
    return b;
}

static lv_obj_t *add_row(lv_obj_t *list, const char *txt,
                         lv_event_cb_t cb, void *ud)
{
    return add_row_ev(list, txt, cb, ud, LV_EVENT_CLICKED);
}

lv_obj_t *scr_podcasts(void)
{
    lv_obj_t *scr = ui_screen_base("Podcasts");
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *list = lv_list_create(body);
    lv_obj_set_size(list, ui_screen_body_w(scr), 196);

    load_feeds();
    if (s_nfeeds == 0) {
        ui_empty_state(scr, "No podcasts yet",
                       "Add RSS feed URLs, one per line, to\n" POD_FEEDS);
        return scr;
    }

    for (int i = 0; i < s_nfeeds; i++) {
        char dir[256], label[160], age[16] = "";
        int have = 0, want = 0;
        feed_dir(i, dir, sizeof dir);

        /* Whether the feed has been indexed at all is a stat(), not a read:
         * up_index_window() always scans to EOF to fill *total, so calling
         * it here just to learn "indexed or not" would be a full read of
         * every feed's index.tsv on every entry to this screen -- about
         * 2 MB of synchronous SD reads across 16 feeds. The exact episode
         * total isn't shown on this row at all (see below), so nothing here
         * needs the window read; the picker screen does its own window read
         * for its own purposes when a feed is actually opened. */
        char ipath[300];
        snprintf(ipath, sizeof ipath, "%s/index.tsv", dir);
        struct stat ist;
        int indexed = stat(ipath, &ist) == 0 && S_ISREG(ist.st_mode);

        char wbuf[WANTED_MAX], wpath[300];
        snprintf(wpath, sizeof wpath, "%s/wanted.txt", dir);
        read_file(wpath, wbuf, sizeof wbuf);
        for (const char *p = wbuf; *p; p++) if (*p == '\n') want++;

        /* Downloaded count comes from the DIRECTORY, not the index: one readdir
         * instead of a full index pass per feed. */
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (pod_name_ok(de->d_name) && is_media(de->d_name)) have++;
            closedir(d);
        }

        /* Index AGE, from the stat() we already did -- no extra read. The spec
         * makes this load-bearing rather than decorative: the enclosure URLs
         * are signed and expiring, so an index more than a few hours old will
         * fail to download and the user needs to see that BEFORE pressing
         * Download rather than reading "Error" afterwards. */
        if (indexed) up_age_str((long)(time(NULL) - ist.st_mtime), age, sizeof age);

        if (!indexed)
            snprintf(label, sizeof label, "%s  (not indexed)", s_feeds[i].name);
        else
            snprintf(label, sizeof label, "%s  %d downloaded, %d queued  %s",
                     s_feeds[i].name, have, want, age);
        add_row(list, label, feed_click_cb, (void *)(intptr_t)i);
    }

    add_row(list, "Refresh feeds",     refresh_click_cb,  NULL);
    add_row(list, "Download selected", download_click_cb, NULL);
    attach_status(body);
    return scr;
}

lv_obj_t *scr_pod_feed(void)
{
    if (s_nfeeds == 0) load_feeds();
    if (s_feed < 0 || s_feed >= s_nfeeds) return NULL;

    char dir[256], ipath[300], title[80];
    feed_dir(s_feed, dir, sizeof dir);
    snprintf(ipath, sizeof ipath, "%s/index.tsv", dir);

    /* UP_PAGE index rows plus room for the orphan rows appended to the last
     * page (see collect_orphans). One allocation, freed with the screen. */
    up_ep_t *eps = malloc(sizeof *eps * (UP_PAGE + UP_ORPHAN_ROWS));
    int n = 0, total = 0, n_orph = 0, orph_more = 0;
    if (eps) {
        FILE *f = fopen(ipath, "r");
        if (f) { n = up_index_window(f, s_page, UP_PAGE, eps, UP_PAGE, &total); fclose(f); }
    }
    int pages = up_page_count(total, UP_PAGE);
    s_page = up_page_clamp(s_page, pages);

    /* Orphans belong at the END of the ordering, so they are collected only on
     * the last page (pages == 0 means "no index at all", which is also a last
     * page -- and the only case where the downloaded files are the ONLY thing
     * there is to show). */
    if (eps && (pages == 0 || s_page + 1 >= pages))
        n_orph = collect_orphans(dir, ipath, eps + n, UP_ORPHAN_ROWS, &orph_more);

    snprintf(title, sizeof title, "%.40s", s_feeds[s_feed].name);
    lv_obj_t *scr = ui_screen_base(title);
    lv_obj_t *body = ui_screen_body(scr);
    lv_obj_t *list = lv_list_create(body);
    lv_obj_set_size(list, ui_screen_body_w(scr), 196);

    if (n + n_orph == 0) {
        free(eps);
        lv_obj_t *l = lv_label_create(body);
        lv_label_set_text(l, total ? "No episodes on this page"
                                   : "Not indexed yet\nRun Refresh feeds");
        lv_obj_center(l);
        ui_screen_keysink(scr);
        return scr;
    }

    s_page_eps = eps;
    s_page_n = n + n_orph;
    lv_obj_add_event_cb(scr, free_eps_cb, LV_EVENT_DELETE, eps);

    char wbuf[WANTED_MAX], wpath[300];
    snprintf(wpath, sizeof wpath, "%s/wanted.txt", dir);
    read_file(wpath, wbuf, sizeof wbuf);

    /* "Newer" first so paging up is reachable without scrolling the whole page. */
    lv_obj_t *first_ep = NULL;
    if (s_page > 0) {
        char nav[64];
        snprintf(nav, sizeof nav, "^ Newer (page %d of %d)", s_page, pages);
        add_row(list, nav, page_click_cb, (void *)(intptr_t)-1);
    }
    for (int i = 0; i < n; i++) {
        int state = on_disk(dir, eps[i].file) ? UP_ST_ONDISK
                  : up_wanted_has(wbuf, eps[i].file) ? UP_ST_WANTED : UP_ST_AVAIL;
        char label[96];
        up_row_label(&eps[i], state, label, sizeof label);
        /* ONDISK rows use SHORT_CLICKED, not CLICKED: CLICKED also fires on
         * release after a long press, which would toggle/play the episode
         * the user just confirmed deleting via the LONG_PRESSED handler
         * below (same reason ui_screens.c's BT rows do it). Every other
         * state has no long-press action at all, so gating on
         * LV_INDEV_DEF_LONG_PRESS_TIME's `long_pr_sent` would make a
         * deliberate slow press do nothing; those rows use plain CLICKED,
         * which fires on release regardless of how long the row was held
         * (lvgl/src/indev/lv_indev.c:1017-1022 -- SHORT_CLICKED is gated on
         * `long_pr_sent == 0`, CLICKED is sent unconditionally right after). */
        lv_obj_t *b = add_row_ev(list, label, ep_click_cb,
                                 (void *)(intptr_t)i,
                                 state == UP_ST_ONDISK ? LV_EVENT_SHORT_CLICKED
                                                        : LV_EVENT_CLICKED);
        if (state == UP_ST_ONDISK)
            lv_obj_add_event_cb(b, ep_longpress_cb, LV_EVENT_LONG_PRESSED,
                                (void *)(intptr_t)i);
        if (!first_ep) first_ep = b;
    }

    /* Downloaded files the feed no longer lists. lv_list_add_text for the
     * heading is deliberate: it is display-only and must NOT be in the focus
     * group (unlike the navigation rows, which must be real buttons). */
    if (n_orph > 0) {
        lv_list_add_text(list, "Downloaded, no longer in the feed");
        for (int i = n; i < n + n_orph; i++) {
            char label[96];
            up_row_label(&eps[i], UP_ST_ONDISK, label, sizeof label);
            lv_obj_t *b = add_row_ev(list, label, ep_click_cb,
                                     (void *)(intptr_t)i, LV_EVENT_SHORT_CLICKED);
            lv_obj_add_event_cb(b, ep_longpress_cb, LV_EVENT_LONG_PRESSED,
                                (void *)(intptr_t)i);
            if (!first_ep) first_ep = b;
        }
        if (orph_more > 0) {
            char note[64];
            snprintf(note, sizeof note, "%d more not shown", orph_more);
            lv_list_add_text(list, note);
        }
    }

    if (s_page + 1 < pages) {
        char nav[64];
        snprintf(nav, sizeof nav, "v Older (page %d of %d)", s_page + 2, pages);
        add_row(list, nav, page_click_cb, (void *)(intptr_t)+1);
    }

    /* Focus an EPISODE row, not the navigation row, so repeated page turns
     * advance instead of sitting still. */
    if (first_ep) lv_group_focus_obj(first_ep);
    attach_status(body);
    return scr;
}
