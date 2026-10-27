# Give every web build its own URLs so browsers never mix an old index.js with a new index.wasm:
# index.html loads index.js?v=<hash>, and Module.locateFile adds the same ?v= to the wasm and the data package.
# Usage: python web_stamp.py build_web/out
import hashlib, io, os, sys

out = sys.argv[1]
h = hashlib.sha1()
for name in ('index.js', 'index.wasm', 'index.data'):
    p = os.path.join(out, name)
    if os.path.exists(p):
        with open(p, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
bid = h.hexdigest()[:12]
page = os.path.join(out, 'index.html')
s = io.open(page, encoding='utf-8').read()
for old in ('src="index.js"', 'src=index.js>', 'src=index.js '):    # emcc may minify the page and drop quotes
    s = s.replace(old, old.replace('index.js', 'index.js?v=%s' % bid))
s = s.replace('__BUILD_ID__', bid)
io.open(page, 'w', encoding='utf-8', newline='\n').write(s)
print('build id', bid)
