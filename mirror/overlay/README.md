# venue-core

Deterministic execution venue core: sequenced journal, checkpoints, matching
with last look, FIX perimeter.

This is a read-only mirror. Contributions go to [FLOX-Foundation/flox](https://github.com/FLOX-Foundation/flox).

## Building

```bash
cmake --preset venue-lite && cmake --build build-venue-lite && ctest --test-dir build-venue-lite
```

## License

MIT. See [LICENSE](LICENSE).
