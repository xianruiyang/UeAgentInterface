#include "UeAgentWorkspaceChangeObserver.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#if defined(UAI_FILE_OBSERVER_STANDALONE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include "Windows/WindowsHWrapper.h"
#endif
#endif

namespace UE::UAI::HjsonWorkspace::FileChanges
{
std::uint64_t FObserver::MonotonicNanoseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

#if defined(_WIN32)
namespace
{
using FReadChanges = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD,
    LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE, int);
// minwinbase.h gates the enum on NTDDI_WIN10_RS3, while UE deliberately
// compiles against an older NTDDI. This dynamically loaded API still uses the
// SDK's 32-bit enum ABI/value (Extended=2); no older notification API is tried.
constexpr int ExtendedNotifyInformation = 2;
#if defined(UAI_FILE_OBSERVER_STANDALONE)
static_assert(sizeof(READ_DIRECTORY_NOTIFY_INFORMATION_CLASS) == sizeof(int)
    && ReadDirectoryNotifyExtendedInformation == ExtendedNotifyInformation);
#endif
std::uint64_t Identity(const BY_HANDLE_FILE_INFORMATION& Info)
{ return (static_cast<std::uint64_t>(Info.nFileIndexHigh) << 32) | Info.nFileIndexLow; }
bool SafeRelative(const std::wstring& Name)
{
    if (Name.empty() || Name.front() == L'\\' || Name.find(L':') != std::wstring::npos || Name.find(L'\0') != std::wstring::npos) return false;
    std::size_t At = 0;
    while (At < Name.size()) {
        const auto End = Name.find(L'\\', At);
        const auto Part = Name.substr(At, End == std::wstring::npos ? End : End - At);
        if (Part.empty() || Part == L"." || Part == L"..") return false;
        if (End == std::wstring::npos) break;
        At = End + 1;
    }
    return true;
}
}

struct FObserver::FState
{
    struct FChannel
    {
        HANDLE Handle = INVALID_HANDLE_VALUE;
        OVERLAPPED Io{};
        std::vector<DWORD> Buffer; // Native contract aligns the address, not the byte count.
        DWORD BufferBytes = 0;
        bool Pending = false;
    };
    std::mutex Mutex;
    EStatus Status = EStatus::Closed;
    FError Error;
    std::wstring Root, Parent, RootName;
    std::uint64_t RootId = 0;
    DWORD Volume = 0;
    FReadChanges Read = nullptr;
    FChannel Channels[2]; // Recursive root and nonrecursive parent (root identity).
    HANDLE Stop = nullptr;
    std::thread Worker;
    // Preserve exact spellings, including case-only renames. Do not collapse
    // distinct names in NTFS case-sensitive subdirectories.
    std::map<std::wstring, FCandidate> Pending;
    std::map<std::uint64_t, FCandidate> RenameOld;
    std::vector<FRename> Renames;
    std::uint64_t Sequence = 0, Records = 0, Batches = 0;

