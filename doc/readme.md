### Describe:

* Run as server

    When constructs `Scadup` instance, you can initialize by call `Initialize(PORT)`. Then call `Start()` to start a server process.

* Run as client

    Call `Initialize(IP, PORT)` to initialize, then call `Connect()` to start a client.

* Run as broker

    Call `Initialize(PORT)` to initialize, then call `Broker()` to start a broker server proxy.

* Run as publisher

    Call `Initialize(IP, PORT)` to initialize, then call `Publisher(topic, payload)` to publish `payload` over `topic` to broker. IP/PORT is the broker ip/port, it is a short connection.

* Run as subscriber

    Call `Initialize(IP, PORT)` to initialize, then call `Subscriber(topic)` to run as subscriber. The `topic` is a mark to match the connection of publisher, so we can get message from the `topic`.

### Usage

* One example case: [Device2Device](https://github.com/tsymiar/Device2Device)
