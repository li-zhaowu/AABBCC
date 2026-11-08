"""Finds the English keys of Tr(L"...") / TrF(L"...") in the viewer sources."""
import os
import re

SOURCES = ['FileManagerWindow.cpp', 'ClientConnectionFiles.cpp', 'ClientConnectionVideo.cpp',
           'ClientConnectionWindow.cpp', 'ClientConnectionAudio.cpp', 'ClientConnection.cpp',
           'MediaSettings.cpp', 'FileManagerDragDrop.cpp']


def decode(lit):
    out, i = [], 0
    while i < len(lit):
        c = lit[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        n = lit[i + 1]
        if n == 'x':
            j = i + 2
            while j < len(lit) and lit[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(chr(int(lit[i + 2:j], 16)))
            i = j
        elif n == 'u':
            out.append(chr(int(lit[i + 2:i + 6], 16)))
            i += 6
        else:
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '0': '\0'}[n])
            i += 2
    return ''.join(out)


LITERAL = re.compile(r'\s*L"((?:[^"\\]|\\.)*)"')


def keys(src_dir):
    found = []
    for name in SOURCES:
        text = open(os.path.join(src_dir, name), encoding='utf-8').read()
        for m in re.finditer(r'\bTrF?\(', text):
            pos, parts = m.end(), []
            while True:
                lm = LITERAL.match(text, pos)
                if not lm:
                    break
                parts.append(decode(lm.group(1)))
                pos = lm.end()
            if parts:
                found.append(''.join(parts))
    return found


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    seen = []
    for k in keys(os.path.dirname(here)):
        if k not in seen:
            seen.append(k)
    for k in seen:
        print(repr(k))
    print(len(seen), 'keys')
