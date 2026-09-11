import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('run_fuzz.py')
SPEC = importlib.util.spec_from_file_location('run_fuzz', SCRIPT)
run_fuzz = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_fuzz)


class RunnerTests(unittest.TestCase):
    def test_all_targets_run_in_parallel_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fuzz_dir = build / 'fuzz'
            fuzz_dir.mkdir()
            suffix = '.exe' if run_fuzz.os.name == 'nt' else ''
            for target in ('vtparse', 'termstate'):
                (fuzz_dir / ('fuzz_' + target + suffix)).touch()

            active = 0
            peak = 0
            lock = threading.Lock()
            rendezvous = threading.Barrier(2)

            def run(command, **kwargs):
                nonlocal active, peak
                with lock:
                    active += 1
                    peak = max(peak, active)
                try:
                    rendezvous.wait(timeout=0.25)
                except threading.BrokenBarrierError:
                    pass
                finally:
                    with lock:
                        active -= 1
                return subprocess.CompletedProcess(command, 0)

            argv = ['run_fuzz.py', '--build', str(build), '--seconds', '1']
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(run_fuzz.subprocess, 'run', side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = run_fuzz.main()

            self.assertEqual(0, result)
            self.assertEqual(2, peak)

    def test_jobs_limits_parallel_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fuzz_dir = build / 'fuzz'
            fuzz_dir.mkdir()
            suffix = '.exe' if run_fuzz.os.name == 'nt' else ''
            for target in ('vtparse', 'termstate'):
                (fuzz_dir / ('fuzz_' + target + suffix)).touch()

            active = 0
            peak = 0
            lock = threading.Lock()
            rendezvous = threading.Barrier(2)

            def run(command, **kwargs):
                nonlocal active, peak
                with lock:
                    active += 1
                    peak = max(peak, active)
                try:
                    rendezvous.wait(timeout=0.5)
                except threading.BrokenBarrierError:
                    pass
                finally:
                    with lock:
                        active -= 1
                return subprocess.CompletedProcess(command, 0)

            argv = ['run_fuzz.py', '--build', str(build), '--seconds', '1',
                    '--jobs', '1']
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(run_fuzz.subprocess, 'run', side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = run_fuzz.main()

            self.assertEqual(0, result)
            self.assertEqual(1, peak)

    def test_memory_budget_reduces_parallel_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fuzz_dir = build / 'fuzz'
            fuzz_dir.mkdir()
            suffix = '.exe' if run_fuzz.os.name == 'nt' else ''
            for target in ('vtparse', 'termstate'):
                (fuzz_dir / ('fuzz_' + target + suffix)).touch()

            active = 0
            peak = 0
            lock = threading.Lock()
            rendezvous = threading.Barrier(2)

            def run(command, **kwargs):
                nonlocal active, peak
                with lock:
                    active += 1
                    peak = max(peak, active)
                try:
                    rendezvous.wait(timeout=0.5)
                except threading.BrokenBarrierError:
                    pass
                finally:
                    with lock:
                        active -= 1
                return subprocess.CompletedProcess(command, 0)

            argv = ['run_fuzz.py', '--build', str(build), '--seconds', '1',
                    '--jobs', '2', '--rss-limit-mb', '1024',
                    '--memory-budget-mb', '1024']
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(run_fuzz.subprocess, 'run', side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = run_fuzz.main()

            self.assertEqual(0, result)
            self.assertEqual(1, peak)

    def test_shards_use_independent_seeds_and_output_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fuzz_dir = build / 'fuzz'
            fuzz_dir.mkdir()
            suffix = '.exe' if run_fuzz.os.name == 'nt' else ''
            (fuzz_dir / ('fuzz_vtparse' + suffix)).touch()

            calls = []
            lock = threading.Lock()
            rendezvous = threading.Barrier(2)

            def run(command, **kwargs):
                with lock:
                    calls.append((command, kwargs['stdout'].name))
                try:
                    rendezvous.wait(timeout=0.25)
                except threading.BrokenBarrierError:
                    pass
                return subprocess.CompletedProcess(command, 0)

            argv = ['run_fuzz.py', '--build', str(build), '--target', 'vtparse',
                    '--seconds', '1', '--seed', '20', '--shards', '2',
                    '--rss-limit-mb', '512', '--memory-budget-mb', '1024']
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(run_fuzz.subprocess, 'run', side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = run_fuzz.main()

            self.assertEqual(0, result)
            self.assertEqual({'-seed=20', '-seed=21'}, {
                next(arg for arg in command if arg.startswith('-seed='))
                for command, _ in calls
            })
            self.assertEqual(2, len({
                next(arg for arg in command if arg.startswith('-artifact_prefix='))
                for command, _ in calls
            }))
            self.assertEqual(2, len({command[-2] for command, _ in calls}))
            self.assertEqual(2, len({log for _, log in calls}))
            self.assertTrue(all('-rss_limit_mb=512' in command for command, _ in calls))

    def test_replay_and_minimize_do_not_create_shard_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            fuzz_dir = build / 'fuzz'
            fuzz_dir.mkdir()
            suffix = '.exe' if run_fuzz.os.name == 'nt' else ''
            (fuzz_dir / ('fuzz_vtparse' + suffix)).touch()
            failing_input = build / 'crash.vt'
            failing_input.touch()

            for operation in ('--replay', '--minimize'):
                calls = []

                def run(command, **kwargs):
                    calls.append(command)
                    return subprocess.CompletedProcess(command, 0)

                argv = ['run_fuzz.py', '--build', str(build), '--target', 'vtparse',
                        '--seconds', '1', '--jobs', '4', '--shards', '3',
                        operation, str(failing_input)]
                with self.subTest(operation=operation), \
                        mock.patch.object(sys, 'argv', argv), \
                        mock.patch.object(run_fuzz.subprocess, 'run', side_effect=run), \
                        contextlib.redirect_stdout(io.StringIO()):
                    result = run_fuzz.main()

                self.assertEqual(0, result)
                self.assertEqual(1, len(calls))
                self.assertFalse(any(arg.startswith('-seed=') for arg in calls[0]))


if __name__ == '__main__':
    unittest.main()
