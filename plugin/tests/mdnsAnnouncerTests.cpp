// MdnsAnnouncer against the responder of the system: what the network is told as the player's name, its
// login, the interfaces it is allowed and the ports it holds change, and that nothing is left when it stops.
// The test asks the network itself, by mDNS queries on every interface. It needs a responder that answers
// here - the DNS-SD of Windows, a running Avahi daemon - and tells the machine cannot run it where there is
// none.
#include "mdnsAnnouncer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "check.h"
#include "dnsSdResponder.h"
#include "listenerStatus.h"
#include "networkInterfaces.h"
#include "networkWatcher.h"
#include "pluginInfo.h"
#include "settings.h"

using namespace std::chrono_literals;

namespace
{
	constexpr unsigned short Port = 3333;
	constexpr std::uint16_t TypeA = 1;
	constexpr std::uint16_t TypePtr = 12;
	constexpr std::uint16_t TypeTxt = 16;
	constexpr std::uint16_t TypeSrv = 33;
	const std::string ServiceName = std::string(MdnsAnnouncer::ServiceType) + ".local";

	struct Record
	{
		std::string Name;
		std::uint16_t Type = 0;
		std::uint32_t Ttl = 0;
		std::string Target;
		std::uint16_t Port = 0;
		std::vector<std::string> Texts;
	};

	using Message = std::vector<unsigned char>;

	std::string Lower(std::string text)
	{
		for (char &c : text)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return text;
	}

	// A name of the message at a position, with the references to earlier names followed. The position moves
	// past the name where it stands.
	bool ReadName(const Message &message, std::size_t &at, std::string &name)
	{
		name.clear();
		std::size_t position = at;
		bool followed = false;
		for (int hops = 0; hops < 64; ++hops)
		{
			if (position >= message.size())
				return false;
			const unsigned char length = message[position];
			if (length == 0)
			{
				if (!followed)
					at = position + 1;
				return true;
			}
			if ((length & 0xC0) == 0xC0)
			{
				if (position + 1 >= message.size())
					return false;
				if (!followed)
					at = position + 2;
				followed = true;
				position = static_cast<std::size_t>(length & 0x3F) << 8 | message[position + 1];
				continue;
			}
			if (length > 63 || position + 1 + length > message.size())
				return false;
			if (!name.empty())
				name += '.';
			name.append(message.begin() + static_cast<std::ptrdiff_t>(position) + 1,
						message.begin() + static_cast<std::ptrdiff_t>(position) + 1 + length);
			position += 1 + static_cast<std::size_t>(length);
		}
		return false;
	}

	std::uint16_t Read16(const Message &message, std::size_t at)
	{
		return static_cast<std::uint16_t>(message[at] << 8 | message[at + 1]);
	}

	// The records of a response; none of a query or of a message that is not whole.
	std::vector<Record> ReadRecords(const Message &message)
	{
		std::vector<Record> records;
		if (message.size() < 12 || (message[2] & 0x80) == 0)
			return records;
		const std::size_t questions = Read16(message, 4);
		const std::size_t count = static_cast<std::size_t>(Read16(message, 6)) + Read16(message, 8) + Read16(message, 10);
		std::size_t at = 12;
		std::string name;
		for (std::size_t i = 0; i < questions; ++i)
		{
			if (!ReadName(message, at, name) || at + 4 > message.size())
				return {};
			at += 4;
		}
		for (std::size_t i = 0; i < count; ++i)
		{
			Record record;
			if (!ReadName(message, at, record.Name) || at + 10 > message.size())
				return records;
			record.Type = Read16(message, at);
			record.Ttl = static_cast<std::uint32_t>(Read16(message, at + 4)) << 16 | Read16(message, at + 6);
			const std::size_t size = Read16(message, at + 8);
			const std::size_t data = at + 10;
			if (data + size > message.size())
				return records;
			std::size_t inner = data;
			if (record.Type == TypePtr)
			{
				if (!ReadName(message, inner, record.Target))
					return records;
			}
			else if (record.Type == TypeSrv && size >= 6)
			{
				record.Port = Read16(message, data + 4);
				inner = data + 6;
				if (!ReadName(message, inner, record.Target))
					return records;
			}
			else if (record.Type == TypeTxt)
			{
				while (inner < data + size)
				{
					const std::size_t length = message[inner];
					if (inner + 1 + length > data + size)
						break;
					record.Texts.emplace_back(message.begin() + static_cast<std::ptrdiff_t>(inner) + 1,
											  message.begin() + static_cast<std::ptrdiff_t>(inner) + 1 + static_cast<std::ptrdiff_t>(length));
					inner += 1 + length;
				}
			}
			records.push_back(std::move(record));
			at = data + size;
		}
		return records;
	}

