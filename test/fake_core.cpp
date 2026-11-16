// Stand-in for D2RCore.dll: writes a log line the way D2RCore's log writer does (open the
// plugin's log file, write, FlushFileBuffers, close), and saves another file the same way.
// Both call FlushFileBuffers through this DLL's import table, as D2RCore does.
#include <windows.h>
#include <cstring>

static void WriteAndFlush(const wchar_t* path, const char* text, DWORD access, DWORD disposition) {
	HANDLE file = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
	WriteFile(file, "\r\n", 2, &written, nullptr);
	FlushFileBuffers(file);
	CloseHandle(file);
}

extern "C" __declspec(dllexport) void CoreLog(const wchar_t* path, const char* line) {
	WriteAndFlush(path, line, GENERIC_READ | FILE_APPEND_DATA, OPEN_ALWAYS);
}

extern "C" __declspec(dllexport) void CoreSave(const wchar_t* path, const char* text) {
	WriteAndFlush(path, text, GENERIC_WRITE, CREATE_ALWAYS);
}
