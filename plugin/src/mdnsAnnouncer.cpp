#include "mdnsAnnouncer.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dnsSd.h"
#include "dnsSdResponder.h"
#include "listenerStatus.h"
#include "networkInterfaces.h"
#include "networkWatcher.h"
#include "pluginInfo.h"
#include "settings.h"

namespace
{
	constexpr auto RetryInterval = std::chrono::seconds(30);

	std::optional<DnsSdService> Describe(const Settings &settings, unsigned short port)
	{
		const std::string name = settings.EffectivePlayerName();
		std::string label = DnsSdInstanceLabel(name);
		if (label.empty())
			return std::nullopt;
		return DnsSdService{std::move(label),
							MdnsAnnouncer::ServiceType,
							port,
							{{"version", PLUGIN_VERSION_STRING},
							 {"auth", settings.Auth.IsActive() ? "1" : "0"},
							 // The label reaches the network changed: its dots replaced, and from Windows in lower
							 // case where it is not ASCII.
							 {"name", DnsSdTxtValue("name", name)}}};
	}

	// The interfaces through which another device reaches the server.
	std::set<unsigned> ServedInterfaces(const std::vector<NetworkAddress> &allowed, const std::set<std::string> &taken)
	{
		std::set<unsigned> interfaces;
		for (const NetworkAddress &a : allowed)
			if (!a.IsLoopback && a.InterfaceIndex != 0 && !taken.contains(a.Address))
				interfaces.insert(a.InterfaceIndex);
		return interfaces;
	}
}

struct MdnsAnnouncer::Impl
{
	const ListenerStatus *Listeners = nullptr;
	const SharedSettings *Settings = nullptr;
	unsigned short Port = 0;
	std::mutex Mutex;
	std::condition_variable WakeUp;
	bool Stopping = false;
	bool Changed = false;
	bool Lost = false;
	std::set<unsigned> Interfaces;
	std::thread Thread;

	// The change tells whether it was one: the watcher repeats itself every few seconds while the server cannot
	// take a port, and a wake-up ends the wait for another try.
	template <typename Change>
	void Wake(Change change)
	{
		{
			std::lock_guard lock(Mutex);
			if (!change())
				return;
			Changed = true;
		}
		WakeUp.notify_all();
	}

	bool StopRequested()
	{
		std::lock_guard lock(Mutex);
		return Stopping;
	}

	void Run()
	{
		DnsSdResponder responder([this]
								 { Wake([this]
										{ return Lost = true; }); });
		if (!responder.Supported())
			return;

		std::optional<DnsSdService> announced;
		std::map<unsigned, DnsSdResponder::Registration> registrations;
		bool retry = false;
		std::unique_lock lock(Mutex);
		for (;;)
		{
			const auto ready = [this]
			{ return Stopping || Changed; };
			if (retry)
				WakeUp.wait_for(lock, RetryInterval, ready);
			else
				WakeUp.wait(lock, ready);
			Changed = false;
			const bool stopping = Stopping;
			const bool lost = std::exchange(Lost, false);
			const std::set<unsigned> interfaces = stopping ? std::set<unsigned>() : Interfaces;
			lock.unlock();

			const std::optional<DnsSdService> wanted = interfaces.empty() ? std::nullopt : Describe(Settings->Get(), Port);
			for (auto it = registrations.begin(); it != registrations.end();)
			{
				if (!lost && wanted == announced && interfaces.contains(it->first))
				{
					++it;
					continue;
				}
				responder.Withdraw(it->second, it->first);
				it = registrations.erase(it);
			}
			announced = wanted;

			retry = false;
			if (wanted)
			{
				for (const unsigned index : interfaces)
				{
					if (registrations.contains(index))
						continue;
					// A registration takes about a second, and the player waits for this thread to close.
					if (StopRequested())
						break;
					if (const DnsSdResponder::Registration registration = responder.Publish(*wanted, index))
						registrations[index] = registration;
					else
						retry = true;
				}
			}
			if (stopping)
				return;
			lock.lock();
		}
	}
};

MdnsAnnouncer::MdnsAnnouncer() = default;
MdnsAnnouncer::~MdnsAnnouncer() { Stop(); }

void MdnsAnnouncer::Start(NetworkWatcher &network, const ListenerStatus &listeners, const SharedSettings &settings,
						  unsigned short port)
{
	if (FImpl)
		return;

	auto impl = std::make_unique<Impl>();
	impl->Listeners = &listeners;
	impl->Settings = &settings;
	impl->Port = port;
	Impl *p = impl.get();
	network.Subscribe([p](const std::vector<NetworkAddress> &allowed)
					  {
						  std::set<unsigned> interfaces = ServedInterfaces(allowed, p->Listeners->TakenAddresses());
						  p->Wake([&]
								  {
									  if (p->Interfaces == interfaces)
										  return false;
									  p->Interfaces = std::move(interfaces);
									  return true; });
						  return true; });
	impl->Thread = std::thread([p]()
							   { p->Run(); });

	FImpl = std::move(impl);
}

void MdnsAnnouncer::Refresh()
{
	if (FImpl)
		FImpl->Wake([]
					{ return true; });
}

void MdnsAnnouncer::Stop()
{
	if (!FImpl)
		return;

	FImpl->Wake([this]
				{ return FImpl->Stopping = true; });
	if (FImpl->Thread.joinable())
		FImpl->Thread.join();

	FImpl.reset();
}
