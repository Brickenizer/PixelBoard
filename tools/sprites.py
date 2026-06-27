#!/usr/bin/env python3
"""
sprites.py — Round-trip converter between sprites.csv and pacman.cpp

Usage:
    python sprites.py export   # pacman.cpp  → sprites.csv
    python sprites.py import   # sprites.csv → pacman.cpp
    python sprites.py check    # validate sprites.csv only

CSV columns:
    name          Base sprite name (no direction suffix)
    start_idx     First frame index in packman6_sprites8x8_data[]
    frame_count   Frames per direction variant
    has_direction yes = sheet has right/left variants at start_idx and start_idx+frame_count
                  no  = single set of frames used for all directions
    role          player | hunter | prey | static | flicker | unused
    score         Text shown on eat/catch (empty = no display, max 6 chars)
                  Can be a number ("100"), a word ("YUM"), or a symbol ("!")
    notes         Free text — not parsed by the importer

Roles:
    player   Pac-Man — always present, not configurable
    hunter   Pursues Pac-Man in MODE_GHOSTS_CHASE
    prey     Flees from Pac-Man in MODE_PACMAN_CHASES
    static   Fixed position in both modes; eaten on overlap
    flicker  Special: alternates with a prey sprite as a warning (gray_ghost only)
    unused   In the sheet but not currently assigned
"""

import csv, re, sys, shutil
from pathlib import Path
from datetime import datetime

SCRIPT_DIR = Path(__file__).parent
CSV_PATH   = SCRIPT_DIR / 'sprites.csv'
CPP_PATH   = SCRIPT_DIR.parent / 'lib' / 'patterns' / 'pacman' / 'pacman.cpp'

BEGIN_HUNTERS = '// <<< GENERATED: HUNTER_SPRITES >>>'
END_HUNTERS   = '// <<< END GENERATED: HUNTER_SPRITES >>>'
BEGIN_PREY    = '// <<< GENERATED: PREY_SPRITES >>>'
END_PREY      = '// <<< END GENERATED: PREY_SPRITES >>>'

VALID_ROLES = {'player', 'hunter', 'prey', 'static', 'flicker', 'unused'}

# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------
def load_csv(path=CSV_PATH):
    with open(path, newline='') as f:
        return [dict(r) for r in csv.DictReader(f)]

def save_csv(rows, path=CSV_PATH):
    fields = ['name','start_idx','frame_count','has_direction','role','score','notes']
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)

def validate_csv(rows):
    errors = []
    seen = {}
    for i, row in enumerate(rows, 2):
        name = row.get('name','').strip()
        if not name:
            errors.append(f"Row {i}: empty name"); continue
        if name in seen:
            errors.append(f"Row {i}: duplicate '{name}' (first at row {seen[name]})")
        seen[name] = i
        try:
            s = int(row['start_idx'])
            if not 0 <= s <= 110:  # allow for expanded sprite sheet
                errors.append(f"Row {i} ({name}): start_idx {s} out of range 0-84")
        except:
            errors.append(f"Row {i} ({name}): invalid start_idx '{row.get('start_idx')}'")
        try:
            fc = int(row['frame_count'])
            if not 1 <= fc <= 15:  # allow up to 15 frames (death anim)
                errors.append(f"Row {i} ({name}): frame_count {fc} looks wrong")
        except:
            errors.append(f"Row {i} ({name}): invalid frame_count '{row.get('frame_count')}'")
        role = row.get('role','').lower()
        if role not in VALID_ROLES:
            errors.append(f"Row {i} ({name}): unknown role '{role}'")
        hd = row.get('has_direction','').lower()
        if hd not in ('yes','no'):
            errors.append(f"Row {i} ({name}): has_direction must be yes/no, got '{hd}'")
        score = row.get('score','')
        if len(score) > 6:
            errors.append(f"Row {i} ({name}): score '{score}' exceeds 6 chars")
    return errors

# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------
INFO_COMMENT = '// SpriteInfo: name, rightStart, leftStart, frameCount, scoreText'

def sprite_line(r):
    name    = r['name']
    start   = int(r['start_idx'])
    fc      = int(r['frame_count'])
    has_dir = r['has_direction'].lower() == 'yes'
    right   = start
    left    = start + fc if has_dir else start
    score   = r.get('score', '')
    notes   = r.get('notes', '')
    comment = f'  // {notes}' if notes else ''
    return f'    {{ "{name}", {right:2d}, {left:2d}, {fc}, "{score}" }},{comment}'

def make_hunter_block(rows):
    hunters = [r for r in rows if r['role'] == 'hunter']
    lines = [INFO_COMMENT,
             'static const SpriteInfo HUNTER_SPRITES[] = {']
    lines += [sprite_line(r) for r in hunters]
    lines += ['};',
              'static const uint8_t HUNTER_SPRITE_COUNT = '
              'sizeof(HUNTER_SPRITES) / sizeof(HUNTER_SPRITES[0]);']
    return '\n'.join(lines)

def make_prey_block(rows):
    prey    = [r for r in rows if r['role'] == 'prey']
    statics = [r for r in rows if r['role'] == 'static']
    flicker = [r for r in rows if r['role'] == 'flicker']

    lines = [INFO_COMMENT,
             'static const SpriteInfo PREY_SPRITES[] = {']
    lines += [sprite_line(r) for r in prey]
    lines += ['};',
              'static const uint8_t PREY_SPRITE_COUNT = '
              'sizeof(PREY_SPRITES) / sizeof(PREY_SPRITES[0]);']

    if flicker:
        lines.append('')
        lines.append('// Flicker sprites: alternate with prey near end of chase')
        for r in flicker:
            tag   = r['name'].upper()
            start = int(r['start_idx'])
            fc    = int(r['frame_count'])
            note  = r.get('notes','')
            c     = f'  // {note}' if note else ''
            lines.append(f'static const uint8_t FLICKER_{tag}_START  = {start};{c}')
            lines.append(f'static const uint8_t FLICKER_{tag}_FRAMES = {fc};')

    if statics:
        lines.append('')
        lines.append('// Static sprites — sit at fixed X, eaten on overlap')
        lines.append('static const SpriteInfo STATIC_SPRITES[] = {')
        lines += [sprite_line(r) for r in statics]
        lines.append('};')
        lines.append('static const uint8_t STATIC_SPRITE_COUNT = '
                     'sizeof(STATIC_SPRITES) / sizeof(STATIC_SPRITES[0]);')

    return '\n'.join(lines)

def wrap(marker, content):
    b = f'// <<< GENERATED: {marker} >>>'
    e = f'// <<< END GENERATED: {marker} >>>'
    return f'{b}\n{content}\n{e}'

# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------
def do_import():
    print(f"Reading  {CSV_PATH}")
    rows   = load_csv()
    errors = validate_csv(rows)
    if errors:
        print(f"Validation errors — not writing:")
        for e in errors: print(f"  {e}")
        sys.exit(1)

    cpp = CPP_PATH.read_text()
    bak = CPP_PATH.with_suffix(f'.cpp.bak_{datetime.now():%Y%m%d_%H%M%S}')
    shutil.copy(CPP_PATH, bak)
    print(f"Backup → {bak.name}")

    hunter_block = wrap('HUNTER_SPRITES', make_hunter_block(rows))
    prey_block   = wrap('PREY_SPRITES',   make_prey_block(rows))

    def replace(text, begin, end, block):
        pat = re.compile(re.escape(begin) + r'.*?' + re.escape(end), re.DOTALL)
        if pat.search(text):
            return pat.sub(block, text)
        print(f"  WARNING: marker '{begin}' not found — appending")
        return text + '\n\n' + block + '\n'

    cpp = replace(cpp, BEGIN_HUNTERS, END_HUNTERS, hunter_block)
    cpp = replace(cpp, BEGIN_PREY,    END_PREY,    prey_block)
    CPP_PATH.write_text(cpp)

    hunters = [r for r in rows if r['role']=='hunter']
    prey    = [r for r in rows if r['role']=='prey']
    statics = [r for r in rows if r['role']=='static']
    flicker = [r for r in rows if r['role']=='flicker']
    unused  = [r for r in rows if r['role']=='unused']
    print(f"Written  {CPP_PATH.name}")
    print(f"  Hunters : {len(hunters):2d}  ({', '.join(r['name'] for r in hunters)})")
    print(f"  Prey    : {len(prey):2d}  ({', '.join(r['name'] for r in prey)})")
    print(f"  Static  : {len(statics):2d}  ({', '.join(r['name'] for r in statics)})")
    print(f"  Flicker : {len(flicker):2d}  ({', '.join(r['name'] for r in flicker)})")
    print(f"  Unused  : {len(unused):2d}")

# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------
def do_export():
    print(f"Reading  {CPP_PATH}")
    cpp = CPP_PATH.read_text()

    def extract(text, begin, end):
        pat = re.compile(re.escape(begin) + r'(.*?)' + re.escape(end), re.DOTALL)
        m   = pat.search(text)
        return m.group(1) if m else ''

    h_block = extract(cpp, BEGIN_HUNTERS, END_HUNTERS)
    p_block = extract(cpp, BEGIN_PREY,    END_PREY)

    # Parse SpriteInfo lines: { "name", right, left, fc, "score" }
    si_re = re.compile(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*\}')

    cpp_hunters = {m.group(1): m for m in si_re.finditer(h_block)}
    cpp_prey    = {m.group(1): m for m in si_re.finditer(p_block)}

    # Also detect STATIC_SPRITES block within prey section
    static_start = p_block.find('STATIC_SPRITES[]')
    cpp_statics  = {}
    if static_start >= 0:
        for m in si_re.finditer(p_block[static_start:]):
            cpp_statics[m.group(1)] = m

    print(f"  Found {len(cpp_hunters)} hunters, {len(cpp_prey)} prey, "
          f"{len(cpp_statics)} statics in C++")

    rows = load_csv()
    bak  = CSV_PATH.with_suffix(f'.csv.bak_{datetime.now():%Y%m%d_%H%M%S}')
    shutil.copy(CSV_PATH, bak)
    print(f"Backup → {bak.name}")

    for r in rows:
        name = r['name']
        if r['role'] == 'flicker': continue
        if name in cpp_hunters:
            r['role']  = 'hunter'
            r['score'] = cpp_hunters[name].group(5)
        elif name in cpp_statics:
            r['role']  = 'static'
            r['score'] = cpp_statics[name].group(5)
        elif name in cpp_prey:
            r['role']  = 'prey'
            r['score'] = cpp_prey[name].group(5)
        elif r['role'] not in ('player','flicker'):
            r['role'] = 'unused'

    save_csv(rows, CSV_PATH)
    print(f"Written  {CSV_PATH.name}")

# ---------------------------------------------------------------------------
# Check
# ---------------------------------------------------------------------------
def do_check():
    print(f"Checking {CSV_PATH}")
    rows   = load_csv()
    errors = validate_csv(rows)
    if errors:
        print(f"Found {len(errors)} error(s):")
        for e in errors: print(f"  {e}")
        sys.exit(1)

    by_role = {}
    for r in rows:
        by_role.setdefault(r['role'], []).append(r['name'])

    print(f"OK — {len(rows)} sprites")
    for role in ('player','hunter','prey','static','flicker','unused'):
        names = by_role.get(role, [])
        print(f"  {role:<8}: {len(names):2d}  ({', '.join(names)})")

# ---------------------------------------------------------------------------
if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('import','export','check'):
        print(__doc__)
        sys.exit(1)
    {'import': do_import, 'export': do_export, 'check': do_check}[sys.argv[1]]()
