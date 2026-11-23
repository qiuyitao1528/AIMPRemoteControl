#pragma once

#include <string>

// A name as the instance label of a DNS-SD service, in the form the responder of the system sends it: at most
// 63 bytes, cut between characters; on Windows with the letters beyond ASCII in lower case; and without the
// characters the DNS-SD of Windows reads a name by, which takes the name apart at the first dot and refuses one
// whose label ends in a backslash.
std::string DnsSdInstanceLabel(const std::string &name);

// A value as long as a TXT item holds it: 255 bytes take the key, "=" and the value. Cut between characters.
std::string DnsSdTxtValue(const std::string &key, const std::string &value);
