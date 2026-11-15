#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Containers/StaticArray.h"

namespace UE::UAI::HjsonWorkspace
{
	// Disposable parser output only. Neither the cache key nor a restored document
	// proves that a logical file is current, semantically valid, or native state.
	struct FHjsonSyntaxCacheStats
	{
		int64 Hits = 0, Misses = 0, Stored = 0, Rejected = 0, LoadedBytes = 0;
		double LoadSeconds = 0, StoreSeconds = 0;
		double StoreLookupSeconds = 0, StoreBuildSeconds = 0, StoreSerializeSeconds = 0, StoreCompressSeconds = 0, StoreIoSeconds = 0;
		int64 StoreBuiltNodes = 0, StoreRawBytes = 0, StorePackedBytes = 0;
	};
	FHjsonSyntaxCacheStats GetHjsonSyntaxCacheStats();
	// Reuse one current directory observation only while holding the existing
	// zero-wait cache budget lock. Blobs still write immediately and atomically.
	// No parsed documents or pending blobs are retained by this scope.
	class FScopedHjsonCacheWrites
	{
	public:
		FScopedHjsonCacheWrites();
		~FScopedHjsonCacheWrites();
		FScopedHjsonCacheWrites(const FScopedHjsonCacheWrites&) = delete;
		FScopedHjsonCacheWrites& operator=(const FScopedHjsonCacheWrites&) = delete;
	private:
		bool bOwns = false;
	};
	bool TryRestoreHjsonSyntax(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint);
	void RememberHjsonSyntax(const FString& Text, const FString& SourceUnit,
		const FHjsonDocument& Document,
		const UE::UAI::ContextResource::FUeAgentResourceUsage& Before,
		const UE::UAI::ContextResource::FUeAgentResourceUsage& After);

	// Disposable derived bytes share the syntax cache's atomic storage and disk
	// budget. The producer owns their version/dependency contract; restoring a
	// blob supplies neither current-source nor successful-semantic proof.
	struct FHjsonDerivedBlob { FString Key; TArray<uint8> Bytes; };
	struct FHjsonDerivedStoreStats
	{
		int32 Stored = 0, EvictedSyntax = 0, EvictedDerived = 0;
		int64 StoredBytes = 0, EvictedBytes = 0, ObservedDiskBytes = 0, ScratchBytes = 0;
		double ObserveSeconds = 0, WriteSeconds = 0;
	};
	bool TryRestoreHjsonDerivedBlob(const FString& Key, TArray<uint8>& OutBytes);
	FHjsonDerivedStoreStats RememberHjsonDerivedBlobs(TConstArrayView<FHjsonDerivedBlob> Blobs);

	// One bounded batch of disposable parser facts. Nested scopes share it;
	// destruction seals changed groups through the existing 512 MiB store.
	// No file observation, semantic validity or native binding is retained.
	class FScopedHjsonSyntaxGroups
	{
	public:
		explicit FScopedHjsonSyntaxGroups(const FString& SourceScope);
		~FScopedHjsonSyntaxGroups();
		FScopedHjsonSyntaxGroups(const FScopedHjsonSyntaxGroups&) = delete;
		FScopedHjsonSyntaxGroups& operator=(const FScopedHjsonSyntaxGroups&) = delete;
	private:
		bool bOwns = false;
	};
	using FHjsonInputDigest = TStaticArray<uint8, 32>;
	bool FindHjsonInputDigest(const FString& SourceUnit, const FString& CurrentDocumentHash,
		const FString& Pointer, FHjsonInputDigest& OutDigest);
	void RememberHjsonInputDigest(const FString& SourceUnit, const FString& CurrentDocumentHash,
		const FString& Pointer, const FHjsonInputDigest& Digest);
}
