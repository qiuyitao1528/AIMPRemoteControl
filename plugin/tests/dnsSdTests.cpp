// What the system's responder is given for a player's name. A label over 63 bytes or with a dot is taken
// without an error and then not announced, or announced under another type; a TXT item holds 255 bytes.
#include "dnsSd.h"

#include <string>

#include "check.h"

namespace
{
	std::string Repeat(const std::string &piece, int count)
	{
		std::string out;
		for (int i = 0; i < count; ++i)
			out += piece;
		return out;
	}
}

int main()
{
	Check("a short name is kept", DnsSdInstanceLabel("Living room") == "Living room");
	Check("an empty name stays empty", DnsSdInstanceLabel("").empty());
	Check("dots become hyphens", DnsSdInstanceLabel("v2.0 of Mr. Smith") == "v2-0 of Mr- Smith");
	Check("backslashes become hyphens", DnsSdInstanceLabel("HOME\\PC\\") == "HOME-PC-");
	Check("a backslash left at the cut does too", DnsSdInstanceLabel(Repeat("x", 62) + "\\tail") == Repeat("x", 62) + "-");

	Check("63 bytes are kept", DnsSdInstanceLabel(Repeat("x", 63)) == Repeat("x", 63));
	Check("64 bytes are cut to 63", DnsSdInstanceLabel(Repeat("x", 64)) == Repeat("x", 63));
	Check("a long name is cut to 63", DnsSdInstanceLabel(Repeat("x", 500)) == Repeat("x", 63));

	// Two bytes a letter: 31 letters fit, the 32nd would end at byte 64.
	Check("31 two-byte letters are kept", DnsSdInstanceLabel(Repeat("я", 31)) == Repeat("я", 31));
	Check("the cut does not split a two-byte letter", DnsSdInstanceLabel(Repeat("я", 32)) == Repeat("я", 31));
	Check("nor after a byte of offset", DnsSdInstanceLabel("x" + Repeat("я", 32)) == "x" + Repeat("я", 31));
	// Four bytes a character: 15 fit in 60, the 16th would end at byte 64.
	Check("the cut does not split a four-byte character", DnsSdInstanceLabel(Repeat("🎵", 16)) == Repeat("🎵", 15));
	Check("a dot past the cut does not matter", DnsSdInstanceLabel(Repeat("x", 63) + ".") == Repeat("x", 63));

#ifdef _WIN32
	// Windows sends the letters beyond ASCII in lower case, and is given the label so.
	Check("letters beyond ASCII go to lower case", DnsSdInstanceLabel("Гостиная ÄÖ") == "гостиная äö");
	Check("ASCII keeps its case", DnsSdInstanceLabel("Home PC") == "Home PC");
	// These letters take two bytes and their lower case three: the limit is on the lower case.
	Check("21 letters growing to 63 bytes are kept", DnsSdInstanceLabel(Repeat("Ⱥ", 21)) == Repeat("ⱥ", 21));
	Check("the 22nd would be sent past the limit", DnsSdInstanceLabel(Repeat("Ⱥ", 22)) == Repeat("ⱥ", 21));
	Check("so with the other such letter", DnsSdInstanceLabel(Repeat("Ⱦ", 31)) == Repeat("ⱦ", 21));
	Check("a growing letter that does not fit is left out", DnsSdInstanceLabel(Repeat("x", 61) + "Ⱥ") == Repeat("x", 61));
	Check("and one that does is kept", DnsSdInstanceLabel(Repeat("x", 60) + "Ⱥ") == Repeat("x", 60) + "ⱥ");
#else
	Check("non-ASCII is kept", DnsSdInstanceLabel("Гостиная") == "Гостиная");
	Check("where a label is sent as it is, a letter counts as it is", DnsSdInstanceLabel(Repeat("Ⱥ", 31)) == Repeat("Ⱥ", 31));
#endif
	Check("bytes that are no text do not overrun", DnsSdInstanceLabel(std::string(200, '\xBA') + "\xC8").size() <= 63);

	Check("a short TXT value is kept", DnsSdTxtValue("name", "Гостиная v2.0") == "Гостиная v2.0");
	Check("a TXT value fills the item with its key", DnsSdTxtValue("name", Repeat("x", 250)) == Repeat("x", 250));
	Check("a longer one is cut to the item", DnsSdTxtValue("name", Repeat("x", 251)) == Repeat("x", 250));
	Check("the cut does not split a letter of a TXT value", DnsSdTxtValue("key", Repeat("я", 200)) == Repeat("я", 125));

	return Finish();
}
