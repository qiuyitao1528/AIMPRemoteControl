#pragma once

#include <string>

// A name as the instance label of a DNS-SD service: at most 63 bytes as the responder of the system sends it,
// cut between characters, and without the characters the DNS-SD of Windows reads a name by: it takes the name
// apart at the first dot, and refuses one whose label ends in a backslash.
std::string DnsSdInstanceLabel(const std::string &name);

// A value as long as a TXT item holds it: 255 bytes take the key, "=" and the value. Cut between characters.
std::string DnsSdTxtValue(const std::string &key, const std::string &value);
