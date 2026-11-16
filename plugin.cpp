// Background Log Flush: takes the disk wait out of every plugin's log lines.
//
// D2RLoader writes plugin log lines in D2RCore.dll (0x47dca0 in the D2RCore of Nov 2026): for
// each line it opens the plugin's log file (context->pluginLogPath), writes the line, calls
// FlushFileBuffers and closes the file, all on the thread that logged. FlushFileBuffers waits
// until the disk has the line; on a slow drive that is 0.25-1 s, and the game froze for that long
// (seen with Lag Finder: the frame thread inside D2RCore's logger, in ZwFlushBuffersFile).
//
// This plugin points D2RCore's import of FlushFileBuffers at its own function. A flush that
// comes from D2RCore's log writer, on a file in a d2rloader\logs folder, is handed to a
// background thread and the logger goes on at once; the line itself has already been written
// (it is with the system, so it reaches the file even if the game crashes). Every other flush
// D2RCore makes (settings, saves, anything outside the logs folders) is passed straight through.
// The background thread waits batch_ms after a line, then flushes each log file once.
//
// Nothing about D2RCore's code is hard-coded: every flush it makes is checked by the name of the
// file (a .log file in a d2rloader\logs folder), which costs about a tenth of a millisecond.

#include <D2RLPlugin/api.h>

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

const D2RL::PluginContext* g_ctx = nullptr;

void Log(const char* format, ...) {
	if (g_ctx == nullptr) {
		return;
	}
	char    text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	g_ctx->LogInfo(text);
}

// ---------------------------------------------------------------- settings

struct Config {
	int enabled = 1;
	int batchMs = 500;
};
Config g_config;

int Clamp(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

void ParseConfig(const char* text) {
	for (const char* line = text; line != nullptr && *line != '\0';) {
		const char* end = strchr(line, '\n');
		char        buffer[256];
		const size_t length = end != nullptr ? static_cast<size_t>(end - line) : strlen(line);
		const size_t copy   = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
		memcpy(buffer, line, copy);
		buffer[copy] = '\0';
		line         = end != nullptr ? end + 1 : nullptr;

		char* p = buffer;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (*p == '#' || *p == '[' || *p == '\0') {
			continue;
		}
		char* equals = strchr(p, '=');
		if (equals == nullptr) {
			continue;
		}
		char* keyEnd = equals;
		while (keyEnd > p && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) {
			--keyEnd;
		}
		*keyEnd     = '\0';
		char* value = equals + 1;
		while (*value == ' ' || *value == '\t') {
			++value;
		}
		if (strcmp(p, "enabled") == 0) {
			g_config.enabled = strncmp(value, "true", 4) == 0 || *value == '1' ? 1 : 0;
		} else if (strcmp(p, "batch_ms") == 0) {
			g_config.batchMs = Clamp(atoi(value), 0, 2000);
		}
	}
}

void LoadConfig() {
	static char text[4096];
	memset(text, 0, sizeof(text));
	if (g_ctx->ReadConfig(text, sizeof(text) - 1)) {
		ParseConfig(text);
	} else {
		Log("config: could not read background-log-flush.toml - using the defaults");
	}
}

// ---------------------------------------------------------------- state

using FlushFn = BOOL(WINAPI*)(HANDLE file);

FlushFn   g_realFlush = nullptr; // what D2RCore's import pointed at before (normally kernel32)
void**    g_slot      = nullptr; // D2RCore's import slot for FlushFileBuffers
uintptr_t g_coreStart = 0;
uintptr_t g_coreEnd   = 0;
std::atomic<bool> g_on { false };

std::atomic<uintptr_t> g_logSite { 0 }; // where D2RCore's log writer flushes (for the log)

// flushes waiting for the background thread (duplicated handles: the logger closes its own)
constexpr int kMaxQueue = 1024;
HANDLE        g_queue[kMaxQueue];
int           g_queued    = 0;
SRWLOCK       g_queueLock = SRWLOCK_INIT;
HANDLE        g_wake      = nullptr;
HANDLE        g_stop      = nullptr;
HANDLE        g_thread    = nullptr;

std::atomic<uint64_t> g_deferred { 0 };  // log line flushes taken off the logging thread
std::atomic<uint64_t> g_flushes { 0 };   // flushes the background thread made
std::atomic<uint64_t> g_merged { 0 };    // deferred flushes covered by another one of the same file
std::atomic<uint64_t> g_dropped { 0 };   // queue full: line written, not waited for
std::atomic<uint64_t> g_passed { 0 };    // flushes left as they were (not a log file)
std::atomic<uint64_t> g_totalUs { 0 };   // time the background flushes took
std::atomic<uint64_t> g_worstUs { 0 };

uint64_t NowUs() {
	static LARGE_INTEGER frequency {};
	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	return static_cast<uint64_t>(counter.QuadPart) * 1000000ull / static_cast<uint64_t>(frequency.QuadPart);
}

// ---------------------------------------------------------------- which flushes are log lines

// 1 = a file in a d2rloader\logs folder ending in .log, 0 = something else, -1 = could not tell
int IsLogFile(HANDLE file) {
	wchar_t     path[1024];
	const DWORD length = GetFinalPathNameByHandleW(file, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])), FILE_NAME_NORMALIZED);
	if (length == 0 || length >= sizeof(path) / sizeof(path[0])) {
		return -1;
	}
	for (DWORD i = 0; i < length; ++i) {
		path[i] = static_cast<wchar_t>(towlower(path[i]));
	}
	const bool inLogs = wcsstr(path, L"\\d2rloader\\logs\\") != nullptr;
	const bool isLog  = length > 4 && wcscmp(path + length - 4, L".log") == 0;
	return inLogs && isLog ? 1 : 0;
}