	Message Question(const std::string &name, std::uint16_t type)
	{
		Message message(12, 0);
		message[5] = 1;
		std::size_t start = 0;
		while (start <= name.size())
		{
			const std::size_t dot = std::min(name.find('.', start), name.size());
			message.push_back(static_cast<unsigned char>(dot - start));
			message.insert(message.end(), name.begin() + static_cast<std::ptrdiff_t>(start), name.begin() + static_cast<std::ptrdiff_t>(dot));
			start = dot + 1;
		}
		message.push_back(0);
		message.insert(message.end(), {static_cast<unsigned char>(type >> 8), static_cast<unsigned char>(type & 0xFF), 0, 1});
		return message;
	}

	// What the responders of an interface's network answer to a question, this computer's own among them. Asked
	// from the mDNS port, as a responder asks: Windows answers no other.
	std::vector<Record> Ask(const std::string &interfaceAddress, const std::string &name, std::uint16_t type)
	{
		std::vector<Record> records;
		asio::io_context io;
		asio::ip::udp::socket socket(io);
		std::error_code ec;
		const asio::ip::address_v4 local = asio::ip::make_address_v4(interfaceAddress, ec);
		const asio::ip::address_v4 group = asio::ip::make_address_v4("224.0.0.251");
		socket.open(asio::ip::udp::v4(), ec);
		socket.set_option(asio::socket_base::reuse_address(true), ec);
		socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 5353), ec);
		if (ec)
			return records;
		socket.set_option(asio::ip::multicast::join_group(group, local), ec);
		socket.set_option(asio::ip::multicast::outbound_interface(local), ec);
		socket.set_option(asio::ip::multicast::enable_loopback(true), ec);
		socket.non_blocking(true, ec);
		socket.send_to(asio::buffer(Question(name, type)), asio::ip::udp::endpoint(group, 5353), 0, ec);
		if (ec)
			return records;

		const auto deadline = std::chrono::steady_clock::now() + 600ms;
		Message buffer(9000);
		while (std::chrono::steady_clock::now() < deadline)
		{
			asio::ip::udp::endpoint sender;
			const std::size_t size = socket.receive_from(asio::buffer(buffer), sender, 0, ec);
			if (ec)
			{
				std::this_thread::sleep_for(20ms);
				continue;
			}
			// Sent to every interface's listeners by some systems: only what came from this interface counts.
			if (sender.address() != local)
				continue;
			const std::vector<Record> read = ReadRecords(Message(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)));
			records.insert(records.end(), read.begin(), read.end());
		}
		return records;
	}

	struct Instance
	{
		std::uint16_t Port = 0;
		std::set<std::string> Texts;
	};

	// The players an interface's network is told of now, by their instance names.
	std::map<std::string, Instance> Players(const std::string &interfaceAddress)
	{
		std::map<std::string, Instance> players;
		const std::vector<Record> records = Ask(interfaceAddress, ServiceName, TypePtr);
		for (const Record &record : records)
			if (record.Type == TypePtr && record.Ttl != 0 && Lower(record.Name) == Lower(ServiceName))
				players[record.Target];
		for (auto &[name, instance] : players)
		{
			std::vector<Record> details = records;
			const auto told = [&](std::uint16_t type)
			{
				return std::any_of(details.begin(), details.end(), [&](const Record &r)
								   { return r.Type == type && r.Name == name; });
			};
			// A responder may leave them to be asked for.
			if (!told(TypeSrv))
			{
				const std::vector<Record> more = Ask(interfaceAddress, name, TypeSrv);
				details.insert(details.end(), more.begin(), more.end());
			}
			if (!told(TypeTxt))
			{
				const std::vector<Record> more = Ask(interfaceAddress, name, TypeTxt);
				details.insert(details.end(), more.begin(), more.end());
			}
			for (const Record &record : details)
			{
				if (record.Name != name || record.Ttl == 0)
					continue;
				if (record.Type == TypeSrv)
					instance.Port = record.Port;
				if (record.Type == TypeTxt)
					instance.Texts.insert(record.Texts.begin(), record.Texts.end());
			}
		}
		return players;
	}

	// The instances that announce a player of this name; the name of an instance is the system's to change.
	std::vector<Instance> Announced(const std::string &interfaceAddress, const std::string &playerName)
	{
		std::vector<Instance> announced;
		for (const auto &[name, instance] : Players(interfaceAddress))
			if (instance.Texts.contains("name=" + playerName))
				announced.push_back(instance);
		return announced;
	}

	bool Eventually(const std::function<bool()> &holds, std::chrono::seconds within = 10s)
	{
		const auto deadline = std::chrono::steady_clock::now() + within;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (holds())
				return true;
			std::this_thread::sleep_for(300ms);
		}
		return false;
	}

	// What a failed check saw.
	void Show(const std::string &interfaceAddress)
	{
		std::printf("     on %s:", interfaceAddress.c_str());
		for (const auto &[name, instance] : Players(interfaceAddress))
		{
			std::printf(" [%s, port %u,", name.c_str(), static_cast<unsigned>(instance.Port));
			for (const std::string &text : instance.Texts)
				std::printf(" %s", text.c_str());
			std::printf("]");
		}
		std::printf("\n");
	}

	bool Appears(const std::string &interfaceAddress, const std::string &playerName, bool auth, std::size_t count = 1)
	{
		const bool appeared = Eventually([&]
						  {
							  const std::vector<Instance> announced = Announced(interfaceAddress, playerName);
							  return announced.size() == count &&
									 std::all_of(announced.begin(), announced.end(), [&](const Instance &i)
												 { return i.Port == Port && i.Texts.contains(auth ? "auth=1" : "auth=0") &&
														  i.Texts.contains("version=" PLUGIN_VERSION_STRING); }); });
		if (!appeared)
			Show(interfaceAddress);
		return appeared;
	}

	// A responder does not repeat a record within a second, so one question without an answer tells nothing:
	// two of them do, more than a second apart.
	bool Disappears(const std::string &interfaceAddress, const std::string &playerName)
	{
		bool once = false;
		const bool disappeared = Eventually([&]
						  {
							  if (!Announced(interfaceAddress, playerName).empty())
							  {
								  once = false;
								  return false;
							  }
							  if (once)
								  return true;
							  once = true;
							  std::this_thread::sleep_for(1100ms);
							  return false; });
		if (!disappeared)
			Show(interfaceAddress);
		return disappeared;
	}

	// The interfaces whose network a responder of this computer answers on, two at most.
	std::vector<NetworkAddress> AnsweredInterfaces()
	{
		std::vector<NetworkAddress> answered;
		for (const NetworkAddress &a : ListIPv4Addresses())
		{
			if (a.IsLoopback || a.InterfaceIndex == 0 || answered.size() == 2)
				continue;
			if (std::any_of(answered.begin(), answered.end(), [&](const NetworkAddress &b)
							{ return b.InterfaceId == a.InterfaceId; }))
				continue;
#ifdef _WIN32
			// Windows answers for the computer's name where its mDNS works.
			const std::string host = HostName() + ".local";
			const bool answers = Eventually([&]
											{
												const std::vector<Record> records = Ask(a.Address, host, TypeA);
												return std::any_of(records.begin(), records.end(), [&](const Record &r)
																   { return r.Type == TypeA && Lower(r.Name) == Lower(host); }); },
											3s);
			if (!answers)
				continue;
#endif
			answered.push_back(a);
		}
		return answered;
	}

