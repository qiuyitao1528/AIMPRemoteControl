#ifndef _WIN32

#include "dnsSdResponder.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include <dlfcn.h>

struct AvahiClient;
struct AvahiEntryGroup;
struct AvahiPoll;
struct AvahiStringList;
struct AvahiThreadedPoll;

namespace
{
	constexpr auto SystemTimeout = std::chrono::seconds(5);

	// As the headers of Avahi have them.
	constexpr int AvahiClientNoFail = 2;
	constexpr int AvahiClientRunning = 2;
	constexpr int AvahiClientFailure = 100;
	constexpr int AvahiGroupEstablished = 2;
	constexpr int AvahiGroupCollision = 3;
	constexpr int AvahiGroupFailure = 4;
	constexpr int AvahiProtoInet = 0;
	constexpr int AvahiErrorCollision = -8;

	// libavahi-client, the way to the Avahi daemon, taken at run time: the plugin loads where it is not
	// installed, and registers nothing there.
	struct AvahiApi
	{
		AvahiThreadedPoll *(*PollNew)() = nullptr;
		const AvahiPoll *(*PollGet)(AvahiThreadedPoll *) = nullptr;
		int (*PollStart)(AvahiThreadedPoll *) = nullptr;
		int (*PollStop)(AvahiThreadedPoll *) = nullptr;
		void (*PollFree)(AvahiThreadedPoll *) = nullptr;
		void (*PollLock)(AvahiThreadedPoll *) = nullptr;
		void (*PollUnlock)(AvahiThreadedPoll *) = nullptr;
		AvahiStringList *(*ListAdd)(AvahiStringList *, const char *) = nullptr;
		void (*ListFree)(AvahiStringList *) = nullptr;
		char *(*AlternativeName)(const char *) = nullptr;
		void (*Free)(void *) = nullptr;
		AvahiClient *(*ClientNew)(const AvahiPoll *, int, void (*)(AvahiClient *, int, void *), void *, int *) = nullptr;
		void (*ClientFree)(AvahiClient *) = nullptr;
		AvahiEntryGroup *(*GroupNew)(AvahiClient *, void (*)(AvahiEntryGroup *, int, void *), void *) = nullptr;
		int (*GroupAddService)(AvahiEntryGroup *, int, int, int, const char *, const char *, const char *, const char *,
							   std::uint16_t, AvahiStringList *) = nullptr;
		int (*GroupCommit)(AvahiEntryGroup *) = nullptr;
		int (*GroupFree)(AvahiEntryGroup *) = nullptr;
		bool Complete = false;

		// Taken once and kept.
		static const AvahiApi &Get()
		{
			static const AvahiApi api;
			return api;
		}

	private:
		AvahiApi()
		{
			void *common = dlopen("libavahi-common.so.3", RTLD_NOW | RTLD_LOCAL);
			void *client = dlopen("libavahi-client.so.3", RTLD_NOW | RTLD_LOCAL);
			if (!common || !client)
				return;
			Complete = Take(common, "avahi_threaded_poll_new", PollNew) && Take(common, "avahi_threaded_poll_get", PollGet) &&
					   Take(common, "avahi_threaded_poll_start", PollStart) && Take(common, "avahi_threaded_poll_stop", PollStop) &&
					   Take(common, "avahi_threaded_poll_free", PollFree) && Take(common, "avahi_threaded_poll_lock", PollLock) &&
					   Take(common, "avahi_threaded_poll_unlock", PollUnlock) && Take(common, "avahi_string_list_add", ListAdd) &&
					   Take(common, "avahi_string_list_free", ListFree) &&
					   Take(common, "avahi_alternative_service_name", AlternativeName) && Take(common, "avahi_free", Free) &&
					   Take(client, "avahi_client_new", ClientNew) && Take(client, "avahi_client_free", ClientFree) &&
					   Take(client, "avahi_entry_group_new", GroupNew) &&
					   Take(client, "avahi_entry_group_add_service_strlst", GroupAddService) &&
					   Take(client, "avahi_entry_group_commit", GroupCommit) && Take(client, "avahi_entry_group_free", GroupFree);
		}

		template <typename Function>
		static bool Take(void *library, const char *name, Function &function)
		{
			function = reinterpret_cast<Function>(dlsym(library, name));
			return function != nullptr;
		}
	};
}

// The library calls back on a thread of its own, which holds the library's lock then; every call into the
// library from here takes that lock.
struct DnsSdResponder::Impl
{
	const AvahiApi &Api = AvahiApi::Get();
	const std::function<void()> Lost;
	const std::thread::id Owner = std::this_thread::get_id();
	AvahiThreadedPoll *Poll = nullptr;
	AvahiClient *Client = nullptr;
	std::mutex Mutex;
	std::condition_variable Changed;
	int ClientState = 0;
	std::map<AvahiEntryGroup *, int> Groups;

	explicit Impl(std::function<void()> lost) : Lost(std::move(lost)) {}

	~Impl()
	{
		if (!Poll)
			return;
		Api.PollStop(Poll);
		if (Client)
			Api.ClientFree(Client);
		Api.PollFree(Poll);
	}

	struct Locked
	{
		const Impl &Held;