    void Fail(EError Code, DWORD Native, const wchar_t* Stage, const std::wstring& Path)
    {
        std::lock_guard<std::mutex> Guard(Mutex);
        if (Status == EStatus::Failed) return;
        Status = EStatus::Failed; Error = {Code, Native, Stage, Path};
    }
    bool Arm(FChannel& Channel, bool Recursive)
    {
        ResetEvent(Channel.Io.hEvent);
        Channel.Pending = Read(Channel.Handle, Channel.Buffer.data(), Channel.BufferBytes, Recursive,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE
                | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES,
            nullptr, &Channel.Io, nullptr, ExtendedNotifyInformation) != 0;
        if (!Channel.Pending) Fail(GetLastError() == ERROR_INVALID_FUNCTION ? EError::Unsupported : EError::ReadFailed,
            GetLastError(), L"ReadDirectoryChangesExW", Root);
        return Channel.Pending;
    }
    bool CheckRoot()
    {
        HANDLE Handle = CreateFileW(Root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION Info{};
        const bool Same = Handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(Handle, &Info)
            && Info.dwVolumeSerialNumber == Volume && Identity(Info) == RootId
            && (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        const DWORD Native = Same ? ERROR_SUCCESS : GetLastError();
        if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
        if (!Same) Fail(EError::RootChanged, Native, L"watched_root_identity", Root);
        return Same;
    }
    bool Parse(const FChannel& Channel, DWORD Count, bool ParentEvents)
    {
        const auto Now = MonotonicNanoseconds();
        std::lock_guard<std::mutex> Guard(Mutex);
        ++Batches;
        const auto Bad = [&]() {
            Status = EStatus::Failed; Error = {EError::MalformedNotification, ERROR_INVALID_DATA, L"notification_record", Root}; return false;
        };
        if (Count > Channel.BufferBytes) return Bad();
        const auto* Bytes = reinterpret_cast<const unsigned char*>(Channel.Buffer.data());
        std::size_t At = 0;
        for (;;) {
            constexpr std::size_t Header = offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName);
            if (Count - At < Header) return Bad();
            FILE_NOTIFY_EXTENDED_INFORMATION Record{};
            std::memcpy(&Record, Bytes + At, Header);
            if (Record.FileNameLength % sizeof(wchar_t) || Record.FileNameLength > Count - At - Header) return Bad();
            std::wstring Path(Record.FileNameLength / sizeof(wchar_t), L'\0');
            std::memcpy(Path.data(), Bytes + At + Header, Record.FileNameLength);
            if (!SafeRelative(Path)) return Bad();
            const auto Id = static_cast<std::uint64_t>(Record.FileId.QuadPart);
            if (ParentEvents) {
                // Do not invalidate on a different, case-sensitive sibling's
                // name. NTFS IDs are authoritative; exact name is only for an
                // unidentified record. Snapshot also reopens the bound root.
                if ((Id == RootId || (!Id && Path == RootName)) && (Record.Action == FILE_ACTION_REMOVED || Record.Action == FILE_ACTION_RENAMED_OLD_NAME)) {
                    Status = EStatus::Failed; Error = {EError::RootChanged, 0, L"watched_root_removed_or_renamed", Root}; return false;
                }
            } else {
                if (Record.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    Status = EStatus::Failed; Error = {EError::Unsupported, ERROR_NOT_SUPPORTED, L"reparse_candidate", Path}; return false;
                }
                const std::uint32_t Flag = Record.Action == FILE_ACTION_ADDED ? Added : Record.Action == FILE_ACTION_REMOVED ? Removed
                    : Record.Action == FILE_ACTION_MODIFIED ? Modified : Record.Action == FILE_ACTION_RENAMED_OLD_NAME ? RenameFrom
                    : Record.Action == FILE_ACTION_RENAMED_NEW_NAME ? RenameTo : 0;
                if (!Flag) return Bad();
                ++Records; ++Sequence;
                auto& Candidate = Pending[Path];
                if (!Candidate.FirstSequence) { Candidate.Path = Path; Candidate.FirstSequence = Sequence; Candidate.FirstObservedNs = Now; }
                Candidate.FileId = Id; Candidate.ParentFileId = static_cast<std::uint64_t>(Record.ParentFileId.QuadPart);
                Candidate.Directory |= (Record.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                Candidate.Changes |= Flag; Candidate.LastSequence = Sequence; Candidate.LastObservedNs = Now;
                if (Flag == RenameFrom && Id) RenameOld[Id] = Candidate;
                if (Flag == RenameTo && Id) {
                    const auto Old = RenameOld.find(Id);
                    if (Old != RenameOld.end()) {
                        Renames.push_back({Old->second.Path, Path, Id, Sequence, Candidate.Directory}); RenameOld.erase(Old);
                    }
                }
                // A removal/addition breaks an unfinished rename incarnation.
                if ((Flag == Added || Flag == Removed) && Id) RenameOld.erase(Id);
            }
            if (!Record.NextEntryOffset) break;
            if (Record.NextEntryOffset < Header + Record.FileNameLength || Record.NextEntryOffset % sizeof(DWORD)
                || Record.NextEntryOffset > Count - At - Header) return Bad();
            At += Record.NextEntryOffset;
        }
        return true;
    }
    void Run()
    {
        HANDLE Events[] = {Stop, Channels[0].Io.hEvent, Channels[1].Io.hEvent};
        for (;;) {
            const auto Wait = WaitForMultipleObjects(3, Events, 0, INFINITE);
            if (Wait == WAIT_OBJECT_0) break;
            if (Wait < WAIT_OBJECT_0 + 1 || Wait > WAIT_OBJECT_0 + 2) { Fail(EError::ReadFailed, GetLastError(), L"wait_notification", Root); break; }
            const unsigned Index = Wait - WAIT_OBJECT_0 - 1;
            auto& Channel = Channels[Index];
            DWORD Count = 0;
            const BOOL Complete = GetOverlappedResult(Channel.Handle, &Channel.Io, &Count, 0);
            Channel.Pending = false;
            if (!Complete) { Fail(GetLastError() == ERROR_NOTIFY_ENUM_DIR ? EError::NotificationLost : EError::ReadFailed,
                GetLastError(), L"notification_completion", Root); break; }
            if (!Count) { Fail(EError::NotificationLost, ERROR_NOTIFY_ENUM_DIR, L"notification_overflow", Root); break; }
            if (!Parse(Channel, Count, Index == 1) || !Arm(Channel, Index == 0)) break;
        }
        // Only this collector touches OVERLAPPEDs. Drain before destruction.
        for (auto& Channel : Channels) if (Channel.Pending) {
            CancelIoEx(Channel.Handle, &Channel.Io);
            DWORD Ignored = 0; GetOverlappedResult(Channel.Handle, &Channel.Io, &Ignored, 1); Channel.Pending = false;
        }
    }
    ~FState()
    {
        if (Stop) SetEvent(Stop);
        if (Worker.joinable()) Worker.join();
        for (auto& Channel : Channels) {
            if (Channel.Pending) { CancelIoEx(Channel.Handle, &Channel.Io); DWORD Count = 0; GetOverlappedResult(Channel.Handle, &Channel.Io, &Count, 1); }
            if (Channel.Io.hEvent) CloseHandle(Channel.Io.hEvent);
            if (Channel.Handle != INVALID_HANDLE_VALUE) CloseHandle(Channel.Handle);
        }
        if (Stop) CloseHandle(Stop);
    }
};
#else
struct FObserver::FState {};
#endif

FObserver::FObserver() = default;
FObserver::~FObserver() = default;
bool FObserver::Start(const std::wstring& AbsoluteRoot, FError& Error, FOptions Options)
{
    Error = {};
    if (State) { Error = {EError::AlreadyStarted, 0, L"close_existing_observation_explicitly", AbsoluteRoot}; return false; }
#if defined(_WIN32)
    auto Next = std::make_unique<FState>();
    const auto Fail = [&](EError Code, DWORD Native, const wchar_t* Stage) {
        Error = {Code, Native, Stage, AbsoluteRoot}; return false;
    };
    if (AbsoluteRoot.empty())
        return Fail(EError::InvalidArgument, ERROR_INVALID_PARAMETER, L"observer_start");
    std::wstring Full(32768, L'\0');
    const DWORD Length = GetFullPathNameW(AbsoluteRoot.c_str(), static_cast<DWORD>(Full.size()), Full.data(), nullptr);
    if (!Length || Length >= Full.size()) return Fail(EError::InvalidArgument, GetLastError(), L"absolute_root");
    Full.resize(Length);
    while (Full.size() > 3 && (Full.back() == L'\\' || Full.back() == L'/')) Full.pop_back();
    const auto Split = Full.find_last_of(L"\\/");
    if (Split == std::wstring::npos || Split + 1 == Full.size()) return Fail(EError::InvalidArgument, ERROR_INVALID_NAME, L"root_requires_parent");
    Next->Root = Full; Next->Parent = Full.substr(0, Split + 1); Next->RootName = Full.substr(Split + 1);
    Next->Read = reinterpret_cast<FReadChanges>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadDirectoryChangesExW"));
    if (!Next->Read) return Fail(EError::Unsupported, ERROR_PROC_NOT_FOUND, L"ReadDirectoryChangesExW");
    for (unsigned Index = 0; Index != 2; ++Index) {
        auto& Channel = Next->Channels[Index];
        const auto& Path = Index ? Next->Parent : Next->Root;
        Channel.Handle = CreateFileW(Path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (Channel.Handle == INVALID_HANDLE_VALUE) return Fail(EError::OpenFailed, GetLastError(), L"open_directory");
        Channel.Io.hEvent = CreateEventW(nullptr, 1, 0, nullptr);
        if (!Channel.Io.hEvent) return Fail(EError::OpenFailed, GetLastError(), L"create_notification_event");
        Channel.BufferBytes = Options.BufferBytes;
        // Keep even a zero-length native request's pointer valid/aligned; pass
        // the requested length unchanged and report the real OS outcome.
        Channel.Buffer.resize(std::max<std::size_t>(1, (static_cast<std::size_t>(Options.BufferBytes) + sizeof(DWORD) - 1) / sizeof(DWORD)));
    }
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(Next->Channels[0].Handle, &Info)) return Fail(EError::OpenFailed, GetLastError(), L"root_identity");
    if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return Fail(EError::Unsupported, ERROR_NOT_SUPPORTED, L"ordinary_directory_required");
    wchar_t FileSystem[32]{};
    if (!GetVolumeInformationByHandleW(Next->Channels[0].Handle, nullptr, 0, nullptr, nullptr, nullptr, FileSystem, 32))
        return Fail(EError::OpenFailed, GetLastError(), L"volume_capability");
    if (std::wstring(FileSystem) != L"NTFS") return Fail(EError::Unsupported, ERROR_NOT_SUPPORTED, L"NTFS_extended_notifications_required");
    Next->RootId = Identity(Info); Next->Volume = Info.dwVolumeSerialNumber;
    Next->Stop = CreateEventW(nullptr, 1, 0, nullptr);
    if (!Next->Stop) return Fail(EError::OpenFailed, GetLastError(), L"create_stop_event");
    Next->Status = EStatus::Watching;
    if (!Next->Arm(Next->Channels[1], false) || !Next->Arm(Next->Channels[0], true)) { Error = Next->Error; return false; }
    if (!Next->CheckRoot()) { Error = Next->Error; return false; }
    auto* Raw = Next.get();
    Next->Worker = std::thread([Raw] { Raw->Run(); });
    State = std::move(Next); return true;
#else
    (void)Options; Error = {EError::Unsupported, 0, L"Windows_NTFS_required", AbsoluteRoot}; return false;
#endif
}
bool FObserver::Snapshot(FSnapshot& Out)
{
    Out = {};
#if defined(_WIN32)
    if (State) {
        State->CheckRoot();
        std::lock_guard<std::mutex> Guard(State->Mutex);
        Out.Status = State->Status; Out.Error = State->Error;
        Out.RootFileId = State->RootId; Out.VolumeSerial = State->Volume;
        Out.ThroughSequence = State->Sequence; Out.NotificationRecords = State->Records; Out.CompletedBatches = State->Batches;
        Out.Candidates.reserve(State->Pending.size());
        for (const auto& Item : State->Pending) Out.Candidates.push_back(Item.second);
        Out.Renames = State->Renames;
        return Out.Status == EStatus::Watching;
    }
#endif
    Out.Error = {EError::Closed, 0, L"observer_not_running", {}}; return false;
}
bool FObserver::Acknowledge(std::uint64_t Through, FError& Error)
{
    Error = {};
#if defined(_WIN32)
    if (State) {
        std::lock_guard<std::mutex> Guard(State->Mutex);
        if (State->Status != EStatus::Watching) { Error = State->Error; return false; }
        if (Through > State->Sequence) { Error = {EError::InvalidArgument, 0, L"acknowledge_future_sequence", State->Root}; return false; }
        for (auto It = State->Pending.begin(); It != State->Pending.end(); ) {
            if (It->second.LastSequence <= Through) It = State->Pending.erase(It); else ++It;
        }
        State->Renames.erase(std::remove_if(State->Renames.begin(), State->Renames.end(),
            [Through](const FRename& Rename) { return Rename.Sequence <= Through; }), State->Renames.end());
        for (auto It = State->RenameOld.begin(); It != State->RenameOld.end(); ) {
            if (It->second.LastSequence <= Through) It = State->RenameOld.erase(It); else ++It;
        }
        return true;
    }
#else
    (void)Through;
#endif
    Error = {EError::Closed, 0, L"observer_not_running", {}}; return false;
}
void FObserver::Close() { State.reset(); }
}
