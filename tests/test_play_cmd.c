/* mpd_play_cmd: the Play button must START a stopped player (a bare "pause"
 * is a no-op in MPD's stop state -- HITL 10-20 SD reinsert), toggle otherwise. */
#include <assert.h>
#include <string.h>
#include "ui_mpd.h"

int main(void)
{
    const char *play = "command_list_begin\nclearerror\nplay\ncommand_list_end";
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: stop\nerror: x\nOK\n"), play));
    assert(!strcmp(mpd_play_cmd("state: stop\nOK\n"), play));               /* first line */
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: play\nOK\n"), "pause"));
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: pause\nOK\n"), "pause"));
    assert(!strcmp(mpd_play_cmd("xstate: stop\nOK\n"), "pause"));          /* whole key only */
    assert(!strcmp(mpd_play_cmd("state: stopped\nOK\n"), "pause"));        /* whole value only */
    /* reply cut short by a long error (mpd_cmd -2, no OK): state still decides */
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: stop\nsong: 3\nerror: Failed to decode \"/media/sd/au"), play));
    /* cut INSIDE the state line: never read as stop (needs the whole "stop\n") */
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: pl"), "pause"));
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: sto"), "pause"));
    assert(!strcmp(mpd_play_cmd("volume: 70\nstate: stop"), "pause"));
    assert(!strcmp(mpd_play_cmd(""), "pause"));                            /* no reply: toggle */
    assert(!strcmp(mpd_play_cmd(NULL), "pause"));

    /* mpd_resume_cmd: card-removal record "<songid> <secs>" (sd-hotplug) */
    char b[96];
    const char *st = "volume: 70\nstate: stop\nsong: 6\nsongid: 57\nerror: x\nOK\n";
    const char *seek = "command_list_begin\nclearerror\nseekid 57 55\ncommand_list_end";
    assert(!strcmp(mpd_resume_cmd(st, "57 55\n", b, sizeof b), seek));      /* resume */
    assert(mpd_resume_cmd(st, "57 55\n", b, sizeof b) == b);
    assert(!strcmp(mpd_resume_cmd(st, "57 55", b, sizeof b), seek));        /* no newline */
    assert(!strcmp(mpd_resume_cmd(st, "5 55\n", b, sizeof b), play));       /* other song */
    assert(!strcmp(mpd_resume_cmd(st, "570 55\n", b, sizeof b), play));     /* id prefix */
    assert(!strcmp(mpd_resume_cmd("volume: 70\nstate: stop\nsongid: 570\nOK\n", "57 55\n", b, sizeof b), play));
    assert(!strcmp(mpd_resume_cmd("volume: 70\nstate: stop\nsongid: 57", "57 55\n", b, sizeof b), play)); /* cut mid-id (572?) */
    assert(!strcmp(mpd_resume_cmd(st, NULL, b, sizeof b), play));            /* no record */
    assert(!strcmp(mpd_resume_cmd(st, "", b, sizeof b), play));
    assert(!strcmp(mpd_resume_cmd(st, "57\n", b, sizeof b), play));         /* malformed */
    assert(!strcmp(mpd_resume_cmd(st, "57 x\n", b, sizeof b), play));
    assert(!strcmp(mpd_resume_cmd(st, "57 55 junk", b, sizeof b), play));
    assert(!strcmp(mpd_resume_cmd("volume: 70\nstate: stop\nOK\n", "57 55\n", b, sizeof b), play)); /* no current song */
    assert(!strcmp(mpd_resume_cmd("volume: 70\nstate: play\nsongid: 57\nOK\n", "57 55\n", b, sizeof b), "pause")); /* playing: toggle */
    assert(!strcmp(mpd_resume_cmd("volume: 70\nstate: pause\nsongid: 57\nOK\n", "57 55\n", b, sizeof b), "pause"));
    assert(!strcmp(mpd_resume_cmd("songid: 57\nstate: stop\nOK\n", "57 55\n", b, sizeof b), seek)); /* first line */
    return 0;
}
