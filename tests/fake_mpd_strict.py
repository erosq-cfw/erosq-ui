# Fake MPD with real MPD's binary semantics, for test_picstream:
#   * the command is `binarylimit` -- `binary_limit` is ACKed as unknown
#     (that typo once left MPD at its 8 KB default: 449 calls for one cover)
#   * readpicture chunks are capped at the negotiated limit (default 8192)
#   * DELAY seconds before the FIRST chunk (a cold SD read of a big cover)
# argv: socket-path picture-path delay-seconds
import socket, sys, os, time
path, pic, delay = sys.argv[1], sys.argv[2], float(sys.argv[3])
data = open(pic, 'rb').read()
if os.path.exists(path): os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(path); srv.listen(1)
open(path + '.ready', 'w').close()
while True:
    c, _ = srv.accept(); f = c.makefile('rb'); c.sendall(b'OK MPD 0.24.0\n'); limit = 8192; calls = 0
    try:
        for line in f:
            parts = line.decode().split()
            if not parts: break
            if parts[0] == 'binarylimit': limit = int(parts[1]); c.sendall(b'OK\n')
            elif parts[0] == 'readpicture':
                calls += 1
                if calls == 1: time.sleep(delay)
                off = int(parts[-1]); chunk = data[off:off + limit]
                c.sendall(b'size: %d\ntype: image/jpeg\nbinary: %d\n' % (len(data), len(chunk)) + chunk + b'\nOK\n')
            else: c.sendall(b'ACK [5@0] {} unknown command "%s"\n' % parts[0].encode())
    except (ConnectionResetError, BrokenPipeError): pass
    c.close()
