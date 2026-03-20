import re
with open('src/native/special_chars.cpp', 'r') as f:
    content = f.read()
chars = [
    ('\u03b8', 'theta'),
    ('\u03c6', 'phi'),
    ('\u03bc', 'mu'),
    ('\u03bd', 'nu'),
    ('\u03b7', 'eta'),
    ('\u03b1', 'alpha'),
    ('\u03b2', 'beta'),
    ('\u03b3', 'gamma'),
    ('\u03b4', 'delta'),
    ('\u03c0', 'pi'),
    ('\u2060', 'word-joiner U+2060'),
    ('\u200b', 'zero-width-space U+200B'),
    ('\u2061', 'function-application U+2061'),
    ('\u2062', 'invisible-times U+2062'),
    ('\u2063', 'invisible-separator U+2063'),
]
for ch, name in chars:
    found = False
    for i, line in enumerate(content.split('\n'), 1):
        if ch in line:
            print(f"{name} ({repr(ch)}) FOUND at line {i}: {line.strip()[:90]}")
            found = True
            break
    if not found:
        print(f"{name} ({repr(ch)}) NOT FOUND")
