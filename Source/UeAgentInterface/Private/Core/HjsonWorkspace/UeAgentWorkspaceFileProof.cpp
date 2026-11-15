#include "Core/HjsonWorkspace/UeAgentWorkspaceFileProof.h"
#include "Core/FileSystem/UeAgentWindowsFileAccess.h"

#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"

#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winioctl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace UE::UAI::HjsonWorkspace
{
    namespace
    {
        thread_local TMap<FString, TWeakPtr<const FWorkspaceFileProof>> Observations;
        thread_local uint64 LastIndexPruneRetirement = 0;
        thread_local bool bPrunedAtCapacity = false;
    }
    static thread_local FWorkspaceCurrentFileScope* CurrentFileScope = nullptr;
    FWorkspaceCurrentFileScope::FWorkspaceCurrentFileScope(TFunction<void*(const FString&)> Resolver)
        : Resolve(MoveTemp(Resolver)), Previous(CurrentFileScope) { CurrentFileScope = this; }
    FWorkspaceCurrentFileScope::~FWorkspaceCurrentFileScope() { CurrentFileScope = Previous; }
#if PLATFORM_WINDOWS
    // UE's compile target can hide the Win8 SDK spelling while the running
    // OS supports it. This is the documented FILE_ID_INFO ABI, not a reduced
    // 64-bit identity. Unsupported kernels fail the query and lose reuse.
    struct FFileIdentityInfo { uint64 VolumeSerialNumber; uint8 FileId[16]; };
    static_assert(sizeof(FFileIdentityInfo) == 24);
    constexpr auto FileIdentityInfoClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
#endif
    struct FWorkspaceFileLeaseState
    {
        mutable FCriticalSection Mutex;
        FString Path;
#if PLATFORM_WINDOWS
        HANDLE File = INVALID_HANDLE_VALUE;
        OVERLAPPED Overlapped{};
        REQUEST_OPLOCK_INPUT_BUFFER Input{};
        REQUEST_OPLOCK_OUTPUT_BUFFER Output{};
        FFileIdentityInfo Identity{};
#endif
        bool bPending = false;

        void Cancel()
        {
            FScopeLock Lock(&Mutex);
            bPending = false;
#if PLATFORM_WINDOWS
            if (File != INVALID_HANDLE_VALUE) CancelIoEx(File, &Overlapped);
#endif
        }

        void Complete()
        {
            FScopeLock Lock(&Mutex);
            // Invalidate before close: the H-break waiter can then proceed with
            // atomic-save/rename. No GameThread callback or acknowledgment wait.
            bPending = false;
#if PLATFORM_WINDOWS
            if (File != INVALID_HANDLE_VALUE) { CloseHandle(File); File = INVALID_HANDLE_VALUE; }
#endif
        }

        bool IsCurrent() const
        {
#if PLATFORM_WINDOWS
            if (CurrentFileScope)
                if (void* Current = CurrentFileScope->Resolve(Path))
                    return FileSystem::Windows::IsOrdinary(Current, false) && IsCurrentHandle(Current);
            DWORD Transferred = 0;
            {
                FScopeLock Lock(&Mutex);
                if (!bPending || File == INVALID_HANDLE_VALUE) return false;
                if (GetOverlappedResult(File, const_cast<OVERLAPPED*>(&Overlapped), &Transferred, false)
                    || GetLastError() != ERROR_IO_INCOMPLETE) return false;
            }
            // The file's RH lease alone does not prove its ancestor path binding.
            // Reopen attributes at the current path; never infer identity from a
            // timestamp, user notification or a handle to a renamed old file.
            HANDLE Current = CreateFileW(*Path, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Current == INVALID_HANDLE_VALUE) return false;
            FFileIdentityInfo CurrentIdentity{};
            FILE_ATTRIBUTE_TAG_INFO Attributes{};
            const bool bSame = GetFileInformationByHandleEx(Current, FileIdentityInfoClass, &CurrentIdentity, sizeof(CurrentIdentity))
                && GetFileInformationByHandleEx(Current, FileAttributeTagInfo, &Attributes, sizeof(Attributes))
                && !(Attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                && CurrentIdentity.VolumeSerialNumber == Identity.VolumeSerialNumber
                && FMemory::Memcmp(CurrentIdentity.FileId, Identity.FileId, sizeof(Identity.FileId)) == 0;
            CloseHandle(Current);
            // A writer can break the lease during the identity observation.
            // Do not hold Mutex across CreateFile: an H-break waiter may need
            // our worker to close the old handle before that open can proceed.
            FScopeLock Lock(&Mutex);
            return bSame && bPending && File != INVALID_HANDLE_VALUE
                && !GetOverlappedResult(File, const_cast<OVERLAPPED*>(&Overlapped), &Transferred, false)
                && GetLastError() == ERROR_IO_INCOMPLETE;
#else
            return false;
#endif
        }

        bool IsCurrentHandle(void* Current) const
        {
#if PLATFORM_WINDOWS
            DWORD Transferred = 0;
            FScopeLock Lock(&Mutex);
            if (!bPending || File == INVALID_HANDLE_VALUE
                || GetOverlappedResult(File, const_cast<OVERLAPPED*>(&Overlapped), &Transferred, false)
                || GetLastError() != ERROR_IO_INCOMPLETE) return false;
            FFileIdentityInfo CurrentIdentity{};
            const bool bSame = GetFileInformationByHandleEx(Current, FileIdentityInfoClass, &CurrentIdentity, sizeof(CurrentIdentity))
                && CurrentIdentity.VolumeSerialNumber == Identity.VolumeSerialNumber
                && FMemory::Memcmp(CurrentIdentity.FileId, Identity.FileId, sizeof(Identity.FileId)) == 0;
            return bSame && !GetOverlappedResult(File, const_cast<OVERLAPPED*>(&Overlapped), &Transferred, false)
                && GetLastError() == ERROR_IO_INCOMPLETE;
#else
            return false;
#endif
        }

        bool IsLeasePending() const
        {
#if PLATFORM_WINDOWS
            DWORD Transferred = 0;
            FScopeLock Lock(&Mutex);
            return bPending && File != INVALID_HANDLE_VALUE
                && !GetOverlappedResult(File, const_cast<OVERLAPPED*>(&Overlapped), &Transferred, false)
                && GetLastError() == ERROR_IO_INCOMPLETE;
#else
            return false;
#endif
        }
    };

    struct FWorkspaceReadPathScope::FState
    {
        FString Root;
        bool bRejected = false;
        int32 ReusedCurrentHandles = 0;
        int32 OpenedCurrentHandles = 0;
#if PLATFORM_WINDOWS
        HANDLE Directory = INVALID_HANDLE_VALUE;
        ~FState() { if (Directory != INVALID_HANDLE_VALUE) CloseHandle(Directory); }
#endif
    };

    FWorkspaceReadPathScope::FWorkspaceReadPathScope(const FString& Root) : State(MakeUnique<FState>())
    {
        State->Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(State->Root);
#if PLATFORM_WINDOWS
        // UNC and other backends retain the existing public path implementation.
        if (State->Root.Len() < 3 || State->Root[1] != TCHAR(':') || State->Root[2] != TCHAR('/')) return;
        State->Directory = CreateFileW(*State->Root, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (State->Directory != INVALID_HANDLE_VALUE) {
            FILE_ATTRIBUTE_TAG_INFO Attributes{};
            const bool bAttributes = GetFileInformationByHandleEx(State->Directory, FileAttributeTagInfo, &Attributes, sizeof(Attributes));
            State->bRejected = bAttributes && ((Attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                || !(Attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY));
            if (!bAttributes || State->bRejected || !MatchWorkspaceFileHandles(State->Directory, State->Directory)) {
                CloseHandle(State->Directory); State->Directory = INVALID_HANDLE_VALUE;
            }
        }
#endif
    }
    FWorkspaceReadPathScope::~FWorkspaceReadPathScope() = default;
    int32 FWorkspaceReadPathScope::GetReusedCurrentHandles() const { return State->ReusedCurrentHandles; }
    int32 FWorkspaceReadPathScope::GetOpenedCurrentHandles() const { return State->OpenedCurrentHandles; }
    bool FWorkspaceReadPathScope::IsRejected() const { return State->bRejected; }
    bool FWorkspaceReadPathScope::IsAvailable() const
    {
#if PLATFORM_WINDOWS
        return State->Directory != INVALID_HANDLE_VALUE;
#else
        return false;
#endif
    }
    bool FWorkspaceReadPathScope::IsCurrent() const
    {
#if PLATFORM_WINDOWS
        if (!IsAvailable()) return false;
        HANDLE Current = CreateFileW(*State->Root, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (Current == INVALID_HANDLE_VALUE) return false;
        const bool bCurrent = FileSystem::Windows::IsOrdinary(Current, true)
            && MatchWorkspaceFileHandles(State->Directory, Current);
        CloseHandle(Current);
        return bCurrent;
#else
        return false;
#endif
    }
    bool FWorkspaceReadPathScope::Observe(const FString& Path,
        TFunctionRef<bool(void*, int64, int64)> Consumer) const
    {
#if PLATFORM_WINDOWS
        return ObserveImpl(Path, [&](void* Handle) {
            BY_HANDLE_FILE_INFORMATION Info{}; SYSTEMTIME Time{};
            if (!GetFileInformationByHandle(Handle, &Info)
                || (Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                || !FileTimeToSystemTime(&Info.ftLastWriteTime, &Time)) return false;
            // Match WindowsPlatformFile::GetStatData's existing second precision.
            const int64 Ticks = FDateTime(Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond).GetTicks();
            return Consumer(Handle, (int64(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow, Ticks);
        }, false);
#else
        return false;
#endif
    }
    bool FWorkspaceReadPathScope::ObserveHandle(const FString& Path, TFunctionRef<bool(void*)> Consumer) const
    {
        return ObserveImpl(Path, Consumer, true);
    }
    bool FWorkspaceReadPathScope::ObserveImpl(const FString& Path, TFunctionRef<bool(void*)> Consumer, bool bCheckOrdinary) const
    {
#if PLATFORM_WINDOWS
        const auto Failed = [&](const TCHAR* Stage, DWORD NativeError, bool bBorrowed = false) {
            UE_LOG(LogTemp, Warning, TEXT("UAI source path observation_failed stage=%s path=%s win32=%lu borrowed_current_guard=%s"),
                Stage, *Path, NativeError, bBorrowed ? TEXT("true") : TEXT("false"));
            return false;
        };
        if (!IsAvailable()) return Failed(TEXT("directory_handle"), ERROR_INVALID_HANDLE);
        const FString Prefix = State->Root + TEXT("/");
        if (!Path.StartsWith(Prefix, ESearchCase::IgnoreCase)) return Failed(TEXT("outside_root"), ERROR_INVALID_NAME);
        FString Relative = Path.Mid(Prefix.Len());
        Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
        TArray<FString> Parts; Relative.ParseIntoArray(Parts, TEXT("/"), false);
        for (const FString& Part : Parts)
            if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.Contains(TEXT(":"))) return Failed(TEXT("relative_path"), ERROR_INVALID_NAME);
        Relative.ReplaceInline(TEXT("/"), TEXT("\\"));
        // A publisher may already own this exact logical leaf and its complete
        // directory chain. Borrow that still-current guard; it is not a cached
        // identity or an old lease handle. New names and guard gaps still use
        // the ordinary relative open, and all current metadata/content checks
        // below execute for either source of the handle.
        HANDLE File = CurrentFileScope ? CurrentFileScope->Resolve(Path) : nullptr;
        const bool bBorrowed = File != nullptr;
        if (bBorrowed) ++State->ReusedCurrentHandles;
        else {
            ++State->OpenedCurrentHandles;
            File = FileSystem::Windows::OpenRelative(State->Directory, Relative, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 1 /* FILE_OPEN */, 0x40 /* NON_DIRECTORY */);
        }
        if (File == INVALID_HANDLE_VALUE) return Failed(TEXT("relative_open"), GetLastError());
        bool bSuccess = !bCheckOrdinary || FileSystem::Windows::IsOrdinary(File, false);
        if (!bSuccess) Failed(TEXT("file_metadata"), GetLastError(), bBorrowed);
        if (bSuccess) {
            bSuccess = Consumer(File);
            if (!bSuccess) Failed(TEXT("source_consumer"), ERROR_SUCCESS, bBorrowed); // Semantic rejection is not an OS deletion error.
        }
        if (!bBorrowed) CloseHandle(File);
        return bSuccess;
#else
        return false;
#endif
    }
    bool FWorkspaceReadPathScope::ReadBytes(void* CurrentFile, TArray<uint8>& Bytes)
    {
        Bytes.Reset();
#if PLATFORM_WINDOWS
        HANDLE ReadHandle = ReOpenFile(CurrentFile, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        if (ReadHandle == INVALID_HANDLE_VALUE) {
            UE_LOG(LogTemp, Warning, TEXT("UAI source bytes read_failed stage=reopen win32=%lu"), GetLastError()); return false;
        }
        LARGE_INTEGER Size{};
        bool bSuccess = GetFileSizeEx(ReadHandle, &Size) && Size.QuadPart >= 0 && Size.QuadPart <= MAX_int32 - 2;
        if (bSuccess) {
            Bytes.SetNumUninitialized(static_cast<int32>(Size.QuadPart));
            int32 Offset = 0;
            while (Offset < Bytes.Num()) {
                DWORD Read = 0;
                if (!ReadFile(ReadHandle, Bytes.GetData() + Offset, Bytes.Num() - Offset, &Read, nullptr) || !Read) {
                    bSuccess = false; break;
                }
                Offset += Read;
            }
        }
        const DWORD Failure = bSuccess ? ERROR_SUCCESS : GetLastError();
        if (!bSuccess) UE_LOG(LogTemp, Warning, TEXT("UAI source bytes read_failed stage=body win32=%lu"), Failure);
        CloseHandle(ReadHandle);
        if (!bSuccess) SetLastError(Failure);
        return bSuccess;
#else
        return false;
#endif
    }

    bool FWorkspaceReadPathScope::ReadRange(void* CurrentFile, int64 Begin, int64 End, TArray<uint8>& Bytes)
    {
        Bytes.Reset();
#if PLATFORM_WINDOWS
        if (Begin < 0 || End <= Begin || End - Begin > MAX_int32) return false;
        HANDLE ReadHandle = ReOpenFile(CurrentFile, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        if (ReadHandle == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER Size{}, Position{}; Position.QuadPart = Begin;
        bool bSuccess = GetFileSizeEx(ReadHandle, &Size) && End <= Size.QuadPart
            && SetFilePointerEx(ReadHandle, Position, nullptr, FILE_BEGIN);
        if (bSuccess) {
            Bytes.SetNumUninitialized(static_cast<int32>(End - Begin));
            int32 Offset = 0;
            while (Offset < Bytes.Num()) {
                DWORD Read = 0;
                if (!ReadFile(ReadHandle, Bytes.GetData() + Offset, Bytes.Num() - Offset, &Read, nullptr) || !Read) {
                    bSuccess = false; break;
                }
                Offset += Read;
            }
        }
        const DWORD Failure = bSuccess ? ERROR_SUCCESS : GetLastError();
        CloseHandle(ReadHandle);
        if (!bSuccess) {
            UE_LOG(LogTemp, Warning, TEXT("UAI source range read_failed begin=%lld end=%lld win32=%lu"), Begin, End, Failure);
            Bytes.Reset(); SetLastError(Failure);
        }
        return bSuccess;
#else
        return false;
#endif
    }

    bool FWorkspaceReadPathScope::ReadText(void* CurrentFile, FString& Text)
    {
        TArray<uint8> Bytes;
        if (!ReadBytes(CurrentFile, Bytes)) return false;
        if (Bytes.IsEmpty()) Text.Reset();
        else FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
        return true;
    }

#if PLATFORM_WINDOWS
    namespace
    {
        class FFileProofCompletionOwner final : public FRunnable
        {
        public:
            ~FFileProofCompletionOwner() { Shutdown(); }

            TSharedPtr<FWorkspaceFileLeaseState, ESPMode::ThreadSafe> Begin(const FString& Path,
                bool* OutBudgetExhausted)
            {
                FReadScopeLock Lifetime(Lifecycle);
                // One worker, bounded live kernel operations and path storage;
                // no contents/AST/model are retained by this backend.
                uint64 CompletionKey = 0;
                {
                    FScopeLock Lock(&Mutex);
                    if (bStopping || NextCompletionKey == MAX_uint64) return {};
                    if (Pending.Num() >= 16384 || PathCharacters + Path.Len() > 4 * 1024 * 1024)
                    {
                        if (OutBudgetExhausted) *OutBudgetExhausted = true;
                        return {};
                    }
                    if (!Port)
                    {
                        Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
                        if (!Port) return {};
                        Thread.Reset(FRunnableThread::Create(this, TEXT("UAIWorkspaceFileProof")));
                        if (!Thread) { CloseHandle(Port); Port = nullptr; return {}; }
                    }
                    CompletionKey = NextCompletionKey++;
                }
                auto State = MakeShared<FWorkspaceFileLeaseState, ESPMode::ThreadSafe>();
                State->Path = Path;
                // File opens can wait for another lease's H-break. The completion
                // worker must remain free to remove and close that older lease.
                State->File = CreateFileW(*Path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (State->File == INVALID_HANDLE_VALUE) return {};
                FILE_ATTRIBUTE_TAG_INFO Attributes{};
                if (!GetFileInformationByHandleEx(State->File, FileIdentityInfoClass, &State->Identity, sizeof(State->Identity))
                    || !GetFileInformationByHandleEx(State->File, FileAttributeTagInfo, &Attributes, sizeof(Attributes))
                    || (Attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                    || !CreateIoCompletionPort(State->File, Port, static_cast<ULONG_PTR>(CompletionKey), 0))
                { State->Complete(); return {}; }
                State->Input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
                State->Input.StructureLength = sizeof(State->Input);
                State->Input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE;
                State->Input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
                State->bPending = true;
                {
                    FScopeLock Lock(&Mutex);
                    if (Pending.Num() >= 16384 || PathCharacters + Path.Len() > 4 * 1024 * 1024)
                    {
                        if (OutBudgetExhausted) *OutBudgetExhausted = true;
                        State->Complete(); return {};
                    }
                    Pending.Add(CompletionKey, State);
                    PathCharacters += Path.Len();
                }
                DWORD Transferred = 0;
                const BOOL Immediate = DeviceIoControl(State->File, FSCTL_REQUEST_OPLOCK,
                    &State->Input, sizeof(State->Input), &State->Output, sizeof(State->Output),
                    &Transferred, &State->Overlapped);
                if (Immediate || GetLastError() != ERROR_IO_PENDING)
                {
                    // Only a pending request can prove an unbroken lease. A
                    // synchronous completion (including success) is already
                    // finished; do not depend on a future completion packet
                    // to retire it. Late packets use a never-reused key.
                    {
                        FScopeLock Lock(&Mutex);
                        if (Pending.Remove(CompletionKey)) PathCharacters -= Path.Len();
                    }
                    State->Complete();
                    return {};
                }
                return State;
            }

            virtual uint32 Run() override
            {
                for (;;)
                {
                    DWORD Bytes = 0;
                    ULONG_PTR Key = 0;
                    OVERLAPPED* Completed = nullptr;
                    GetQueuedCompletionStatus(Port, &Bytes, &Key, &Completed, INFINITE);
                    TSharedPtr<FWorkspaceFileLeaseState, ESPMode::ThreadSafe> State;
                    bool bFinished = false;
                    {
                        FScopeLock Lock(&Mutex);
                        if (Completed)
                        {
                            if (auto* Found = Pending.Find(static_cast<uint64>(Key)); Found && &(*Found)->Overlapped == Completed)
                            {
                                State = *Found;
                                PathCharacters -= State->Path.Len();
                                Pending.Remove(static_cast<uint64>(Key));
                            }
                        }
                        bFinished = bStopping && Pending.IsEmpty();
                    }
                    if (State) {
                        State->Complete();
                        RetirementRevision.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (bFinished) return 0;
                }
            }

            FWorkspaceFileProofUsage GetUsage(bool bClassifyPending)
            {
                FScopeLock Lock(&Mutex);
                FWorkspaceFileProofUsage Usage{Pending.Num(), PathCharacters};
                if (bClassifyPending) {
                    Usage.LeasePendingOperations = 0;
                    for (const auto& Pair : Pending)
                        Usage.LeasePendingOperations += Pair.Value->IsLeasePending() ? 1 : 0;
                    Usage.RetiringOperations = Usage.PendingOperations - Usage.LeasePendingOperations;
                }
                return Usage;
            }

            uint64 GetRetirementRevision() const
            {
                return RetirementRevision.load(std::memory_order_relaxed);
            }

            int32 RetireForRename(const FString& Path, const bool bDirectory)
            {
                FReadScopeLock Lifetime(Lifecycle);
                TArray<TSharedPtr<FWorkspaceFileLeaseState, ESPMode::ThreadSafe>> Retired;
                const FString Prefix = Path + TEXT("/");
                {
                    FScopeLock Lock(&Mutex);
                    for (const auto& Pair : Pending) {
                        const FString& ObservedPath = Pair.Value->Path;
                        if (ObservedPath.Equals(Path, ESearchCase::IgnoreCase)
                            || (bDirectory && ObservedPath.StartsWith(Prefix, ESearchCase::IgnoreCase)))
                            Retired.Add(Pair.Value);
                    }
                }
                // Invalidate every proof first. Close our optional RH handles
                // before the directory rename asks Windows to break them one
                // at a time. Pending owns OVERLAPPED storage until IOCP removes
                // it; cancellation/close never frees an in-flight IO buffer.
                for (const auto& State : Retired) State->Cancel();
                for (const auto& State : Retired) State->Complete();
                if (!Retired.IsEmpty()) RetirementRevision.fetch_add(1, std::memory_order_relaxed);
                return Retired.Num();
            }

            void Shutdown()
            {
                FWriteScopeLock Lifetime(Lifecycle);
                {
                    FScopeLock Lock(&Mutex);
                    if (!Port) return;
                    bStopping = true;
                    for (auto& Pair : Pending) Pair.Value->Cancel();
                    PostQueuedCompletionStatus(Port, 0, 0, nullptr);
                }
                if (Thread) { Thread->WaitForCompletion(); Thread.Reset(); }
                FScopeLock Lock(&Mutex);
                CloseHandle(Port); Port = nullptr;
                bStopping = false;
            }
        private:
            FRWLock Lifecycle;
            FCriticalSection Mutex;
            HANDLE Port = nullptr;
            TUniquePtr<FRunnableThread> Thread;
            TMap<uint64, TSharedPtr<FWorkspaceFileLeaseState, ESPMode::ThreadSafe>> Pending;
            int64 PathCharacters = 0;
            uint64 NextCompletionKey = 1;
            std::atomic<uint64> RetirementRevision{0};
            bool bStopping = false;
        };

        FFileProofCompletionOwner& CompletionOwner()
        {
            static FFileProofCompletionOwner Owner;
            return Owner;
        }
    }
#endif

    FWorkspaceFileProof::FWorkspaceFileProof(TSharedRef<FWorkspaceFileLeaseState, ESPMode::ThreadSafe> InState)
        : State(MoveTemp(InState)) {}
    FWorkspaceFileProof::~FWorkspaceFileProof() { State->Cancel(); }
    bool FWorkspaceFileProof::IsCurrent() const { return State->IsCurrent(); }
    bool FWorkspaceFileProof::IsCurrentHandle(void* CurrentFile) const { return State->IsCurrentHandle(CurrentFile); }
    bool FWorkspaceFileProof::IsLeasePending() const { return State->IsLeasePending(); }

    bool MatchWorkspaceFileHandles(void* Left, void* Right)
    {
#if PLATFORM_WINDOWS
        FFileIdentityInfo A{}, B{};
        return Left && Right
            && GetFileInformationByHandleEx(Left, FileIdentityInfoClass, &A, sizeof(A))
            && GetFileInformationByHandleEx(Right, FileIdentityInfoClass, &B, sizeof(B))
            && A.VolumeSerialNumber == B.VolumeSerialNumber
            && FMemory::Memcmp(A.FileId, B.FileId, sizeof(A.FileId)) == 0;
#else
        return false;
#endif
    }

    FWorkspaceFileProofPtr TryBeginWorkspaceFileObservation(const FString& AbsolutePath,
        bool* OutBudgetExhausted)
    {
        if (OutBudgetExhausted) *OutBudgetExhausted = false;
#if PLATFORM_WINDOWS
        if (FPaths::IsRelative(AbsolutePath)) return {};
        FString Path = FPaths::ConvertRelativePathToFull(AbsolutePath);
        FPaths::NormalizeFilename(Path);
        if (auto* Previous = Observations.Find(Path))
            if (auto Proof = Previous->Pin(); Proof && Proof->IsCurrent()) return Proof;
        const uint64 Retirement = CompletionOwner().GetRetirementRevision();
        if (Observations.Num() >= 16384 && !Observations.Contains(Path)
            && (!bPrunedAtCapacity || LastIndexPruneRetirement != Retirement)) {
            // A full weak index is optional reuse, not a reason to rescan every
            // live lease for each rejected new path. Retry maintenance after
            // its native owner actually retires work; current-file validation
            // and the bounded native Begin/fallback below remain unchanged.
            // Capture before sweeping so a concurrent retirement is retried.
            LastIndexPruneRetirement = Retirement;
            bPrunedAtCapacity = true;
            const int32 Before = Observations.Num();
            for (auto It = Observations.CreateIterator(); It; ++It)
                if (!It.Value().IsValid()) It.RemoveCurrent();
            const int32 Expired = Before - Observations.Num();
            int32 Finished = 0;
            if (Observations.Num() >= 16384) {
                for (auto It = Observations.CreateIterator(); It; ++It) {
                    const auto Proof = It.Value().Pin();
                    if (!Proof || !Proof->IsLeasePending()) { It.RemoveCurrent(); ++Finished; }
                }
            }
            // A live source proof can still be owned by its accepted document.
            // Forgetting its weak index would acquire a second kernel lease for
            // the same path at publication. Retire only unusable index entries;
            // native capacity and current path/content checks remain unchanged.
            UE_LOG(LogTemp, Display, TEXT("UAI file proof index prune before=%d expired=%d finished=%d retained=%d"),
                Before, Expired, Finished, Observations.Num());
        }
        if (auto State = CompletionOwner().Begin(Path, OutBudgetExhausted))
        {
            auto Proof = MakeShared<const FWorkspaceFileProof>(State.ToSharedRef());
            if (Observations.Num() < 16384 || Observations.Contains(Path)) Observations.Add(Path, Proof);
            return Proof;
        }
#endif
        return {};
    }

    void ResetWorkspaceFileProofs()
    {
        Observations.Reset();
        LastIndexPruneRetirement = 0;
        bPrunedAtCapacity = false;
#if PLATFORM_WINDOWS
        CompletionOwner().Shutdown();
#endif
    }

    FWorkspaceFileProofUsage GetWorkspaceFileProofUsage(bool bClassifyPending)
    {
#if PLATFORM_WINDOWS
        return CompletionOwner().GetUsage(bClassifyPending);
#else
        return {};
#endif
    }

    int32 RetireWorkspaceFileObservationsForRename(const FString& AbsolutePath, const bool bDirectory)
    {
#if PLATFORM_WINDOWS
        if (FPaths::IsRelative(AbsolutePath)) return 0;
        FString Path = FPaths::ConvertRelativePathToFull(AbsolutePath);
        FPaths::NormalizeFilename(Path);
        while (Path.EndsWith(TEXT("/"))) Path.LeftChopInline(1);
        return CompletionOwner().RetireForRename(Path, bDirectory);
#else
        return 0;
#endif
    }
}
