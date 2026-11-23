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

	// The DNS-SD of Windows sends the letters of a label beyond ASCII in lower case. The label is given to it as
	// it will send it: its limit is on what it sends, and it does not know a name for taken when that is there
	// in other letters than the ones given, but makes one record of the two.
	std::string LabelPrefix(const std::string &text, std::size_t maxBytes)
	{
		const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring given(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), given.data(), size);
		std::wstring sent = given;
		LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, given.data(), size, sent.data(), size, nullptr, nullptr, 0);
		for (std::size_t i = 0; i < sent.size(); ++i)
			if (given[i] < 0x80)
				sent[i] = given[i];

		std::size_t kept = 0;
		std::size_t bytes = 0;
		while (kept < sent.size())
		{
			const std::size_t units = kept + 1 < sent.size() && IS_SURROGATE_PAIR(sent[kept], sent[kept + 1]) ? 2 : 1;
			const std::size_t width = Utf8Size(&sent[kept], units);
			if (bytes + width > maxBytes)
				break;
			bytes += width;
			kept += units;
		}

		std::string prefix(bytes, '\0');
		WideCharToMultiByte(CP_UTF8, 0, sent.data(), static_cast<int>(kept), prefix.data(), static_cast<int>(prefix.size()),
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
