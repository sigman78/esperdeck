"""Run native libFuzzer targets, keeping mutations and crash artifacts in the build tree."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--config', default='RelWithDebInfo')
    parser.add_argument('--target', choices=('vtparse', 'termstate', 'all'), default='all')
    parser.add_argument('--seconds', type=int, default=60,
                        help='Time per target and shard')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--jobs', type=int, default=2,
                        help='Maximum simultaneous fuzz processes')
    parser.add_argument('--shards', type=int, default=1,
                        help='Independent fuzz campaigns per target')
    parser.add_argument('--rss-limit-mb', type=int, default=1024,
                        help='Per-process libFuzzer RSS limit')
    parser.add_argument('--memory-budget-mb', type=int, default=2048,
                        help='Aggregate memory budget for fuzz processes')
    parser.add_argument('--replay', type=Path, help='Replay one saved input')
    parser.add_argument('--minimize', type=Path, help='Minimize one failing input')
    args = parser.parse_args()
    if min(args.seconds, args.jobs, args.shards, args.rss_limit_mb,
           args.memory_budget_mb) < 1:
        parser.error('Seconds, jobs, shards, RSS limit, and memory budget must be positive')
    if args.memory_budget_mb < args.rss_limit_mb:
        parser.error('--memory-budget-mb must be at least --rss-limit-mb')
    if args.replay and args.minimize:
        parser.error('Use at most one of --replay/--minimize')
    if (args.replay or args.minimize) and args.target == 'all':
        parser.error('Replay/minimize requires a specific --target')
    build = args.build.resolve()
    targets = ('vtparse', 'termstate') if args.target == 'all' else (args.target,)
    executables = {}
    for target in targets:
        filename = 'fuzz_' + target + ('.exe' if os.name == 'nt' else '')
        candidates = (build / 'fuzz' / filename, build / 'fuzz' / args.config / filename)
        exe = next((p for p in candidates if p.is_file()), None)
        if exe is None:
            parser.error(f'{filename} missing under {build}; build with TSM_BUILD_FUZZERS=ON')
        executables[target] = exe

    fuzzing = not (args.replay or args.minimize)
    tasks = ([(target, shard) for target in targets for shard in range(args.shards)]
             if fuzzing else [(target, None) for target in targets])

    def run_target(task):
        target, shard = task
        exe = executables[target]
        work = build / 'fuzz-results' / target
        if shard is not None:
            work /= f'shard-{shard + 1}'
        artifacts = work / 'artifacts'
        corpus = work / 'corpus'
        artifacts.mkdir(parents=True, exist_ok=True)
        corpus.mkdir(exist_ok=True)
        command = [str(exe), '-max_len=65536', '-timeout=10',
                   f'-rss_limit_mb={args.rss_limit_mb}',
                   '-print_final_stats=1', '-artifact_prefix=' + artifacts.as_posix() + '/']
        if args.replay:
            command += [str(args.replay.resolve())]
        elif args.minimize:
            command += ['-minimize_crash=1', f'-max_total_time={args.seconds}',
                        '-exact_artifact_path=' + str(artifacts / 'minimized.vt'),
                        str(args.minimize.resolve())]
        else:
            command += [f'-max_total_time={args.seconds}', f'-seed={args.seed + shard}',
                        '-dict=' + str(ROOT / 'vt.dict'), str(corpus), str(ROOT / 'corpus')]
        log = work / ('replay.log' if args.replay else 'minimize.log' if args.minimize else 'fuzz.log')
        label = target if shard is None else f'{target}/shard-{shard + 1}'
        print(f'{label}: starting; log: {log}', flush=True)
        with log.open('wb') as output:
            try:
                result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                        timeout=args.seconds + 60)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
                output.write(b'\nRunner wall-clock timeout exceeded.\n')
        print(f'{label}: exit {code}; artifacts: {artifacts}', flush=True)
        return code

    max_workers = (min(args.jobs, args.memory_budget_mb // args.rss_limit_mb)
                   if fuzzing else 1)
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        codes = executor.map(run_target, tasks)
        return int(any(code != 0 for code in codes))


if __name__ == '__main__':
    sys.exit(main())
