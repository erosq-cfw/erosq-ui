# Minimal fake MPD: greeting, binary_limit, readpicture/albumart chunked binary replies.
# argv: socket-path jpeg-path mode [embedded-path]   mode: embedded | cover | none
#   | badembed (readpicture serves embedded-path, albumart serves jpeg-path)
#   | badembed-nocover (readpicture serves embedded-path, no cover file)
import socket, sys, os
path, jpg, mode = sys.argv[1:4]
data = open(jpg, 'rb').read()
emb = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else data
if os.path.exists(path): os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(path); srv.listen(1)
open(path + '.ready', 'w').close()
while True:
    c, _ = srv.accept(); f = c.makefile('rb'); c.sendall(b'OK MPD 0.24.0\n'); limit = 8192
    try:
      for line in f:
          parts = line.decode().split()
          if not parts: break
          if parts[0] == 'binary_limit': limit = int(parts[1]); c.sendall(b'OK\n')
          elif parts[0] in ('readpicture', 'albumart'):
              rp = parts[0] == 'readpicture'
              have = (mode in ('embedded', 'badembed', 'badembed-nocover') and rp) or (mode in ('cover', 'badembed') and not rp)
              if parts[0] == 'albumart' and not have: c.sendall(b'ACK [50@0] {albumart} No file exists\n'); continue
              if not have: c.sendall(b'OK\n'); continue          # readpicture w/o picture = bare OK
              d = emb if rp and mode.startswith('badembed') else data
              off = int(parts[-1]); chunk = d[off:off + limit]
              c.sendall(b'size: %d\ntype: image/jpeg\nbinary: %d\n' % (len(d), len(chunk)) + chunk + b'\nOK\n')
          else: c.sendall(b'OK\n')
    except ConnectionResetError: pass
    c.close()
