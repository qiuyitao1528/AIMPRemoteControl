#include "dnsSd.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
	std::string Utf8Prefix(const std::string &text, std::size_t maxBytes)
	{
		if (text.size() <= maxBytes)
			return text;
		std::size_t end = maxBytes;
		while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			--end;
		return text.substr(0, end);
	}

#ifdef _WIN32
	std::size_t Utf8Size(const wchar_t *text, std::size_t units)
	{
		return static_cast<std::size_t>(
			WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(units), nullptr, 0, nullptr, nullptr));
	}

	// The DNS-SD of Windows sends a label in lower case, some letters of it at least, and its limit is on what
	// it sends. A character counts here for the longer of itself and its lower case as the system has that:
	// there are letters whose lower case takes more bytes.
	std::string LabelPrefix(const std::string &text, std::size_t maxBytes)
	{
		const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring wide(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
		std::wstring lower = wide;
		LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), size, lower.data(), size, nullptr, nullptr, 0);

		std::size_t kept = 0;
		std::size_t sent = 0;
		while (kept < wide.size())
		{
			const std::size_t units = kept + 1 < wide.size() && IS_SURROGATE_PAIR(wide[kept], wide[kept + 1]) ? 2 : 1;
			const std::size_t width = std::max(Utf8Size(&wide[kept], units), Utf8Size(&lower[kept], units));
			if (sent + width > maxBytes)
				break;
			sent += width;
			kept += units;
		}

		std::string prefix(Utf8Size(wide.data(), kept), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(kept), prefix.data(), static_cast<int>(prefix.size()),
							nullptr, nullptr);
		return prefix;
	}
#else
	std::string LabelPrefix(const std::string &text, std::size_t maxBytes)
	{
		return Utf8Prefix(text, maxBytes);
	}
#endif
}

std::string DnsSdInstanceLabel(const std::string &name)
{
	constexpr std::size_t MaxLabel = 63;

	std::string label = name;
	for (char &c : label)
		if (c == '.' || c == '\\')
			c = '-';
	return LabelPrefix(label, MaxLabel);
}

std::string DnsSdTxtValue(const std::string &key, const std::string &value)
{
	constexpr std::size_t MaxItem = 255;

	return Utf8Prefix(value, MaxItem - std::min(MaxItem, key.size() + 1));
}
