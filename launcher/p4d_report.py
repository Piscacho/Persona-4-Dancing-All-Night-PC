# SPDX-License-Identifier: GPL-2.0-or-later
"""Problem reports from the launcher: what the player answers, the PC and the game copy, the
port's settings and the last run's log, in one .zip to attach to a GitHub issue.

Nothing of the game ships in it (no game files, no saves) and paths under the user's profile are
written as %USERPROFILE%, so the report holds no user name."""
import ctypes
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
import urllib.parse
import zipfile

WINDOWS = sys.platform == 'win32'
NO_WINDOW = 0x08000000 if WINDOWS else 0
LOG_LINES = 4000  # the end of last_run.log: where a crash shows

# The answers (stored by key; the report is read by the developer in Spanish).
PROBLEMS = [('crash', 'The game closes (crash)'), ('freeze', 'The game freezes or the screen stays black'),
            ('graphics', 'Graphics look wrong'), ('audio', 'Audio problem'),
            ('controls', 'Controls or gamepad'), ('performance', 'Low FPS or stutter'),
            ('launcher', 'Launcher problem'), ('other', 'Something else')]
MOMENTS = [('launcher', 'When the launcher opens'), ('play', 'When pressing PLAY'),
           ('loading', 'On the logos or while loading'), ('menu', 'In a menu'), ('song', 'During a song'),
           ('trophy', 'When a trophy is earned'), ('saving', 'When saving'), ('other', 'Other moment')]
FREQUENCIES = [('always', 'Every time'), ('sometimes', 'Sometimes'), ('once', 'Only once')]
SPANISH = {
    'crash': 'El juego se cierra (crash)', 'freeze': 'Se congela o se queda en negro',
    'graphics': 'Problema gráfico', 'audio': 'Problema de audio', 'controls': 'Controles o mando',
    'performance': 'FPS bajos o tirones', 'launcher': 'Problema del launcher', 'other': 'Otro',
    'play': 'Al pulsar JUGAR', 'loading': 'En los logos o la carga', 'menu': 'En un menú',
    'song': 'Durante una canción', 'trophy': 'Al ganar un trofeo', 'saving': 'Al guardar',
    'always': 'Siempre', 'sometimes': 'A veces', 'once': 'Solo una vez',
}
MOMENT_SPANISH = {'launcher': 'Al abrir el launcher', 'other': 'Otro momento'}


def anonymize(text):
    """The profile folder and the user name replaced, wherever they appear."""
    home = str(Path.home())
    name = os.environ.get('USERNAME') or os.environ.get('USER') or ''
    for variant in {home, home.replace('\\', '/'), home.replace('/', '\\')}:
        text = re.sub(re.escape(variant), '%USERPROFILE%', text, flags=re.IGNORECASE)
    if len(name) >= 3:
        text = re.sub(r'(?<![A-Za-z0-9])' + re.escape(name) + r'(?![A-Za-z0-9])', '<user>', text, flags=re.IGNORECASE)
    return text


def windows_version():
    if not WINDOWS:
        return platform.platform()
    build = sys.getwindowsversion().build
    edition = platform.win32_edition() if hasattr(platform, 'win32_edition') else ''
    return f"Windows {'11' if build >= 22000 else platform.release()} {edition} (build {build})".replace('  ', ' ')


def processor():
    if WINDOWS:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
                return winreg.QueryValueEx(key, 'ProcessorNameString')[0].strip()
        except OSError:
            pass
    return platform.processor() or '?'


def memory_gb():
    if not WINDOWS:
        try:
            return round(os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / 2 ** 30, 1)
        except (ValueError, OSError, AttributeError):
            return None

    class MemoryStatus(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('available', ctypes.c_ulonglong), ('page_total', ctypes.c_ulonglong),
                    ('page_available', ctypes.c_ulonglong), ('virtual_total', ctypes.c_ulonglong),
                    ('virtual_available', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    status = MemoryStatus(length=ctypes.sizeof(MemoryStatus))
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return round(status.total / 2 ** 30, 1)
    return None


def graphics_cards(vulkan_gpus=()):
    """[(name, driver version)]: Windows' video controllers (the driver version comes from there),
    else the Vulkan list the launcher already has."""
    cards = []
    if WINDOWS:
        try:
            done = subprocess.run(['powershell', '-NoProfile', '-Command',
                                   'Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | '
                                   'ConvertTo-Json -Compress'],
                                  capture_output=True, text=True, timeout=15, creationflags=NO_WINDOW)
            data = json.loads(done.stdout or '[]')
            for item in data if isinstance(data, list) else [data]:
                if item.get('Name'):
                    cards.append((item['Name'].strip(), (item.get('DriverVersion') or '?').strip()))
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    if not cards:
        cards = [(row[1], '?') for row in vulkan_gpus if len(row) > 1]
    return cards


def last_lines(path, count=LOG_LINES):
    try:
        lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return ''
    return '\n'.join(lines[-count:])


def report_markdown(answers, info):
    """The report as Markdown: the answers first, then the detected facts."""
    moment = MOMENT_SPANISH.get(answers['moment']) or SPANISH.get(answers['moment'], answers['moment'])
    lines = [f"## Problema: {SPANISH.get(answers['problem'], answers['problem'])}", '',
             f"- **Cuándo:** {moment}",
             f"- **¿Se repite?:** {SPANISH.get(answers['frequency'], answers['frequency'])}"]
    if answers.get('contact'):
        lines.append(f"- **Contacto:** {answers['contact']}")
    lines += ['', '### Descripción', '', answers.get('description') or '(sin descripción)', '',
              '### Datos detectados', '']
    for label, value in info:
        lines.append(f'- **{label}:** {value}')
    return '\n'.join(lines) + '\n'


def write_report(folder, stem, answers, info, extra_files):
    """report-<stem>-<date>.zip in `folder` with report.md, info.json and `extra_files`
    ({name in the zip: text}); returns its path."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"report-{stem}-{time.strftime('%Y%m%d-%H%M%S')}.zip"
    body = anonymize(report_markdown(answers, info))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('report.md', body)
        archive.writestr('info.json', anonymize(json.dumps({'answers': answers, 'info': dict(info)},
                                                           ensure_ascii=False, indent=2)))
        for name, text in extra_files.items():
            if text:
                archive.writestr(name, anonymize(text))
    return path


def issue_url(repository_page, answers, info, title_prefix):
    """A GitHub "new issue" link filled with the answers and the detected facts (the zip is attached
    by hand: GitHub takes no files through the link)."""
    match = re.match(r'(https://github\.com/[^/]+/[^/]+)', repository_page or '')
    if not match:
        return None
    body = anonymize(report_markdown(answers, info)) + \
        '\n> Adjunta aquí el archivo `.zip` del informe (arrástralo a este cuadro).\n'
    title = f"{title_prefix}: {SPANISH.get(answers['problem'], answers['problem'])}"
    query = urllib.parse.urlencode({'title': title, 'body': body[:6000]})
    return f'{match.group(1)}/issues/new?{query}'
