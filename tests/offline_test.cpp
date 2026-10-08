// Offline test of FNV Multithreaded AI's batching, outside the game: the plugin source with the game's functions
// replaced by stand-ins (a thread-safe fake check, a fake high-process array). Checks that every answer equals the one-by-one
// result, each check runs once, batches run only inside the detection pass, and the hooks keep the game's calling convention.
// Build + run: tests/run_offline_test.ps1
#define MTAI_TEST
#include "../src/main.cpp"

#include <thread>
#include <functional>

static int Failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); Failures++; } } while (0)

// A stand-in for Actor::GetDetectionLevelAgainstActor: thiscall, 7 stack args.
struct FakeActor {
	int id;
	__declspec(noinline) int __thiscall Level(int a1, FakeActor* target, UInt8* seen, int targetInCombat, int a5, int a6, UInt8* selfCombat);
};
static int FakeCalls;
int __thiscall FakeActor::Level(int a1, FakeActor* target, UInt8* seen, int targetInCombat, int a5, int a6, UInt8* selfCombat) {
	{
		FakeCalls++;
		*seen = 42;
		return id * 100 + target->id + a1 + targetInCombat + a5 + a6 + (selfCombat ? *selfCombat : 0);
	}
};
static auto FakeLevelPtr = &FakeActor::Level;

// ParallelDetection stand-ins: a thread-safe check, a high-process array, distances = 100 x id difference
static volatile LONG ParCalls;
static volatile LONG ParThreadIds[64];
struct ParActor : FakeActor {
	__declspec(noinline) int __thiscall ParLevel(int a1, ParActor* target, UInt8* seen, int targetInCombat, int a5, int a6, UInt8* selfCombat) {
		const LONG n = InterlockedIncrement(&ParCalls);
		if (n <= 64) ParThreadIds[n - 1] = (LONG)GetCurrentThreadId();
		const UInt64 until = __rdtsc() + (UInt64)(0.05 * TicksPerMs);   // ~50 us, like a light check
		while (__rdtsc() < until) {}
		*seen = (UInt8)(target->id & 1);
		const int r = id * 1000 + target->id * 10 + a1 + targetInCombat + a5 + a6 + *selfCombat;
		if (target->id % 4 == 0) *selfCombat = 1;
		return r;
	}
	__declspec(noinline) bool __thiscall Disabled() { return id == 30; }
	__declspec(noinline) float __thiscall Distance(ParActor* to, int, int) { return 100.0f * (float)abs(id - to->id); }
	__declspec(noinline) void* __thiscall Cell() { return nullptr; }
};
static int ExpectedPar(ParActor* self, ParActor* t, int tic, UInt8 selfCombat, UInt8* seen, UInt8* selfOut) {
	*seen = (UInt8)(t->id & 1);
	*selfOut = t->id % 4 == 0 ? 1 : selfCombat;
	return self->id * 1000 + t->id * 10 + 1 + tic + selfCombat;
}
struct ParArray {
	ParActor* items[40]; UInt32 n;
	__declspec(noinline) UInt32 __thiscall Count(int) { return n; }
	__declspec(noinline) UInt8* __thiscall At(UInt32 i) { return (UInt8*)items[i]; }
};
static ParArray TheParArray;
struct ParPM { __declspec(noinline) void* __thiscall High() { return &TheParArray; } };
struct ParActor;
static std::function<void(ParActor*)> ParRunBody;   // stands in for the rest of HighProcess::RunDetection
struct ParProcess {
	__declspec(noinline) bool __thiscall ShouldRun(void*, void*) { return true; }
	__declspec(noinline) void __thiscall TestRun(ParActor* actor, float) { ParRunBody(actor); }
};
static ParProcess TheParProcess;
static UInt32 ParRun;
static bool ParUsable(UInt8*) { return true; }
static bool ParInCombat(UInt8* a) { return ((ParActor*)a)->id % 3 == 0; }
static void* ParProcessOf(UInt8*) { return &TheParProcess; }
static UInt32 ParRunNumber(UInt8*) { return ParRun; }
static bool ParSwimming(UInt8*) { return false; }
static float ParRange() { return 1000.0f; }   // doubled outside (no cell here): 2000 = 20 ids
static bool ParSelfInCombat(UInt8*) { return false; }
static bool ParDue(UInt8* a) { const int id = ((ParActor*)a)->id; return id % 5 == 0 && id != 30; }
static bool ParAllowed() { return true; }
// test pool: plain threads woken by events
static HANDLE ParStartEv[3], ParDoneEv[3];
static DWORD WINAPI ParWorkerProc(LPVOID p) {
	const int i = (int)(intptr_t)p;
	for (;;) { WaitForSingleObject(ParStartEv[i], INFINITE); MTAI::Parallel::WorkerTask(); SetEvent(ParDoneEv[i]); }
}
static void ParStart(int i) { SetEvent(ParStartEv[i]); }
static void ParWait(int i) { WaitForSingleObject(ParDoneEv[i], INFINITE); }
struct FakeCombat { __declspec(noinline) bool __thiscall FightsWith(void*) { return true; } };
static auto FakeFightsPtr = &FakeCombat::FightsWith;
// Calls whatever address the game would call, with the game's argument order (1, target, &seen, 3, 0, 0, &combat) and ecx = self,
// then checks esp is back where it was.
static __declspec(noinline) int CallPairLevelLikeTheGame(FakeActor* self, FakeActor* target, UInt8* seen, UInt8* combat, int targetInCombat = 3) {
	int result = 0, espBefore = 0, espAfter = 0;
	void* fn = (void*)MTAI::PairLevel;
	__asm {
		mov espBefore, esp
		push combat
		push 0
		push 0
		push targetInCombat
		push seen
		push target
		push 1
		mov ecx, self
		call fn
		mov result, eax
		mov espAfter, esp
	}
	if (espBefore != espAfter) { printf("FAIL: stack off by %d bytes after PairLevel\n", espBefore - espAfter); Failures++; }
	return result;
}

