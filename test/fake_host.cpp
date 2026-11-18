// Loads the plugin the way D2RLoader does, next to a stand-in D2RCore.dll whose flushes are made
// slow (300 ms each, like the drive that froze the game), and checks:
//  - a log line made the logging thread wait before, and does not with the plugin
//  - every line is in the file, in order, and the files are flushed on another thread, merged
//  - flushes outside the logs folder still wait on the caller's thread
//  - unloading puts D2RCore's import back and flushes what was left
// usage: fake_host.exe <plugin.dll> <work folder> [disabled]
#include <D2RLPlugin/api.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using CoreFn = void (*)(const wchar_t* path, const char* text);
CoreFn g_coreLog  = nullptr;
CoreFn g_coreSave = nullptr;

int g_failures = 0;
void Check(bool ok, const char* what) {
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	fflush(stdout);
	if (!ok) {
		++g_failures;
	}
}

double NowMs() {
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// the slow disk: every flush D2RCore makes takes 300 ms
using FlushFn = BOOL(WINAPI*)(HANDLE);
FlushFn          g_kernelFlush = nullptr;
std::atomic<int> g_slowFlushes { 0 };
std::atomic<int> g_slowOnMain { 0 };
DWORD            g_mainThread = 0;
BOOL WINAPI SlowFlush(HANDLE file) {
	Sleep(300);
	++g_slowFlushes;
	if (GetCurrentThreadId() == g_mainThread) {
		++g_slowOnMain;
	}
	return g_kernelFlush(file);
}

void** FindSlot(HMODULE module) {
	auto base = reinterpret_cast<uintptr_t>(module);
	auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name != 0; ++d) {
		auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
		auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
		for (size_t i = 0; names[i].u1.AddressOfData != 0; ++i) {
			if (!IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)
				&& strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name, "FlushFileBuffers") == 0) {
				return reinterpret_cast<void**>(&slots[i].u1.Function);
			}
		}
	}
	return nullptr;
}

void SetSlot(void** slot, void* value) {
	DWORD old;
	VirtualProtect(slot, 8, PAGE_READWRITE, &old);
	*slot = value;
	VirtualProtect(slot, 8, old, &old);
}

// the loader's side
std::string                    g_config;
D2RL::ConsoleCommandCallback   g_console = nullptr;
std::string                    g_consoleText;
void __cdecl HostLog(const D2RL::PluginContext* ctx, const char* message) noexcept {
	g_coreLog(ctx->pluginLogPath, message); // D2RLoader's log functions end in D2RCore's log writer
}
bool __cdecl ReadConfig(const D2RL::PluginContext*, char* output, uint32_t size, uint32_t* required) noexcept {
	if (required != nullptr) {
		*required = static_cast<uint32_t>(g_config.size() + 1);
	}
	if (output == nullptr || size <= g_config.size()) {
		return false;
	}
	memcpy(output, g_config.c_str(), g_config.size() + 1);
	return true;
}
bool __cdecl RegisterConsoleCommand(const D2RL::PluginContext*, const D2RL::ConsoleCommandRegistration* command) noexcept {
	g_console = command->callback;
	return true;
}
void __cdecl WriteConsoleMessage(const D2RL::PluginContext*, const char* message, D2RL::ConsoleMessageKind) noexcept {
	g_consoleText = message;
	printf("  [console] %s\n", message);
}

std::string ReadFileText(const std::wstring& path) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return {};
	}
	std::string text;
	char        buffer[4096];
	DWORD       got = 0;
	while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
		text.append(buffer, got);
	}
	CloseHandle(file);
	return text;
}