		explicit Locked(const Impl &impl) : Held(impl) { Held.Api.PollLock(Held.Poll); }
		~Locked() { Held.Api.PollUnlock(Held.Poll); }
	};

	bool Connect()
	{
		if (!Poll)
		{
			Poll = Api.PollNew();
			if (!Poll)
				return false;
			if (Api.PollStart(Poll) < 0)
			{
				Api.PollFree(Poll);
				Poll = nullptr;
				return false;
			}
		}

		std::unique_lock lock(Mutex);
		if (Client && ClientState == AvahiClientFailure)
		{
			// A client the daemon has left does not find it again. Its groups go with it, so they have to
			// be withdrawn first: the user of the responder does that on being told.
			if (!Groups.empty())
				return false;
			lock.unlock();
			{
				const Locked locked(*this);
				Api.ClientFree(Client);
			}
			Client = nullptr;
			lock.lock();
		}
		if (!Client)
		{
			ClientState = 0;
			lock.unlock();
			{
				const Locked locked(*this);
				Client = Api.ClientNew(Api.PollGet(Poll), AvahiClientNoFail, &Impl::ClientChanged, this, nullptr);
			}
			lock.lock();
		}
		return Client && ClientState == AvahiClientRunning;
	}

	// A name another program of this computer has registered is refused at once, a name taken in the network
	// only after the daemon has asked there.
	AvahiEntryGroup *Offer(const std::string &label, const DnsSdService &service, unsigned interfaceIndex, bool &taken)
	{
		const Locked locked(*this);
		AvahiEntryGroup *group = Api.GroupNew(Client, &Impl::GroupChanged, this);
		if (!group)
			return nullptr;
		{
			std::lock_guard lock(Mutex);
			Groups[group] = 0;
		}
		// The list grows at its head.
		AvahiStringList *txt = nullptr;
		for (auto item = service.Txt.rbegin(); item != service.Txt.rend(); ++item)
			txt = Api.ListAdd(txt, (item->first + "=" + item->second).c_str());
		// IPv4 only, as the server listens.
		int result = Api.GroupAddService(group, static_cast<int>(interfaceIndex), AvahiProtoInet, 0, label.c_str(),
										 service.Type.c_str(), nullptr, nullptr, service.Port, txt);
		if (result >= 0)
			result = Api.GroupCommit(group);
		Api.ListFree(txt);
		if (result >= 0)
			return group;
		taken = result == AvahiErrorCollision;
		{
			std::lock_guard lock(Mutex);
			Groups.erase(group);
		}
		Api.GroupFree(group);
		return nullptr;
	}

	int Await(AvahiEntryGroup *group)
	{
		std::unique_lock lock(Mutex);
		int &state = Groups[group];
		Changed.wait_for(lock, SystemTimeout, [&]
						 { return state == AvahiGroupEstablished || state == AvahiGroupCollision || state == AvahiGroupFailure; });
		return state;
	}

	void Free(AvahiEntryGroup *group)
	{
		{
			std::lock_guard lock(Mutex);
			Groups.erase(group);
		}
		const Locked locked(*this);
		Api.GroupFree(group);
	}

	static void ClientChanged(AvahiClient *, int state, void *self)
	{
		auto *impl = static_cast<Impl *>(self);
		{
			std::lock_guard lock(impl->Mutex);
			impl->ClientState = state;
		}
		// The first call comes from within the making of the client, on the owner's thread.
		if (std::this_thread::get_id() != impl->Owner)
			impl->Lost();
	}

	static void GroupChanged(AvahiEntryGroup *group, int state, void *self)
	{
		auto *impl = static_cast<Impl *>(self);
		bool lost = false;
		{
			std::lock_guard lock(impl->Mutex);
			const auto known = impl->Groups.find(group);
			if (known == impl->Groups.end())
				return;
			lost = known->second == AvahiGroupEstablished && state != AvahiGroupEstablished;
			known->second = state;
		}
		impl->Changed.notify_all();
		if (lost)
			impl->Lost();
	}
};

DnsSdResponder::DnsSdResponder(std::function<void()> lost) : FImpl(std::make_unique<Impl>(std::move(lost))) {}
DnsSdResponder::~DnsSdResponder() = default;

bool DnsSdResponder::Supported() const { return FImpl->Api.Complete; }

// Null also while the daemon is not there.
DnsSdResponder::Registration DnsSdResponder::Publish(const DnsSdService &service, unsigned interfaceIndex)
{
	constexpr int MaxNames = 10;

	if (!FImpl->Connect())
		return nullptr;
	std::string label = service.Label;
	for (int attempt = 0; attempt < MaxNames; ++attempt)
	{
		bool taken = false;
		if (AvahiEntryGroup *group = FImpl->Offer(label, service, interfaceIndex, taken))
		{
			const int state = FImpl->Await(group);
			if (state == AvahiGroupEstablished)
				return group;
			FImpl->Free(group);
			taken = state == AvahiGroupCollision;
		}
		if (!taken)
			return nullptr;
		char *other = FImpl->Api.AlternativeName(label.c_str());
		if (!other)
			return nullptr;
		label = other;
		FImpl->Api.Free(other);
	}
	return nullptr;
}

void DnsSdResponder::Withdraw(Registration registration, unsigned)
{
	FImpl->Free(static_cast<AvahiEntryGroup *>(registration));
}

#endif
