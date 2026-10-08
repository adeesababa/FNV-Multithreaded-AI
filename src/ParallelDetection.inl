// Multithreaded detection: NPC-vs-NPC detection checks run on several CPU cores at once.
// Included inside namespace MTAI (main.cpp), after the game addresses and before the pair hook.
// Game addresses are for FalloutNV.exe 1.4.0.525; every patched spot is byte-checked first (LoopChanged, RedirectCall).
//
// What the game does: ProcessLists::RunDetectionForAllActors 0x0096C330 (AI thread 2, or the combined AI thread) calls
// HighProcess::RunDetection 0x008E40D0 for each NPC whose detection timer ran out. That loops over the high-process
// array and calls Actor::GetDetectionLevelAgainstActor 0x008A0D10 at 0x008E494B for every actor in range, one after
// another (line-of-sight ray casts; ~0.15-0.2 ms each, 1,000+ per frame right after a few hundred NPCs appear). The
// loop's own writes (the NPC's detection list, the target's 'detected by' entry) come AFTER each call, from its result.
//
// What this does: when the first NPC's run of a detection pass starts (HighProcess vtable +0x2F0, wrapped), it takes that
// NPC and every later actor that RunDetectionForAllActors will run now (the pass's own test, 0x0096C3EC-0x0096C4E6:
// usable, +0xBC set, 3D loaded, not the player, detection timer (vtable +0x2B4) below 0, not unconscious, not dead; no
// fader, the flag at 0x011F1221). Phase 1 finds each NPC's checks with the loop's own filters (0x008E47C8-0x008E4931:
// not disabled, not this NPC, ShouldRunCombatDetection when in combat, in range (Setting 0x011CD7D8, doubled outside),
// the target within 8192 of the player, farther than 2) and arguments (target in combat = actor vtable +0x274(1)), plus
// the NPC-vs-player check RunDetection makes before its loop (0x008E4359-0x008E439C). Phase 2 runs all those checks on
// worker threads plus this thread. Each NPC's run then takes its answers (same target and arguments, used once);
// anything not in the batch is checked by the game as before. The same checks with the same inputs: same results.
//
// Why that is safe enough to try (it is EXPERIMENTAL):
//  - With a1 = 1, a5 = 0 and an NPC as 'this', the check reads game state, plus: two debug counters, a lazily cached
//    actor height (same value), the NPC's combat controller 'target segment in view' (one field), a 5-slot static
//    position ring used only for swimming NPCs (those are left to the game), and the worn-armor lookup below.
//  - Batches run ONLY inside RunDetectionForAllActors, on the thread running it (InPass, set around its calls at
//    0x008C7F84 and 0x008C7CA6). RunDetection is also called elsewhere -- e.g. package procedures on AI thread 1 run the
//    detection of a leader's followers (HighProcess::UpdateFollowers 0x00900830) while AI thread 2 may be stepping Havok;
//    a batch there cast hundreds of rays against a world being changed. Those runs stay the game's own.
//  - The worn-armor lookup (Actor::GetArmorBeingWorn at 0x008A107A, boot weight -> noise) is not read-only: for an item
//    with a leveled extra and a negative count, InventoryChanges::GetWornItem calls ItemChange::SetWorn(0, 0, 1)
//    (0x004C8CCA), which removes and deletes a worn entry of the target's real inventory. Two threads checking the same
//    target could both delete it (heap corruption). That call is serialized (ArmorLock).
//  - The workers are the game's own AILinearTaskThread (0x008C7630) with OnStartup replicated (Havok thread memory,
//    its router, a 200 KB Havok stack). Havok's memory system has 32 thread slots; the workers always leave 10 free.
namespace Parallel {
	static int Level;                    // on/off (Scroll Lock when ParallelDetectionScrollLockKey is set)
	static bool Ready;                   // the pair hook is in and the loop's bytes are as checked
	static bool KeyWasDown;
	static __declspec(thread) int InPass;   // this thread is inside ProcessLists::RunDetectionForAllActors

	// the game, as a table so the offline test can stand in for it
	struct GameApi {
		void* (__thiscall* HighArray)(void* processManager);           // 0x00717E50 (RunDetection: mov ecx, 0x011E0E80; call)
		UInt32(__thiscall* ArrayCount)(void* array, int level);        // 0x005BE5C0 ProcessArray::GetProcessLevelTail
		UInt8* (__thiscall* ArrayAt)(void* array, UInt32 index);        // 0x00968670
		bool(__thiscall* IsDisabled)(void* ref);                        // 0x00440DA0 TESForm::GetDisabled
		float(__thiscall* Distance)(void* from, void* to, int, int);    // 0x005723B0 TESObjectREFR::GetDistanceFromReference
		bool(__thiscall* ShouldRunCombat)(void* process, void* self, void* target);  // 0x008FFD10
		void* (__thiscall* ParentCell)(void* ref);                      // 0x008D6F30
		bool(__thiscall* IsInterior)(void* cell);                       // 0x00425FD0
		bool (*Usable)(UInt8* actor);                                   // actor vtable +0x100 (the loop's check)
		bool (*TargetInCombat)(UInt8* actor);                           // actor vtable +0x274(1)
		void* (*ProcessOf)(UInt8* actor);                               // Actor +0x68
		UInt32(*RunNumber)(UInt8* actor);                               // HighProcess +0x3D8: RunDetection's run counter (bumped at its end)
		bool (*Swimming)(UInt8* actor);                                 // Actor::IsSwimming 0x005A2030 reads +0x14D
		float (*Range)();                                               // Setting 0x011CD7D8, value at +4
		bool (*SelfInCombat)(UInt8* actor);                             // Actor::IsInCombat 0x00493BB0 (RunDetection, 0x008E41C1)
		bool (*DueNow)(UInt8* actor);                                   // RunDetectionForAllActors' 'run it now' test
		bool (*DetectionAllowed)();                                     // no fader and the flag RunDetection needs
		UInt32 processManager;
	};
	typedef bool(__thiscall* VBool)(void*);
	static bool __cdecl GameUsable(UInt8* a) { return ((VBool)(*(void***)a)[0x100 / 4])(a); }
	static bool __cdecl GameTargetInCombat(UInt8* a) { typedef bool(__thiscall* Fn)(void*, int); return ((Fn)(*(void***)a)[0x274 / 4])(a, 1); }
	static void* __cdecl GameProcessOf(UInt8* a) { return *(void**)(a + 0x68); }
	static UInt32 __cdecl GameRunNumber(UInt8* a) { UInt8* p = *(UInt8**)(a + 0x68); return p ? *(UInt32*)(p + 0x3D8) : 0; }
	static bool __cdecl GameSwimming(UInt8* a) { return a[0x14D] != 0; }
	static float __cdecl GameRange() { return *(float*)(0x011CD7D8 + 4); }
	static bool __cdecl GameSelfInCombat(UInt8* a) { return ((VBool)0x00493BB0)(a); }
	static bool __cdecl GameDueNow(UInt8* a) {
		void** vt = *(void***)a;
		if (!((VBool)vt[0x100 / 4])(a) || !a[0xBC]) return false;
		if (!((void* (__thiscall*)(void*))vt[0x1D0 / 4])(a)) return false;                    // Get3D
		if (!(((float(__thiscall*)(void*))vt[0x2B4 / 4])(a) < 0.0f)) return false;           // Actor::GetDetectionTimer
		if (((VBool)0x00437BD0)(a)) return false;                                            // unconscious (0x0096C4BF)
		return !((bool(__thiscall*)(void*, int))vt[0x22C / 4])(a, 0);                        // dead
	}
	static bool __cdecl GameDetectionAllowed() {
		const float alpha = ((float(__thiscall*)(void*, int))0x007014E0)(*(void**)0x011D8804, 1);   // FaderManager::GetFaderAlpha(1)
		return !(alpha > 0.0f) && *(UInt8*)0x011F1221 != 0;
	}
	static GameApi Game = {
		(void* (__thiscall*)(void*))0x00717E50, (UInt32(__thiscall*)(void*, int))0x005BE5C0, (UInt8 * (__thiscall*)(void*, UInt32))0x00968670,
		(bool(__thiscall*)(void*))0x00440DA0, (float(__thiscall*)(void*, void*, int, int))0x005723B0,
		(bool(__thiscall*)(void*, void*, void*))0x008FFD10, (void* (__thiscall*)(void*))0x008D6F30, (bool(__thiscall*)(void*))0x00425FD0,
		GameUsable, GameTargetInCombat, GameProcessOf, GameRunNumber, GameSwimming, GameRange,
		GameSelfInCombat, GameDueNow, GameDetectionAllowed, 0x011E0E80 };

