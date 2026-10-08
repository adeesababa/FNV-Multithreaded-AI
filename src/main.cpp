// FNV Multithreaded AI (EXPERIMENTAL): an xNVSE plugin for Fallout: New Vegas 1.4.0.525 that runs the NPC-vs-NPC
// detection checks (line-of-sight ray casts) of a detection pass on several CPU cores at once, so big crowds stop
// freezing the game when they spot each other. Same checks, same inputs, same results; nothing is written to saves.
// The work is in ParallelDetection.inl; this file is the plugin around it: settings, log, the frame tick and the hook
// on the detection loop's check call.
//
// Settings: MultithreadedAI.ini next to the DLL. Log: MultithreadedAI.log in the game folder (replaced each launch).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <share.h>
#include <intrin.h>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>

typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef unsigned long long UInt64;

static const UInt32 kPluginVersion = 1;
static const char* kPluginName = "MultithreadedAI";

// ---------------------------------------------------------------------------------------------------------------
// xNVSE plugin interface (only what is used; layouts from xNVSE PluginAPI.h)
struct PluginInfo { UInt32 infoVersion; const char* name; UInt32 version; };
struct NVSEInterface {
	UInt32 nvseVersion, runtimeVersion, editorVersion, isEditor;
	bool (*RegisterCommand)(void*);
	void (*SetOpcodeBase)(UInt32);
	void* (*QueryInterface)(UInt32);
	UInt32 (*GetPluginHandle)(void);
};
struct NVSEMessagingInterface {
	struct Message { const char* sender; UInt32 type; UInt32 dataLen; void* data; };
	typedef void (*EventCallback)(Message* msg);
	UInt32 version;
	bool (*RegisterListener)(UInt32 listener, const char* sender, EventCallback handler);
	bool (*Dispatch)(UInt32 sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};
enum { kInterface_Messaging = 2 };
enum { kMessage_DeferredInit = 18, kMessage_MainGameLoop = 20 };

// ---------------------------------------------------------------------------------------------------------------
// Log
static FILE* Log;
static void LogLine(const char* fmt, ...) {
	if (!Log) return;
	SYSTEMTIME t; GetLocalTime(&t);
	fprintf(Log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list a; va_start(a, fmt); vfprintf(Log, fmt, a); va_end(a);
	fputc('\n', Log);
}
static void LogFlush() { if (Log) fflush(Log); }

static std::string GameFolder() {
	char path[MAX_PATH] = {};
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	std::string s(path, n);
	size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

static std::string ModuleOf(UInt32 address) {
	HMODULE m = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &m) || !m)
		return "no module";
	char path[MAX_PATH] = {};
	GetModuleFileNameA(m, path, MAX_PATH);
	const char* base = strrchr(path, '\\');
	char out[MAX_PATH + 32];
	snprintf(out, sizeof(out), "%s+0x%X", base ? base + 1 : path, address - (UInt32)m);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Settings: MultithreadedAI.ini next to the DLL. "[Section]" + "Key = value" with ; or # comments.
struct Settings {
	int parLevel = 1;             // [Main] Enabled: 1 = on at start, 0 = off (the Scroll Lock key can still switch it on)
	bool parHotkey = true;        // [Main] ScrollLockToggle: the Scroll Lock key switches it on/off in game (to compare)
	int parThreads = 0;           // [Main] Threads: helper threads, 0 = automatic (logical cores - 3, at most 8)
	int parMinJobs = 12;          // [Advanced] MinChecks: a lone NPC's detection run with fewer checks stays one by one
	bool parSerialChecks = false; // [Advanced] SerialChecks: diagnostic, the checks themselves run one by one (no speed-up)
	int summarySeconds = 10;      // [Advanced] LogSummarySeconds: how often the log gets a line of numbers (0 = never)
};
static Settings Config;

static std::string Trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::string ReadSettings(HMODULE self) {
	char path[MAX_PATH] = {};
	GetModuleFileNameA(self, path, MAX_PATH);
	std::string ini(path);
	size_t dot = ini.find_last_of('.');
	ini = (dot == std::string::npos ? ini : ini.substr(0, dot)) + ".ini";
	FILE* f = _fsopen(ini.c_str(), "r", _SH_DENYNO);
	if (!f) return ini + " (not found: defaults used)";
	char line[512];
	std::string section;
	while (fgets(line, sizeof(line), f)) {
		std::string s(line);
		size_t comment = s.find_first_of(";#");
		if (comment != std::string::npos) s.resize(comment);
		s = Trim(s);
		if (s.empty()) continue;
		if (s[0] == '[') { section = Trim(s.substr(1, s.find(']') - 1)); continue; }
		size_t eq = s.find('=');
		if (eq == std::string::npos) continue;
		std::string key = section + "." + Trim(s.substr(0, eq)), value = Trim(s.substr(eq + 1));
		const bool b = value == "1" || value == "true" || value == "True" || value == "TRUE";
		const double d = atof(value.c_str());
		if (key == "Main.Enabled") Config.parLevel = b ? 1 : 0;
		else if (key == "Main.ScrollLockToggle") Config.parHotkey = b;
		else if (key == "Main.Threads") Config.parThreads = std::max(0, std::min(15, (int)d));
		else if (key == "Advanced.MinChecks") Config.parMinJobs = std::max(2, (int)d);
		else if (key == "Advanced.SerialChecks") Config.parSerialChecks = b;
		else if (key == "Advanced.LogSummarySeconds") Config.summarySeconds = std::max(0, (int)d);
	}
	fclose(f);
	return ini;
}

// Clock: rdtsc for the batch timings, converted with a rate measured against QueryPerformanceCounter between plugin
// load and DeferredInit (seconds apart).
static UInt64 LoadTsc, LoadQpc;
static double TicksPerMs = 1.0;
static void CalibrateClock() {
	LARGE_INTEGER q, f;
	QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
	UInt64 tsc = __rdtsc();
	double ms = double(q.QuadPart - LoadQpc) * 1000.0 / double(f.QuadPart);
	if (ms < 50) {
		Sleep(100);
		QueryPerformanceCounter(&q); tsc = __rdtsc();
		ms = double(q.QuadPart - LoadQpc) * 1000.0 / double(f.QuadPart);
	}
	TicksPerMs = double(tsc - LoadTsc) / ms;
}

// ---------------------------------------------------------------------------------------------------------------
namespace MTAI {
	static const UInt32 kPairSite = 0x008E494B, kGetDetectionLevel = 0x008A0D10;
	typedef int(__thiscall* GetLevelFn)(void* self, int a1, void* target, UInt8* outSeen, int targetInCombat, int a5, int a6, UInt8* selfInCombat);
	static GetLevelFn OrigGetLevel = (GetLevelFn)kGetDetectionLevel;   // variables so the offline test can stand in for them
	static UInt32 PlayerPtrAddress = 0x011DEA3C;                          // PlayerCharacter* (xNVSE g_thePlayer)
	static volatile LONG FrameNo;                                          // bumped once per frame (xNVSE MainGameLoop)
	static UInt64 FrameCount;

#include "ParallelDetection.inl"

	// The detection loop's check call (0x008E494B, thiscall, ecx = this NPC, 7 args, ret 0x1C): the batch's answer when it
	// has one, the game's own check otherwise.
	static int __fastcall PairLevel(UInt8* self, void*, int a1, UInt8* target, UInt8* outSeen, int targetInCombat, int a5, int a6, UInt8* selfInCombat) {
		int r;
		if (Parallel::Answer(self, a1, target, outSeen, targetInCombat, a5, a6, selfInCombat, &r)) return r;
		return OrigGetLevel(self, a1, target, outSeen, targetInCombat, a5, a6, selfInCombat);
	}

	static bool InstallPairs() {
		UInt8* p = (UInt8*)kPairSite;
		if (p[0] != 0xE8 || kPairSite + 5 + *(int*)(p + 1) != kGetDetectionLevel) {
			LogLine("REFUSED: %08X is not 'call Actor::GetDetectionLevelAgainstActor' (bytes %02X %02X %02X %02X %02X): another plugin patched it",
				kPairSite, p[0], p[1], p[2], p[3], p[4]);
			return false;
		}
		DWORD old;
		VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
		*(int*)(p + 1) = (int)((UInt32)PairLevel - (kPairSite + 5));
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
		return true;
	}

	static UInt64 SummaryFrames;
	static void OnFrame() {
		InterlockedIncrement(&FrameNo);
		FrameCount++;
		Parallel::OnFrameStart();
		if (Config.summarySeconds > 0 && Parallel::Ready && ++SummaryFrames >= (UInt64)Config.summarySeconds * 60) {
			Parallel::Write((size_t)SummaryFrames);
			SummaryFrames = 0;
			LogFlush();
		}
	}
}

static HMODULE Self;
static UInt32 PluginHandle;

static void MessageHandler(NVSEMessagingInterface::Message* msg) {
	if (msg->type == kMessage_MainGameLoop) MTAI::OnFrame();
	else if (msg->type == kMessage_DeferredInit) {
		CalibrateClock();
		if (!MTAI::Parallel::WantHook()) LogLine("Enabled = 0 and ScrollLockToggle = 0: nothing is hooked; the game runs as before.");
		else if (const char* what = MTAI::Parallel::LoopChanged())
			LogLine("REFUSED: the game code it copies changed (%s): another plugin patched it. Nothing is hooked; the game runs as before.", what);
		else if (MTAI::InstallPairs()) MTAI::Parallel::Install();
		LogFlush();
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) Self = instance;
	return TRUE;
}

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = 2;
	info->name = kPluginName;
	info->version = kPluginVersion;
	return !nvse->isEditor && nvse->runtimeVersion == 0x040020D0; // 1.4.0.525
}

__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
	if (nvse->isEditor) return true;
	LARGE_INTEGER q; QueryPerformanceCounter(&q);
	LoadQpc = q.QuadPart; LoadTsc = __rdtsc();
	const std::string settingsPath = ReadSettings(Self);
	Log = _fsopen((GameFolder() + "MultithreadedAI.log").c_str(), "w", _SH_DENYWR);
	SYSTEMTIME t; GetLocalTime(&t);
	LogLine("FNV Multithreaded AI %u (EXPERIMENTAL), started %04u-%02u-%02u %02u:%02u:%02u", kPluginVersion, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	LogLine("settings %s: Enabled %d, ScrollLockToggle %d, Threads %d (0 = automatic), MinChecks %d, SerialChecks %d, LogSummarySeconds %d",
		settingsPath.c_str(), Config.parLevel, Config.parHotkey, Config.parThreads, Config.parMinJobs, Config.parSerialChecks, Config.summarySeconds);
	PluginHandle = nvse->GetPluginHandle();
	NVSEMessagingInterface* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	if (!messaging || !messaging->RegisterListener(PluginHandle, "NVSE", MessageHandler))
		LogLine("could not register for xNVSE messages; nothing will be hooked");
	LogFlush();
	return true;
}
}