bool Enqueue(HANDLE file) {
	AcquireSRWLockExclusive(&g_queueLock);
	const bool wasEmpty = g_queued == 0;
	const bool room     = g_queued < kMaxQueue;
	if (room) {
		g_queue[g_queued++] = file;
	}
	ReleaseSRWLockExclusive(&g_queueLock);
	if (room && wasEmpty) {
		SetEvent(g_wake);
	}
	return room;
}

// D2RCore's FlushFileBuffers from now on.
BOOL WINAPI DeferredFlush(HANDLE file) {
	const auto ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
	// every flush is checked by file name: only log files are left to the background thread
	if (!g_on.load(std::memory_order_acquire) || ret < g_coreStart || ret >= g_coreEnd || IsLogFile(file) != 1) {
		g_passed.fetch_add(1, std::memory_order_relaxed);
		return g_realFlush(file);
	}
	if (g_logSite.load(std::memory_order_relaxed) == 0) {
		g_logSite.store(ret, std::memory_order_relaxed);
	}
	HANDLE copy = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		g_passed.fetch_add(1, std::memory_order_relaxed);
		return g_realFlush(file);
	}
	if (Enqueue(copy)) {
		g_deferred.fetch_add(1, std::memory_order_relaxed);
	} else {
		CloseHandle(copy); // the line is written; it just is not waited for
		g_dropped.fetch_add(1, std::memory_order_relaxed);
	}
	return TRUE;
}

// ---------------------------------------------------------------- the background thread

void FlushPending() {
	static HANDLE batch[kMaxQueue];
	AcquireSRWLockExclusive(&g_queueLock);
	const int count = g_queued;
	memcpy(batch, g_queue, static_cast<size_t>(count) * sizeof(HANDLE));
	g_queued = 0;
	ReleaseSRWLockExclusive(&g_queueLock);

	struct Key {
		DWORD volume, high, low;
	};
	static Key done[kMaxQueue];
	int        doneCount = 0;
	for (int i = 0; i < count; ++i) {
		BY_HANDLE_FILE_INFORMATION info {};
		const bool known = GetFileInformationByHandle(batch[i], &info) != 0;
		bool       seen  = false;
		for (int j = 0; known && j < doneCount && !seen; ++j) {
			seen = done[j].volume == info.dwVolumeSerialNumber && done[j].high == info.nFileIndexHigh && done[j].low == info.nFileIndexLow;
		}
		if (seen) {
			g_merged.fetch_add(1, std::memory_order_relaxed);
		} else {
			const uint64_t start = NowUs();
			g_realFlush(batch[i]);
			const uint64_t took = NowUs() - start;
			g_flushes.fetch_add(1, std::memory_order_relaxed);
			g_totalUs.fetch_add(took, std::memory_order_relaxed);
			uint64_t worst = g_worstUs.load(std::memory_order_relaxed);
			while (took > worst && !g_worstUs.compare_exchange_weak(worst, took)) {
			}
			if (known) {
				done[doneCount++] = { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
			}
		}
		CloseHandle(batch[i]);
	}
}

DWORD WINAPI FlushThread(void*) {
	HANDLE waits[2] = { g_stop, g_wake };
	for (;;) {
		const DWORD woke     = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		bool        stopping = woke != WAIT_OBJECT_0 + 1;
		if (!stopping && g_config.batchMs > 0) {
			stopping = WaitForSingleObject(g_stop, static_cast<DWORD>(g_config.batchMs)) == WAIT_OBJECT_0; // let a burst of lines gather
		}
		FlushPending();
		if (stopping) {
			return 0;
		}
	}
}

// ---------------------------------------------------------------- D2RCore's import

void** FindImportSlot(HMODULE module, const char* function) {
	const auto base = reinterpret_cast<uintptr_t>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return nullptr;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return nullptr;
	}
	g_coreStart = base;
	g_coreEnd   = base + nt->OptionalHeader.SizeOfImage;
	const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (imports.VirtualAddress == 0) {
		return nullptr;
	}
	for (const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); descriptor->Name != 0; ++descriptor) {
		if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
			continue; // no name table to read
		}
		const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
		auto*       slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
		for (size_t i = 0; names[i].u1.AddressOfData != 0; ++i) {
			if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) {
				continue;
			}
			const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + static_cast<uintptr_t>(names[i].u1.AddressOfData));
			if (strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0) {
				return reinterpret_cast<void**>(&slots[i].u1.Function);
			}
		}
	}
	return nullptr;
}