	// one check, filled in by whichever thread runs it
	struct Job {
		UInt8* self;
		UInt8* target;
		int targetInCombat;
		UInt8 selfInCombatIn;
		UInt8 a6;                             // the player pair passes 'this in combat' here (unused by the function)
		bool playerPair;                      // RunDetection's check against the player before its loop (0x008E439C)
		int level;
		UInt8 seen, selfInCombatOut;
		bool used;
		UInt64 ticks;
	};
	static UInt64 Key(UInt8* self, UInt8* target) { return ((UInt64)(UInt32)self << 32) | (UInt32)target; }
	// this frame's checks, batch after batch; AI thread 2 only (BatchLock), workers fill results inside a batch
	static std::vector<Job> Jobs;
	static std::unordered_map<UInt64, UInt32> JobIndex;   // (NPC, target) -> Jobs index
	static std::unordered_map<UInt8*, UInt32> Covered;    // NPC -> its run counter when its checks were collected
	static UInt32 StoreFrame = 0xFFFFFFFF;
	static volatile LONG NextJob;
	static LONG JobEnd;
	struct SelfWork { UInt8* self; UInt8 inCombat; bool skip; std::vector<Job> jobs; };
	static std::vector<SelfWork> Selves;
	static volatile LONG NextSelf;
	static LONG SelfEnd;
	static void* CurArray; static UInt32 CurCount; static UInt8* CurPlayer;
	static volatile LONG Phase;           // 1 = collect each NPC's checks, 2 = run the checks
	static SRWLOCK BatchLock = SRWLOCK_INIT;

	// [Advanced] CheckLock. 2 (default) = each check runs one at a time except its line-of-sight part (Actor::LineOfSight,
	// called at 0x008A0FB1: thiscall(ecx = this NPC, 1, target, 1, &type, 1), ret 0x14, result in al), which the threads run
	// at the same time (SightOutsideLock). Line of sight is ~99% of a check's time, so this keeps the speed-up. Running the
	// rest at the same time too (0) crashed in big human fights (Havok heightfield data overwritten, 2026-10-08), while
	// whole checks one at a time (1) and this mode (2) ran clean in the same fight. 1 = whole checks one at a time.
	static SRWLOCK CheckLockS = SRWLOCK_INIT;
	static __declspec(thread) int HoldsCheckLock;
	static inline void LockCheck() { if (Config.checkLock) { AcquireSRWLockExclusive(&CheckLockS); HoldsCheckLock = 1; } }
	static inline void UnlockCheck() { if (Config.checkLock) { HoldsCheckLock = 0; ReleaseSRWLockExclusive(&CheckLockS); } }
	static const UInt32 kSightSite = 0x008A0FB1, kLineOfSight = 0x0088B880;
	typedef UInt8(__thiscall* SightFn)(void* self, int a1, void* target, int a3, int* a4, int a5);
	static SightFn OrigSight = (SightFn)kLineOfSight;
	static volatile LONGLONG WinSightTicks;
	// line-of-sight self-check: how often line of sight answers 'visible' in the game's own checks (bucket 0), in the batch
	// on the AI thread (1) and in the batch on the helpers (2); 1 and 2 get the same mix of pairs (jobs go to whichever
	// thread is free), so their rates should match. [Advanced] VerifySight also records a sample of the batch's calls and
	// re-runs them on the AI thread right after the batch (VerifySightSamples), counting the answers that differ.
	static __declspec(thread) int IsWorker;   // a detection worker thread
	static __declspec(thread) int InJobs;     // this thread is running batch checks (RunJobs)
	static volatile LONG WinSightN[3], WinSightYes[3];
	struct SightSample { void* self; void* target; int a1, a3, a5, typeIn, typeOut; UInt8 r, onWorker; };
	static const int kSightSamples = 16;
	static SightSample SightSamples[kSightSamples];
	static volatile LONG SightSampleTick, SightSampleCount;
	static volatile LONG WinVerify[2], WinVerifyDiff[2], WinVerifyTypeDiff[2];
	static UInt8 __fastcall SightOutsideLock(void* self, void*, int a1, void* target, int a3, int* a4, int a5) {
		const UInt64 t0 = __rdtsc();
		const int typeIn = a4 ? *a4 : 0;
		UInt8 r;
		if (HoldsCheckLock) {
			ReleaseSRWLockExclusive(&CheckLockS);
			r = OrigSight(self, a1, target, a3, a4, a5);
			AcquireSRWLockExclusive(&CheckLockS);
		}
		else r = OrigSight(self, a1, target, a3, a4, a5);
		InterlockedExchangeAdd64(&WinSightTicks, (LONGLONG)(__rdtsc() - t0));
		const int bucket = !InJobs ? 0 : IsWorker ? 2 : 1;
		InterlockedIncrement(&WinSightN[bucket]);
		if (r) InterlockedIncrement(&WinSightYes[bucket]);
		if (bucket && Config.verifySight && (InterlockedIncrement(&SightSampleTick) & 15) == 0) {
			const LONG k = InterlockedIncrement(&SightSampleCount) - 1;
			if (k < kSightSamples) SightSamples[k] = { self, target, a1, a3, a5, typeIn, a4 ? *a4 : 0, r, (UInt8)(IsWorker ? 1 : 0) };
		}
		return r;
	}
	// on the AI thread after the batch (the workers are done): the same calls again, the game's way, on the game's thread
	static void VerifySightSamples() {
		const LONG n = std::min((LONG)SightSampleCount, (LONG)kSightSamples);
		SightSampleCount = 0;
		for (LONG i = 0; i < n; i++) {
			const SightSample& s = SightSamples[i];
			int type = s.typeIn;
			const UInt8 r = OrigSight(s.self, s.a1, s.target, s.a3, &type, s.a5);
			InterlockedIncrement(&WinVerify[s.onWorker]);
			if (!r != !s.r) InterlockedIncrement(&WinVerifyDiff[s.onWorker]);
			if (type != s.typeOut) InterlockedIncrement(&WinVerifyTypeDiff[s.onWorker]);
		}
	}
	static void RunJobs() {
		InJobs = 1;
		for (;;) {
			const LONG i = InterlockedIncrement(&NextJob) - 1;
			if (i >= JobEnd) { InJobs = 0; return; }
			Job& j = Jobs[i];
			const UInt64 t0 = __rdtsc();
			j.seen = 0;                               // the loop clears its 'seen' byte before the call (0x008E48C3)
			j.selfInCombatOut = j.selfInCombatIn;
			LockCheck();
			j.level = OrigGetLevel(j.self, 1, j.target, &j.seen, j.targetInCombat, 0, j.a6, &j.selfInCombatOut);
			UnlockCheck();
			j.ticks = __rdtsc() - t0;
		}
	}

