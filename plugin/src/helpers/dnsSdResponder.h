#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct DnsSdService
{
	// The name of the instance, as DnsSdInstanceLabel gives one.
	std::string Label;
	// Such as "_http._tcp".
	std::string Type;
	unsigned short Port = 0;
	std::vector<std::pair<std::string, std::string>> Txt;

	bool operator==(const DnsSdService &) const = default;
};

// The mDNS responder of the system: the DNS-SD of Windows, the Avahi daemon elsewhere. dnsSdResponderWindows.cpp
// and dnsSdResponderAvahi.cpp implement it, each of them empty on the other system: the build takes every
// source file. To use from the thread that made it.
class DnsSdResponder
{
public:
	using Registration = void *;

	// The function is called, on a thread of the system's, when what was registered is so no longer or can be
	// registered now: the registrations are then to be withdrawn and made anew.
	explicit DnsSdResponder(std::function<void()> lost);
	~DnsSdResponder();

	DnsSdResponder(const DnsSdResponder &) = delete;
	DnsSdResponder &operator=(const DnsSdResponder &) = delete;

	// False where the system has no responder to register with.
	bool Supported() const;

	// On one interface, by the number the system knows it by. The system registers the service under another
	// name when the wanted one is taken in that network. Null when it did not register it.
	Registration Publish(const DnsSdService &service, unsigned interfaceIndex);
	void Withdraw(Registration registration, unsigned interfaceIndex);

private:
	struct Impl;
	std::unique_ptr<Impl> FImpl;
};
