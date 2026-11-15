#include "Core/ArtifactPublisher/UeAgentArtifactGenerationFiles.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceFileProof.h"
#include "Core/FileSystem/UeAgentWindowsFileAccess.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace UE::UAI::ArtifactPublisher
{
#if PLATFORM_WINDOWS
    struct FGenerationDirectoryCounters { int32 Opened = 0, Reused = 0, Closed = 0; };
    struct FGenerationDirectoryHandle
    {
        HANDLE Value;
        TSharedPtr<FGenerationDirectoryCounters> Counters;
        FGenerationDirectoryHandle(HANDLE InValue, TSharedPtr<FGenerationDirectoryCounters> InCounters)
            : Value(InValue), Counters(MoveTemp(InCounters)) { if (Counters) ++Counters->Opened; }
        ~FGenerationDirectoryHandle() { CloseHandle(Value); if (Counters) ++Counters->Closed; }
    };
#endif
    namespace
    {
#if PLATFORM_WINDOWS
        struct FDirectoryCapabilities
        {
            TMap<FString, TSharedPtr<FGenerationDirectoryHandle>> Handles;
            TSharedRef<FGenerationDirectoryCounters> Counters = MakeShared<FGenerationDirectoryCounters>();
            int64 PathCharacters = 0;
            ~FDirectoryCapabilities() {
                const double Started = FPlatformTime::Seconds();
                Handles.Reset();
                UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=generation_directory_handles opened=%d reused=%d closed=%d release_s=%.6f"),
                    Counters->Opened, Counters->Reused, Counters->Closed, FPlatformTime::Seconds() - Started);
            }
            void Retain(const FString& Path, const TSharedPtr<FGenerationDirectoryHandle>& Handle, const FString& ParentPath = FString()) {
                // A cached leaf capability is usable directly only while its
                // complete namespace chain is retained in this same scope.
                if (!ParentPath.IsEmpty() && !Handles.Contains(ParentPath)) return;
                if (Handles.Contains(Path) || Handles.Num() >= 16384 || PathCharacters + Path.Len() > 4 * 1024 * 1024) return;
                Handles.Add(Path, Handle); PathCharacters += Path.Len();
            }
            void ReleaseTree(const FString& Path) {
                FString Normalized = FPaths::ConvertRelativePathToFull(Path); FPaths::NormalizeDirectoryName(Normalized);
                for (auto It = Handles.CreateIterator(); It; ++It) {
                    if (It.Key() == Normalized || It.Key().StartsWith(Normalized + TEXT("/"), ESearchCase::IgnoreCase)) {
                        PathCharacters -= It.Key().Len(); It.RemoveCurrent();
                    }
                }
            }
        };
        thread_local FDirectoryCapabilities* DirectoryCapabilities = nullptr;
#endif
        bool NativeFailure(FArtifactPublishError& Error, const FString& Code, const FString& Message, const FString& Path)
        {
            Error = Contract::FDiagnostic::Failure(Code, Message, TEXT("artifact_publish"), Path);
            Error.TargetPath = Path;
#if PLATFORM_WINDOWS
            Error.InternalContext = FString::Printf(TEXT("win32_error=%lu"), GetLastError());
#endif
            return false;
        }
#if PLATFORM_WINDOWS
        // The Win32 rename wrapper rejects RootDirectory on supported Windows
        // versions. Use the documented native relative-name operations, never
        // resolve an absolute destination again after acquiring its parents.
        using FileSystem::Windows::OpenRelative;
        bool OrdinaryHandle(HANDLE Handle, bool bDirectory)
        {
            return FileSystem::Windows::IsOrdinary(Handle, bDirectory);
        }
        struct FRelativeParent
        {
            TArray<TSharedPtr<FGenerationDirectoryHandle>> Parents;
            HANDLE Pin = INVALID_HANDLE_VALUE;
            FString Leaf;
            ~FRelativeParent() {
                if (Pin != INVALID_HANDLE_VALUE) CloseHandle(Pin);
            }
            HANDLE Get() const { return Parents.IsEmpty() ? nullptr : Parents.Last()->Value; }
            bool Open(const FString& Path, bool bCreate, FArtifactPublishError& Error)
            {
                FString Absolute = FPaths::ConvertRelativePathToFull(Path);
                FPaths::NormalizeFilename(Absolute);
                if (!FPaths::CollapseRelativeDirectories(Absolute) || Absolute.Len() < 3
                    || Absolute[1] != TCHAR(':') || Absolute[2] != TCHAR('/'))
                    return NativeFailure(Error, TEXT("workspace_generation_path_backend_unavailable"),
                        TEXT("Guarded relative publication requires a local drive-rooted path."), Path);
                FString ParentPath = FPaths::GetPath(Absolute);
                if (ParentPath.Len() == 2 && ParentPath[1] == TCHAR(':')) ParentPath += TEXT("/");
                if (const auto* Cached = DirectoryCapabilities ? DirectoryCapabilities->Handles.Find(ParentPath) : nullptr) {
                    Parents.Add(*Cached);
                    Leaf = FPaths::GetCleanFilename(Absolute);
                    // All ancestors deny deletion/rename for this request. A
                    // possibly empty final directory is still checked by the
                    // actual relative child open, and pinned before mutation.
                    return true;
                }
                const FString Volume = Absolute.Left(3);
                const auto* CachedRoot = DirectoryCapabilities ? DirectoryCapabilities->Handles.Find(Volume) : nullptr;
                HANDLE Root = CachedRoot ? (*CachedRoot)->Value : CreateFileW(*Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (Root == INVALID_HANDLE_VALUE || (!CachedRoot && !OrdinaryHandle(Root, true))) {
                    if (Root != INVALID_HANDLE_VALUE && !CachedRoot) CloseHandle(Root);
                    return NativeFailure(Error, TEXT("workspace_publication_path_invalid"), TEXT("Could not guard the ordinary volume root."), Path);
                }
                Parents.Add(CachedRoot ? *CachedRoot : MakeShared<FGenerationDirectoryHandle>(Root,
                    DirectoryCapabilities ? TSharedPtr<FGenerationDirectoryCounters>(DirectoryCapabilities->Counters) : nullptr));
                if (!CachedRoot && DirectoryCapabilities) DirectoryCapabilities->Retain(Volume, Parents.Last());
                TArray<FString> Parts; Absolute.Mid(3).ParseIntoArray(Parts, TEXT("/"), true);
                if (Parts.IsEmpty()) { Leaf.Empty(); return true; }
                Leaf = Parts.Pop();
                FString Current = Volume;
                for (const FString& Part : Parts) {
                    const FString RetainedParent = Current;
                    Current = Current / Part;
                    const auto* Cached = DirectoryCapabilities ? DirectoryCapabilities->Handles.Find(Current) : nullptr;
                    HANDLE Directory = Cached ? (*Cached)->Value : OpenRelative(Get(), Part, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        bCreate ? 3 /* FILE_OPEN_IF */ : 1 /* FILE_OPEN */, 1 /* FILE_DIRECTORY_FILE */);
                    if (Directory == INVALID_HANDLE_VALUE || (!Cached && !OrdinaryHandle(Directory, true))) {
                        if (Directory != INVALID_HANDLE_VALUE && !Cached) CloseHandle(Directory);
                        return NativeFailure(Error, TEXT("workspace_publication_path_invalid"),
                            TEXT("A publication ancestor is unavailable, renamed, or a reparse point."), Path);
                    }
                    Parents.Add(Cached ? *Cached : MakeShared<FGenerationDirectoryHandle>(Directory,
                        DirectoryCapabilities ? TSharedPtr<FGenerationDirectoryCounters>(DirectoryCapabilities->Counters) : nullptr));
                    if (!Cached && DirectoryCapabilities) DirectoryCapabilities->Retain(Current, Parents.Last(), RetainedParent);
                }
                return true;
            }
            bool PinForMutation(const FString& Path, FArtifactPublishError& Error)
            {
                if (Pin != INVALID_HANDLE_VALUE) return true;
                // A read directory handle excludes rename/delete, but Windows
                // still permits WRITE_ATTRIBUTES to turn an empty directory into
                // a reparse point. A relative, delete-on-close regular file keeps
                // the destination nonempty during this one mutation. Creation
                // itself rejects a reparse parent; no absolute-path retry.
                Pin = OpenRelative(Get(), TEXT(".uai-pin-") + FGuid::NewGuid().ToString(EGuidFormats::Digits),
                    GENERIC_WRITE | DELETE, 0, 2 /* FILE_CREATE */, 0x1040 /* NON_DIRECTORY | DELETE_ON_CLOSE */);
                return Pin != INVALID_HANDLE_VALUE || NativeFailure(Error, TEXT("workspace_publication_path_invalid"),
                    TEXT("Could not pin the exact ordinary destination directory before mutation."), Path);
            }
        };
        bool RenameRelative(HANDLE File, FRelativeParent& Parent, bool bReplace, const FString& Destination, FArtifactPublishError& Error)
        {
            if (!Parent.PinForMutation(Destination, Error)) return false;
            const uint32 NameBytes = Parent.Leaf.Len() * sizeof(WCHAR);
            TArray<uint8> Buffer; Buffer.SetNumZeroed(sizeof(FILE_RENAME_INFO) + NameBytes);
            auto* Rename = reinterpret_cast<FILE_RENAME_INFO*>(Buffer.GetData());
            Rename->ReplaceIfExists = bReplace; Rename->RootDirectory = Parent.Get(); Rename->FileNameLength = NameBytes;
            FMemory::Memcpy(Rename->FileName, *Parent.Leaf, NameBytes);
            return FileSystem::Windows::SetInformation(File, Buffer.GetData(), Buffer.Num(), 10 /* FileRenameInformation */)
                || NativeFailure(Error, TEXT("workspace_publication_rename_failed"), TEXT("The guarded relative rename failed."), Destination);
        }
#endif
    }
    FGenerationDirectoryScope::FGenerationDirectoryScope() : State(nullptr)
    {
#if PLATFORM_WINDOWS
        if (!DirectoryCapabilities) { State = new FDirectoryCapabilities(); DirectoryCapabilities = static_cast<FDirectoryCapabilities*>(State); }
#endif
    }
    FGenerationDirectoryScope::~FGenerationDirectoryScope()
    {
#if PLATFORM_WINDOWS
        if (State) { DirectoryCapabilities = nullptr; delete static_cast<FDirectoryCapabilities*>(State); }
#endif
    }
    FGenerationFileGuard::FGenerationFileGuard() : Handle(nullptr) {}
    FGenerationFileGuard::~FGenerationFileGuard() { Close(); }
    void FGenerationFileGuard::Close()
    {
#if PLATFORM_WINDOWS
        if (DirectoryHandle) DirectoryHandle.Reset();
        else if (Handle) CloseHandle(Handle);
#endif
        Handle = nullptr;
        bReadOnlyAtOpen = false;
    }
    bool FGenerationFileGuard::Open(const FString& Path, bool bDeleteAccess, bool bDirectory, FArtifactPublishError& Error)
    {
        Close();
#if PLATFORM_WINDOWS
        FString Absolute = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Absolute);
        if (bDirectory && !bDeleteAccess && DirectoryCapabilities) {
            if (const auto* Existing = DirectoryCapabilities->Handles.Find(Absolute)) {
                // Sharing a directory capability does not freeze its attributes:
                // an empty ordinary directory may have become a reparse point.
                if (!OrdinaryHandle((*Existing)->Value, true))
                    return NativeFailure(Error, TEXT("workspace_publication_path_invalid"),
                        TEXT("The retained publication directory is no longer ordinary."), Path);
                DirectoryHandle = *Existing; Handle = DirectoryHandle->Value; CurrentPath = Path;
                ++DirectoryCapabilities->Counters->Reused;
                return true;
            }
        }
        FRelativeParent Parent;
        if (!Parent.Open(Path, false, Error)) return false;
        HANDLE Opened = INVALID_HANDLE_VALUE;
        if (Parent.Leaf.IsEmpty() && bDirectory && !bDeleteAccess) {
            if (!DuplicateHandle(GetCurrentProcess(), Parent.Get(), GetCurrentProcess(), &Opened, 0, false, DUPLICATE_SAME_ACCESS))
                Opened = INVALID_HANDLE_VALUE;
        } else {
            Opened = OpenRelative(Parent.Get(), Parent.Leaf, GENERIC_READ | (bDeleteAccess ? DELETE : 0),
                bDirectory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
                1 /* FILE_OPEN */, bDirectory ? 1 /* DIRECTORY */ : 0x40 /* NON_DIRECTORY */);
        }
        if (Opened == INVALID_HANDLE_VALUE) return NativeFailure(Error, TEXT("workspace_publication_writer_conflict"),
            TEXT("Could not exclude a concurrent writer or namespace change. Close the conflicting writer and retry export; no native import is replayed."), Path);
        DWORD Attributes = 0;
        if (!FileSystem::Windows::IsOrdinary(Opened, bDirectory, &Attributes)) {
            CloseHandle(Opened);
            return NativeFailure(Error, TEXT("workspace_publication_path_invalid"), TEXT("Publication requires an ordinary file/directory without reparse indirection."), Path);
        }
        if (bDirectory && !bDeleteAccess) {
            DirectoryHandle = MakeShared<FGenerationDirectoryHandle>(Opened,
                DirectoryCapabilities ? TSharedPtr<FGenerationDirectoryCounters>(DirectoryCapabilities->Counters) : nullptr);
            if (DirectoryCapabilities) {
                FString ParentPath = FPaths::GetPath(Absolute);
                if (ParentPath.Len() == 2 && ParentPath[1] == TCHAR(':')) ParentPath += TEXT("/");
                DirectoryCapabilities->Retain(Absolute, DirectoryHandle, Parent.Leaf.IsEmpty() ? FString() : ParentPath);
            }
        }
        bReadOnlyAtOpen = (Attributes & FILE_ATTRIBUTE_READONLY) != 0;
        Handle = Opened; CurrentPath = Path; return true;
#else
        return NativeFailure(Error, TEXT("workspace_generation_backend_unavailable"), TEXT("Guarded generation publication is unavailable on this platform."), Path);
#endif
    }
    bool FGenerationFileGuard::Rename(const FString& Destination, FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        if (!Handle) return NativeFailure(Error, TEXT("workspace_publication_guard_missing"), TEXT("The source rename guard is unavailable."), CurrentPath);
        FRelativeParent Parent;
        if (!Parent.Open(Destination, false, Error) || !RenameRelative(Handle, Parent, false, Destination, Error)) return false;
        HANDLE Observed = OpenRelative(Parent.Get(), Parent.Leaf, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 1, 0x40);
        const bool bSame = Observed != INVALID_HANDLE_VALUE && HjsonWorkspace::MatchWorkspaceFileHandles(Handle, Observed);
        if (Observed != INVALID_HANDLE_VALUE) CloseHandle(Observed);
        if (!bSame) return NativeFailure(Error, TEXT("workspace_publication_rename_identity_mismatch"),
            TEXT("The renamed handle is not bound to the exact intended destination path."), Destination);
        CurrentPath = Destination; return true;
#else
        return false;
#endif
    }
    void* FGenerationFileGuard::GetCurrentReadHandle(const FString& Path) const
    {
#if PLATFORM_WINDOWS
        void* Current = GetCurrentObservationHandle(Path);
        return Current && OrdinaryHandle(Current, false) ? Current : nullptr;
#else
        return nullptr;
#endif
    }
    void* FGenerationFileGuard::GetCurrentObservationHandle(const FString& Path) const
    {
        // A deny-write/delete guard binds this path, but attribute-only writes
        // are not excluded by sharing. Type observation belongs to the caller.
        return Handle && !DirectoryHandle && CurrentPath.Equals(Path, ESearchCase::CaseSensitive) ? Handle : nullptr;
    }
    bool FGenerationFileGuard::Read(TArray<uint8>& Bytes, FArtifactPublishError& Error) const
    {
#if PLATFORM_WINDOWS
        LARGE_INTEGER Size{}, Zero{};
        if (!Handle || !GetFileSizeEx(Handle, &Size) || Size.QuadPart < 0 || Size.QuadPart > MAX_int32
            || !SetFilePointerEx(Handle, Zero, nullptr, FILE_BEGIN))
            return NativeFailure(Error, TEXT("workspace_guarded_read_failed"), TEXT("Could not size the guarded file."), CurrentPath);
        Bytes.SetNumUninitialized(static_cast<int32>(Size.QuadPart));
        DWORD ReadBytes = 0;
        if (!Bytes.IsEmpty() && (!ReadFile(Handle, Bytes.GetData(), Bytes.Num(), &ReadBytes, nullptr) || ReadBytes != static_cast<DWORD>(Bytes.Num())))
            return NativeFailure(Error, TEXT("workspace_guarded_read_failed"), TEXT("Could not read the complete guarded file."), CurrentPath);
        return true;
#else
        return false;
#endif
    }
    FGenerationPublishLock::FGenerationPublishLock() : Handle(nullptr) {}
    bool FGenerationFileGuard::Remove(FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        FILE_DISPOSITION_INFO Disposition{true};
        if (!Handle || !SetFileInformationByHandle(Handle, FileDispositionInfo, &Disposition, sizeof(Disposition)))
            return NativeFailure(Error, TEXT("workspace_private_cleanup_failed"), TEXT("Could not remove the guarded transaction file/directory."), CurrentPath);
        Close(); return true;
#else
        return false;
#endif
    }
    FGenerationPublishLock::~FGenerationPublishLock()
    {
#if PLATFORM_WINDOWS
        if (Handle) CloseHandle(Handle);
#endif
    }
    bool FGenerationPublishLock::Acquire(const FString& Root, FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        const FString Path = Root + TEXT(".uai-publication.lock");
        FRelativeParent Parent;
        if (!Parent.Open(Path, false, Error) || !Parent.PinForMutation(Path, Error)) return false;
        HANDLE Opened = OpenRelative(Parent.Get(), Parent.Leaf, GENERIC_READ | GENERIC_WRITE | DELETE, 0, 2, 0x1040);
        if (Opened == INVALID_HANDLE_VALUE) return NativeFailure(Error, TEXT("workspace_publication_locked"),
            TEXT("Another publisher or an unrecognized lock file occupies the workspace publication lock."), Path);
        Handle = Opened; return true;
#else
        return false;
#endif
    }
    bool SaveGenerationJournal(const FString& Path, const TSharedRef<FJsonObject>& Object, FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        FString Text;
        if (!FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text)))
            return NativeFailure(Error, TEXT("workspace_journal_encode_failed"), TEXT("Could not encode the generation journal."), Path);
        const FString Next = Path + TEXT(".next");
        FRelativeParent Parent;
        if (!Parent.Open(Next, false, Error) || !Parent.PinForMutation(Next, Error)) return false;
        HANDLE File = OpenRelative(Parent.Get(), Parent.Leaf, GENERIC_WRITE | DELETE, 0, 2, 0x42 /* NON_DIRECTORY | WRITE_THROUGH */);
        if (File == INVALID_HANDLE_VALUE) return NativeFailure(Error, TEXT("workspace_journal_write_failed"), TEXT("Could not exclusively create the next journal record."), Next);
        FTCHARToUTF8 Utf8(*Text); DWORD Written = 0;
        const bool bSaved = WriteFile(File, Utf8.Get(), Utf8.Length(), &Written, nullptr) && Written == static_cast<DWORD>(Utf8.Length()) && FlushFileBuffers(File);
        // Both names share the same already-guarded parent. Retain the source
        // handle across flush/rename, so an external replacement is not used.
        Parent.Leaf = FPaths::GetCleanFilename(Path);
        if (!bSaved || !RenameRelative(File, Parent, true, Path, Error)) {
            FILE_DISPOSITION_INFO Disposition{true};
            SetFileInformationByHandle(File, FileDispositionInfo, &Disposition, sizeof(Disposition));
            CloseHandle(File);
            return NativeFailure(Error, TEXT("workspace_journal_write_failed"), TEXT("Could not durably publish the generation journal record."), Path);
        }
        CloseHandle(File);
        return true;
