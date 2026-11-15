#pragma once

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

namespace UE::UAI::ArtifactPublisher
{
    struct FGenerationDirectoryHandle;
    /** Request-local directory capabilities; no content/namespace observation is skipped. */
    class FGenerationDirectoryScope
    {
    public:
        FGenerationDirectoryScope();
        ~FGenerationDirectoryScope();
        FGenerationDirectoryScope(const FGenerationDirectoryScope&) = delete;
    private:
        void* State;
    };
    /** Native primitives for the /3 publisher. All handles are request scoped. */
    class FGenerationFileGuard
    {
    public:
        FGenerationFileGuard();
        ~FGenerationFileGuard();
        FGenerationFileGuard(const FGenerationFileGuard&) = delete;
        bool Open(const FString& Path, bool bDeleteAccess, bool bDirectory, FArtifactPublishError& Error);
        bool Rename(const FString& Destination, FArtifactPublishError& Error);
        bool Read(TArray<uint8>& Bytes, FArtifactPublishError& Error) const;
        bool Remove(FArtifactPublishError& Error);
        /** Only valid while the caller retains this file and its current directory
         * chain guards. Rename rebinds to its verified destination; the old path,
         * a closed handle, or a type mismatch returns null. */
        void* GetCurrentReadHandle(const FString& Path) const;
        /** Current logical leaf binding only. The synchronous consumer must
         * query its ordinary-file type before observing or accepting bytes. */
        void* GetCurrentObservationHandle(const FString& Path) const;
        bool WasReadOnlyAtOpen() const { return bReadOnlyAtOpen; }
        int64 GetRetainedBytes() const { return sizeof(*this) + CurrentPath.GetAllocatedSize(); }
        void Close();
    private:
        void* Handle;
        TSharedPtr<FGenerationDirectoryHandle> DirectoryHandle;
        bool bReadOnlyAtOpen = false;
        FString CurrentPath;
    };
    class FGenerationPublishLock
    {
    public:
        FGenerationPublishLock();
        ~FGenerationPublishLock();
        bool Acquire(const FString& Root, FArtifactPublishError& Error);
    private:
        void* Handle;
    };
    bool SaveGenerationJournal(const FString& Path, const TSharedRef<FJsonObject>& Object, FArtifactPublishError& Error);
    bool MakeGenerationDirectory(const FString& Path, FArtifactPublishError& Error);
    bool WriteGenerationFile(const FString& Path, const TArray<uint8>& Bytes, FArtifactPublishError& Error);
    bool GuardGenerationDirectories(const FString& Root, const TArray<FString>& RelativePaths,
        TArray<TUniquePtr<FGenerationFileGuard>>& Guards, FArtifactPublishError& Error);
    bool RemoveGenerationPrivateDirectory(const FString& Path, FArtifactPublishError& Error);
}