	// the loop's filters (0x008E47C8-0x008E4931) for one NPC, targets from array index 'from' on; withPlayer adds the
	// check against the player that RunDetection makes before its loop (0x008E4359-0x008E439C: when this NPC is in
	// combat only if ShouldRunCombatDetection(player); target in combat = Actor::IsInCombat(player); a6 = this in combat;
	// the last argument's byte starts as 'this in combat')
	static void CollectChecks(SelfWork& w, UInt32 from, bool withPlayer) {
		UInt8* self = w.self;
		float range = Game.Range();
		void* cell = Game.ParentCell(self);
		if (!cell || !Game.IsInterior(cell)) range += range;
		void* process = Game.ProcessOf(self);
		if (withPlayer && CurPlayer && CurPlayer != self && (!w.inCombat || Game.ShouldRunCombat(process, self, CurPlayer))) {
			Job j = {};
			j.self = self;
			j.target = CurPlayer;
			j.targetInCombat = Game.SelfInCombat(CurPlayer) ? 1 : 0;
			j.selfInCombatIn = w.inCombat;
			j.a6 = w.inCombat;
			j.playerPair = true;
			w.jobs.push_back(j);
		}
		for (UInt32 i = from; i < CurCount; i++) {
			UInt8* t = Game.ArrayAt(CurArray, i);
			if (!t || t == self || t == CurPlayer || Game.IsDisabled(t) || !Game.Usable(t)) continue;   // the player pair stays with the game
			const float d = Game.Distance(self, t, 0, 0);
			if (w.inCombat && !Game.ShouldRunCombat(process, self, t)) continue;
			if (range < d) continue;
			if (CurPlayer && !(Game.Distance(t, CurPlayer, 0, 0) <= 8192.0f)) continue;
			if (!(d > 2.0f)) continue;
			Job j = {};
			j.self = self;
			j.target = t;
			j.targetInCombat = Game.TargetInCombat(t) ? 1 : 0;
			j.selfInCombatIn = w.inCombat;
			w.jobs.push_back(j);
		}
	}
	static void RunCollect() {
		for (;;) {
			const LONG i = InterlockedIncrement(&NextSelf) - 1;
			if (i >= SelfEnd) return;
			SelfWork& w = Selves[i];
			if (w.skip) continue;
			w.inCombat = Game.SelfInCombat(w.self) ? 1 : 0;
			CollectChecks(w, 0, true);
		}
	}
	static void __cdecl WorkerTask() { IsWorker = 1; if (Phase == 1) RunCollect(); else RunJobs(); }   // AILinearTaskThread::RunLinearTasks calls [this+0x4C]() each start

	// the check's worn-armor lookup (0x008A107A: push 2; ecx = target; call Actor::GetArmorBeingWorn, ret 4), one at a time
	static SRWLOCK ArmorLock = SRWLOCK_INIT;
	static const UInt32 kArmorSite = 0x008A107A, kGetArmorBeingWorn = 0x00891B90, kFixupSite = 0x004C8CCA, kItemSetWorn = 0x004BE3D0;
	typedef void* (__thiscall* ArmorFn)(void* actor, int slot);
	static ArmorFn OrigArmor = (ArmorFn)kGetArmorBeingWorn;
	static volatile LONG WinFixups, WinFixupsOnWorkers;
	static void* __fastcall ArmorBeingWorn(void* actor, void*, int slot) {
		AcquireSRWLockExclusive(&ArmorLock);
		void* r = OrigArmor(actor, slot);
		ReleaseSRWLockExclusive(&ArmorLock);
		return r;
	}
	// [Advanced] PickLock (diagnostic): the check's line-of-sight ray cast (Actor::LineOfSight -> TES::HavokPick at
	// 0x0088C16A, thiscall(ecx = TES, pick data), ret 4) one thread at a time, for every caller.
	static SRWLOCK PickLock = SRWLOCK_INIT;
	static const UInt32 kPickSite = 0x0088C16A, kHavokPick = 0x00458420;
	typedef void* (__thiscall* PickFn)(void* tes, void* pickData);
	static PickFn OrigPick = (PickFn)kHavokPick;
	static void* __fastcall LockedPick(void* tes, void*, void* pickData) {
		AcquireSRWLockExclusive(&PickLock);
		void* r = OrigPick(tes, pickData);
		ReleaseSRWLockExclusive(&PickLock);
		return r;
	}
	// GetWornItem's fix-up (0x004C8CCA: ItemChange::SetWorn(0, 0, 1), ret 0xC): counted only, every caller passes through
	typedef void(__thiscall* SetWornFn)(void* item, int worn, int left, int remove);
	static SetWornFn OrigSetWorn = (SetWornFn)kItemSetWorn;
	static void __fastcall CountFixup(void* item, void*, int worn, int left, int remove) {
		InterlockedIncrement(&WinFixups);
		if (IsWorker) InterlockedIncrement(&WinFixupsOnWorkers);
		OrigSetWorn(item, worn, left, remove);
	}

	// the worker pool: game threads in the game, plain threads in the offline test
	static const int kMaxWorkers = 15, kDefaultMaxWorkers = 8;
	static void* Workers[kMaxWorkers];
	static int WorkerCount;
	static bool WorkersTried;
	static void (*StartWorker)(int i);
	static void (*WaitWorker)(int i);
	static void GameStart(int i) { ((void(__thiscall*)(void*))0x008C9FB0)(Workers[i]); }   // BSLinearTaskThread::StartProcessingTasks
	static void GameWait(int i) { ((void(__thiscall*)(void*))0x008C7490)(Workers[i]); }    // BSLinearTaskThread::WaitForCompletion

