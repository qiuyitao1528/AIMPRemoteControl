#pragma once

#include <memory>

#include "pluginInfo.h"

class ListenerStatus;
class NetworkWatcher;
class SharedSettings;

// Announces the player over mDNS/DNS-SD, through the responder of the system, on the interfaces where the
// server listens.
class MdnsAnnouncer
{
public:
	static constexpr const char *ServiceType = PLUGIN_MDNS_SERVICE_TYPE;

	MdnsAnnouncer();
	~MdnsAnnouncer();

	MdnsAnnouncer(const MdnsAnnouncer &) = delete;
	MdnsAnnouncer &operator=(const MdnsAnnouncer &) = delete;

	// To call once the server has subscribed to the network: the watcher tells its subscribers in their order,
	// and this one then asks where the server could not listen.
	void Start(NetworkWatcher &network, const ListenerStatus &listeners, const SharedSettings &settings, unsigned short port);
	// The player's name and whether it asks for a login are announced: to call when the settings change.
	void Refresh();
	void Stop();

private:
	struct Impl;
	std::unique_ptr<Impl> FImpl;
};
