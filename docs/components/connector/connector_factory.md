# ConnectorFactory

`ConnectorFactory` is a global registry and dynamic constructor for `ExchangeConnector` instances, used to instantiate exchange adapters based on type and symbol at runtime.

```cpp
class ConnectorFactory {
public:
  using CreatorFunc = std::move_only_function<std::shared_ptr<ExchangeConnector>(const std::string&)>;

  static ConnectorFactory& instance();
  void registerConnector(const std::string& type, CreatorFunc creator);
  std::shared_ptr<ExchangeConnector> createConnector(const std::string& type, const std::string& symbol);

private:
  std::unordered_map<std::string, CreatorFunc> _creators;
};
```

## Purpose

* Enable pluggable, type-based instantiation of exchange connectors without static bindings.

## Responsibilities

| Aspect       | Details                                                                  |
| ------------ | ------------------------------------------------------------------------ |
| Registration | Maps string identifiers (e.g. `"bybit"`, `"mock"`) to creator functions. |
| Construction | Calls registered factory to produce a connector for a given `symbol`.    |
| Lifetime     | Singleton pattern via `instance()`; all connectors share same registry.  |

## Notes

* Uses `std::move_only_function` to avoid overhead of `std::function` and enable capture of ownership semantics.
* Supports dynamic module systems or runtime configuration of connector types.
* Not thread-safe by default — external synchronization may be required during registration.