	static int RunPool(LONG phase, LONG items, int perThread) {
		Phase = phase;
		const int use = Config.parSerialChecks && phase == 2 ? 0 : std::min(WorkerCount, (int)(items / perThread));   // diagnostic: no worker runs the checks
		for (int w = 0; w < use; w++) StartWorker(w);
		if (phase == 1) RunCollect(); else RunJobs();
		for (int w = 0; w < use; w++) WaitWorker(w);
		return use;
	}

	static volatile LONG WinRuns, WinCross, WinCrossSelves, WinSmall, WinJobs, WinAnswered, WinNoEntry, WinDifferent, WinUnused, WinWorkersUsed, WinSerialCalls, WinOutside;
	static volatile LONGLONG WinWallTicks, WinJobTicks, WinWallMax;

	static void NewFrameCheck() {
		const UInt32 frame = (UInt32)FrameNo;
		if (StoreFrame == frame) return;
		LONG unused = 0;
		for (const Job& j : Jobs) if (!j.used) unused++;
		if (unused) InterlockedExchangeAdd(&WinUnused, unused);
		Jobs.clear(); JobIndex.clear(); Covered.clear();
		StoreFrame = frame;
	}

	// adds the collected checks to this frame's store and runs them on all threads
	static void AddAndRun(UInt64 t0) {
		const LONG begin = (LONG)Jobs.size();
		for (SelfWork& w : Selves) {
			Covered[w.self] = Game.RunNumber(w.self);
			if (w.skip) continue;
			if ((int)w.jobs.size() < Config.parMinJobs && Selves.size() == 1) { InterlockedIncrement(&WinSmall); continue; }   // one small NPC: the game does it
			for (Job& j : w.jobs) { JobIndex[Key(j.self, j.target)] = (UInt32)Jobs.size(); Jobs.push_back(j); }
		}
		const LONG count = (LONG)Jobs.size() - begin;
		if (count <= 0) return;
		NextJob = begin;
		JobEnd = begin + count;
		const int use = RunPool(2, count, 4);
		const LONGLONG wall = (LONGLONG)(__rdtsc() - t0);
		if (Config.verifySight) VerifySightSamples();
		LONGLONG cpu = 0;
		for (LONG i = begin; i < JobEnd; i++) cpu += (LONGLONG)Jobs[i].ticks;
		InterlockedIncrement(&WinRuns);
		InterlockedExchangeAdd(&WinJobs, count);
		InterlockedExchangeAdd(&WinWorkersUsed, use);
		InterlockedExchangeAdd64(&WinWallTicks, wall);
		InterlockedExchangeAdd64(&WinJobTicks, cpu);
		if (wall > WinWallMax) WinWallMax = wall;
	}

	static bool Prepare() {
		CurPlayer = *(UInt8**)PlayerPtrAddress;
		CurArray = Game.HighArray((void*)Game.processManager);
		if (!CurArray) return false;
		CurCount = Game.ArrayCount(CurArray, 0);
		return true;
	}
	static UInt32 IndexOf(UInt8* actor) {
		UInt32 k = 0;
		while (k < CurCount && Game.ArrayAt(CurArray, k) != actor) k++;
		return k;
	}

	// v1: one NPC whose run was not foreseen, from the loop's current target on
	static void BatchOne(UInt8* self, UInt8* first, UInt8 selfInCombat) {
		const UInt64 t0 = __rdtsc();
		Selves.clear();
		Covered[self] = Game.RunNumber(self);
		if (Game.Swimming(self) || !Prepare()) { InterlockedIncrement(&WinSmall); return; }
		const UInt32 k = IndexOf(first);
		if (k == CurCount) { InterlockedIncrement(&WinSmall); return; }
		Selves.push_back({ self, selfInCombat, false, {} });
		CollectChecks(Selves[0], k, false);   // its player pair has happened already
		AddAndRun(t0);
	}

	// v2: at the start of an NPC's run, that NPC and every later NPC the detection pass runs now
	static void BatchPass(UInt8* first) {
		const UInt64 t0 = __rdtsc();
		if (!Game.DetectionAllowed() || !Prepare()) return;
		const UInt32 a = IndexOf(first);
		if (a == CurCount) return;
		Selves.clear();
		for (UInt32 i = a; i < CurCount; i++) {
			UInt8* s = Game.ArrayAt(CurArray, i);
			if (!s || s == CurPlayer) continue;
			if (i > a && !Game.DueNow(s)) continue;
			if (i > a) { auto c = Covered.find(s); if (c != Covered.end() && c->second == Game.RunNumber(s)) continue; }
			// RunDetection returns before its loop for one farther than 8192 from the player (0x008E4168); swimming: left to the game
			const bool skip = Game.Swimming(s) || (CurPlayer && !(Game.Distance(s, CurPlayer, 0, 0) < 8192.0f));
			Selves.push_back({ s, 0, skip, {} });
		}
		for (SelfWork& w : Selves) w.jobs.reserve(64);
		NextSelf = 0;
		SelfEnd = (LONG)Selves.size();
		RunPool(1, SelfEnd, 2);   // phase 1 on the pool too: each NPC walks the whole array
		InterlockedIncrement(&WinCross);
		InterlockedExchangeAdd(&WinCrossSelves, SelfEnd);
		AddAndRun(t0);
	}

	// HighProcess vtable +0x2F0 (RunDetection, thiscall(actor, float), ret 8)
	typedef void(__thiscall* RunFn)(void*, void*, float);
	static RunFn OrigRun = (RunFn)0x008E40D0;
	static void __fastcall RunHook(void* process, void*, UInt8* actor, float t) {
		if (Ready && Level && (WorkerCount || Config.noHelpers) && actor && actor != *(UInt8**)PlayerPtrAddress) {
			if (!InPass) InterlockedIncrement(&WinOutside);   // e.g. a follower's run on AI thread 1: the game's, one by one
			else if (TryAcquireSRWLockExclusive(&BatchLock)) {
				NewFrameCheck();
				auto it = Covered.find(actor);
				if (it == Covered.end() || it->second != Game.RunNumber(actor)) BatchPass(actor);
				ReleaseSRWLockExclusive(&BatchLock);
			}
		}
		OrigRun(process, actor, t);
	}

	// around RunDetectionForAllActors (thiscall, ecx = process manager, no stack arguments): the only place batches run
	typedef void(__thiscall* PassFn)(void* processManager);
	static const UInt32 kRunAllActors = 0x0096C330;
	static PassFn PrevPassAI2 = (PassFn)kRunAllActors, PrevPassCombined = (PassFn)kRunAllActors;
	static void __fastcall PassAI2(void* processManager) { InPass++; PrevPassAI2(processManager); InPass--; }
	static void __fastcall PassCombined(void* processManager) { InPass++; PrevPassCombined(processManager); InPass--; }

