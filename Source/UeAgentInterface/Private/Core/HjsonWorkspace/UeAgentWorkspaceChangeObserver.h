#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace UE::UAI::HjsonWorkspace::FileChanges
{

// Continuous-session OS candidates, not byte equality or UE edit intent.
enum class EStatus { Closed, Watching, Failed };
enum class EError { None, Closed, InvalidArgument, AlreadyStarted, Unsupported, OpenFailed,
    ReadFailed, NotificationLost, RootChanged, MalformedNotification };
enum EChange : std::uint32_t
{
    Added = 1, Removed = 2, Modified = 4, RenameFrom = 8, RenameTo = 16
};
struct FError
{
    EError Code = EError::None;
    std::uint32_t NativeCode = 0;
    std::wstring Stage, Path;
};
struct FCandidate
{
    std::wstring Path; // Relative to the original watched root; Windows spelling.
    std::uint64_t FileId = 0, ParentFileId = 0; // NTFS Ex notification's 64-bit IDs.
    std::uint32_t Changes = 0;
    bool Directory = false; // A directory invalidates its subtree, not just this name.
    std::uint64_t FirstSequence = 0, LastSequence = 0;
    std::uint64_t FirstObservedNs = 0, LastObservedNs = 0;
};
struct FRename
{
    std::wstring OldPath, NewPath;
    std::uint64_t FileId = 0, Sequence = 0;
    bool Directory = false;
};
struct FSnapshot
{
    EStatus Status = EStatus::Closed;
    FError Error;
    std::vector<FCandidate> Candidates;
    std::vector<FRename> Renames;
    std::uint64_t RootFileId = 0;
    std::uint32_t VolumeSerial = 0; // IDs are meaningful only in this session/volume.
    std::uint64_t ThroughSequence = 0, NotificationRecords = 0, CompletedBatches = 0;
};
struct FOptions { std::uint32_t BufferBytes = 64 * 1024; };

// Source/platform owner. No UE state, directory enumeration, content reads/hash,
// polling fallback, USN, persistent cursor or author/system attribution.
// Start and Close are lifecycle-thread operations; Snapshot/Acknowledge may run
// concurrently with collection. A failed observer stays failed until closed.
class FObserver final
{
public:
    FObserver();
    ~FObserver();
    FObserver(const FObserver&) = delete;
    FObserver& operator=(const FObserver&) = delete;
    bool Start(const std::wstring& AbsoluteRoot, FError& Error, FOptions Options = {});
    bool Snapshot(FSnapshot& Out); // Does not consume; false never means "clean".
    bool Acknowledge(std::uint64_t ThroughSequence, FError& Error);
    void Close(); // Cancel + drain pending kernel IO + join before freeing buffers.
    static std::uint64_t MonotonicNanoseconds();
private:
    struct FState;
    std::unique_ptr<FState> State;
};

}
