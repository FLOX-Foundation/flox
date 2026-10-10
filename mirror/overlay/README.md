# venue-core

Deterministic execution venue core: sequenced journal, checkpoints, matching
with last look, FIX perimeter.

This is a read-only mirror. Contributions go to [lrvx/lrvx](https://github.com/lrvx/lrvx).

## Building

```bash
cmake --preset venue-lite && cmake --build build-venue-lite && ctest --test-dir build-venue-lite
```

## License

MIT. See [LICENSE](LICENSE).
