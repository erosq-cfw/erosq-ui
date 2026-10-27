#!/usr/bin/env python3
"""fake_mpd_lib.py <socket> [albums] [songs_per_album] [groups]: a fake MPD with
a big library, for the host preview (EROSQ_MPD_SOCK=<socket> build-host/erosq-ui).

Default: 2,000 albums by 2,000 artists, 10 songs each (20,000 songs), filed as
<group>/<album>/<nn> Title.flac under 500 top-level folders. A few artists
differ only in case ("TOOL"/"Tool") to exercise the artist case-folding.
`update`/`rescan` report updating_db for 3 s, then adds an album."""
import os, socket, sys, threading, time, shlex

path = sys.argv[1]
NA = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
SPA = int(sys.argv[3]) if len(sys.argv) > 3 else 10
NG = int(sys.argv[4]) if len(sys.argv) > 4 else 500
lock = threading.Lock()
state = {"albums": NA, "upd_until": 0.0, "job": 0}

def artist(a):
    if a == 1: return "TOOL"
    if a == 2: return "Tool"
    return "Artist %04d" % a

def songs():
    out = []
    for a in range(state["albums"]):
        for t in range(SPA):
            out.append((a, t, "Group %03d/Album %04d/%02d Track %d.flac" % (a % NG, a, t + 1, t + 1)))
    return out

def meta(a, t, f):
    return ("file: %s\nLast-Modified: 2026-10-01T12:00:00Z\nFormat: 44100:16:2\nTime: 215\n"
            "duration: 215.000\nArtist: %s\nAlbum: Album %04d\nTitle: Track %d\nTrack: %d\nGenre: Pop\n"
            % (f, artist(a), a, t + 1, t + 1))

def reply(line):
    args = shlex.split(line)
    if not args: return "OK\n"
    c, a = args[0], args[1:]
    with lock:
        if time.time() > state["upd_until"] and state["upd_until"]:
            state["upd_until"] = 0.0; state["albums"] += 1
        upd = state["upd_until"] > 0
    if c == "status":
        return "volume: 50\nrepeat: 0\nrandom: 0\nstate: stop\n" + ("updating_db: %d\n" % state["job"] if upd else "") + "OK\n"
    if c == "stats":
        n = state["albums"]
        return "artists: %d\nalbums: %d\nsongs: %d\nOK\n" % (n, n, n * SPA)
    if c in ("update", "rescan"):
        with lock:
            if state["upd_until"]: return "ACK [54@0] {%s} already updating\n" % c
            state["upd_until"] = time.time() + 3; state["job"] += 1
        return "updating_db: %d\nOK\n" % state["job"]
    if c == "list" and a and a[0].lower() == "artist":
        return "".join("Artist: %s\n" % artist(x) for x in range(state["albums"])) + "OK\n"
    if c == "list" and a and a[0].lower() == "album":
        if len(a) >= 3:   # list album artist X
            return "".join("Album: Album %04d\n" % x for x in range(state["albums"]) if artist(x) == a[2]) + "OK\n"
        return "".join("Album: Album %04d\n" % x for x in range(state["albums"])) + "OK\n"
    if c == "list" and a and a[0].lower() == "genre": return "Genre: Pop\nOK\n"
    if c == "listplaylists": return "OK\n"
    if c == "lsinfo":
        p = a[0] if a else ""
        if not p: return "".join("directory: Group %03d\n" % g for g in range(NG)) + "OK\n"
        parts = p.split("/")
        if len(parts) == 1:
            g = int(parts[0].split()[-1])
            return "".join("directory: %s/Album %04d\n" % (p, x) for x in range(state["albums"]) if x % NG == g) + "OK\n"
        x = int(parts[1].split()[-1])
        return "".join(meta(x, t, "%s/%02d Track %d.flac" % (p, t + 1, t + 1)) for t in range(SPA)) + "OK\n"
    if c == "find":
        kv = dict(zip(a[0::2], a[1::2]))
        alb = kv.get("album"); art = kv.get("artist")
        return "".join(meta(x, t, f) for x, t, f in songs()
                       if "Album %04d" % x == alb and (art is None or artist(x) == art)) + "OK\n"
    if c == "search" and "window" in a:
        lo, hi = map(int, a[a.index("window") + 1].split(":"))
        return "".join(meta(*s) for s in songs()[lo:hi]) + "OK\n"
    return "OK\n"

def serve(conn):
    try:
        conn.sendall(b"OK MPD 0.24.0\n")
        f = conn.makefile("rb")
        for raw in f:
            conn.sendall(reply(raw.decode().rstrip("\n")).encode())
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        conn.close()

if os.path.exists(path): os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(path); srv.listen(16)
open(path + ".ready", "w").close()
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
