#pragma once

#include <string>
#include <vector>

struct NetworkAddress
{
	std::string InterfaceId;
	std::string InterfaceName;
	std::string Address;
	bool IsLoopback = false;
	// The number the system knows the interface by.
	unsigned InterfaceIndex = 0;
};

std::vector<NetworkAddress> ListIPv4Addresses();

std::string HostName();
