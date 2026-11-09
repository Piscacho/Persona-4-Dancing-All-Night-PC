"""Linux AppImage updates: the launcher downloads the release's AppImage and swaps the running file."""
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from test_launcher_controls import launcher

RELEASE = {'tag_name': 'windows-v9.9.9', 'html_url': 'https://example.invalid/release',
           'assets': [{'name': 'p4d-x86_64.AppImage', 'browser_download_url': 'https://example.invalid/a.AppImage'},
                      {'name': 'Port-v9.9.9.zip', 'browser_download_url': 'https://example.invalid/b.zip'}]}


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def release_response(*args, **kwargs):
    return FakeResponse(json.dumps(RELEASE).encode())


class AppImageUpdateTest(unittest.TestCase):
    def latest(self, windows, appimage):
        env = {'APPIMAGE': appimage} if appimage else {}
        with mock.patch.object(launcher, 'WINDOWS', windows), mock.patch.dict(os.environ, env, clear=False), \
                mock.patch.object(launcher, 'RELEASES_API', 'https://example.invalid/api'), \
                mock.patch.object(launcher.urllib.request, 'urlopen', release_response):
            if not appimage:
                os.environ.pop('APPIMAGE', None)
            return launcher.latest_release()

    def test_appimage_takes_the_appimage_asset(self):
        self.assertEqual(self.latest(False, '/home/deck/p4d.AppImage'),
                         ('9.9.9', 'https://example.invalid/a.AppImage', 'https://example.invalid/release'))

    def test_windows_and_plain_linux_take_the_zip(self):
        self.assertEqual(self.latest(True, '')[1], 'https://example.invalid/b.zip')
        self.assertEqual(self.latest(False, '')[1], 'https://example.invalid/b.zip')

    def test_replace_swaps_the_file_and_makes_it_executable(self):
        with tempfile.TemporaryDirectory() as folder:
            old, new = Path(folder, 'p4d.AppImage'), Path(folder, 'p4d.AppImage.new')
            old.write_bytes(b'\x7fELF old')
            new.write_bytes(b'\x7fELF new')
            launcher.replace_appimage(str(old), new)
            self.assertEqual(old.read_bytes(), b'\x7fELF new')
            self.assertFalse(new.exists())
            if os.name != 'nt':
                self.assertTrue(os.access(old, os.X_OK))

    def test_replace_refuses_a_download_that_is_not_an_appimage(self):
        with tempfile.TemporaryDirectory() as folder:
            old, new = Path(folder, 'p4d.AppImage'), Path(folder, 'p4d.AppImage.new')
            old.write_bytes(b'\x7fELF old')
            new.write_bytes(b'<html>error page</html>')
            with self.assertRaises(OSError):
                launcher.replace_appimage(str(old), new)
            self.assertEqual(old.read_bytes(), b'\x7fELF old')


if __name__ == '__main__':
    unittest.main()