#ifndef _WIN32
	// The socket of the daemon stays in its place when the daemon is stopped: only a connection tells.
	bool AvahiListens()
	{
		for (const char *path : {"/run/avahi-daemon/socket", "/var/run/avahi-daemon/socket"})
		{
			asio::io_context io;
			asio::local::stream_protocol::socket socket(io);
			std::error_code ec;
			socket.connect(asio::local::stream_protocol::endpoint(path), ec);
			if (!ec)
				return true;
		}
		return false;
	}
#endif

	std::vector<std::string> AllBut(const std::vector<NetworkAddress> &kept)
	{
		std::vector<std::string> ids;
		for (const NetworkAddress &a : ListIPv4Addresses())
			if (std::none_of(kept.begin(), kept.end(), [&](const NetworkAddress &k)
							 { return k.InterfaceId == a.InterfaceId; }))
				ids.push_back(a.InterfaceId);
		return ids;
	}
}

int main()
{
	if (!DnsSdResponder([] {}).Supported())
		return Skip("the system has no DNS-SD to register a service with");
#ifndef _WIN32
	if (!AvahiListens())
		return Skip("the Avahi daemon is not running");
#endif
	const std::vector<NetworkAddress> interfaces = AnsweredInterfaces();
	if (interfaces.empty())
		return Skip("no interface with a network a responder of this computer answers on");
	const NetworkAddress &first = interfaces.front();
	const NetworkAddress &last = interfaces.back();
	const std::vector<std::string> unused = AllBut(interfaces);
	std::printf("interfaces: %s%s%s\n", first.Address.c_str(), interfaces.size() > 1 ? ", " : "",
				interfaces.size() > 1 ? last.Address.c_str() : "");

	// Other players of the network, and other runs of the test, are told apart by the name.
	const std::string tag = std::to_string(std::random_device()() % 100000);
	const std::string plain = "Test " + tag;
	const std::string renamed = "Тест v2.0 " + tag;
	// On all the interfaces at a time: every question waits for its answers.
	const auto everywhere = [&](const std::function<bool(const std::string &)> &holds)
	{
		std::vector<std::future<bool>> held;
		for (const NetworkAddress &a : interfaces)
			held.push_back(std::async(std::launch::async, holds, a.Address));
		bool all = true;
		for (std::future<bool> &one : held)
			all = one.get() && all;
		return all;
	};

	SharedSettings settings;
	ListenerStatus listeners;
	Settings s;
	s.PlayerName = plain;
	settings.Set(s);
	{
		NetworkWatcher watcher;
		MdnsAnnouncer announcer;
		const auto exclude = [&](std::vector<std::string> ids)
		{
			ids.insert(ids.end(), unused.begin(), unused.end());
			watcher.SetExcludedInterfaces(ids);
		};
		announcer.Start(watcher, listeners, settings, Port);
		exclude({});
		watcher.Start();
		Check("a started player is announced on every interface, with its port, version and no login",
			  everywhere([&](const std::string &address)
						 { return Appears(address, plain, false); }));

		s.PlayerName = renamed;
		s.Auth.Enabled = true;
		s.Auth.Username = "user";
		s.Auth.Ha1 = "ha1";
		settings.Set(s);
		announcer.Refresh();
		Check("renamed and given a login, it is announced so", everywhere([&](const std::string &address)
																			{ return Appears(address, renamed, true); }));
		Check("and no longer under the former name", everywhere([&](const std::string &address)
																  { return Disappears(address, plain); }));
		Check("a dot of the name is not in the name of the instance",
			  [&]
			  {
				  for (const auto &[name, instance] : Players(first.Address))
					  if (instance.Texts.contains("name=" + renamed))
						  return name.substr(0, name.size() - ServiceName.size() - 1).find('.') == std::string::npos;
				  return false;
			  }());

		exclude({last.InterfaceId});
		Check("an interface the settings exclude stops answering", Disappears(last.Address, renamed));
		if (interfaces.size() > 1)
			Check("while the other goes on", Appears(first.Address, renamed, true));
		exclude({});
		Check("allowed again, it answers", Appears(last.Address, renamed, true));

		listeners.SetTakenAddresses({first.Address});
		exclude({});
		Check("an address whose port the server could not take stops answering", Disappears(first.Address, renamed));
		if (interfaces.size() > 1)
			Check("while the other goes on", Appears(last.Address, renamed, true));
		listeners.SetTakenAddresses({});
		exclude({});
		Check("the port taken after all, it answers", Appears(first.Address, renamed, true));

		{
			NetworkWatcher otherWatcher;
			MdnsAnnouncer other;
			// On another port: a service that is the same in everything Avahi keeps as one, without a conflict.
			other.Start(otherWatcher, listeners, settings, Port + 1);
			std::vector<std::string> ids = unused;
			otherWatcher.SetExcludedInterfaces(ids);
			otherWatcher.Start();
			Check("a second player of the same name is announced beside the first, under a name of its own",
				  everywhere([&](const std::string &address)
							 {
								 const bool both = Eventually([&]
															  {
																  const std::vector<Instance> announced = Announced(address, renamed);
																  return announced.size() == 2 && announced[0].Port + announced[1].Port == 2 * Port + 1; });
								 if (!both)
									 Show(address);
								 return both; }));
			otherWatcher.Stop();
			other.Stop();
			Check("and gone, leaves the first", everywhere([&](const std::string &address)
															 { return Appears(address, renamed, true, 1); }));
		}

		watcher.Stop();
		announcer.Stop();
		Check("a stopped player is announced nowhere", everywhere([&](const std::string &address)
																	{ return Disappears(address, renamed); }));
	}

	// A stop is where a late answer of the system meets what the announcer has let go: at once, in the middle
	// of a registration and after it.
	for (const auto up : {0ms, 400ms, 1200ms, 2500ms, 0ms, 900ms})
	{
		NetworkWatcher watcher;
		MdnsAnnouncer announcer;
		announcer.Start(watcher, listeners, settings, Port);
		watcher.SetExcludedInterfaces(unused);
		watcher.Start();
		std::this_thread::sleep_for(up);
		watcher.Stop();
		announcer.Stop();
	}
	Check("started and stopped over and over, it leaves nothing announced", everywhere([&](const std::string &address)
																						 { return Disappears(address, renamed); }));

	return Finish();
}
