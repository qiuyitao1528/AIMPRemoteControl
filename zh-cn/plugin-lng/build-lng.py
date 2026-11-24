# Build the plugin language file from the UTF-8 source: UTF-16LE with BOM and CRLF,
# the format the plugin ships (plugin/Langs/english.lng). Key set is checked against
# the original before writing.
# usage: uv run python -I build-lng.py <original english.lng> <source .lng> <output .lng>
import re
import sys

orig_path, src_path, out_path = sys.argv[1:4]


def parse(text):
    pairs = re.findall(r'^([^\[\]=\r\n]+)=(.*)$', text, flags=re.M)
    return [(k, len(re.findall(r'\{\d+\}', v))) for k, v in pairs], len(re.findall(r'^\[.*\]$', text, flags=re.M))


orig = open(orig_path, encoding='utf-16').read()
src = open(src_path, encoding='utf-8').read()

orig_keys, orig_sections = parse(orig)
src_keys, src_sections = parse(src)
# (key, placeholder count) pairs: the same key can sit in two sections, so compare per line
assert orig_keys == src_keys, f'key or placeholder mismatch: {set(orig_keys) ^ set(src_keys)}'
assert orig_sections == src_sections, f'section count {orig_sections} != {src_sections}'

out = '\r\n'.join(src.replace('\r\n', '\n').rstrip('\n').split('\n')) + '\r\n'
open(out_path, 'w', encoding='utf-16-le', newline='').write('\ufeff' + out)
print(f'ok: {len(src_keys)} keys, {src_sections} sections -> {out_path}')