#else
        return false;
#endif
    }
    bool MakeGenerationDirectory(const FString& Path, FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        // The fictitious final leaf is never opened; every actual directory is
        // acquired relative to its guarded parent and rejects reparse traversal.
        FRelativeParent Parent;
        return Parent.Open(Path / TEXT(".uai-directory-anchor"), true, Error);
#else
        return NativeFailure(Error, TEXT("workspace_generation_backend_unavailable"), TEXT("Guarded directory creation is unavailable."), Path);
#endif
    }
    bool WriteGenerationFile(const FString& Path, const TArray<uint8>& Bytes, FArtifactPublishError& Error)
    {
#if PLATFORM_WINDOWS
        FRelativeParent Parent;
        if (!Parent.Open(Path, true, Error) || !Parent.PinForMutation(Path, Error)) return false;
        HANDLE File = OpenRelative(Parent.Get(), Parent.Leaf, GENERIC_WRITE | DELETE, 0, 2, 0x42);
        if (File == INVALID_HANDLE_VALUE) return NativeFailure(Error, TEXT("workspace_generation_stage_conflict"),
            TEXT("Could not exclusively create the exact staged file; existing content is preserved."), Path);
        DWORD Written = 0;
        const bool bSaved = (Bytes.IsEmpty() || (WriteFile(File, Bytes.GetData(), Bytes.Num(), &Written, nullptr)
            && Written == static_cast<DWORD>(Bytes.Num()))) && FlushFileBuffers(File);
        if (!bSaved) {
            FILE_DISPOSITION_INFO Disposition{true};
            SetFileInformationByHandle(File, FileDispositionInfo, &Disposition, sizeof(Disposition));
        }
        CloseHandle(File);
        return bSaved || NativeFailure(Error, TEXT("workspace_generation_stage_failed"), TEXT("Could not durably write the staged file."), Path);
#else
        return NativeFailure(Error, TEXT("workspace_generation_backend_unavailable"), TEXT("Guarded staging is unavailable."), Path);
#endif
    }
    bool GuardGenerationDirectories(const FString& Root, const TArray<FString>& RelativePaths,
        TArray<TUniquePtr<FGenerationFileGuard>>& Guards, FArtifactPublishError& Error)
    {
        TSet<FString> Directories;
        auto AddParents = [&](FString Path) {
            while (!Path.IsEmpty() && !Directories.Contains(Path)) {
                Directories.Add(Path);
                const FString Parent = FPaths::GetPath(Path);
                if (Parent == Path || Parent.IsEmpty()) break;
                if (Parent.Len() == 2 && Parent[1] == TCHAR(':')) { Directories.Add(Parent + TEXT("/")); break; }
                Path = Parent;
            }
        };
        AddParents(Root);
        for (const FString& Relative : RelativePaths) AddParents(FPaths::GetPath(Root / Relative));
        TArray<FString> Ordered = Directories.Array(); Ordered.Sort();
        for (const FString& Directory : Ordered) {
            if (!IFileManager::Get().DirectoryExists(*Directory)) continue;
            auto Guard = MakeUnique<FGenerationFileGuard>();
            if (!Guard->Open(Directory, false, true, Error)) return false;
            Guards.Add(MoveTemp(Guard));
        }
        return true;
    }
    bool RemoveGenerationPrivateDirectory(const FString& Path, FArtifactPublishError& Error)
    {
        if (!IFileManager::Get().DirectoryExists(*Path)) return true;
        TArray<TUniquePtr<FGenerationFileGuard>> Parents;
        if (!GuardGenerationDirectories(FPaths::GetPath(Path), {}, Parents, Error)) return false;
        TFunction<bool(const FString&)> RemoveTree = [&](const FString& Directory) {
            FGenerationFileGuard DirectoryGuard;
            if (!DirectoryGuard.Open(Directory, false, true, Error)) return false;
            TArray<FString> Children;
            IFileManager::Get().FindFiles(Children, *(Directory / TEXT("*")), true, true);
            for (const FString& Child : Children) {
                if (Child == TEXT(".") || Child == TEXT("..")) continue;
                const FString ChildPath = Directory / Child;
                if (IFileManager::Get().DirectoryExists(*ChildPath)) {
                    if (!RemoveTree(ChildPath)) return false;
                } else {
                    FGenerationFileGuard FileGuard;
                    if (!FileGuard.Open(ChildPath, true, false, Error) || !FileGuard.Remove(Error)) return false;
                }
            }
            DirectoryGuard.Close();
#if PLATFORM_WINDOWS
            // The read capabilities deliberately deny directory deletion. Only
            // release this now-empty private subtree, then acquire DELETE on the
            // exact directory object through its still-guarded parent.
            if (DirectoryCapabilities) DirectoryCapabilities->ReleaseTree(Directory);
#endif
            return DirectoryGuard.Open(Directory, true, true, Error) && DirectoryGuard.Remove(Error);
        };
        return RemoveTree(Path);
    }
}
