// Real NTFS mutations + final filesystem oracle; links the production owner.
#include "UeAgentWorkspaceChangeObserver.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace Changes = UE::UAI::HjsonWorkspace::FileChanges;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static std::uint64_t Now() { return Changes::FObserver::MonotonicNanoseconds(); }
static double Ms(std::uint64_t N) { return static_cast<double>(N) / 1000000.; }
static void Require(bool Good, const char* Reason) { if (!Good) throw std::runtime_error(Reason); }
static std::string Read(const fs::path& Path)
{ std::ifstream In(Path, std::ios::binary); Require(In.good(), "oracle read failed"); return {std::istreambuf_iterator<char>(In), {}}; }
static void Write(const fs::path& Path, const std::string& Text)
{
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(File != INVALID_HANDLE_VALUE, "fixture open failed"); DWORD Count = 0;
    const bool Good = WriteFile(File, Text.data(), static_cast<DWORD>(Text.size()), &Count, nullptr)
        && Count == Text.size() && FlushFileBuffers(File);
    CloseHandle(File); Require(Good, "fixture write/flush failed");
}
static const Changes::FCandidate* Find(const Changes::FSnapshot& S, const std::wstring& Path)
{
    for (const auto& C : S.Candidates) if (C.Path == Path) return &C;
    return nullptr;
}
static Changes::FSnapshot Wait(Changes::FObserver& O, const std::function<bool(const Changes::FSnapshot&)>& Predicate)
{
    const auto Until = Clock::now() + std::chrono::seconds(5);
    Changes::FSnapshot S;
    do {
        if (!O.Snapshot(S)) { std::cerr << "observer failure code=" << static_cast<int>(S.Error.Code) << " native=" << S.Error.NativeCode << '\n'; throw std::runtime_error("observer failed"); }
        if (Predicate(S)) return S;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < Until);
    throw std::runtime_error("notification deadline, not assumed clean");
}
static Changes::FSnapshot Path(Changes::FObserver& O, const std::wstring& Name, std::uint32_t Flag)
{ return Wait(O, [&](const auto& S) { const auto* C = Find(S, Name); return C && (C->Changes & Flag); }); }
static void Ack(Changes::FObserver& O, const Changes::FSnapshot& S)
{ Changes::FError E; Require(O.Acknowledge(S.ThroughSequence, E), "ack failed"); }
static bool RenamePresent(const Changes::FSnapshot& S, const std::wstring& Old, const std::wstring& New)
{ for (const auto& R : S.Renames) if (R.OldPath == Old && R.NewPath == New && R.FileId) return true; return false; }
static double Quantile(std::vector<double> Values, double Q)
{ std::sort(Values.begin(), Values.end()); return Values[static_cast<std::size_t>((Values.size() - 1) * Q)]; }
static void Distribution(const char* Name, const std::vector<double>& Values)
{ std::cout << '"' << Name << "\":{\"n\":" << Values.size() << ",\"p50_ms\":" << Quantile(Values, .5) << ",\"p95_ms\":" << Quantile(Values, .95) << ",\"max_ms\":" << *std::max_element(Values.begin(), Values.end()) << '}'; }
static std::vector<DWORD> ThreadsOf(DWORD Pid)
{
    std::vector<DWORD> Result;
    const HANDLE All = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); Require(All != INVALID_HANDLE_VALUE, "thread oracle");
    THREADENTRY32 Item{}; Item.dwSize = sizeof(Item);
    if (Thread32First(All, &Item)) do {
        if (Item.th32OwnerProcessID == Pid) Result.push_back(Item.th32ThreadID);
    } while (Thread32Next(All, &Item));
    CloseHandle(All); return Result;
}
// Test-only isolated CHILD process, with a separate heap and a kill-on-close job.
// Never suspend a thread that shares the mutator's allocator/locks.
struct FOverflowChild
{
    HANDLE Job = nullptr, Ready = nullptr;
    PROCESS_INFORMATION Process{};
    std::vector<HANDLE> Paused;
    ~FOverflowChild()
    {
        for (HANDLE T : Paused) { ResumeThread(T); CloseHandle(T); }
        if (Process.hProcess) { if (WaitForSingleObject(Process.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(Process.hProcess, 9); CloseHandle(Process.hProcess); }
        if (Process.hThread) CloseHandle(Process.hThread);
        if (Ready) CloseHandle(Ready);
        if (Job) CloseHandle(Job);
    }
    void Run(const fs::path& Root)
    {
        const std::wstring Name = L"Local\\UAI_FileChanges_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(Now());
        Ready = CreateEventW(nullptr, TRUE, FALSE, Name.c_str()); Require(Ready, "overflow ready event");
        Job = CreateJobObjectW(nullptr, nullptr); Require(Job, "overflow child job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limit{}; Limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limit, sizeof(Limit)), "overflow job limit");
        std::wstring Exe(32768, L'\0'); Exe.resize(GetModuleFileNameW(nullptr, Exe.data(), static_cast<DWORD>(Exe.size())));
        std::wstring Command = L"\"" + Exe + L"\" --overflow-child \"" + Root.wstring() + L"\" \"" + Name + L"\"";
        STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup);
        Require(CreateProcessW(Exe.c_str(), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, nullptr, &Startup, &Process), "create own overflow child");
        Require(AssignProcessToJobObject(Job, Process.hProcess), "assign own overflow child");
        Require(ResumeThread(Process.hThread) == 1, "start own overflow child");
        Require(WaitForSingleObject(Ready, 5000) == WAIT_OBJECT_0, "overflow child ready deadline");
        const auto Ids = ThreadsOf(Process.dwProcessId); Require(Ids.size() >= 2, "own child collector not started");
        for (DWORD Id : Ids) {
            HANDLE T = OpenThread(THREAD_SUSPEND_RESUME, FALSE, Id); Require(T, "open own child thread");
            const auto Before = SuspendThread(T);
            if (Before != 0) { if (Before != static_cast<DWORD>(-1)) ResumeThread(T); CloseHandle(T); throw std::runtime_error("suspend own child thread"); }
            Paused.push_back(T);
        }
        for (unsigned I = 0; I < 200; ++I) { std::ofstream F(Root / std::to_wstring(I)); F << I; }
        for (HANDLE T : Paused) { Require(ResumeThread(T) == 1, "resume own child thread"); CloseHandle(T); }
        Paused.clear();
        Require(WaitForSingleObject(Process.hProcess, 10000) == WAIT_OBJECT_0, "overflow child exit deadline");
        DWORD Code = 99; Require(GetExitCodeProcess(Process.hProcess, &Code) && Code == 0, "real overflow/sticky-error child failed");
    }
};

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--buffer-contract") {
            const fs::path Root = fs::absolute(argv[2]); Require(!fs::exists(Root), "new buffer fixture required"); fs::create_directories(Root);
            bool First = true; std::cout << "{\"success\":true,\"case\":\"native_buffer_length_contract\",\"outcomes\":[";
            for (const std::uint32_t Size : {0u, 1u, 127u, 1025u}) {
                const auto Item = Root / std::to_wstring(Size); fs::create_directory(Item);
                Changes::FObserver O; Changes::FError E; Changes::FOptions Options; Options.BufferBytes = Size;
                const bool Started = O.Start(Item.wstring(), E, Options);
                bool Selected = false;
                if (!Started) Require(E.Stage == L"ReadDirectoryChangesExW" && E.NativeCode, "local buffer threshold still present");
                else {
                    Write(Item / L"a.hjson", "native notification\n");
                    Changes::FSnapshot S; const auto Until = Clock::now() + std::chrono::seconds(3);
                    do {
                        if (!O.Snapshot(S)) { E = S.Error; break; }
                        if (Find(S, L"a.hjson")) { Selected = true; break; }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } while (Clock::now() < Until);
                    Require(Selected || (E.Code == Changes::EError::NotificationLost && E.NativeCode), "small native buffer neither selected nor reported real loss");
                }
                if (Size == 1025) Require(Started && Selected, "non-four-multiple native length must work for small edit");
                if (!First) std::cout << ','; First = false;
                std::cout << "{\"requested_bytes\":" << Size << ",\"started\":" << (Started ? "true" : "false")
                    << ",\"selected\":" << (Selected ? "true" : "false") << ",\"native_error\":" << E.NativeCode << '}';
                O.Close();
            }
            fs::remove_all(Root); std::cout << "]}\n"; return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--start-contract") {
            const fs::path Root = fs::absolute(argv[2]); Require(!fs::exists(Root), "new contract fixture required"); fs::create_directories(Root);
            Changes::FObserver O; Changes::FError E; Require(O.Start(Root.wstring(), E), "start contract");
            Write(Root / L"unconsumed.hjson", "must survive rejected restart\n");
            const auto Before = Path(O, L"unconsumed.hjson", Changes::Added);
            Require(!O.Start(Root.wstring(), E) && E.Code == Changes::EError::AlreadyStarted, "start silently replaced session");
            Changes::FSnapshot After; Require(O.Snapshot(After) && Find(After, L"unconsumed.hjson")
                && After.ThroughSequence >= Before.ThroughSequence && After.RootFileId == Before.RootFileId, "restart dropped candidates");
            const auto Sibling = fs::path(Root.wstring() + L"-sibling"); fs::create_directory(Sibling);
            fs::rename(Sibling, fs::path(Sibling.wstring() + L"-renamed")); fs::remove(fs::path(Sibling.wstring() + L"-renamed"));
            Write(Root / L"after-sibling.hjson", "healthy\n");
            Path(O, L"after-sibling.hjson", Changes::Added);
            const auto Moved = fs::path(Root.wstring() + L"-moved"); fs::rename(Root, Moved);
            const auto Until = Clock::now() + std::chrono::seconds(3);
            while (O.Snapshot(After) && Clock::now() < Until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(After.Status == Changes::EStatus::Failed && After.Error.Code == Changes::EError::RootChanged, "root identity loss not observed");
            O.Close(); fs::remove_all(Moved);
            std::cout << "{\"success\":true,\"case\":\"restart_pending_and_parent_identity_boundary\"}\n"; return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--overflow-child") {
            Changes::FObserver O; Changes::FError E; Changes::FOptions Tiny; Tiny.BufferBytes = 1024;
            Require(O.Start(argv[2], E, Tiny), "child observer start");
            HANDLE Ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]); Require(Ready, "child ready"); SetEvent(Ready); CloseHandle(Ready);
            Changes::FSnapshot S; const auto Until = Clock::now() + std::chrono::seconds(8);
            while (O.Snapshot(S) && Clock::now() < Until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Require(S.Status == Changes::EStatus::Failed && S.Error.Code == Changes::EError::NotificationLost, "real child kernel overflow not observed");
            Require(!O.Snapshot(S) && S.Error.Code == Changes::EError::NotificationLost, "child loss not sticky");
            Require(!O.Acknowledge(S.ThroughSequence, E), "child lost acknowledge allowed"); O.Close(); return 0;
        }
        Require(argc == 2, "pass a NEW dedicated run directory");
        const fs::path Run = fs::absolute(argv[1]);
        std::vector<std::string> Cases;
        Require(!fs::exists(Run), "refuse overwrite existing run"); fs::create_directories(Run);
        const auto Root = Run / L"small"; fs::create_directory(Root);
        Write(Root / L"A.hjson", "false\n"); Write(Root / L"B.hjson", "B unchanged\n");
        Changes::FObserver O; Changes::FError E;
        const auto Start = Now(); Require(O.Start(Root.wstring(), E), "start small"); const auto StartNs = Now() - Start;
        Write(Root / L"A.hjson", "true\n"); auto S = Path(O, L"A.hjson", Changes::Modified);
        Require(S.Candidates.size() == 1 && Read(Root / L"A.hjson") == "true\n" && Read(Root / L"B.hjson") == "B unchanged\n", "modify oracle/candidates");
        Require(Find(S, L"A.hjson")->FileId && Find(S, L"A.hjson")->ParentFileId, "extended IDs missing"); Ack(O, S);
        Require(S.RootFileId && S.VolumeSerial, "session identity missing"); Cases.push_back("in_place_modify_and_unrelated_file");
        Write(Root / L"新增中文.hjson", "new\n"); S = Path(O, L"新增中文.hjson", Changes::Added); Require(Read(Root / L"新增中文.hjson") == "new\n", "create oracle"); Ack(O, S);
        Cases.push_back("unicode_create");
        fs::rename(Root / L"新增中文.hjson", Root / L"renamed.hjson");
        S = Wait(O, [](const auto& V) { return RenamePresent(V, L"新增中文.hjson", L"renamed.hjson"); });
        Require(Find(S, L"新增中文.hjson") && Find(S, L"renamed.hjson") && !fs::exists(Root / L"新增中文.hjson") && Read(Root / L"renamed.hjson") == "new\n", "rename oracle"); Ack(O, S);
        Cases.push_back("rename_preserves_both_paths_and_id");
        fs::remove(Root / L"renamed.hjson"); S = Path(O, L"renamed.hjson", Changes::Removed); Require(!fs::exists(Root / L"renamed.hjson"), "delete oracle"); Ack(O, S);
        Cases.push_back("delete");
        Write(Root / L".save.tmp", "atomic replacement\n");
        Require(MoveFileExW((Root / L".save.tmp").c_str(), (Root / L"A.hjson").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "atomic fixture rename");
        S = Wait(O, [](const auto& V) { return RenamePresent(V, L".save.tmp", L"A.hjson"); });
        Require(Find(S, L"A.hjson") && Read(Root / L"A.hjson") == "atomic replacement\n" && !fs::exists(Root / L".save.tmp"), "atomic oracle"); Ack(O, S);
        Cases.push_back("temp_atomic_replace");

        // Preserve unconsumed A while B arrives; acknowledging old S must not erase newer A.
        Write(Root / L"A.hjson", "pending A1\n"); const auto Old = Path(O, L"A.hjson", Changes::Modified);
        Write(Root / L"B.hjson", "pending B\n"); Write(Root / L"A.hjson", "pending A2\n");
        S = Wait(O, [&](const auto& V) { const auto* A = Find(V, L"A.hjson"); return A && A->LastSequence > Old.ThroughSequence && Find(V, L"B.hjson"); });
        Ack(O, Old); Require(O.Snapshot(S) && Find(S, L"A.hjson") && Find(S, L"B.hjson"), "ack erased newer event");
        Require(Read(Root / L"A.hjson") == "pending A2\n" && Read(Root / L"B.hjson") == "pending B\n", "pending oracle"); Ack(O, S);
        Cases.push_back("pending_A_B_and_acknowledge_race");
        for (unsigned I = 0; I < 30; ++I) Write(Root / L"A.hjson", "burst " + std::to_string(I));
        S = Path(O, L"A.hjson", Changes::Modified); std::this_thread::sleep_for(std::chrono::milliseconds(30)); Require(O.Snapshot(S), "burst continuity");
        Require(S.Candidates.size() == 1 && Read(Root / L"A.hjson") == "burst 29", "burst dedupe/oracle");
        const auto BurstRecords = S.NotificationRecords; Ack(O, S);
        Cases.push_back("thirty_writes_one_dirty_path");

        // Moving an existing tree may notify only its root: return an explicit subtree candidate.
        fs::create_directories(Run / L"incoming" / L"nested"); Write(Run / L"incoming" / L"nested" / L"中文.hjson", "tree\n");
        fs::rename(Run / L"incoming", Root / L"entered"); S = Path(O, L"entered", Changes::Added);
        Require(Find(S, L"entered")->Directory && Read(Root / L"entered" / L"nested" / L"中文.hjson") == "tree\n", "enter subtree oracle"); Ack(O, S);
        Cases.push_back("existing_nested_tree_enter_prefix");
        fs::rename(Root / L"entered", Root / L"moved"); S = Wait(O, [](const auto& V) { return RenamePresent(V, L"entered", L"moved"); });
        Require(Find(S, L"moved")->Directory && Read(Root / L"moved" / L"nested" / L"中文.hjson") == "tree\n", "move subtree oracle"); Ack(O, S);
        Cases.push_back("nested_tree_rename_prefix");
        fs::remove_all(Root / L"moved"); S = Path(O, L"moved", Changes::Removed); Require(Find(S, L"moved")->Directory && !fs::exists(Root / L"moved"), "remove subtree oracle"); Ack(O, S);
        Cases.push_back("nested_tree_delete_prefix");
        O.Close(); Require(!O.Snapshot(S) && S.Error.Code == Changes::EError::Closed, "closed must not report clean");
        Cases.push_back("cancel_close_not_clean");

        DWORD HandlesBefore = 0, HandlesAfter = 0; GetProcessHandleCount(GetCurrentProcess(), &HandlesBefore);
        for (unsigned I = 0; I < 20; ++I) { Require(O.Start(Root.wstring(), E), "restart lifecycle"); O.Close(); }
        GetProcessHandleCount(GetCurrentProcess(), &HandlesAfter); Require(HandlesBefore == HandlesAfter, "handle leak");
        Cases.push_back("twenty_start_cancel_drain_no_handle_leak");
        Require(O.Start(Root.wstring(), E), "start invalidation"); fs::rename(Root, Run / L"small-renamed");
        const auto Until = Clock::now() + std::chrono::seconds(5);
        while (O.Snapshot(S) && Clock::now() < Until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(S.Status == Changes::EStatus::Failed && S.Error.Code == Changes::EError::RootChanged, "root rename must invalidate"); O.Close();
        Cases.push_back("root_rename_invalidates_session");
        Require(!O.Start((Run / L"missing").wstring(), E) && E.Code == Changes::EError::OpenFailed, "missing root must fail");
        Cases.push_back("missing_root_explicit_error");

        const auto Large = Run / L"many-unchanged"; fs::create_directory(Large);
        const auto SeedStart = Now();
        for (unsigned I = 0; I < 20000; ++I) { std::ofstream F(Large / (L"unchanged-" + std::to_wstring(I) + L".hjson"), std::ios::binary); F << "unchanged " << I << '\n'; Require(F.good(), "seed failed"); }
        Write(Large / L"selected.hjson", "0\n"); const auto SeedNs = Now() - SeedStart;
        const auto ManyStart = Now(); Require(O.Start(Large.wstring(), E), "start many"); const auto ManyStartNs = Now() - ManyStart;
        std::vector<double> WriteMs, CollectedMs, ConsumeMs, EndToEndMs;
        std::uint64_t Prior = 0, Unexpected = 0;
        for (unsigned I = 0; I < 100; ++I) {
            const auto Before = Now(); Write(Large / L"selected.hjson", std::to_string(I + 1) + "\n"); const auto Written = Now();
            S = Wait(O, [&](const auto& V) { const auto* C = Find(V, L"selected.hjson"); return C && C->LastSequence > Prior; });
            const auto Finished = Now();
            for (const auto& C : S.Candidates) if (C.Path != L"selected.hjson") ++Unexpected;
            const auto* C = Find(S, L"selected.hjson"); Require(C, "lost selected");
            CollectedMs.push_back(Ms(C->LastObservedNs - Before)); WriteMs.push_back(Ms(Written - Before)); EndToEndMs.push_back(Ms(Finished - Before));
            const auto ConsumeStart = Now(); Require(O.Snapshot(S), "consume many"); ConsumeMs.push_back(Ms(Now() - ConsumeStart));
            Prior = S.ThroughSequence; Ack(O, S);
        }
        Require(!Unexpected && Read(Large / L"selected.hjson") == "100\n" && Read(Large / L"unchanged-0.hjson") == "unchanged 0\n"
            && Read(Large / L"unchanged-19999.hjson") == "unchanged 19999\n", "many oracle/candidate scope");
        O.Close();
        Cases.push_back("twenty_thousand_unchanged_hundred_selected_mutations");

        // A tiny real kernel buffer plus concurrent real mutations. Report only observed OS loss.
        const auto Flood = Run / L"overflow"; fs::create_directory(Flood);
        { FOverflowChild Child; Child.Run(Flood); }
        const bool Overflow = true; // Child exit 0 required actual ERROR_NOTIFY_ENUM_DIR/zero-byte completion.
        Cases.push_back("controlled_real_kernel_overflow_sticky_error");
        std::cout << std::fixed << std::setprecision(6) << "{\"success\":true,\"functional_scenarios\":" << Cases.size() << ",\"cases\":[";
        for (std::size_t I = 0; I < Cases.size(); ++I) { if (I) std::cout << ','; std::cout << '"' << Cases[I] << '"'; }
        std::cout << "],\"lifecycle_cycles\":20,\"handle_delta\":" << static_cast<int>(HandlesAfter) - static_cast<int>(HandlesBefore)
            << ",\"small_start_ms\":" << Ms(StartNs) << ",\"unchanged_files\":20000,\"seed_ms\":" << Ms(SeedNs) << ",\"many_start_ms\":" << Ms(ManyStartNs)
            << ",\"unexpected_hot_paths\":" << Unexpected << ",\"burst_total_raw_records\":" << BurstRecords << ",\"real_overflow_observed\":" << (Overflow ? "true" : "false") << ',';
        Distribution("writer_flush", WriteMs); std::cout << ','; Distribution("mutation_start_to_application_collection", CollectedMs);
        std::cout << ','; Distribution("snapshot_call", ConsumeMs); std::cout << ','; Distribution("mutation_start_to_consume", EndToEndMs); std::cout << "}\n";
        // Only known test fixture paths are removed. Preserve result/build logs outside them.
        fs::remove_all(Run / L"small-renamed"); fs::remove_all(Large); fs::remove_all(Flood);
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 3; }
}
