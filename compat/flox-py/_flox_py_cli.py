"""The ``flox`` console script: says it is renamed, then runs ``lrvx``."""
import sys


def main(argv=None):
    print("flox-py is renamed to lrvx; install lrvx and run `lrvx` "
          "instead of `flox`", file=sys.stderr)
    from lrvx.cli import main as lrvx_main
    return lrvx_main(argv)


if __name__ == "__main__":
    sys.exit(main())
