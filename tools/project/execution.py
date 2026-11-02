"""Command execution and stage logs shared by host workflows."""
import contextlib
import importlib
import os
from pathlib import Path
import runpy
import subprocess
import sys


def invoke(module_name: str, arguments: list[str]) -> None:
    previous = sys.argv
    module = importlib.import_module(module_name)
    sys.argv = [module.__file__, *map(str, arguments)]
    try:
        if hasattr(module, 'main'):
            result = module.main()
            if isinstance(result, int) and result:
                raise RuntimeError(f'{module_name} exited with status {result}')
        else:
            runpy.run_module(module_name, run_name='__main__')
    except SystemExit as error:
        if error.code:
            raise RuntimeError(f'{module_name} exited with status {error.code}') from error
    finally:
        sys.argv = previous


@contextlib.contextmanager
def stage(config: dict, name: str):
    path = Path(config['workspace']) / 'logs' / (name + '.log')
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f'[{name}] {path}', flush=True)
    sys.stdout.flush()
    sys.stderr.flush()
    descriptors = os.dup(1), os.dup(2)
    try:
        with path.open('a') as log:
            os.dup2(log.fileno(), 1)
            os.dup2(log.fileno(), 2)
            print(f'command: {sys.argv!r}', flush=True)
            yield
    except BaseException:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(descriptors[0], 1)
        os.dup2(descriptors[1], 2)
        print('\n'.join(path.read_text(errors='replace').splitlines()[-40:]), file=sys.stderr)
        print(f'Failed stage: {name}; log: {path}', file=sys.stderr)
        raise
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        os.dup2(descriptors[0], 1)
        os.dup2(descriptors[1], 2)
        for descriptor in descriptors:
            os.close(descriptor)


def external(arguments, cwd=None):
    subprocess.run(list(map(str, arguments)), cwd=cwd, check=True)