int main() {
	TicksPerMs = 0; { LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a); UInt64 t = __rdtsc(); Sleep(50); QueryPerformanceCounter(&b); TicksPerMs = double(__rdtsc() - t) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart)); }
	printf("clock %.3f GHz\n", TicksPerMs / 1e6);
	// 1d. ParallelDetection: the game's loop over 40 actors; every answer must equal the one-by-one result, each check
	// is run once (on several threads), changed arguments and a new run fall back / rebuild.
	{
		namespace P = MTAI::Parallel;
		static ParActor actors[40];
		for (int i = 0; i < 40; i++) { actors[i].id = i; TheParArray.items[i] = &actors[i]; }
		TheParArray.n = 40;
		auto lvl = &ParActor::ParLevel; MTAI::OrigGetLevel = (MTAI::GetLevelFn)*(void**)&lvl;
		auto cnt = &ParArray::Count; P::Game.ArrayCount = (decltype(P::Game.ArrayCount))*(void**)&cnt;
		auto at = &ParArray::At; P::Game.ArrayAt = (decltype(P::Game.ArrayAt))*(void**)&at;
		auto hi = &ParPM::High; P::Game.HighArray = (decltype(P::Game.HighArray))*(void**)&hi;
		auto dis = &ParActor::Disabled; P::Game.IsDisabled = (decltype(P::Game.IsDisabled))*(void**)&dis;
		auto dst = &ParActor::Distance; P::Game.Distance = (decltype(P::Game.Distance))*(void**)&dst;
		auto run = &ParProcess::ShouldRun; P::Game.ShouldRunCombat = (decltype(P::Game.ShouldRunCombat))*(void**)&run;
		auto cel = &ParActor::Cell; P::Game.ParentCell = (decltype(P::Game.ParentCell))*(void**)&cel;
		P::Game.Usable = ParUsable; P::Game.TargetInCombat = ParInCombat; P::Game.ProcessOf = ParProcessOf;
		P::Game.RunNumber = ParRunNumber; P::Game.Swimming = ParSwimming; P::Game.Range = ParRange;
		P::Game.SelfInCombat = ParSelfInCombat; P::Game.DueNow = ParDue; P::Game.DetectionAllowed = ParAllowed;
		static void* noPlayer2 = nullptr;
		MTAI::PlayerPtrAddress = (UInt32)&noPlayer2;
		for (int i = 0; i < 3; i++) { ParStartEv[i] = CreateEventA(NULL, FALSE, FALSE, NULL); ParDoneEv[i] = CreateEventA(NULL, FALSE, FALSE, NULL); CreateThread(NULL, 0, ParWorkerProc, (LPVOID)(intptr_t)i, 0, NULL); }
		P::WorkerCount = 3; P::StartWorker = ParStart; P::WaitWorker = ParWait;
		P::Ready = true; P::Level = 1; Config.parMinJobs = 4;
		P::InPass = 1;   // this thread plays AI thread 2 inside RunDetectionForAllActors (1g: outside it)
		ParActor* self = &actors[5];
		// the game's loop for one run: same filters as HighProcess::RunDetection
		auto gameLoop = [&](UInt8 selfCombat, int flipTic, int* wrong) {
			for (int i = 0; i < 40; i++) {
				ParActor* t = &actors[i];
				if (t == self || t->id == 30) continue;
				if (100.0f * abs(t->id - self->id) > 2000.0f) continue;
				int tic = ParInCombat((UInt8*)t) ? 1 : 0;
				if (flipTic) tic ^= 1;
				UInt8 seen = 0xEE, combat = selfCombat, eSeen, eCombat;
				const int r = CallPairLevelLikeTheGame((FakeActor*)self, (FakeActor*)t, &seen, &combat, tic);
				const int e = ExpectedPar(self, t, tic, selfCombat, &eSeen, &eCombat);
				if (r != e || seen != eSeen || combat != eCombat) (*wrong)++;
			}
		};
		const int eligible = 25;   // ids 0..25 without 5
		ParCalls = 0; ParRun = 1; MTAI::FrameNo = 100;
		int wrong = 0;
		gameLoop(0, 0, &wrong);
		CHECK(wrong == 0);
		CHECK(ParCalls == eligible);                    // each check once, none again from the loop
		CHECK(P::WinAnswered == eligible);
		int threads = 0;
		for (int i = 0; i < eligible && i < 64; i++) {
			bool dup = false;
			for (int j = 0; j < i; j++) if (ParThreadIds[j] == ParThreadIds[i]) dup = true;
			if (!dup) threads++;
		}
		printf("parallel detection: %d checks on %d threads\n", (int)ParCalls, threads);
		CHECK(threads >= 2);
		// same run again with other arguments: the batch is used up / different, the game's own calls answer, still right
		wrong = 0;
		gameLoop(0, 1, &wrong);
		CHECK(wrong == 0);
		CHECK(ParCalls == 2 * eligible);
		// a new run (the counter moved), self in combat: a fresh batch, answered in parallel again
		ParRun = 2; ParCalls = 0; P::WinAnswered = 0;
		wrong = 0;
		gameLoop(1, 0, &wrong);
		CHECK(wrong == 0);
		CHECK(ParCalls == eligible);
		CHECK(P::WinAnswered == eligible);
		// off: one by one
		P::Level = 0; ParRun = 3; ParCalls = 0; P::WinAnswered = 0;
		wrong = 0;
		gameLoop(0, 0, &wrong);
		CHECK(wrong == 0 && ParCalls == eligible && P::WinAnswered == 0);

		// 1f. serial checks (diagnostic switch): the same answers, and every check runs on this thread (no worker runs one)
		Config.parSerialChecks = true;
		P::Level = 1; ParRun = 5; ParCalls = 0; P::WinAnswered = 0; P::WinCross = 0;
		wrong = 0;
		gameLoop(0, 0, &wrong);
		bool allHere = true;
		for (int i = 0; i < eligible; i++) if (ParThreadIds[i] != GetCurrentThreadId()) allHere = false;
		Config.parSerialChecks = false;
		printf("serial checks: %d checks, all on this thread: %s\n", (int)ParCalls, allHere ? "yes" : "no");
		CHECK(wrong == 0);
		CHECK(ParCalls == eligible);
		CHECK(P::WinAnswered == eligible);
		CHECK(allHere);

		// 1e. a whole detection pass: NPCs 0, 5, 10, ... (not 30) are due this frame; the first one's run start batches
		// them all at once, every later run only takes answers
		P::Level = 1; ParRun = 4; ParCalls = 0; P::WinAnswered = 0; P::WinCross = 0;
		MTAI::FrameNo = 200;
		auto tr = &ParProcess::TestRun; P::OrigRun = (P::RunFn)*(void**)&tr;
		wrong = 0;
		ParRunBody = [&](ParActor* a) { self = a; gameLoop(0, 0, &wrong); };
		int expected = 0;
		for (int i = 0; i < 40; i++) {
			if (!ParDue((UInt8*)&actors[i])) continue;
			for (int t = 0; t < 40; t++) if (t != i && t != 30 && abs(t - i) <= 20) expected++;
			P::RunHook(&TheParProcess, nullptr, (UInt8*)&actors[i], 0.5f);   // as RunDetectionForAllActors calls each due NPC
		}
		printf("whole pass: %d checks expected, %d run, %d answered, %d batches\n", expected, (int)ParCalls, (int)P::WinAnswered, (int)P::WinCross);
		CHECK(wrong == 0);
		CHECK(ParCalls == expected);           // each pair once
		CHECK(P::WinAnswered == expected);     // all from the batch
		CHECK(P::WinCross == 1);               // one batch for the whole pass

		// 1g. the same runs outside RunDetectionForAllActors (a follower's run from AI thread 1's package code): no batch,
		// no answers from a store, every check by the game one by one, each run counted as outside
		P::InPass = 0;
		P::Level = 1; ParRun = 6; ParCalls = 0; P::WinAnswered = 0; P::WinCross = 0; P::WinOutside = 0;
		MTAI::FrameNo = 300;
		wrong = 0;
		int dueRuns = 0;
		for (int i = 0; i < 40; i++) {
			if (!ParDue((UInt8*)&actors[i])) continue;
			dueRuns++;
			P::RunHook(&TheParProcess, nullptr, (UInt8*)&actors[i], 0.5f);
		}
		printf("outside the pass: %d checks expected, %d run, %d answered, %d batches, %d runs counted outside\n", expected, (int)ParCalls, (int)P::WinAnswered, (int)P::WinCross, (int)P::WinOutside);
		CHECK(wrong == 0);
		CHECK(ParCalls == expected);
		CHECK(P::WinAnswered == 0);
		CHECK(P::WinCross == 0);
		CHECK(P::WinOutside == dueRuns);

		// 1h. the pass wrapper: the flag is up only during RunDetectionForAllActors, ecx goes through
		{
			static int inPassSeen = -1;
			static void* ecxSeen = nullptr;
			struct FakePass { __declspec(noinline) void __thiscall Run() { inPassSeen = P::InPass; ecxSeen = this; } };
			auto fp = &FakePass::Run; P::PrevPassAI2 = (P::PassFn)*(void**)&fp;
			FakePass pm;
			P::PassAI2(&pm);
			CHECK(inPassSeen == 1 && ecxSeen == &pm && P::InPass == 0);
		}

		// 1i. the worn-armor wrapper: called the game's way (ecx = target, push 2, callee pops 4), and never two threads
		// inside the game's function at once
		{
			static volatile LONG inside, maxInside, calls;
			struct FakeArmor {
				__declspec(noinline) void* __thiscall Get(int slot) {
					const LONG n = InterlockedIncrement(&inside);
					if (n > maxInside) maxInside = n;
					const UInt64 until = __rdtsc() + (UInt64)(0.02 * TicksPerMs);
					while (__rdtsc() < until) {}
					InterlockedDecrement(&inside);
					InterlockedIncrement(&calls);
					return (void*)((UInt32)this + slot);
				}
			};
			auto g = &FakeArmor::Get; P::OrigArmor = (P::ArmorFn)*(void**)&g;
			static FakeArmor fa;
			void* r = nullptr; int espBefore = 0, espAfter = 0;
			void* fn = (void*)P::ArmorBeingWorn; FakeArmor* pfa = &fa;
			__asm {
				mov espBefore, esp
				push 2
				mov ecx, pfa
				call fn
				mov r, eax
				mov espAfter, esp
			}
			CHECK(r == (void*)((UInt32)&fa + 2) && espBefore == espAfter);
			std::vector<std::thread> ts;
			for (int t = 0; t < 4; t++) ts.emplace_back([] { for (int i = 0; i < 200; i++) P::ArmorBeingWorn(&fa, nullptr, 2); });
			for (auto& t : ts) t.join();
			printf("worn-armor lock: %d calls, at most %d inside at once\n", (int)calls, (int)maxInside);
			CHECK(maxInside == 1 && calls == 801);

			// the fix-up counter: SetWorn(0, 0, 1) the game's way (push 1, 0, 0; callee pops 12), passed through and counted
			static int argsSeen[3]; static void* itemSeen;
			struct FakeItem { __declspec(noinline) void __thiscall SetWorn(int a, int b, int c) { itemSeen = this; argsSeen[0] = a; argsSeen[1] = b; argsSeen[2] = c; } };
			auto sw = &FakeItem::SetWorn; P::OrigSetWorn = (P::SetWornFn)*(void**)&sw;
			static FakeItem item;
			P::WinFixups = 0;
			void* fn2 = (void*)P::CountFixup; FakeItem* pitem = &item;
			__asm {
				mov espBefore, esp
				push 1
				push 0
				push 0
				mov ecx, pitem
				call fn2
				mov espAfter, esp
			}
			CHECK(espBefore == espAfter && itemSeen == &item && argsSeen[0] == 0 && argsSeen[1] == 0 && argsSeen[2] == 1 && P::WinFixups == 1);
		}

		// 1j. CheckLock 2: the line-of-sight wrapper is called the game's way (ecx = this NPC, push 5, callee pops 0x14, al =
		// result) and passes everything through; the rest of each check never has two threads inside at once, while line of
		// sight does run on several threads at the same time
		{
			static volatile LONG inLocked, maxLocked, inSight, maxSight, sights;
			static int argsSeen[5]; static void* selfSeen; static void* targetSeen;
			auto spin = [](double ms) { const UInt64 until = __rdtsc() + (UInt64)(ms * TicksPerMs); while (__rdtsc() < until) {} };
			struct FakeSight {
				__declspec(noinline) UInt8 __thiscall See(int a1, void* target, int a3, int* a4, int a5) {
					const LONG n = InterlockedIncrement(&inSight);
					if (n > maxSight) InterlockedExchange(&maxSight, n);
					const UInt64 until = __rdtsc() + (UInt64)(0.05 * TicksPerMs);
					while (__rdtsc() < until) {}
					InterlockedDecrement(&inSight);
					InterlockedIncrement(&sights);
					selfSeen = this; targetSeen = target; argsSeen[0] = a1; argsSeen[1] = a3; argsSeen[2] = *a4; argsSeen[3] = a5;
					return (UInt8)(a1 + a3 + a5 + 0x10);
				}
			};
			auto see = &FakeSight::See; P::OrigSight = (P::SightFn)*(void**)&see;
			static FakeSight fs; static int typeArg = 7;
			const int keep = Config.checkLock;
			Config.checkLock = 2;
			// the call layout, outside any check (no lock held)
			UInt8 r8 = 0; int espBefore = 0, espAfter = 0;
			void* fn = (void*)P::SightOutsideLock; FakeSight* pfs = &fs; void* tgt = (void*)0x1234; int* pType = &typeArg;
			__asm {
				mov espBefore, esp
				push 1
				push pType
				push 1
				push tgt
				push 1
				mov ecx, pfs
				call fn
				mov r8, al
				mov espAfter, esp
			}
			CHECK(espBefore == espAfter && r8 == 0x13 && selfSeen == &fs && targetSeen == (void*)0x1234 && argsSeen[0] == 1 && argsSeen[1] == 1 && argsSeen[2] == 7 && argsSeen[3] == 1);
			// four threads, each check = locked part, line of sight, locked part
			sights = 0;
			auto locked = [&] {
				const LONG n = InterlockedIncrement(&inLocked);
				if (n > maxLocked) InterlockedExchange(&maxLocked, n);
				spin(0.005);
				InterlockedDecrement(&inLocked);
			};
			std::vector<std::thread> ts;
			for (int t = 0; t < 4; t++) ts.emplace_back([&] {
				for (int i = 0; i < 200; i++) {
					P::LockCheck();
					locked();
					CHECK(P::SightOutsideLock(&fs, nullptr, 1, nullptr, 1, &typeArg, 1) == 0x13);
					locked();
					P::UnlockCheck();
				}
			});
			for (auto& t : ts) t.join();
			printf("line-of-sight lock: %d checks, rest of the check at most %d inside at once, line of sight up to %d at once\n", (int)sights, (int)maxLocked, (int)maxSight);
			CHECK(sights == 800 && maxLocked == 1 && maxSight >= 2);
			Config.checkLock = keep;
		}
		P::Ready = false;
	}

	printf(Failures ? "%d check(s) FAILED\n" : "all offline tests passed\n", Failures);
	return Failures ? 1 : 0;
}
