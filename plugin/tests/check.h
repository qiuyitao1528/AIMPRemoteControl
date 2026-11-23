#pragma once

#include <cstdio>

// What a test program needs: named checks printed as they run, and an exit code
// that tells CTest whether any failed.

inline int checkFailures = 0;

inline void Check(const char *name, bool ok)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
	if (!ok)
		++checkFailures;
}

inline int Finish()
{
	std::printf("%s\n", checkFailures ? "FAILED" : "ALL PASSED");
	return checkFailures ? 1 : 0;
}

// For a test the machine cannot run, which is not a failure: CTest is told that this exit code means skipped.
inline int Skip(const char *reason)
{
	std::printf("SKIPPED: %s\n", reason);
	return 77;
}
