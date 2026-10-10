# flox-py (deprecated)

`flox-py` is renamed to [`lrvx`](https://pypi.org/project/lrvx/). This
package is deprecated and receives no further development.

Installing it installs `lrvx`. `import flox_py` keeps working: it re-exports
everything `lrvx` exports, `flox_py.<module>` is the same module as
`lrvx.<module>`, and the `flox` command runs `lrvx`. Each of them says it
is deprecated.

Move to the new name:

```bash
pip uninstall flox-py
pip install lrvx
```

and replace `import flox_py` (or `import flox_py as lrvx`) with
`import lrvx`. The migration guide is at
https://docs.lrvx.dev/how-to/migrate-from-flox/.
