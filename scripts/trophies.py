#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Trophies of Persona 4: Dancing All Night (CUSA12112): the list and the unlocks.

The game unlocks a trophy through sceNpTrophyUnlockTrophy with its id; the runtime appends
"id unix-time" to <user>/trophies.log and shows the notice in game. The names and descriptions
live here because the game's own trophy texts (TROP.ESFM) are encrypted; the icons are read
from sce_sys/trophy/trophy00.trp, where they are plain PNG files (TROP000.PNG = trophy 0).

As a script: trophies.py <game> --out <dir> writes <dir>/trophies.txt (id, grade, name per line,
read by the runtime) and the icons to <dir>/trophies/.
"""
import argparse
from pathlib import Path
import struct

PLATINUM, GOLD, SILVER, BRONZE = 'platinum', 'gold', 'silver', 'bronze'
GRADES = (PLATINUM, GOLD, SILVER, BRONZE)
PLATINUM_ID = 0
LOG_NAME = 'trophies.log'

# (id, grade, name, description)
TROPHIES = [
    (0, PLATINUM, 'Dancing All Night', 'Obtained all trophies'),
    (1, BRONZE, 'The Festival Begins', 'Completed Chapter 1'),
    (2, BRONZE, 'Closing Night', 'Rescued Tamami Uesugi'),
    (3, BRONZE, 'A Night to Remember', 'Rescued Tomoe Sayama'),
    (4, BRONZE, 'Tidy Up Your Toys', 'Rescued Sumomo Ujima'),
    (5, BRONZE, 'Standing Room Only', 'Rescued Nozomi Nakahara'),
    (6, BRONZE, 'Bearwell!', 'Completed Chapter 4'),
    (7, BRONZE, 'The Empress Dethroned', 'Rescued Kyoka Ochimizu'),
    (8, GOLD, 'Bond Dancers, Go!', 'Got through to Mikuratana-no-Kami'),
    (9, BRONZE, 'Historian', 'Watched all events, including pre-story'),
    (10, BRONZE, 'Time to Party Harder', 'Margaret joins the dance'),
    (11, SILVER, 'To Greater Heights', 'Unlocked highest difficulty (ALL NIGHT)'),
    (12, BRONZE, 'Appearance Matters', 'Bought a costume for the first time'),
    (13, SILVER, 'Best Dressed', 'Bought half of all costumes'),
    (14, GOLD, 'Closet Full of Dreams', 'Bought all costumes'),
    (15, SILVER, 'Lapidary', 'Obtained all accessories'),
    (16, SILVER, 'Hooked on Shopping', 'Bought all items in Shopping'),
    (17, SILVER, 'Regular Customer', 'Used a total of P$500,000 in Shopping'),
    (18, BRONZE, 'That Familiar Rush', 'Watched replay data from Score'),
    (19, BRONZE, 'I Am Thou, Thou Art I', 'Completed the Database'),
    (20, BRONZE, 'Empty Seats', 'A performance ended prematurely'),
    (21, BRONZE, 'Hit the Dance Floor', 'Cleared 1 track in Free Dance'),
    (22, SILVER, 'The Party Never Ends', 'Cleared all tracks in Free Dance'),
    (23, SILVER, 'Love Connections', 'Performed a full combo on a track'),
    (24, BRONZE, 'Born Entertainer', 'Obtained top rank (King Crazy) on a track'),
    (25, SILVER, 'What\'s a "Miss"?', 'Cleared a track with no MISSes'),
    (26, SILVER, 'Perfectionist', '50%+ Perfect clear on HARD track or higher'),
    (27, BRONZE, 'New Personal Best', 'Beat your own high score'),
    (28, BRONZE, 'Fashion Police', 'Changed costume and began dancing'),
    (29, BRONZE, 'The Final Touch', 'Equipped accessory and began dancing'),
    (30, BRONZE, 'Costume Coordinator', 'Cleared a track with each character in changed costumes'),
    (31, SILVER, 'Star Dancer', 'Danced using 2 or more items to raise difficulty'),
    (32, BRONZE, 'Working Together', 'Triggered a Bond Fever at least once'),
    (33, GOLD, 'Inseparable', 'Triggered 30 types of Bond Fevers'),
    (34, SILVER, 'Nanako Forever!', 'Triggered all dances/Bond Fevers with Nanako'),
    (35, BRONZE, 'Dream Team', 'Paired Rise and Kanami in Free Dance'),
    (36, SILVER, "Senpai, You're So Cool!", 'Seen all Bond Fevers between Yu and Rise'),
    (37, SILVER, 'Red and Green', 'Seen all Bond Fevers between Yukiko and Chie'),
    (38, SILVER, 'The Junes Special', 'Seen all Bond Fevers between Yosuke and Teddie'),
    (39, SILVER, 'Beauty and the Beast', 'Seen all Bond Fevers between Naoto and Kanji'),
]
BY_ID = {t[0]: t for t in TROPHIES}
# Spanish: id -> (name, description); a name of None keeps the English one (the game kept
# its English trophy names).
SPANISH = {
    0: (None, 'Consigue el resto de trofeos del juego.'),
    1: (None, 'Completa el Capítulo 1.'),
    2: (None, 'Rescata a Tamami Uesugi.'),
    3: (None, 'Rescata a Tomoe Sayama.'),
    4: (None, 'Rescata a Sumomo Ujima.'),
    5: (None, 'Rescata a Nozomi Nakahara.'),
    6: (None, 'Completa el Capítulo 4.'),
    7: (None, 'Rescata a Kyoka Ochimizu.'),
    8: (None, 'Completa Mikuratana-no-Kami.'),
    9: (None, 'Mira todos los eventos, incluida la pre-historia.'),
    10: (None, 'Margaret se une al baile.'),
    11: (None, 'Desbloquea la dificultad más alta (ALL NIGHT).'),
    12: (None, 'Compra un traje por primera vez.'),
    13: (None, 'Compra la mitad de los trajes.'),
    14: (None, 'Compra todos los trajes.'),
    15: (None, 'Consigue todos los accesorios.'),
    16: (None, 'Compra todos los objetos de la tienda.'),
    17: (None, 'Gasta un total de P$500.000 en la tienda.'),
    18: (None, 'Mira repeticiones desde Puntuación.'),
    19: (None, 'Completa la Base de datos.'),
    20: (None, 'Una actuación terminó prematuramente.'),
    21: (None, 'Completa 1 pista en Free Dance.'),
    22: (None, 'Completa todas las pistas en Free Dance.'),
    23: (None, 'Realiza un combo completo en una pista.'),
    24: (None, 'Consigue el máximo rango (KING CRAZY) en una pista.'),
    25: (None, 'Completa una pista sin MISSes.'),
    26: (None, 'Completa una pista en HARD o superior con más del 50% de PERFECT.'),
    27: (None, 'Supera tu propia puntuación más alta.'),
    28: (None, 'Cambia de traje y empieza a bailar.'),
    29: (None, 'Equipa un accesorio y empieza a bailar.'),
    30: (None, 'Completa una pista con cada personaje con el traje cambiado.'),
    31: (None, 'Baila usando 2 o más objetos que aumenten la dificultad.'),
    32: (None, 'Activa un Bond Fever al menos una vez.'),
    33: (None, 'Activa 30 tipos de Bond Fever.'),
    34: (None, 'Activa todos los bailes y Bond Fevers con Nanako.'),
    35: (None, 'Empareja a Rise y Kanami en Free Dance.'),
    36: (None, 'Mira todos los Bond Fevers entre Yu y Rise.'),
    37: (None, 'Mira todos los Bond Fevers entre Yukiko y Chie.'),
    38: (None, 'Mira todos los Bond Fevers entre Yosuke y Teddie.'),
    39: (None, 'Mira todos los Bond Fevers entre Naoto y Kanji.'),
}
TRANSLATIONS = {'es': SPANISH}


def text(trophy, language=None):
    """(name, description) of a trophy in `language` ('es', ...) when there is a translation,
    else in English."""
    _id, _grade, name, description = BY_ID[trophy]
    local = TRANSLATIONS.get((language or '')[:2], {}).get(trophy)
    if local:
        return local[0] or name, local[1] or description
    return name, description


def read_log(path):
    """{id: earliest unlock time} from a trophies.log; broken lines are skipped."""
    unlocked = {}
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return unlocked
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            trophy, when = int(parts[0]), int(parts[1])
            if trophy in BY_ID and (trophy not in unlocked or when < unlocked[trophy]):
                unlocked[trophy] = when
    return unlocked


def trp_files(path):
    """{name: bytes} of the unencrypted entries (the PNG icons) of a trophy00.trp."""
    data = Path(path).read_bytes()
    magic, _version, _size, count, entry_size = struct.unpack_from('>IIQII', data, 0)
    if magic != 0xDCA24D00 or entry_size < 52:
        raise ValueError('not a trophy file')
    files = {}
    for i in range(count):
        at = 0x60 + i * entry_size
        name = data[at:at + 32].split(b'\0')[0].decode('ascii', 'replace')
        offset, size, flags = struct.unpack_from('>QQI', data, at + 32)
        blob = data[offset:offset + size]
        if blob[:4] == b'\x89PNG':
            files[name] = blob
    return files


def icon_name(trophy):
    return f'TROP{trophy:03d}.PNG'


def prepare(game, out, language=None):
    """trophies.txt for the runtime (names in `language` when translated) and the icons, from the
    game's trophy file when it has one."""
    out = Path(out)
    folder = out / 'trophies'
    folder.mkdir(parents=True, exist_ok=True)
    icons = 0
    trp = Path(game) / 'sce_sys' / 'trophy' / 'trophy00.trp'
    try:
        for name, blob in trp_files(trp).items():
            target = folder / name
            if not target.is_file() or target.stat().st_size != len(blob):
                target.write_bytes(blob)
            icons += name.startswith('TROP')
    except (OSError, ValueError, struct.error) as error:
        print(f'Trophies: no icons ({error})')
    lines = [f'{trophy}\t{grade}\t{text(trophy, language)[0]}' for trophy, grade, _name, _text in TROPHIES]
    (out / 'trophies.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'Trophies: {len(TROPHIES)} known, {icons} icons')
    return out / 'trophies.txt', folder


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--language', default=None)
    args = parser.parse_args()
    prepare(args.game, args.out, args.language)
