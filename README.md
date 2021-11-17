# Service config manager

The service config manager provides a D-Bus interface to manage BMC services
as described by the [service management D-Bus interfaces][].

The configuration settings are intended to persist across BMC reboots.

An example use case for this service is BMCWeb's implementation of the Redfish
NetworkProtocol schema.

[service management D-Bus interfaces]: https://github.com/openbmc/phosphor-dbus-interfaces/tree/master/yaml/xyz/openbmc_project/Control/Service
[BMCWeb's implementation of the Redfish NetworkProtocol schema]: https://github.com/openbmc/bmcweb/blob/master/redfish-core/lib/network_protocol.hpp

## Design

Implementation details are described in the [D-Bus interface README].

The service config manager generally makes configuration changes to `systemd` units via D-Bus interfaces.

The design pattern to add new services or controls is:
- Determine if the service you want to control is socket activated.
- To control the `Running` and `Enabled` properties of a service:
   - For a service which uses socket activation, control the socket.
   - For other services, control the service unit itself.

[D-Bus interface README]: https://github.com/openbmc/phosphor-dbus-interfaces/blob/master/yaml/xyz/openbmc_project/Control/Service/README.md
