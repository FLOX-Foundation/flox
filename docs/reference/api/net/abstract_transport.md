# ITransport

`ITransport` defines an abstract interface for sending asynchronous HTTP POST requests, allowing components to perform remote communication with external services.

```cpp
class ITransport {
public:
  virtual ~ITransport() = default;

  virtual void post(
    std::string_view url,
    std::string_view body,
    const std::vector<std::pair<std::string_view, std::string_view>>& headers,
    std::move_only_function<void(std::string_view)> onSuccess,
    std::move_only_function<void(std::string_view)> onError) = 0;
};
```

## Purpose

* Provide a generic mechanism to send HTTP POST requests without coupling to a specific transport library or implementation.
* Enable integration with APIs, webhooks, or external risk/configuration services.

## Responsibilities

| Method   | Description                                                                                                                               |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `post()` | Sends a POST request to the specified URL with custom headers and body. Invokes success or error callback based on result. |
