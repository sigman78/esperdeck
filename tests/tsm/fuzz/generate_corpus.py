"""Generate original VT seeds and harvest C string literals from local Unity tests."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def decode_c(text):
    out = bytearray()
    i = 0
    escapes = {'a': 7, 'b': 8, 't': 9, 'n': 10, 'v': 11, 'f': 12,
               'r': 13, '\\': 92, '"': 34, "'": 39, '?': 63}
    while i < len(text):
        if text[i] != '\\':
            out.extend(text[i].encode('utf-8'))
            i += 1
            continue
        i += 1
        if text[i] == 'x':
            match = re.match(r'x([0-9a-fA-F]+)', text[i:])
            value = int(match[1], 16)
            if value > 255:
                raise ValueError('C hex escape exceeds one byte')
            out.append(value)
            i += len(match[0])
        elif text[i] in '01234567':
            match = re.match(r'[0-7]{1,3}', text[i:])
            out.append(int(match[0], 8))
            i += len(match[0])
        else:
            out.append(escapes[text[i]])
            i += 1
    return bytes(out)


def main():
    corpus = ROOT / 'corpus'
    corpus.mkdir(exist_ok=True)
    manifest_path = ROOT / 'corpus_manifest.json'
    previous = json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.exists() else {}
    manifest = {}

    def add(data, source):
        if not data:
            return
        name = hashlib.sha256(data).hexdigest()[:24] + '.vt'
        (corpus / name).write_bytes(data)
        manifest.setdefault(name, {'bytes': len(data), 'sources': []})['sources'].append(source)

    # Tokenize comments separately so quoted prose cannot become terminal input.
    token = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)
    for name in ('test_vtparse.c', 'test_termstate.c'):
        source = (ROOT.parent / name).read_text(encoding='utf-8')
        pending, end, line = [], -1, 0
        for match in token.finditer(source):
            if not match[0].startswith('"'):
                continue
            if pending and source[end:match.start()].strip():
                data = b''.join(pending)
                if any(b < 32 or b >= 127 for b in data):
                    add(data, f'{name}:{line} (C string)')
                pending = []
            if not pending:
                line = source.count('\n', 0, match.start()) + 1
            pending.append(decode_c(match[0][1:-1]))
            end = match.end()
        if pending:
            data = b''.join(pending)
            if any(b < 32 or b >= 127 for b in data):
                add(data, f'{name}:{line} (C string)')

    esc = b'\x1b'
    for n in (1, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1024):
        add(b'A' * n + b'\r\n', f'generated/print-{n}')
        add(esc + b']2;' + b'T' * n + esc + b'\\', f'generated/osc-{n}')
    for n in (0, 1, 15, 16, 17, 32):
        add(esc + b'[' + b'1;' * n + b'31mX', f'generated/params-{n}')
    for value in (b'0', b'1', b'2147483647', b'2147483648', b'4294967295', b'9' * 128):
        for final in (b'A', b'B', b'C', b'D', b'E', b'F', b'H', b'X', b'S', b'T', b'@', b'P', b'm'):
            add(esc + b'[2;3H' + esc + b'[' + value + final, f'generated/numeric-{value[:12].decode()}-{final.decode()}')
    for prefix in (b'[', b']2;', b'P1;2q', b'X', b'^', b'_'):
        for ending in (b'', b'\x18X', b'\x1aX', b'\x07X', b'\x9cX', esc + b'\\X', esc + b'[H'):
            add(esc + prefix + b'abc' + ending, 'generated/string-recovery')
    for data in (bytes(range(256)), b'\xc0\xaf', b'\xe0\x80\x80', b'\xed\xa0\x80',
                 b'\xf4\x90\x80\x80', b'\xf5\x80\x80\x80', b'\xe2\x82',
                 'ASCII café 界 😀'.encode(), b'\xe2' + esc + b'[31mX'):
        add(data, 'generated/utf8-and-binary')
    add(b''.join(f'line {i:03d}\r\n'.encode() for i in range(150)), 'generated/scrollback-wrap')
    add(esc + b'[?1049h' + esc + b'[2J' + esc + b'[H' + b'TUI title\r\n' +
        (esc + b'[38;2;10;200;90mstatus' + esc + b'[0m\r\n') * 70 +
        esc + b'[2;3r' + esc + b'[?6h' + esc + b'[999;999H' + b'X' +
        esc + b'[?1049l' + esc + b'[6n', 'generated/synthetic-tui-session')
    add(b''.join(esc + b'[?' + str(m).encode() + op for m in
        (1, 6, 7, 25, 47, 1047, 1049, 2004, 2026) for op in (b'h', b'l')),
        'generated/mode-switches')
    add(esc + b'(0lqqk\r\nx  x\r\nmqqj' + esc + b'(B' + esc + b'c', 'generated/charset-reset')
    for name in previous.keys() - manifest.keys():
        if re.fullmatch(r'[0-9a-f]{24}\.vt', name):
            (corpus / name).unlink(missing_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    tokens = [esc + x for x in (b'[', b']', b'P', b'\\', b'[?1049h', b'[?1049l',
              b'[38;2;', b'[48;5;', b'[?6h', b'[2;3r', b'[6n', b'[?2026h', b'[?2026l', b'c')]
    tokens += [b'2147483647', b'2147483648', b';', b':', b'\x18', b'\x1a', b'\xc2', b'\xe2\x82\xac']
    (ROOT / 'vt.dict').write_text(''.join('"' + ''.join(f'\\x{b:02x}' for b in t) + '"\n' for t in tokens), encoding='ascii')
    print(f'{len(manifest)} seeds, {sum(item["bytes"] for item in manifest.values())} bytes')


if __name__ == '__main__':
    main()