std::wstring Widen(const char* text) {
	wchar_t buffer[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, MAX_PATH);
	return buffer;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: fake_host.exe <plugin.dll> <work folder> [disabled]\n");
		return 2;
	}
	const bool         disabled = argc > 3 && strcmp(argv[3], "disabled") == 0;
	const std::wstring work     = Widen(argv[2]);
	CreateDirectoryW((work + L"\\d2rloader").c_str(), nullptr);
	CreateDirectoryW((work + L"\\d2rloader\\logs").c_str(), nullptr);
	CreateDirectoryW((work + L"\\saves").c_str(), nullptr);
	const std::wstring ownLog   = work + L"\\d2rloader\\logs\\background-log-flush.log";
	const std::wstring otherLog = work + L"\\d2rloader\\logs\\other-plugin.log";
	const std::wstring save     = work + L"\\saves\\character.d2s";
	g_mainThread                = GetCurrentThreadId();

	const HMODULE core = LoadLibraryW(L"D2RCore.dll");
	Check(core != nullptr, "the stand-in D2RCore.dll loads");
	if (core == nullptr) {
		return 1;
	}
	g_coreLog  = reinterpret_cast<CoreFn>(reinterpret_cast<void*>(GetProcAddress(core, "CoreLog")));
	g_coreSave = reinterpret_cast<CoreFn>(reinterpret_cast<void*>(GetProcAddress(core, "CoreSave")));
	void** slot = FindSlot(core);
	Check(slot != nullptr, "the stand-in imports FlushFileBuffers");
	g_kernelFlush = reinterpret_cast<FlushFn>(*slot);
	SetSlot(slot, reinterpret_cast<void*>(&SlowFlush));

	D2RL::PluginContext other {}; // another plugin's context (only its log path is used)
	other.pluginLogPath = otherLog.c_str();

	double start = NowMs();
	HostLog(&other, "before: a line with the slow disk");
	Check(NowMs() - start >= 280, "without the plugin a log line waits for the slow disk (300 ms)");

	// shaped like the real settings file: comments, a section, a comment after a value, CRLF
	g_config = disabled ? "# settings\r\nenabled = false\r\n" : "# Background Log Flush\r\n\r\n[general]\r\nenabled = true   # on\r\nbatch_ms = 200\r\n";
	static D2RL::PluginApi api {};
	api.apiSize                = D2RL::PluginApiSize;
	api.logInfo = api.logWarn = api.logError = HostLog;
	api.readConfig             = ReadConfig;
	api.registerConsoleCommand = RegisterConsoleCommand;
	api.writeConsoleMessage    = WriteConsoleMessage;
	static D2RL::PluginContext context {};
	context.contextSize   = static_cast<uint32_t>(sizeof(context));
	context.abiVersion    = D2RL_PLUGIN_ABI_VERSION;
	context.api           = &api;
	context.pluginId      = "background-log-flush";
	context.pluginLogPath = ownLog.c_str();

	const HMODULE plugin = LoadLibraryW(Widen(argv[1]).c_str());
	Check(plugin != nullptr, "the plugin loads");
	using LoadFn   = bool (*)(const D2RL::PluginContext*);
	using UnloadFn = void (*)();
	const auto load   = reinterpret_cast<LoadFn>(reinterpret_cast<void*>(GetProcAddress(plugin, "D2RLoaderLoadPlugin")));
	const auto unload = reinterpret_cast<UnloadFn>(reinterpret_cast<void*>(GetProcAddress(plugin, "D2RLoaderUnloadPlugin")));
	Check(load != nullptr && load(&context), "D2RLoaderLoadPlugin succeeds");

	if (disabled) {
		Check(*slot == reinterpret_cast<void*>(&SlowFlush), "enabled = false: D2RCore's import is left alone");
		start = NowMs();
		HostLog(&other, "disabled: a line");
		Check(NowMs() - start >= 280, "enabled = false: log lines are flushed as before");
		Check(ReadFileText(ownLog).find("off in the settings") != std::string::npos, "and the plugin's log says it is off");
		printf(g_failures == 0 ? "ALL PASSED (0 failed)\n" : "FAILURES: %d\n", g_failures);
		return g_failures == 0 ? 0 : 1;
	}

	Check(*slot != reinterpret_cast<void*>(&SlowFlush), "D2RCore's import of FlushFileBuffers now points at the plugin");
	Check(ReadFileText(ownLog).find("found D2RCore's log writer") != std::string::npos, "the plugin found the log writer from its own first line");
	Check(ReadFileText(ownLog).find("(200 ms after a line)") != std::string::npos, "the settings were read (batch_ms = 200)");

	// a burst of lines from another plugin, as at the moment of joining a game
	const int flushesBefore = g_slowFlushes.load();
	const int mainBefore    = g_slowOnMain.load();
	double    worst         = 0;
	for (int i = 0; i < 30; ++i) {
		char line[64];
		snprintf(line, sizeof(line), "burst line %02d", i);
		start = NowMs();
		HostLog(&other, line);
		const double took = NowMs() - start;
		worst             = took > worst ? took : worst;
	}
	printf("  [host] worst log line in the burst: %.2f ms\n", worst);
	Check(worst < 50, "with the plugin no log line waits for the disk (each under 50 ms, the disk takes 300)");
	Check(g_slowOnMain.load() == mainBefore, "no flush of a log line ran on the logging thread");

	// the background thread: 200 ms gathering, then one 300 ms flush per file. Wine can hold a
	// thread back for a moment, so give it up to 5 s; a flush that was never started fails anyway.
	Sleep(1500);
	for (int waited = 1500; waited < 5000 && g_slowFlushes.load() == flushesBefore; waited += 100) {
		Sleep(100);
	}
	const int burstFlushes = g_slowFlushes.load() - flushesBefore;
	printf("  [host] flushes for the 30 lines: %d\n", burstFlushes);
	Check(burstFlushes >= 1 && burstFlushes <= 4, "the 30 lines were flushed in a few background flushes, not 30");

	const std::string text  = ReadFileText(otherLog);
	bool              order = true;
	size_t            at    = 0;
	for (int i = 0; i < 30 && order; ++i) {
		char line[64];
		snprintf(line, sizeof(line), "burst line %02d", i);
		const size_t found = text.find(line, at);
		order              = found != std::string::npos;
		at                 = order ? found : at;
	}
	Check(order, "every line is in the other plugin's log, in order");

	// a file outside the logs folder keeps its flush, on the caller's thread
	const int mainBeforeSave = g_slowOnMain.load();
	start                    = NowMs();
	g_coreSave(save.c_str(), "character data");
	Check(NowMs() - start >= 280 && g_slowOnMain.load() == mainBeforeSave + 1, "a save outside the logs folder is still flushed before the call returns");
	start = NowMs();
	g_coreSave(save.c_str(), "character data again");
	Check(NowMs() - start >= 280, "and again");

	// a log line from another thread (plugins log from their own threads too)
	struct Job {
		const D2RL::PluginContext* ctx;
		double                     took;
	} job { &other, 0 };
	HANDLE thread = CreateThread(nullptr, 0, [](void* p) -> DWORD {
		auto*        j = static_cast<Job*>(p);
		const double s = NowMs();
		HostLog(j->ctx, "a line from another thread");
		j->took = NowMs() - s;
		return 0;
	}, &job, 0, nullptr);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	Check(job.took < 50, "a line logged from another thread does not wait either");

	g_console(nullptr, nullptr, nullptr);
	Check(g_consoleText.find("Background Log Flush: on. 3") != std::string::npos || g_consoleText.find("did not wait for the disk") != std::string::npos,
		"logflush in the console reports the counts");

	// the last lines before closing are flushed, and D2RCore gets its import back
	HostLog(&other, "last line before closing");
	const int flushesBeforeUnload = g_slowFlushes.load();
	unload();
	Check(*slot == reinterpret_cast<void*>(&SlowFlush), "unloading puts D2RCore's import back as it was");
	Check(g_slowFlushes.load() > flushesBeforeUnload, "and the lines still waiting were flushed before it finished");
	Check(ReadFileText(otherLog).find("last line before closing") != std::string::npos, "the last line is in the file");
	start = NowMs();
	HostLog(&other, "after: a line with the slow disk");
	Check(NowMs() - start >= 280, "after unloading, log lines are flushed as before");

	printf(g_failures == 0 ? "ALL PASSED (0 failed)\n" : "FAILURES: %d\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
