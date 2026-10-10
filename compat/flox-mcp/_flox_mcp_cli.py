"""The ``flox-mcp`` console script: says it is renamed, then runs ``lrvx-mcp``."""
import sys


def main(argv=None):
    print("flox-mcp is renamed to lrvx-mcp; install lrvx-mcp and run "
          "`lrvx-mcp` instead of `flox-mcp`", file=sys.stderr)
    from lrvx_mcp.cli import main as lrvx_mcp_main
    return lrvx_mcp_main(argv)


if __name__ == "__main__":
    sys.exit(main())
