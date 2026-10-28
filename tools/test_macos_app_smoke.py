#!/usr/bin/env python3
"""Small native loader regressions. No game files, installed app or GPU used."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import smoke_macos_app as smoke


@unittest.skipUnless(sys.platform == 'darwin', 'requires the macOS dynamic loader')
class SmokeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='las-loader-fixture-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Fixture App.app'
        self.exe = self.app / 'Contents/MacOS/lasrecomp'
        self.lib = self.app / 'Contents/Frameworks/libanswer.dylib'
        self.exe.parent.mkdir(parents=True)
        self.lib.parent.mkdir()
        self.source = self.root / 'answer.c'
        self.library('int answer(void) { return 42; }')
        main = self.root / 'main.c'
        main.write_text('#include <stdio.h>\nint answer(void);\nint main(void) {\n'
                        'if (answer() != 42) return 1;\n'
                        'puts("' + smoke.GUARD + '"); return 3; }\n')
        subprocess.run(['clang', str(main), str(self.lib), '-Wl,-rpath,@executable_path/../Frameworks',
                        '-o', str(self.exe)], check=True, capture_output=True)

    def library(self, code):
        self.source.write_text(code)
        subprocess.run(['clang', '-dynamiclib', str(self.source), '-install_name', '@rpath/libanswer.dylib',
                        '-o', str(self.lib)], check=True, capture_output=True)

    def run_check(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            smoke.check(self.app)
        return output.getvalue()

    def test_matching_library_launches(self):
        self.assertIn('dynamic-library loading passed', self.run_check())

    def test_missing_library_fails(self):
        self.lib.unlink()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(RuntimeError):
            smoke.check(self.app)
        self.assertIn('Library not loaded', output.getvalue())

    def test_missing_symbol_fails(self):
        self.library('int other(void) { return 42; }')
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(RuntimeError):
            smoke.check(self.app)
        self.assertIn('symbol not found', output.getvalue().lower())

    def test_exit_without_guard_fails(self):
        with patch.object(smoke.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '')):
            with self.assertRaises(RuntimeError):
                self.run_check()

    def test_hung_startup_times_out(self):
        with patch.object(smoke.subprocess, 'run', side_effect=subprocess.TimeoutExpired('fixture', 20)):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.run_check()

    def test_isolation_and_redirected_log(self):
        def run(*args, **kwargs):
            env = kwargs['env']
            self.assertNotIn('DYLD_LIBRARY_PATH', env)
            self.assertNotIn('LARECOMP_INPUT', env)
            self.assertNotIn('LASRECOMP_INFO_LOGGED', env)
            self.assertEqual(env['LARECOMP_NO_LAUNCHER'], '1')
            self.assertEqual(env['LARECOMP_FORCE_LAUNCHER'], '1')
            self.assertEqual(env['HOME'], kwargs['cwd'])
            self.assertEqual(env['LARECOMP_DATA_ROOT'], kwargs['cwd'])
            (Path(kwargs['cwd']) / 'lasrecomp.log').write_text(smoke.GUARD)
            return subprocess.CompletedProcess(args[0], 3, '')
        with patch.dict(os.environ, DYLD_LIBRARY_PATH='/bad', LARECOMP_INPUT='bad', LASRECOMP_INFO_LOGGED='1'):
            with patch.object(smoke.subprocess, 'run', side_effect=run):
                self.assertIn('passed', self.run_check())


if __name__ == '__main__':
    unittest.main()