bool WriteSlot(void* value) {
	DWORD old = 0;
	if (!VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
		return false;
	}
	InterlockedExchangePointer(g_slot, value); // one aligned write: callers see the old or the new
	DWORD unused = 0;
	VirtualProtect(g_slot, sizeof(void*), old, &unused);
	return true;
}

bool Install() {
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core == nullptr) {
		Log("D2RCore.dll is not loaded - nothing done");
		return false;
	}
	g_slot = FindImportSlot(core, "FlushFileBuffers");
	if (g_slot == nullptr || *g_slot == nullptr) {
		Log("D2RCore.dll does not import FlushFileBuffers by name - nothing done (log lines are written as before)");
		return false;
	}
	g_realFlush = reinterpret_cast<FlushFn>(*g_slot);

	// stay loaded for good: D2RCore will call into this DLL
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DeferredFlush), &self);

	g_wake   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_stop   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_thread = g_wake != nullptr && g_stop != nullptr ? CreateThread(nullptr, 0, FlushThread, nullptr, 0, nullptr) : nullptr;
	if (g_thread == nullptr) {
		Log("could not start the background thread - nothing done");
		return false;
	}
	using DescribeFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	if (const auto describe = reinterpret_cast<DescribeFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))) {
		describe(g_thread, L"BgLogFlush"); // the thread's name in tools such as Lag Finder
	}
	g_on.store(true, std::memory_order_release);
	if (!WriteSlot(reinterpret_cast<void*>(&DeferredFlush))) {
		g_on.store(false);
		Log("could not change D2RCore's import - nothing done");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- console

D2RL::ConsoleCommandResult __cdecl OnConsole(D2R::Game::Client*, const D2RL::ConsoleCommandContext*, void*) noexcept {
	char text[512];
	if (!g_on.load()) {
		snprintf(text, sizeof(text), "Background Log Flush: off (log lines wait for the disk as before). See background-log-flush.log.");
	} else {
		snprintf(text, sizeof(text),
			"Background Log Flush: on. %llu log lines did not wait for the disk; %llu background flushes (%llu lines merged into them), worst %.0f ms, %.1f s in all. "
			"Other flushes left alone: %llu.%s",
			static_cast<unsigned long long>(g_deferred.load()), static_cast<unsigned long long>(g_flushes.load()), static_cast<unsigned long long>(g_merged.load()),
			static_cast<double>(g_worstUs.load()) / 1000.0, static_cast<double>(g_totalUs.load()) / 1e6, static_cast<unsigned long long>(g_passed.load()),
			g_dropped.load() != 0 ? " Some lines were too many to queue and were not flushed (they are still written)." : "");
	}
	g_ctx->WriteConsoleMessage(text);
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "background-log-flush",
	.name        = "Background Log Flush",
	.version     = "1.0.0",
	.author      = "Latedozer",
	.description = "Plugin log lines no longer make the game wait for the disk: D2RLoader's flush after each line is done on a background thread.",
	.flags       = D2RL::PluginFlags::Shared,
	.reserved    = {},
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->GetApi() == nullptr) {
		return false;
	}
	g_ctx = context;
	LoadConfig();
	static_cast<void>(context->RegisterConsoleCommand("logflush", OnConsole, "Background Log Flush: how many log lines did not wait for the disk"));
	if (g_config.enabled == 0) {
		Log("Background Log Flush %s: off in the settings (enabled = false)", kPluginInfo.version);
		return true;
	}
	if (!Install()) {
		return true;
	}
	// this line goes through D2RCore's log writer, which shows us where it flushes
	Log("Background Log Flush %s: on - log lines are flushed to disk on a background thread (%d ms after a line)", kPluginInfo.version, g_config.batchMs);
	if (const uintptr_t site = g_logSite.load(); site != 0) {
		Log("found D2RCore's log writer (flush returns to D2RCore+0x%llx)", static_cast<unsigned long long>(site - g_coreStart));
	} else {
		Log("D2RCore's log writer was not seen on this line - log flushes may still wait for the disk");
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	if (!g_on.load()) {
		return;
	}
	Log("closing: %llu log lines did not wait for the disk, %llu background flushes, worst %.0f ms", static_cast<unsigned long long>(g_deferred.load()),
		static_cast<unsigned long long>(g_flushes.load()), static_cast<double>(g_worstUs.load()) / 1000.0);
	if (*g_slot == reinterpret_cast<void*>(&DeferredFlush)) {
		WriteSlot(reinterpret_cast<void*>(g_realFlush));
	}
	g_on.store(false, std::memory_order_release); // anything still arriving here goes straight through
	SetEvent(g_stop);
	WaitForSingleObject(g_thread, 5000); // the last lines are flushed before the thread ends
}