	// RunDetection's call against the player (0x008E439C), from the whole-pass batch
	static volatile LONG WinPlayerAnswered, WinPlayerCalls;
	static int __fastcall PlayerPair(UInt8* self, void*, int a1, UInt8* target, UInt8* outSeen, int targetInCombat, int a5, int a6, UInt8* selfInCombat) {
		int r = 0;
		bool answered = false;
		if (InPass && Ready && Level && a1 == 1 && !a5 && self && target && outSeen && selfInCombat && target == *(UInt8**)PlayerPtrAddress && self != target
			&& TryAcquireSRWLockExclusive(&BatchLock)) {
			NewFrameCheck();
			auto c = Covered.find(self);
			auto it = JobIndex.find(Key(self, target));
			if (c != Covered.end() && c->second == Game.RunNumber(self) && it != JobIndex.end()) {
				Job& j = Jobs[it->second];
				if (j.playerPair && !j.used && j.targetInCombat == (targetInCombat & 0xFF ? 1 : 0) && j.selfInCombatIn == *selfInCombat && j.a6 == (UInt8)a6) {
					j.used = true;
					*outSeen = j.seen;
					*selfInCombat = j.selfInCombatOut;
					r = j.level;
					answered = true;
				}
			}
			ReleaseSRWLockExclusive(&BatchLock);
		}
		InterlockedIncrement(&WinPlayerCalls);
		if (answered) InterlockedIncrement(&WinPlayerAnswered);
		else r = OrigGetLevel(self, a1, target, outSeen, targetInCombat, a5, a6, selfInCombat);
		return r;
	}

	// From PairLevel: true = answered (result in *out). Only for the loop's call shape (a1 1, a5 0, a6 0).
	static bool Answer(UInt8* self, int a1, UInt8* target, UInt8* outSeen, int targetInCombat, int a5, int a6, UInt8* selfInCombat, int* out) {
		if (!InPass || !Ready || !Level || a1 != 1 || a5 || a6 || !self || !target || !outSeen || !selfInCombat) return false;
		UInt8* player = *(UInt8**)PlayerPtrAddress;
		if (self == player || target == player) return false;
		if (!TryAcquireSRWLockExclusive(&BatchLock)) return false;   // only one loop at a time uses the store
		bool answered = false;
		NewFrameCheck();
		auto c = Covered.find(self);
		if (c == Covered.end() || c->second != Game.RunNumber(self)) BatchOne(self, target, *selfInCombat);
		auto it = JobIndex.find(Key(self, target));
		if (it == JobIndex.end() || Jobs[it->second].used) InterlockedIncrement(&WinNoEntry);
		else {
			Job& j = Jobs[it->second];
			if (j.targetInCombat != (targetInCombat & 0xFF ? 1 : 0) || j.selfInCombatIn != *selfInCombat) InterlockedIncrement(&WinDifferent);
			else {
				j.used = true;
				*outSeen = j.seen;
				*selfInCombat = j.selfInCombatOut;
				*out = j.level;
				answered = true;
				InterlockedIncrement(&WinAnswered);
			}
		}
		ReleaseSRWLockExclusive(&BatchLock);
		if (!answered) InterlockedIncrement(&WinSerialCalls);
		return answered;
	}

	// ---- setup ----
	static bool BytesAre(UInt32 address, const char* hex) {
		const UInt8* p = (const UInt8*)address;
		for (size_t i = 0; hex[i * 2] && hex[i * 2 + 1]; i++) {
			unsigned v; sscanf(hex + i * 2, "%2x", &v);
			if (p[i] != (UInt8)v) return false;
		}
		return true;
	}

	// The loop instructions the batch filter copies, as in the dumped exe. Another plugin changing any of them = refuse.
	static const char* LoopChanged() {
		struct { UInt32 at; const char* bytes; const char* what; } k[] = {
			{ 0x008E4770, "B9D8D71C01E8A6F6B1FF", "range setting" },
			{ 0x008E47A9, "B9800E1E01E89D36E3FF", "process manager / high array" },
			{ 0x008E47DB, "6A008B4DFCE8DB9DCDFF", "array count" },
			{ 0x008E47F2, "8B4DFCE8763E0800", "array element" },
			{ 0x008E4860, "742A8B45083B053CEA1D01", "combat filter" },
			{ 0x008E48B2, "DC1D284D0801", "8192 player range" },
			{ 0x008E48F8, "8B8274020000FFD0", "target in combat (vtable +0x274)" },
			{ 0x008E4913, "DC1D90150101", "2-unit shortcut" },
			{ 0x008E4931, "8D55 9F 52 6A00 6A00 0FB645 9D 50 8D4D 93 51 8B55F8 52 6A01 8B4D08", "call arguments" },
			{ 0x008C7630, "558BEC6AFF68C858F100", "AILinearTaskThread constructor" },
			{ 0x00AA6430, "558BEC83EC08894DF8", "BSThread::Initialize" },
			{ 0x008C9FB0, "558BEC51894DFC8B4DFC83C130", "StartProcessingTasks" },
			{ 0x008C7490, "558BEC51894DFC8B4DFC83C13C", "WaitForCompletion" },
			{ 0x008C6FF0, "558BEC83EC10894DF8E8C24F3F008945F4518BCC6A01E8A501000068C45508018B45F883C050508B4DF48B118B4DF48B420CFFD08B4DF883C15051E800493F0083C4048B4DF883C150E8727DB8FF508B4DF883C150E8B68CB5FF8B4DF883C150E85B7DB8FF508B4DF883C150E8FFDE0B008B4DF883C150E8447DB8FF8945F068002003008B55F08B028B4DF08B5004FFD28B4DF889417468002003008B55F88B4274508B4DF883C150E822A5DBFF8BC8E81B000000", "AILinearTaskThread::OnStartup (copied)" },
			{ 0x01085688, "30788C0020778C00F06F8C0010718C0090718C00", "AILinearTaskThread vtable" },
			{ 0x010855C4, "41494C696E6561725461736B54687265616400", "thread name" },
			{ 0x0096C420, "0FB691BC000000", "detection pass: +0xBC test" },
			{ 0x0096C48A, "8B82B4020000FFD0", "detection pass: timer (vtable +0x2B4)" },
			{ 0x0096C4BF, "E80CB7ACFF", "detection pass: unconscious test" },
			{ 0x0096C4EE, "8B0D04881D01E8E74FD9FF", "detection pass: fader test" },
			{ 0x008E417F, "0FB61521121F01", "RunDetection flag" },
			{ 0x008E41C1, "E8EAF9BAFF", "RunDetection in-combat test" },
			{ 0x008E42F3, "8B4DF8E8B5F8BAFF", "player pair: player in combat" },
			{ 0x008E4359, "0FB645EF85C0", "player pair: combat filter" },
			{ 0x008E4361, "8B4DF8518B5508528B8D68FFFFFFE89CB90100", "player pair: ShouldRunCombatDetection" },
			{ 0x008E437F, "8D4DDF510FB655EF526A000FB645D7508D4DDD518B55F8526A018B4D08", "player pair: call arguments" },
			{ 0x008A1075, "6A028B4D0C", "worn-armor lookup: arguments (slot 2, the target)" },
			{ 0x00891BCF, "C20400", "Actor::GetArmorBeingWorn: ret 4" },
			{ 0x004C8CC1, "6A016A006A008B4DE4", "worn-item fix-up: arguments (0, 0, 1)" },
			{ 0x004BE570, "C20C00", "ItemChange::SetWorn: ret 0xC" },
			{ 0x0088C163, "508B0D10EA1D01", "line-of-sight pick: arguments (pick data, ecx = TES)" },
			{ 0x008A0FA2, "516A018B550C526A018B8D68FFFFFF", "line-of-sight call: arguments (1, target, 1, &type, 1; ecx = this NPC)" },
			{ 0x0088C23D, "C21400", "Actor::LineOfSight: ret 0x14" },
			{ 0x00458438, "C20400", "TES::HavokPick: ret 4" },
		};
		// the ShouldRunCombatDetection call the batch filter copies
		{
			const UInt8* p = (const UInt8*)0x008E487B;
			const UInt32 to = p[0] == 0xE8 ? 0x008E487B + 5 + *(const int*)(p + 1) : 0;
			if (to != 0x008FFD10) return "ShouldRunCombatDetection";
		}
		for (auto& e : k) {
			std::string clean;
			for (const char* c = e.bytes; *c; c++) if (*c != ' ') clean += *c;
			if (!BytesAre(e.at, clean.c_str())) return e.what;
		}
		return nullptr;
	}

