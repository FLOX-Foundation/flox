# IWebSocketClient

`IWebSocketClient` defines a minimal interface for managing WebSocket connections, including lifecycle control and message handling via callbacks.

```cpp
class IWebSocketClient : public ISubsystem {
public:
  virtual ~IWebSocketClient() = default;

  virtual void onOpen(std::move_only_function<void()> cb) = 0;
  virtual void onMessage(std::move_only_function<void(std::string_view)> cb) = 0;
  virtual void onClose(std::move_only_function<void(int, std::string_view)> cb) = 0;

  virtual void send(const std::string& data) = 0;
};
```

## Purpose

* Provide an abstraction for WebSocket communication used by exchange connectors and other components.
* Enable pluggable transport implementations with consistent interface.

## Responsibilities

| Method        | Description                                                                                               |
| ------------- | ----------------------------------------------------------------------------------------------------------|
| `onOpen()`    | Registers a callback to be invoked when the connection is successfully opened.                            |
| `onMessage()` | Registers a callback for receiving incoming text messages.                                                |
| `onClose()`   | Registers a callback to handle disconnection events with code and reason.                                 |
| `send()`      | Sends a text message over the active WebSocket connection.                                                |
| `start()`     | Initiates the WebSocket connection and starts event processing (inherited from ISubsystem)                |
| `stop()`      | Gracefully closes the connection and stops background processing (inherited from ISubsystem)              |
