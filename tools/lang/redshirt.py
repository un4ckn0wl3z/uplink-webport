# Decode one of Uplink's REDSHIRT archives (data.dat, world.dat) to a plain zip for reading. The files are a
# "REDSHIRT\0" header followed by the zip with every byte offset by 0x80.
# Usage: python redshirt.py data.dat out.zip
import sys
d = open(sys.argv[1], 'rb').read()
if not d.startswith(b'REDSHIRT\0'):
    sys.exit('not a REDSHIRT file')
body = bytes((b - 0x80) & 0xff for b in d[9:])
open(sys.argv[2], 'wb').write(body)
