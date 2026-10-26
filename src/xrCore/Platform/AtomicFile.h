#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef IXR_WINDOWS
#include <Windows.h>
#else
#include <unistd.h>
#include <cstdlib>
#endif

namespace Platform
{
// Complete the replacement before exposing it to readers. Failure leaves the
// previous file intact; neither path falls back to truncating the destination.
inline bool WriteTextFileAtomically(const char* FilePath, const char* Text, unsigned long& Error)
{
	Error = 0;
	if (!FilePath || !Text) { Error = EINVAL; return false; }
#ifdef IXR_WINDOWS
	const int Count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, FilePath, -1, nullptr, 0);
	if (Count <= 0) { Error = ::GetLastError(); return false; }
	std::wstring Target(Count, L'\0');
	if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, FilePath, -1, Target.data(), Count))
	{
		Error = ::GetLastError(); return false;
	}
	Target.resize(Count - 1);
	static volatile LONG Sequence = 0;
	const std::wstring Staging = Target + L".tmp." + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(::InterlockedIncrement(&Sequence));
	HANDLE File = ::CreateFileW(Staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (File == INVALID_HANDLE_VALUE) { Error = ::GetLastError(); return false; }
	const char* Cursor = Text;
	size_t Remaining = std::strlen(Text);
	while (Remaining > 0)
	{
		const DWORD Chunk = Remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(Remaining);
		DWORD Written = 0;
		if (!::WriteFile(File, Cursor, Chunk, &Written, nullptr)) { Error = ::GetLastError(); break; }
		if (Written == 0) { Error = ERROR_WRITE_FAULT; break; }
		Cursor += Written; Remaining -= Written;
	}
	if (Error == 0 && !::FlushFileBuffers(File)) { Error = ::GetLastError(); }
	if (!::CloseHandle(File) && Error == 0) { Error = ::GetLastError(); }
	if (Error == 0 && !::MoveFileExW(Staging.c_str(), Target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		Error = ::GetLastError();
	}
	if (Error != 0) { ::DeleteFileW(Staging.c_str()); return false; }
#else
	std::string Staging = std::string(FilePath) + ".tmp.XXXXXX";
	const int File = mkstemp(Staging.data());
	if (File < 0) { Error = errno; return false; }
	const char* Cursor = Text;
	size_t Remaining = std::strlen(Text);
	while (Remaining > 0)
	{
		const ssize_t Written = write(File, Cursor, Remaining);
		if (Written < 0 && errno == EINTR) { continue; }
		if (Written <= 0) { Error = Written < 0 ? errno : EIO; break; }
		Cursor += Written; Remaining -= Written;
	}
	if (Error == 0 && fsync(File) != 0) { Error = errno; }
	const int Closed = close(File);
	if (Closed != 0 && Error == 0) { Error = errno; }
	if (Error == 0 && std::rename(Staging.c_str(), FilePath) != 0) { Error = errno; }
	if (Error != 0) { std::remove(Staging.c_str()); return false; }
#endif
	return true;
}
}
