#ifdef _WIN32

#include "dnsSdResponder.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#include "networkInterfaces.h"

namespace
{
	constexpr auto SystemTimeout = std::chrono::seconds(5);

	std::wstring Widen(const std::string &utf8)
	{
		const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		std::wstring wide(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
		return wide;
	}

	// The DNS-SD of Windows 10 1903 and later, taken by name: imported, it would keep the plugin from
	// loading in an older system. Taken once and kept: an answer of the system may come late.
	struct DnsSdApi
	{
		decltype(&DnsServiceConstructInstance) Construct = nullptr;
		decltype(&DnsServiceFreeInstance) Free = nullptr;
		decltype(&DnsServiceRegister) Register = nullptr;
		decltype(&DnsServiceDeRegister) DeRegister = nullptr;
		decltype(&DnsServiceRegisterCancel) Cancel = nullptr;

		static const DnsSdApi &Get()
		{
			static const DnsSdApi api;
			return api;
		}

		bool Complete() const { return Construct && Free && Register && DeRegister && Cancel; }

	private:
		DnsSdApi()
		{
			const HMODULE module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!module)
				return;
			Construct = reinterpret_cast<decltype(Construct)>(GetProcAddress(module, "DnsServiceConstructInstance"));
			Free = reinterpret_cast<decltype(Free)>(GetProcAddress(module, "DnsServiceFreeInstance"));
			Register = reinterpret_cast<decltype(Register)>(GetProcAddress(module, "DnsServiceRegister"));
			DeRegister = reinterpret_cast<decltype(DeRegister)>(GetProcAddress(module, "DnsServiceDeRegister"));
			Cancel = reinterpret_cast<decltype(Cancel)>(GetProcAddress(module, "DnsServiceRegisterCancel"));
		}
	};

	// A request to the system, answered on a thread of the system's. The call holds itself until then:
	// the system keeps the request's address, also when nobody waits for the answer any longer, and that
	// thread may be the last to let the call go, after the responder that made it.
	struct Call
	{
		std::mutex Mutex;
		std::condition_variable Done;
		bool Answered = false;
		DWORD Status = ERROR_SUCCESS;
		PDNS_SERVICE_INSTANCE Instance = nullptr;
		PDNS_SERVICE_INSTANCE Result = nullptr;
		DNS_SERVICE_REGISTER_REQUEST Request{};
		DNS_SERVICE_CANCEL Cancel{};
		std::shared_ptr<Call> Self;

		~Call()
		{
			if (Result && Result != Instance)
				DnsSdApi::Get().Free(Result);
			if (Instance)
				DnsSdApi::Get().Free(Instance);
		}

		static VOID WINAPI Answer(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
		{
			auto *call = static_cast<Call *>(context);
			std::shared_ptr<Call> self;
			{
				std::lock_guard lock(call->Mutex);
				self = std::move(call->Self);
				call->Status = status;
				call->Result = instance;
				call->Answered = true;
				call->Done.notify_all();
			}
		}
	};

	enum class Outcome
	{
		Answered,
		Refused,
		// The system has the request and has said nothing; the call waits for it by itself.
		Silent
	};

	Outcome Ask(const std::shared_ptr<Call> &call, unsigned interfaceIndex, bool registering)
	{
		const DnsSdApi &api = DnsSdApi::Get();
		call->Request.Version = DNS_QUERY_REQUEST_VERSION1;
		call->Request.InterfaceIndex = interfaceIndex;
		call->Request.pServiceInstance = call->Instance;
		call->Request.pRegisterCompletionCallback = &Call::Answer;
		call->Request.pQueryContext = call.get();
		call->Request.unicastEnabled = FALSE;
		call->Self = call;
		const DWORD started = registering ? api.Register(&call->Request, &call->Cancel) : api.DeRegister(&call->Request, nullptr);
		if (started != DNS_REQUEST_PENDING)
		{
			call->Self.reset();
			return Outcome::Refused;
		}

		const auto answered = [&]
		{ return call->Answered; };
		std::unique_lock lock(call->Mutex);
		if (call->Done.wait_for(lock, SystemTimeout, answered))
			return Outcome::Answered;
		if (registering)
		{
			// Given up on, the registration could still be made, with nothing to take it back. Cancelled, the
			// request is answered as any other is.
			lock.unlock();
			api.Cancel(&call->Cancel);
			lock.lock();
			if (call->Done.wait_for(lock, SystemTimeout, answered))
				return Outcome::Answered;
		}
		// The answer may come when the player has let the plugin go: the plugin stays loaded for it.
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
						   reinterpret_cast<LPCWSTR>(&Call::Answer), &module);
		return Outcome::Silent;
	}