	// Calls inside GetDetectionLevelAgainstActor that go to another plugin: logged once (their code then also runs on
	// the workers).
	static void LogForeignCalls() {
		const UInt8* p = (const UInt8*)kGetDetectionLevel;
		for (UInt32 i = 0; i + 5 <= 2464; i++) {
			if (p[i] != 0xE8) continue;
			const UInt32 site = kGetDetectionLevel + i, target = site + 5 + *(const int*)(p + i + 1);
			if (target >= 0x00401000 && target < 0x01500000) continue;
			const std::string where = ModuleOf(target);
			if (where.empty()) continue;   // not code in a loaded module: a byte that only looks like a call
			LogLine("PARALLEL DETECTION note: the check's call at %08X goes to %s (%08X); it runs on the workers too", site, where.c_str(), target);
		}
	}

	// AILinearTaskThread::OnStartup 0x008C6FF0 instruction for instruction (Havok thread memory, its router, a 200 KB
	// Havok stack), WITHOUT its last call, TESDataHandler::CreateThreadSafeFiles: that opens a private copy of every
	// plugin file for the thread (TESFile::GetThreadSafeFileForThread 0x00473AE0), thousands of files for a pool, and a
	// detection check never reads plugin files (the game makes such a copy on demand, so a stray read still works).
	static __declspec(naked) void WorkerOnStartup() {
		__asm {
			push ebp
			mov ebp, esp
			sub esp, 0x10
			mov [ebp - 8], ecx
			mov eax, 0x00CBBFC0              // hkMemorySystem::getInstance
			call eax
			mov [ebp - 0xC], eax
			push ecx                         // a 4-byte argument built in place:
			mov ecx, esp
			push 1
			mov eax, 0x008C71B0
			call eax
			push 0x010855C4                  // "AILinearTaskThread"
			mov eax, [ebp - 8]
			add eax, 0x50                    // the thread's hkMemoryRouter
			push eax
			mov ecx, [ebp - 0xC]
			mov edx, [ecx]
			mov eax, [edx + 0xC]             // threadInit(router, name, frame info)
			call eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			push ecx
			mov eax, 0x00CBB930              // hkBaseSystem::initThread(router)
			call eax
			add esp, 4
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x0044EDB0
			call eax
			push eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x0041FD00
			call eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x0044EDB0
			call eax
			push eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x00984F60
			call eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x0044EDB0
			call eax
			mov [ebp - 0x10], eax
			push 0x32000
			mov edx, [ebp - 0x10]
			mov eax, [edx]
			mov ecx, [ebp - 0x10]
			mov edx, [eax + 4]               // block alloc 0x32000
			call edx
			mov ecx, [ebp - 8]
			mov [ecx + 0x74], eax
			push 0x32000
			mov edx, [ebp - 8]
			mov eax, [edx + 0x74]
			push eax
			mov ecx, [ebp - 8]
			add ecx, 0x50
			mov eax, 0x006815C0              // the router's stack
			call eax
			mov ecx, eax
			mov eax, 0x008C70C0              // hkStackMemory::initMemory(block, 0x32000)
			call eax
			mov esp, ebp
			pop ebp
			ret
		}
	}
	static void* WorkerVtable[5];   // AILinearTaskThread's (0x01085688, 5 slots) with OnStartup swapped

