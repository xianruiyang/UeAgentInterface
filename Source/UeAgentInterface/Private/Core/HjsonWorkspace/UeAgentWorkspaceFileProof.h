#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::HjsonWorkspace
{
    struct FWorkspaceFileLeaseState;

    /** Proof of unchanged bytes, acquired BEFORE a caller's real file read.
     * This owns no file contents or parser/model state. Unsupported filesystems,
     * exhausted budgets and broken leases simply fail proof and require a read. */
    class FWorkspaceFileProof final
    {
    public:
        explicit FWorkspaceFileProof(TSharedRef<FWorkspaceFileLeaseState, ESPMode::ThreadSafe> InState);
        ~FWorkspaceFileProof();
        bool IsCurrent() const;
        /** The caller has opened this current logical path without reparse
         * traversal. The pending lease is checked around its full file identity. */
        bool IsCurrentHandle(void* CurrentFile) const;
        /** Index retirement only; pending IO alone proves neither current path nor bytes. */
        bool IsLeasePending() const;
    private:
        TSharedRef<FWorkspaceFileLeaseState, ESPMode::ThreadSafe> State;
    };
    using FWorkspaceFileProofPtr = TSharedPtr<const FWorkspaceFileProof>;

    /** Optional failure classification for consumers that require a lease rather
     * than falling back to a real read. It does not change the shared budget. */
    FWorkspaceFileProofPtr TryBeginWorkspaceFileObservation(const FString& AbsolutePath,
        bool* OutBudgetExhausted = nullptr);
    struct FWorkspaceFileProofUsage
    {
        int32 PendingOperations = 0;
        int64 PathCharacters = 0;
        int32 LeasePendingOperations = INDEX_NONE;
        int32 RetiringOperations = INDEX_NONE;
    };
    /** Diagnostic snapshot; cancelled IO remains charged until completion. */
    FWorkspaceFileProofUsage GetWorkspaceFileProofUsage(bool bClassifyPending = false);
    /** Native identity only: both current handles must report the same volume
     * and full 128-bit file id. This does not itself prove unchanged content. */
    bool MatchWorkspaceFileHandles(void* Left, void* Right);
    /** Request-local handoff from an owner that currently guards the logical
     * file path. Every consumer still queries the current ordinary-file type.
     * Missing paths use the normal reopen.
     * The callback must never return a handle through a rename/guard gap. */
    class FWorkspaceCurrentFileScope final
    {
    public:
        explicit FWorkspaceCurrentFileScope(TFunction<void*(const FString&)> Resolver);
        ~FWorkspaceCurrentFileScope();
        FWorkspaceCurrentFileScope(const FWorkspaceCurrentFileScope&) = delete;
    private:
        TFunction<void*(const FString&)> Resolve;
        FWorkspaceCurrentFileScope* Previous;
        friend struct FWorkspaceFileLeaseState;
        friend class FWorkspaceReadPathScope;
    };
    /** One synchronous read observation, not a namespace cache or a writer lock.
     * All handles share DELETE; every file resolves its complete relative path
     * again. Rebind the root at the end before accepting any observation. */
    class FWorkspaceReadPathScope final
    {
    public:
        explicit FWorkspaceReadPathScope(const FString& Root);
        ~FWorkspaceReadPathScope();
        bool IsAvailable() const;
        bool IsRejected() const;
        bool IsCurrent() const;
        bool Observe(const FString& Path, TFunctionRef<bool(void*, int64, int64)> Consumer) const;
        /** Addressed readers need the current ordinary leaf, not its unused
         * mtime/size projection. The same relative-path owner opens it. */
        bool ObserveHandle(const FString& Path, TFunctionRef<bool(void*)> Consumer) const;
        int32 GetReusedCurrentHandles() const;
        int32 GetOpenedCurrentHandles() const;
        static bool ReadBytes(void* CurrentFile, TArray<uint8>& Bytes);
        /** Addressed bytes on this current handle; no shared cursor mutation. */
        static bool ReadRange(void* CurrentFile, int64 Begin, int64 End, TArray<uint8>& Bytes);
        static bool ReadText(void* CurrentFile, FString& Text);
    private:
        bool ObserveImpl(const FString& Path, TFunctionRef<bool(void*)> Consumer, bool bCheckOrdinary) const;
        struct FState;
        TUniquePtr<FState> State;
    };
    /** Cancels and drains kernel operations before backend/module destruction. */
    void ResetWorkspaceFileProofs();
    /** Retire this process's optional content observations after a v2 publisher
     * has consumed its final source checkpoint, before renaming that directory.
     * Does not validate input or replace any commit guard. Later readers must
     * observe the new logical path again. Pending IO backing survives completion. */
    int32 RetireWorkspaceFileObservationsForRename(const FString& AbsolutePath, bool bDirectory);
}