	// By what the system registered and not by the wanted name, which would take away the service of another
	// program and leave this one behind.
	void TakeBack(PDNS_SERVICE_INSTANCE registration, unsigned interfaceIndex)
	{
		const auto call = std::make_shared<Call>();
		call->Instance = registration;
		Ask(call, interfaceIndex, false);
	}
}

struct DnsSdResponder::Impl
{
	// A registration the system has not answered, not even cancelled. No other is asked for until it is.
	std::shared_ptr<Call> Unanswered;

	~Impl() { Settled(); }

	bool Settled()
	{
		if (!Unanswered)
			return true;
		std::unique_lock lock(Unanswered->Mutex);
		if (!Unanswered->Answered)
			return false;
		const PDNS_SERVICE_INSTANCE late =
			Unanswered->Status == ERROR_SUCCESS ? std::exchange(Unanswered->Result, nullptr) : nullptr;
		const unsigned interfaceIndex = Unanswered->Request.InterfaceIndex;
		lock.unlock();
		Unanswered.reset();
		if (late)
			TakeBack(late, interfaceIndex);
		return true;
	}
};

// Windows tells nothing of a service once that is registered.
DnsSdResponder::DnsSdResponder(std::function<void()>) : FImpl(std::make_unique<Impl>()) {}
DnsSdResponder::~DnsSdResponder() = default;

bool DnsSdResponder::Supported() const { return DnsSdApi::Get().Complete(); }

DnsSdResponder::Registration DnsSdResponder::Publish(const DnsSdService &service, unsigned interfaceIndex)
{
	// The system answers for the addresses of the computer's own name only.
	const std::string hostName = HostName();
	if (hostName.empty() || !FImpl->Settled())
		return nullptr;

	const std::wstring instance = Widen(service.Label + "." + service.Type + ".local");
	const std::wstring host = Widen(hostName + ".local");
	std::vector<std::wstring> keyTexts, valueTexts;
	for (const auto &[key, value] : service.Txt)
	{
		keyTexts.push_back(Widen(key));
		valueTexts.push_back(Widen(value));
	}
	std::vector<PCWSTR> keys, values;
	for (std::size_t i = 0; i < keyTexts.size(); ++i)
	{
		keys.push_back(keyTexts[i].c_str());
		values.push_back(valueTexts[i].c_str());
	}

	const auto call = std::make_shared<Call>();
	call->Instance = DnsSdApi::Get().Construct(instance.c_str(), host.c_str(), nullptr, nullptr, service.Port, 0, 0,
											   static_cast<DWORD>(keys.size()), keys.data(), values.data());
	if (!call->Instance)
		return nullptr;
	const Outcome outcome = Ask(call, interfaceIndex, true);
	if (outcome == Outcome::Silent)
		FImpl->Unanswered = call;
	if (outcome != Outcome::Answered || call->Status != ERROR_SUCCESS)
		return nullptr;
	return std::exchange(call->Result, nullptr);
}

void DnsSdResponder::Withdraw(Registration registration, unsigned interfaceIndex)
{
	TakeBack(static_cast<PDNS_SERVICE_INSTANCE>(registration), interfaceIndex);
}

#endif