	// Havok's memory system (hkFreeListMemorySystem, its pointer at 0x01268418) keeps 32 thread slots: threadInit
	// 0x00CD0830 takes the first slot whose in-use byte (+0xD98 + i x 0x138) is 0, and when all 32 are taken it uses
	// 'slot 32', which lies on the system's own critical section (+0x336C); every later thread shares it. No error, just
	// heap corruption. The game's own threads take slots too (main, the AI threads, up to 6 tasklet and 6 Havok job
	// threads, the IO / parallel-task / music threads), some only when first needed, so the workers leave kHavokSlotReserve.
	static const UInt32 kHavokMemoryPtr = 0x01268418, kFreeListMemoryVtbl = 0x010CAD74;
	static const int kHavokSlots = 32, kHavokSlotReserve = 10;
	static int LastSlotsLogged = -2;
	static int HavokSlotsInUse() {   // -1 = not the memory system this was checked against
#ifdef MTAI_TEST
		return 0;
#else
		__try {
			const UInt8* m = *(const UInt8**)kHavokMemoryPtr;
			if (!m || *(const UInt32*)m != kFreeListMemoryVtbl) return -1;
			int n = 0;
			for (int i = 0; i < kHavokSlots; i++) if (m[0xD98 + i * 0x138]) n++;
			return n;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
#endif
	}

	static void CreateWorkers() {
		static const char* kNames[kMaxWorkers] = { "MTAI Detection 1", "MTAI Detection 2", "MTAI Detection 3", "MTAI Detection 4",
			"MTAI Detection 5", "MTAI Detection 6", "MTAI Detection 7", "MTAI Detection 8", "MTAI Detection 9",
			"MTAI Detection 10", "MTAI Detection 11", "MTAI Detection 12", "MTAI Detection 13", "MTAI Detection 14", "MTAI Detection 15" };
		SYSTEM_INFO si; GetSystemInfo(&si);
		if (Config.noHelpers) { LogLine("PARALLEL DETECTION: NoHelpers = 1 (diagnostic): no helper threads; the batches run on the AI thread alone"); return; }
		// the main thread and the two AI threads keep their cores; past 8 the gain is small and each worker takes a Havok slot.
		// A CPU with 3 or fewer threads gets none (the game's own threads already fill it): detection runs as before.
		int want = Config.parThreads > 0 ? Config.parThreads : std::min(kDefaultMaxWorkers, (int)si.dwNumberOfProcessors - 3);
		if (want < 1) {
			LogLine("PARALLEL DETECTION: this CPU has %u logical cores, too few to spare any for detection; detection runs as before", (unsigned)si.dwNumberOfProcessors);
			return;
		}
		want = std::min(want, kMaxWorkers);
		const int used = HavokSlotsInUse();
		if (used < 0) { LogLine("PARALLEL DETECTION: Havok's memory system is not the one this was checked against; no workers (detection runs as before)"); return; }
		const int room = kHavokSlots - kHavokSlotReserve - used;
		LogLine("PARALLEL DETECTION: Havok thread slots in use before the workers: %d of %d (%d kept free for the game)", used, kHavokSlots, kHavokSlotReserve);
		if (room < 1) { LogLine("PARALLEL DETECTION: no room for workers; detection runs as before"); return; }
		want = std::min(want, room);
		memcpy(WorkerVtable, (void*)0x01085688, sizeof(WorkerVtable));
		WorkerVtable[2] = (void*)WorkerOnStartup;
		for (int i = 0; i < want; i++) {
			void* t = ((void* (__cdecl*)(UInt32))0x00401000)(0x78);            // the game heap, as AddThread does
			if (!t) break;
			((void* (__thiscall*)(void*, void(__cdecl*)()))0x008C7630)(t, WorkerTask);
			if (*(UInt32*)t != 0x01085688) { LogLine("PARALLEL DETECTION: the thread object has an unexpected vtable %08X; no workers", *(UInt32*)t); break; }
			*(void***)t = WorkerVtable;                                           // before the thread starts
			((void(__thiscall*)(void*, SIZE_T, const char*, bool))0x00AA6430)(t, 0x40000, kNames[i], false);   // 256 KB stack (the AI threads get 64 KB)
			Workers[WorkerCount++] = t;
		}
		StartWorker = GameStart;
		WaitWorker = GameWait;
		LogLine("PARALLEL DETECTION: %d worker threads started (CPU has %u logical cores)", WorkerCount, (unsigned)si.dwNumberOfProcessors);
	}

	static bool WantHook() { return Config.parLevel > 0 || Config.parHotkey; }

	// points the call at 'site' (to 'expected') at 'to'; false when it goes elsewhere (another plugin patched it)
	static bool RedirectCall(UInt32 site, UInt32 expected, void* to) {
		UInt8* p = (UInt8*)site;
		if (p[0] != 0xE8 || site + 5 + *(int*)(p + 1) != expected) return false;
		DWORD old;
		VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
		*(int*)(p + 1) = (int)((UInt32)to - (site + 5));
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
		return true;
	}

	// points the call at 'site' (call ProcessLists::RunDetectionForAllActors) at 'wrap'. When another plugin already took
	// that call (its target lies outside the game's code), the wrapper chains onto it: InPass is only a flag set around
	// whatever runs the pass, and every batch answer is still matched to its exact NPC, target and arguments.
	static bool WrapPassSite(UInt32 site, void* wrap, PassFn* prev) {
		UInt8* p = (UInt8*)site;
		if (p[0] != 0xE8) return false;
		const UInt32 target = site + 5 + *(int*)(p + 1);
		if (target != kRunAllActors && target >= 0x00401000 && target < 0x00FDF000) return false;   // elsewhere in the game: refuse
		*prev = (PassFn)target;
		if (target != kRunAllActors) LogLine("PARALLEL DETECTION note: the call at %08X already goes to %s (%08X, another plugin); chained onto it", site, ModuleOf(target).c_str(), target);
		return RedirectCall(site, target, wrap);
	}

	// after the pair hook is in (InstallPairs)
	static void Install() {
		if (!WantHook()) return;
		if (const char* what = LoopChanged()) { LogLine("PARALLEL DETECTION: REFUSED, the game code it copies changed (%s): another plugin patched it", what); return; }
		if (!RedirectCall(kArmorSite, kGetArmorBeingWorn, (void*)ArmorBeingWorn)) {
			LogLine("PARALLEL DETECTION: REFUSED, the check's worn-armor lookup (008A107A) does not call Actor::GetArmorBeingWorn: another plugin patched it");
			return;
		}
		if (!WrapPassSite(0x008C7F84, (void*)PassAI2, &PrevPassAI2) || !WrapPassSite(0x008C7CA6, (void*)PassCombined, &PrevPassCombined)) {
			LogLine("PARALLEL DETECTION: REFUSED, a call to ProcessLists::RunDetectionForAllActors (008C7F84 / 008C7CA6) goes elsewhere: another plugin patched it");
			return;
		}
		if (Config.checkLock == 2) {
			if (RedirectCall(kSightSite, kLineOfSight, (void*)SightOutsideLock)) LogLine("PARALLEL DETECTION: CheckLock = 2: each check's line of sight runs in parallel, the rest of it one at a time");
			else LogLine("PARALLEL DETECTION note: the line-of-sight call (008A0FB1) goes elsewhere (another plugin); whole checks run one at a time (little speed-up)");
		}
		else if (Config.checkLock == 1) LogLine("PARALLEL DETECTION: CheckLock = 1: whole checks one at a time (little speed-up)");
		else LogLine("PARALLEL DETECTION: CheckLock = 0: whole checks in parallel (TEST ONLY: this crashed in big fights with human NPCs)");
		if (Config.pickLock) {
			if (RedirectCall(kPickSite, kHavokPick, (void*)LockedPick)) LogLine("PARALLEL DETECTION: PickLock = 1 (diagnostic): line-of-sight ray casts run one at a time");
			else LogLine("PARALLEL DETECTION note: PickLock could not be set (0088C16A goes elsewhere); ray casts stay parallel");
		}
		if (!RedirectCall(kFixupSite, kItemSetWorn, (void*)CountFixup))
			LogLine("PARALLEL DETECTION note: the worn-item fix-up call (004C8CCA) goes elsewhere; it is not counted");
		LogForeignCalls();
		{
			UInt32* slot = (UInt32*)(0x01087864 + 0x2F0);   // HighProcess vtable: RunDetection
			if (*slot != 0x008E40D0) { LogLine("PARALLEL DETECTION: REFUSED, HighProcess::RunDetection's vtable slot goes to %08X (%s)", *slot, ModuleOf(*slot).c_str()); return; }
			OrigRun = (RunFn)*slot;
			DWORD old;
			VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
			InterlockedExchange((volatile LONG*)slot, (LONG)(UInt32)RunHook);
			VirtualProtect(slot, 4, old, &old);
		}
		{
			UInt8* p = (UInt8*)0x008E439C;
			if (p[0] != 0xE8 || 0x008E439C + 5 + *(int*)(p + 1) != kGetDetectionLevel) { LogLine("PARALLEL DETECTION: REFUSED, 008E439C is not 'call Actor::GetDetectionLevelAgainstActor'"); return; }
			DWORD old;
			VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
			*(int*)(p + 1) = (int)((UInt32)PlayerPair - (0x008E439C + 5));
			VirtualProtect(p, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, 5);
		}
		Level = Config.parLevel > 0 ? 1 : 0;
		Jobs.reserve(4096);
		JobIndex.reserve(8192);
		Covered.reserve(1024);
		Ready = true;
		LogLine("PARALLEL DETECTION starts %s (NPC-vs-NPC detection checks run on several cores; same checks, same results)%s; workers start at the first frame",
			Level ? "ON" : "OFF", Config.parHotkey ? "; the Scroll Lock key switches it on/off" : "");
	}

	static void OnFrameStart() {
		if (!Ready) return;
		if (!WorkersTried && FrameCount > 30) { WorkersTried = true; CreateWorkers(); }   // main thread, game data loaded
		if (!Config.parHotkey) return;
		const bool down = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
		if (down && !KeyWasDown) {
			Level = Level ? 0 : 1;
			LogLine("PARALLEL DETECTION switched %s (Scroll Lock key)", Level ? "ON" : "OFF");
			LogFlush();
		}
		KeyWasDown = down;
	}

	static void Write(size_t frames) {
		if (!Ready || !frames) return;
		const double f = double(frames);
		const LONG runs = InterlockedExchange(&WinRuns, 0), jobs = InterlockedExchange(&WinJobs, 0), workers = InterlockedExchange(&WinWorkersUsed, 0);
		const LONG cross = InterlockedExchange(&WinCross, 0), crossSelves = InterlockedExchange(&WinCrossSelves, 0);
		const LONG playerCalls = InterlockedExchange(&WinPlayerCalls, 0), playerAnswered = InterlockedExchange(&WinPlayerAnswered, 0);
		const LONG answered = InterlockedExchange(&WinAnswered, 0), serial = InterlockedExchange(&WinSerialCalls, 0);
		const LONGLONG wall = InterlockedExchange64(&WinWallTicks, 0), cpu = InterlockedExchange64(&WinJobTicks, 0), wallMax = InterlockedExchange64(&WinWallMax, 0);
		if (runs || serial || playerCalls) LogLine("PARALLEL DETECTION %s: NPC-vs-NPC checks %.1f/frame done in parallel (%.1f batches/frame, %.1f threads each), %.1f/frame by the game one by one | the batches took %.3f ms/frame (longest %.2f ms) for %.3f ms/frame of checks: %.1fx",
			Level ? "ON" : "OFF", answered / f, runs / f, runs ? double(workers) / runs + 1.0 : 0.0, serial / f,
			double(wall) / TicksPerMs / f, double(wallMax) / TicksPerMs, double(cpu) / TicksPerMs / f, wall ? double(cpu) / wall : 0.0);
		if (playerCalls) LogLine("PARALLEL DETECTION NPC-vs-player checks: %.1f/frame, %.1f of them from the batch", playerCalls / f, playerAnswered / f);
		const LONG tooSmall = InterlockedExchange(&WinSmall, 0), noEntry = InterlockedExchange(&WinNoEntry, 0), different = InterlockedExchange(&WinDifferent, 0), unused = InterlockedExchange(&WinUnused, 0);
		const LONG outside = InterlockedExchange(&WinOutside, 0);
		if (cross || tooSmall || noEntry || different || unused || outside || jobs != answered)
			LogLine("PARALLEL DETECTION details: whole-pass batches %.2f/frame covering %.1f NPCs each | single-NPC runs too small or swimming %.1f, checks computed %.1f, not asked for by the game %.1f, asked but not in the batch %.1f, arguments differed %.1f /frame | NPC runs outside the detection pass (followers etc., left to the game) %.2f/frame",
				cross / f, cross ? double(crossSelves) / cross : 0.0, tooSmall / f, jobs / f, unused / f, noEntry / f, different / f, outside / f);
		const LONGLONG sight = InterlockedExchange64(&WinSightTicks, 0);
		if (sight) LogLine("PARALLEL DETECTION line of sight (in parallel): %.3f ms/frame of the checks' time", double(sight) / TicksPerMs / f);
		LONG sn[3], sy[3];
		for (int b = 0; b < 3; b++) { sn[b] = InterlockedExchange(&WinSightN[b], 0); sy[b] = InterlockedExchange(&WinSightYes[b], 0); }
		auto pct = [](LONG y, LONG n) { return n ? 100.0 * y / n : 0.0; };
		if (sn[0] || sn[1] || sn[2])
			LogLine("PARALLEL DETECTION line of sight answers 'visible': game's own checks %.1f%% of %ld | batch on the AI thread %.1f%% of %ld | batch on helpers %.1f%% of %ld",
				pct(sy[0], sn[0]), sn[0], pct(sy[1], sn[1]), sn[1], pct(sy[2], sn[2]), sn[2]);
		LONG vn[2], vd[2], vt[2];
		for (int b = 0; b < 2; b++) { vn[b] = InterlockedExchange(&WinVerify[b], 0); vd[b] = InterlockedExchange(&WinVerifyDiff[b], 0); vt[b] = InterlockedExchange(&WinVerifyTypeDiff[b], 0); }
		if (vn[0] || vn[1])
			LogLine("PARALLEL DETECTION VerifySight (same calls again on the AI thread after the batch): helpers' answers %ld re-run, %ld different (%ld other type) | AI thread's answers %ld re-run, %ld different (%ld other type)",
				vn[1], vd[1], vt[1], vn[0], vd[0], vt[0]);
		const LONG fixups = InterlockedExchange(&WinFixups, 0), fixupsOnWorkers = InterlockedExchange(&WinFixupsOnWorkers, 0);
		if (fixups)
			LogLine("PARALLEL DETECTION worn-item fix-ups (GetWornItem deletes a worn entry of an item; the unsafe write, now one thread at a time): %ld in %zu frames, %ld of them on detection workers",
				fixups, frames, fixupsOnWorkers);
		const int slots = WorkerCount ? HavokSlotsInUse() : -2;
		if (slots != LastSlotsLogged && slots != -2) {
			LogLine("PARALLEL DETECTION: Havok thread slots in use %d of %d (%d detection workers)%s", slots, kHavokSlots, WorkerCount,
				slots >= kHavokSlots - 1 ? " -- NEARLY FULL: a thread past 32 corrupts Havok's memory" : "");
			LastSlotsLogged = slots;
		}
	}
}
