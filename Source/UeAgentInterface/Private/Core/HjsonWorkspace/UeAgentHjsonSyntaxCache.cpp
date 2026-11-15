#include "Core/HjsonWorkspace/UeAgentHjsonSyntaxCache.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
#include <zlib.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	namespace
	{
		// Bump for any parser grammar, provenance, diagnostic-success, or charge
		// change. This private representation is not a workspace format/version.
		constexpr uint32 SyntaxVersion = 1;
		constexpr int64 MaxBlobBytes = 32ll * 1024 * 1024;
		constexpr int64 DiskBudgetBytes = 512ll * 1024 * 1024;
		constexpr int32 MaxNodes = 1024 * 1024;
		constexpr uint8 DimensionCount = static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes) + 1;
		thread_local FHjsonSyntaxCacheStats Stats;
		struct FCacheFile { FString Path; int64 Bytes; FDateTime Time; };
		struct FCacheWrite { FString Path; const TArray<uint8>* Bytes; bool bReplace = false; const TArray<uint8>* Header = nullptr; };
		const TCHAR* CacheWriteMutexName = TEXT("UAI.HjsonSyntaxCache.Budget.v1");

		FString CacheRoot() { return FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/HjsonSyntax/v1"); }
		bool Eligible(const FString& Text) { return Text.Len() >= 32768 && Text.Len() <= 8 * 1024 * 1024; }
		void Digest(const void* Data, const int64 Size, uint8* Out) { SHA256(static_cast<const uint8*>(Data), Size, Out); }
		FString Key(const FString& Text, const FString& SourceUnit)
		{
			SHA256_CTX Hash; SHA256_Init(&Hash);
			const uint32 Version[] = {SyntaxVersion, sizeof(TCHAR), static_cast<uint32>(FCompression::GetCompressorVersion(NAME_Zlib))};
			SHA256_Update(&Hash, Version, sizeof(Version));
			for (const FString* Input : {&SourceUnit, &Text}) {
				const int32 Length = Input->Len(); SHA256_Update(&Hash, &Length, sizeof(Length));
				SHA256_Update(&Hash, **Input, static_cast<int64>(Length) * sizeof(TCHAR));
			}
			uint8 Bytes[SHA256_DIGEST_LENGTH]; SHA256_Final(Bytes, &Hash);
			static const TCHAR Hex[] = TEXT("0123456789abcdef");
			FString Result; Result.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (uint8 B : Bytes) { Result.AppendChar(Hex[B >> 4]); Result.AppendChar(Hex[B & 15]); }
			return Result;
		}
		struct FWriter
		{
			TArray<uint8> Bytes; bool bValid = true;
			void Raw(const void* Data, int64 Size) {
				if (!bValid || Size < 0 || Size > MaxBlobBytes - Bytes.Num()) { bValid = false; return; }
				Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			}
			template<typename T> void Pod(T Value) { Raw(&Value, sizeof(Value)); }
			void Text(const FString& Value) { Pod<int32>(Value.Len()); Raw(*Value, static_cast<int64>(Value.Len()) * sizeof(TCHAR)); }
		};
		struct FReader
		{
			const TArray<uint8>& Bytes; int64 Offset = 0; bool bValid = true;
			bool Raw(void* Dest, int64 Size) {
				if (!bValid || Size < 0 || Size > Bytes.Num() - Offset) { bValid = false; return false; }
				FMemory::Memcpy(Dest, Bytes.GetData() + Offset, Size); Offset += Size; return true;
			}
			template<typename T> T Pod() { T Value{}; Raw(&Value, sizeof(T)); return Value; }
			FString Text() {
				const int32 Length = Pod<int32>();
				if (!bValid || Length < 0 || static_cast<int64>(Length) * static_cast<int64>(sizeof(TCHAR)) > Bytes.Num() - Offset) { bValid = false; return {}; }
				FString Result;
				if (Length) { auto& Chars = Result.GetCharArray(); Chars.SetNumUninitialized(Length + 1); Raw(Chars.GetData(), static_cast<int64>(Length) * sizeof(TCHAR)); Chars[Length] = 0; }
				return Result;
			}
		};
		void ObserveCacheDirectory(TArray<FCacheFile>& CacheFiles, int64& CacheBytes)
		{
			TArray<FString> Names; IFileManager::Get().FindFiles(Names, *(CacheRoot() / TEXT("*.syntax")), true, false);
			for (const FString& Name : Names) {
				const FString Path = CacheRoot() / Name; const int64 Size = IFileManager::Get().FileSize(*Path);
				if (Size > 0) { CacheFiles.Add({Path, Size, IFileManager::Get().GetTimeStamp(*Path)}); CacheBytes += Size; }
			}
			CacheFiles.Sort([](const FCacheFile& A, const FCacheFile& B) { return A.Time < B.Time; });
		}
		struct FCacheWriteContext
		{
			TUniquePtr<FSystemWideCriticalSection> Lock;
			TArray<FCacheFile> Files;
			int64 Bytes = 0, PathBytes = 0, Calls = 0, Observations = 0;
			double ObserveSeconds = 0;
			bool Acquire(FHjsonDerivedStoreStats& Result) {
				++Calls;
				if (!Lock) {
					Lock = MakeUnique<FSystemWideCriticalSection>(CacheWriteMutexName, FTimespan::Zero());
					if (!Lock->IsValid()) { Lock.Reset(); return false; }
					const double Started = FPlatformTime::Seconds();
					ObserveCacheDirectory(Files, Bytes);
					Result.ObserveSeconds = FPlatformTime::Seconds() - Started;
					ObserveSeconds += Result.ObserveSeconds; ++Observations;
					for (const auto& File : Files) PathBytes += File.Path.GetAllocatedSize();
				}
				Result.ObservedDiskBytes = Bytes;
				Result.ScratchBytes = Files.GetAllocatedSize() + PathBytes;
				return true;
			}
			void RemoveAt(int32 Index) { PathBytes -= Files[Index].Path.GetAllocatedSize(); Files.RemoveAt(Index); }
			void Add(const FString& Path, int64 Size) {
				Files.Add({Path, Size, FDateTime::UtcNow()}); PathBytes += Files.Last().Path.GetAllocatedSize();
			}
		};
		thread_local TUniquePtr<FCacheWriteContext> ActiveCacheWrites;

		void DeleteInvalidCache(const FString& Path)
		{
			// A parser can reject a candidate after an earlier parser in this
			// scope acquired the writer lock. Keep that same live catalog current.
			if (ActiveCacheWrites && ActiveCacheWrites->Lock) {
				if (IFileManager::Get().Delete(*Path, false, true, true)) {
					const int32 Index = ActiveCacheWrites->Files.IndexOfByPredicate([&](const FCacheFile& File) { return File.Path == Path; });
					if (Index != INDEX_NONE) { ActiveCacheWrites->Bytes -= ActiveCacheWrites->Files[Index].Bytes; ActiveCacheWrites->RemoveAt(Index); }
				}
				return;
			}
			FSystemWideCriticalSection Lock(CacheWriteMutexName, FTimespan::Zero());
			if (Lock.IsValid()) IFileManager::Get().Delete(*Path, false, true, true);
		}

		FHjsonDerivedStoreStats WriteCacheBatch(TConstArrayView<FCacheWrite> Writes)
		{
			FHjsonDerivedStoreStats Result;
			if (Writes.IsEmpty()) return Result;
			// A disposable cache never waits for another Editor. The budget is
			// observed under the same cross-process lock as the atomic write;
			// a previous process's accounting is not evidence of current usage.
			FCacheWriteContext Local;
			auto& Context = ActiveCacheWrites ? *ActiveCacheWrites : Local;
			if (!Context.Acquire(Result)) return Result;
			auto& CacheFiles = Context.Files; auto& CacheBytes = Context.Bytes;
			IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
			if (!Platform.CreateDirectoryTree(*CacheRoot())) return Result;
			for (const FCacheWrite& Write : Writes) {
				const FString& Path = Write.Path; const TArray<uint8>& Bytes = *Write.Bytes;
				const int64 StoredSize = Bytes.Num() + (Write.Header ? Write.Header->Num() : 0);
				if (Bytes.IsEmpty() || StoredSize > MaxBlobBytes) continue;
				int32 Existing = CacheFiles.IndexOfByPredicate([&](const FCacheFile& File) { return File.Path == Path; });
				if (Existing != INDEX_NONE && !Write.bReplace) continue;
				const int64 OldBytes = Existing == INDEX_NONE ? 0 : CacheFiles[Existing].Bytes;
				for (int32 Index = 0; CacheBytes - OldBytes > DiskBudgetBytes - StoredSize && Index < CacheFiles.Num();) {
					const FCacheFile Old = CacheFiles[Index];
					if (Old.Path == Path) { ++Index; continue; }
					if (IFileManager::Get().Delete(*Old.Path, false, true, true)) {
						CacheBytes -= Old.Bytes; Result.EvictedBytes += Old.Bytes;
						if (FPaths::GetCleanFilename(Old.Path).StartsWith(TEXT("derived-"))) ++Result.EvictedDerived;
						else ++Result.EvictedSyntax;
						Context.RemoveAt(Index);
					} else ++Index;
				}
				if (CacheBytes - OldBytes > DiskBudgetBytes - StoredSize) continue;
				const double WriteStarted = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { Result.WriteSeconds += FPlatformTime::Seconds() - WriteStarted; };
				const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
				ON_SCOPE_EXIT { IFileManager::Get().Delete(*Temporary, false, true, true); };
				TUniquePtr<IFileHandle> Handle(Platform.OpenWrite(*Temporary));
				if (!Handle || (Write.Header && !Handle->Write(Write.Header->GetData(), Write.Header->Num()))
					|| !Handle->Write(Bytes.GetData(), Bytes.Num()) || !Handle->Flush(true)) continue;
				Handle.Reset();
				if (!IFileManager::Get().Move(*Path, *Temporary, true, false, false, true)) continue;
				Existing = CacheFiles.IndexOfByPredicate([&](const FCacheFile& File) { return File.Path == Path; });
				if (Existing != INDEX_NONE) Context.RemoveAt(Existing);
				Context.Add(Path, StoredSize); CacheBytes += StoredSize - OldBytes;
				++Result.Stored; Result.StoredBytes += StoredSize;
			}
			return Result;
		}
		bool WriteCache(const FString& Path, const TArray<uint8>& Bytes)
		{
			const FCacheWrite Write{Path, &Bytes, false};
			return WriteCacheBatch(MakeArrayView(&Write, 1)).Stored == 1;
		}
	}

	FHjsonSyntaxCacheStats GetHjsonSyntaxCacheStats() { return Stats; }
	FScopedHjsonCacheWrites::FScopedHjsonCacheWrites()
	{
		if (!ActiveCacheWrites) { ActiveCacheWrites = MakeUnique<FCacheWriteContext>(); bOwns = true; }
	}
	FScopedHjsonCacheWrites::~FScopedHjsonCacheWrites()
	{
		if (!bOwns) return;
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=syntax_cache_write_scope calls=%lld directory_observations=%lld observe_s=%.6f catalog_bytes=%lld"),
			ActiveCacheWrites->Calls, ActiveCacheWrites->Observations, ActiveCacheWrites->ObserveSeconds,
			static_cast<int64>(ActiveCacheWrites->Files.GetAllocatedSize()) + ActiveCacheWrites->PathBytes);
		ActiveCacheWrites.Reset();
	}

	bool TryRestoreHjsonDerivedBlob(const FString& InputKey, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		const FString StoredKey = Key(InputKey, TEXT("derived-blob-v1"));
		const FString Path = CacheRoot() / (TEXT("derived-") + StoredKey + TEXT(".syntax"));
		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		const int64 Size = Handle ? Handle->Size() : -1;
		if (Size < 48 || Size > MaxBlobBytes) return false;
		TArray<uint8> Raw; Raw.SetNumUninitialized(static_cast<int32>(Size));
		if (!Handle->Read(Raw.GetData(), Size) || Handle->Size() != Size) return false;
		FReader Reader{Raw};
		if (Reader.Pod<uint32>() != 0x55414944 || Reader.Pod<uint32>() != 1
			|| !Reader.Text().Equals(StoredKey, ESearchCase::CaseSensitive)) return false;
		uint8 Expected[SHA256_DIGEST_LENGTH], Actual[SHA256_DIGEST_LENGTH];
		Reader.Raw(Expected, sizeof(Expected));
		const int64 PayloadSize = Reader.Pod<int64>();
		if (!Reader.bValid || PayloadSize < 1 || PayloadSize != Raw.Num() - Reader.Offset) return false;
		Digest(Raw.GetData() + Reader.Offset, PayloadSize, Actual);
		if (FMemory::Memcmp(Expected, Actual, sizeof(Actual))) return false;
		Raw.RemoveAt(0, static_cast<int32>(Reader.Offset), EAllowShrinking::No);
		OutBytes = MoveTemp(Raw);
		return true;
	}

	FHjsonDerivedStoreStats RememberHjsonDerivedBlobs(TConstArrayView<FHjsonDerivedBlob> Blobs)
	{
		// One bounded batch shares one actual directory observation/lock with
		// syntax storage. The .syntax suffix also keeps old readers' disk-budget
		// accounting complete; the distinct prefix/header cannot be parsed as AST.
		// Keep only small envelopes here. The caller's bounded payload remains
		// the write backing, rather than making a second whole-batch byte copy.
		TArray<TArray<uint8>> Headers; TArray<FCacheWrite> Writes;
		Headers.Reserve(Blobs.Num()); Writes.Reserve(Blobs.Num());
		int64 PendingBytes = 0;
		for (const auto& Blob : Blobs) {
			if (Blob.Bytes.IsEmpty() || Blob.Bytes.Num() > MaxBlobBytes - 512) continue;
			if (PendingBytes + Blob.Bytes.Num() + 512 > 64ll * 1024 * 1024) break;
			const FString StoredKey = Key(Blob.Key, TEXT("derived-blob-v1"));
			uint8 Hash[SHA256_DIGEST_LENGTH]; Digest(Blob.Bytes.GetData(), Blob.Bytes.Num(), Hash);
			FWriter Writer; Writer.Pod<uint32>(0x55414944); Writer.Pod<uint32>(1); Writer.Text(StoredKey);
			Writer.Raw(Hash, sizeof(Hash)); Writer.Pod<int64>(Blob.Bytes.Num());
			if (!Writer.bValid) continue;
			PendingBytes += Writer.Bytes.Num() + Blob.Bytes.Num(); Headers.Add(MoveTemp(Writer.Bytes));
			Writes.Add({CacheRoot() / (TEXT("derived-") + StoredKey + TEXT(".syntax")), &Blob.Bytes, true, &Headers.Last()});
		}
		auto Result = WriteCacheBatch(Writes);
		Result.ScratchBytes += Headers.GetAllocatedSize() + Writes.GetAllocatedSize();
		for (const auto& Header : Headers) Result.ScratchBytes += Header.GetAllocatedSize();
		for (const auto& Write : Writes) Result.ScratchBytes += Write.Path.GetAllocatedSize();
		return Result;
	}

	namespace
	{
		struct FInputKey
		{
			FHjsonInputDigest Bytes;
			bool operator==(const FInputKey& Other) const { return !FMemory::Memcmp(Bytes.GetData(), Other.Bytes.GetData(), 32); }
			friend uint32 GetTypeHash(const FInputKey& Value) { uint32 Hash; FMemory::Memcpy(&Hash, Value.Bytes.GetData(), sizeof(Hash)); return Hash; }
		};
		FInputKey PointerKey(const FString& Pointer) {
			FInputKey Result; Digest(*Pointer, static_cast<int64>(Pointer.Len()) * sizeof(TCHAR), Result.Bytes.GetData()); return Result;
		}
		struct FGroupedDocument
		{
			FString InputHash;
			TMap<FInputKey, FHjsonInputDigest> Inputs;
			int64 Charge() const { return sizeof(*this) + InputHash.GetAllocatedSize() + Inputs.GetAllocatedSize(); }
		};
		struct FSyntaxGroup
		{
			bool bDirty = false;
			TMap<FString, FGroupedDocument, FDefaultSetAllocator, StateSemantic::TExactStringMapKeyFuncs<FGroupedDocument>> Documents;
			int64 Charge() const {
				int64 Bytes = sizeof(*this) + Documents.GetAllocatedSize();
				for (const auto& Pair : Documents) Bytes += Pair.Key.GetAllocatedSize() + Pair.Value.Charge();
				return Bytes;
			}
		};
		struct FSyntaxGroups
		{
			static constexpr int32 GroupCount = 128;
			static constexpr int64 MemoryBudget = 64ll * 1024 * 1024;
			TMap<int32, FSyntaxGroup> Groups;
			FString Scope;
			int64 Bytes = 0, Peak = 0, InputHits = 0, InputMisses = 0, Loaded = 0, Rejected = 0;
			double KeySeconds = 0, LoadSeconds = 0, SealSeconds = 0;
			FHjsonDerivedStoreStats Store;
			explicit FSyntaxGroups(const FString& InScope) : Scope(FPaths::ConvertRelativePathToFull(InScope)) {
				FPaths::NormalizeDirectoryName(Scope); Groups.Reserve(GroupCount);
				Bytes = sizeof(*this) + Scope.GetAllocatedSize() + Groups.GetAllocatedSize(); Peak = Bytes;
			}
			// Private input-digest material only; old publication-only fields are
			// disposable misses, independent of public workspace/schema versions.
			FString GroupKey(int32 Index) const { return FString::Printf(TEXT("syntax-group-v4|parser=%u|tchar=%u|zlib=%u|%d|%s"),
				SyntaxVersion, uint32(sizeof(TCHAR)), uint32(FCompression::GetCompressorVersion(NAME_Zlib)), Index, *Scope); }
			FSyntaxGroup& Select(const FString& SourceUnit)
			{
				// Partition by high hash bits; the per-group TMap uses low bits.
				// Selecting both with low bits degenerates every map bucket chain.
				const int32 Index = GetTypeHash(SourceUnit) >> 25;
				if (auto* Existing = Groups.Find(Index)) return *Existing;
				auto& Group = Groups.Add(Index);
				const int64 EmptyCharge = Group.Charge(); Bytes += EmptyCharge;
				const double Started = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { LoadSeconds += FPlatformTime::Seconds() - Started; };
				TArray<uint8> Raw;
				if (!TryRestoreHjsonDerivedBlob(GroupKey(Index), Raw)) return Group;
				Loaded += Raw.Num(); Peak = FMath::Max<int64>(Peak, Bytes + Raw.GetAllocatedSize());
				FReader Reader{Raw};
				const uint32 Version = Reader.Pod<uint32>(), Count = Reader.Pod<uint32>();
				if (!Reader.bValid || Version != 4 || Count > 16384) { ++Rejected; return Group; }
				int64 DocumentBytes = 0;
				for (uint32 I = 0; I < Count && Reader.bValid; ++I) {
					FString Name = Reader.Text(); FGroupedDocument Document;
					Document.InputHash = Reader.Text();
					if (!Reader.bValid || Name.IsEmpty()
						|| int32(GetTypeHash(Name) >> 25) != Index || Group.Documents.Contains(Name)) { Reader.bValid = false; break; }
					const uint32 Inputs = Reader.Pod<uint32>();
					if (!Reader.bValid || Inputs > uint64(Raw.Num() - Reader.Offset) / 64) { Reader.bValid = false; break; }
					for (uint32 J = 0; J < Inputs; ++J) {
						FInputKey Input; FHjsonInputDigest Value;
						Reader.Raw(Input.Bytes.GetData(), 32); Reader.Raw(Value.GetData(), 32);
						if (Document.Inputs.Contains(Input)) { Reader.bValid = false; break; }
						Document.Inputs.Add(Input, Value);
					}
					DocumentBytes += Name.GetAllocatedSize() + Document.Charge();
					Group.Documents.Add(MoveTemp(Name), MoveTemp(Document));
					const int64 CandidateBytes = Bytes + DocumentBytes + Group.Documents.GetAllocatedSize();
					Peak = FMath::Max<int64>(Peak, CandidateBytes + Raw.GetAllocatedSize());
					if (CandidateBytes > MemoryBudget) { Reader.bValid = false; break; }
				}
				if (!Reader.bValid || Reader.Offset != Raw.Num()) { Group.Documents.Empty(); ++Rejected; }
				Bytes += Group.Charge() - EmptyCharge; Peak = FMath::Max<int64>(Peak, Bytes + Raw.GetAllocatedSize());
				return Group;
			}
			void Finish()
			{
				const double Started = FPlatformTime::Seconds();
				TArray<FHjsonDerivedBlob> Blobs; int64 PendingBytes = 0;
				for (const auto& Pair : Groups) {
					if (!Pair.Value.bDirty) continue;
					FWriter Writer; Writer.Pod<uint32>(4); Writer.Pod<uint32>(Pair.Value.Documents.Num());
					for (const auto& Entry : Pair.Value.Documents) {
						Writer.Text(Entry.Key); const auto& Document = Entry.Value;
						Writer.Text(Document.InputHash);
						Writer.Pod<uint32>(Document.Inputs.Num());
						for (const auto& Input : Document.Inputs) { Writer.Raw(Input.Key.Bytes.GetData(), 32); Writer.Raw(Input.Value.GetData(), 32); }
					}
					if (!Writer.bValid || PendingBytes + Writer.Bytes.Num() + 512 > MemoryBudget) { ++Rejected; continue; }
					PendingBytes += Writer.Bytes.Num() + 512;
					Blobs.Add({GroupKey(Pair.Key), MoveTemp(Writer.Bytes)});
				}
				Peak = FMath::Max(Peak, Bytes + PendingBytes); SealSeconds = FPlatformTime::Seconds() - Started;
				Store = RememberHjsonDerivedBlobs(Blobs);
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=syntax_groups input_hits=%lld input_misses=%lld publication_hits=%lld loaded_bytes=%lld retained_bytes=%lld peak_bytes=%lld rejected=%lld key_s=%.6f load_s=%.6f seal_s=%.6f stored=%d stored_bytes=%lld store_observe_s=%.6f store_write_s=%.6f evicted_syntax=%d evicted_derived=%d evicted_bytes=%lld"),
					InputHits, InputMisses, int64(0), Loaded, Bytes, Peak, Rejected, KeySeconds, LoadSeconds, SealSeconds,
					Store.Stored, Store.StoredBytes, Store.ObserveSeconds, Store.WriteSeconds, Store.EvictedSyntax, Store.EvictedDerived, Store.EvictedBytes);
			}
		};
		thread_local TUniquePtr<FSyntaxGroups> ActiveSyntaxGroups;
	}

	FScopedHjsonSyntaxGroups::FScopedHjsonSyntaxGroups(const FString& SourceScope) { if (!ActiveSyntaxGroups) { ActiveSyntaxGroups = MakeUnique<FSyntaxGroups>(SourceScope); bOwns = true; } }
	FScopedHjsonSyntaxGroups::~FScopedHjsonSyntaxGroups() { if (bOwns) { ActiveSyntaxGroups->Finish(); ActiveSyntaxGroups.Reset(); } }
	bool FindHjsonInputDigest(const FString& SourceUnit, const FString& CurrentHash, const FString& Pointer, FHjsonInputDigest& Out)
	{
		if (!ActiveSyntaxGroups || CurrentHash.IsEmpty()) return false;
		auto& State = *ActiveSyntaxGroups; auto& Group = State.Select(SourceUnit);
		const double Started = FPlatformTime::Seconds(); const auto Key = PointerKey(Pointer);
		State.KeySeconds += FPlatformTime::Seconds() - Started;
		if (const auto* Document = Group.Documents.Find(SourceUnit); Document && Document->InputHash.Equals(CurrentHash, ESearchCase::CaseSensitive))
			if (const auto* Digest = Document->Inputs.Find(Key)) { Out = *Digest; ++State.InputHits; return true; }
		++State.InputMisses; return false;
	}
	void RememberHjsonInputDigest(const FString& SourceUnit, const FString& CurrentHash, const FString& Pointer, const FHjsonInputDigest& Value)
	{
		if (!ActiveSyntaxGroups || CurrentHash.IsEmpty()) return;
		auto& State = *ActiveSyntaxGroups; auto& Group = State.Select(SourceUnit);
		// A failed admission keeps all previously useful facts. A single row
		// never retires the complete candidate or raises its original budget.
		const auto* Prior = Group.Documents.Find(SourceUnit);
		const int64 Admission = SourceUnit.GetAllocatedSize() + CurrentHash.GetAllocatedSize() + 1024
			+ (Prior ? 0 : Group.Documents.GetAllocatedSize());
		if (State.Bytes + Admission > FSyntaxGroups::MemoryBudget) { ++State.Rejected; return; }
		const int64 Before = Group.Documents.GetAllocatedSize() + (Prior ? Prior->Charge() : 0);
		auto& Document = Group.Documents.FindOrAdd(SourceUnit);
		if (!Document.InputHash.Equals(CurrentHash, ESearchCase::CaseSensitive)) { Document.InputHash = CurrentHash; Document.Inputs.Empty(); }
		const double KeyStarted = FPlatformTime::Seconds();
		const auto InputKey = PointerKey(Pointer);
		State.KeySeconds += FPlatformTime::Seconds() - KeyStarted;
		Document.Inputs.Add(InputKey, Value); Group.bDirty = true;
		int64 Delta = Group.Documents.GetAllocatedSize() + Document.Charge() - Before + (Prior ? 0 : SourceUnit.GetAllocatedSize());
		State.Peak = FMath::Max(State.Peak, State.Bytes + Delta);
		if (State.Bytes + Delta > FSyntaxGroups::MemoryBudget) {
			Document.Inputs.Remove(InputKey); Document.Inputs.Shrink(); ++State.Rejected;
			Delta = Group.Documents.GetAllocatedSize() + Document.Charge() - Before + (Prior ? 0 : SourceUnit.GetAllocatedSize());
		}
		State.Bytes += Delta; State.Peak = FMath::Max(State.Peak, State.Bytes);
	}

	bool TryRestoreHjsonSyntax(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint)
	{
		if (!Eligible(Text)) return false;
		if (Checkpoint && !(*Checkpoint)(TEXT("hjson_syntax_restore"), OutError)) return false;
		const double Started = FPlatformTime::Seconds();
		ON_SCOPE_EXIT { Stats.LoadSeconds += FPlatformTime::Seconds() - Started; };
		const FString InputKey = Key(Text, SourceUnit);
		const FString Path = CacheRoot() / (InputKey + TEXT(".syntax"));
		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		const int64 FileSize = Handle ? Handle->Size() : -1;
		if (FileSize < 48 || FileSize > MaxBlobBytes) { ++Stats.Misses; return false; }
		TArray<uint8> Packed; Packed.SetNumUninitialized(static_cast<int32>(FileSize));
		if (!Handle->Read(Packed.GetData(), FileSize) || Handle->Size() != FileSize) return false;
		++Stats.Rejected;
		bool bRestored = false;
		ON_SCOPE_EXIT {
			Handle.Reset();
			if (!bRestored && !OutError.HasErrors()) {
				// A corrupt disposable candidate must not cause a permanent miss loop.
				DeleteInvalidCache(Path);
			}
		};
		// Size and bytes belong to the same handle; a concurrently replaced cache
		// pathname cannot turn a bounded candidate into an unbounded file load.
		Handle.Reset();
		if (Checkpoint && !(*Checkpoint)(TEXT("hjson_syntax_restore"), OutError)) return false;
		FReader Header{Packed};
		if (Header.Pod<uint32>() != SyntaxVersion || Header.Pod<uint32>() != sizeof(TCHAR)) return false;
		const int64 RawSize = Header.Pod<int64>();
		uint8 Expected[SHA256_DIGEST_LENGTH]; Header.Raw(Expected, sizeof(Expected));
		if (!Header.bValid || RawSize < 1 || RawSize > MaxBlobBytes) return false;
		TArray<uint8> Raw; Raw.SetNumUninitialized(static_cast<int32>(RawSize));
		if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize,
			Packed.GetData() + Header.Offset, Packed.Num() - Header.Offset)) return false;
		if (Checkpoint && !(*Checkpoint)(TEXT("hjson_syntax_restore"), OutError)) return false;
		uint8 Actual[SHA256_DIGEST_LENGTH]; Digest(Raw.GetData(), Raw.Num(), Actual);
		if (FMemory::Memcmp(Expected, Actual, sizeof(Actual))) return false;
		FReader Reader{Raw};
		// A copied/renamed cache file is not a proof for another source input.
		if (!Reader.Text().Equals(InputKey, ESearchCase::CaseSensitive)) return false;
		TMap<EUeAgentResourceDimension, int64> Charges;
		for (uint8 D = 0; D < DimensionCount; ++D) {
			const int64 Amount = Reader.Pod<int64>(); if (Amount < 0 || Amount == MAX_int64) return false;
			if (Amount) Charges.Add(static_cast<EUeAgentResourceDimension>(D), Amount);
		}
		const uint32 StringCount = Reader.Pod<uint32>();
		if (!Reader.bValid || StringCount > static_cast<uint64>(Raw.Num()) / sizeof(int32)) return false;
		TArray<FString> Strings; Strings.Reserve(static_cast<int32>(StringCount));
		for (uint32 I = 0; I < StringCount && Reader.bValid; ++I) {
			if (Checkpoint && I % 1024 == 0 && !(*Checkpoint)(TEXT("hjson_syntax_restore"), OutError)) return false;
			Strings.Add(Reader.Text());
		}
		const uint32 Count = Reader.Pod<uint32>();
		if (!Reader.bValid || Count < 1 || Count > MaxNodes || Count > static_cast<uint64>(Raw.Num()) / 42) return false;
		TArray<FHjsonNodeRef> Nodes; Nodes.Reserve(static_cast<int32>(Count));
		const auto Source = MakeShared<const FString>(SourceUnit);
		for (uint32 I = 0; I < Count; ++I) {
			if (Checkpoint && I % 1024 == 0 && !(*Checkpoint)(TEXT("hjson_syntax_restore"), OutError)) return false;
			const uint8 Kind = Reader.Pod<uint8>(), Boolean = Reader.Pod<uint8>();
			const int32 Line = Reader.Pod<int32>(), Column = Reader.Pod<int32>();
			const int64 Start = Reader.Pod<int64>(), End = Reader.Pod<int64>();
			const uint32 Pointer = Reader.Pod<uint32>(), Lexeme = Reader.Pod<uint32>(), Value = Reader.Pod<uint32>(), OriginalKey = Reader.Pod<uint32>();
			const uint32 Children = Reader.Pod<uint32>();
			if (!Reader.bValid || Kind > static_cast<uint8>(EHjsonNodeKind::Object) || Boolean > 1 || Line < 1 || Column < 1 || Start < 0 || End < Start
				|| Pointer >= StringCount || Lexeme >= StringCount || Value >= StringCount || OriginalKey >= StringCount || Children > I
				|| (Kind != static_cast<uint8>(EHjsonNodeKind::Array) && Kind != static_cast<uint8>(EHjsonNodeKind::Object) && Children)) return false;
			TArray<FHjsonNodeRef> Items; TArray<TPair<FString, FHjsonNodeRef>> Entries;
			for (uint32 C = 0; C < Children; ++C) {
				const uint32 Field = Kind == static_cast<uint8>(EHjsonNodeKind::Object) ? Reader.Pod<uint32>() : 0;
				const uint32 Child = Reader.Pod<uint32>();
				if (!Reader.bValid || Child >= I || Field >= StringCount) return false;
				if (Kind == static_cast<uint8>(EHjsonNodeKind::Object)) Entries.Emplace(Strings[Field], Nodes[Child]); else Items.Add(Nodes[Child]);
			}
			Nodes.Add(MakeShared<FHjsonNode>(static_cast<EHjsonNodeKind>(Kind),
				FSourceProvenance(Source, Strings[Pointer], Line, Column, Start, End, Strings[OriginalKey]),
				Strings[Lexeme], Strings[Value], Boolean != 0, MoveTemp(Items), MoveTemp(Entries)));
		}
		if (!Reader.bValid || Reader.Offset != Raw.Num()) return false;
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany(Charges, TEXT("hjson:") + SourceUnit, ChargeError)) { OutError.SetFailure(MoveTemp(ChargeError), SourceUnit); return false; }
		OutDocument = MakeShared<FHjsonDocument>(SourceUnit, Nodes.Last());
		bRestored = true;
		--Stats.Rejected; ++Stats.Hits; Stats.LoadedBytes += Packed.Num();
		return true;
	}

	void RememberHjsonSyntax(const FString& Text, const FString& SourceUnit, const FHjsonDocument& Document,
		const FUeAgentResourceUsage& Before, const FUeAgentResourceUsage& After)
	{
		if (!Eligible(Text)) return;
		const double Started = FPlatformTime::Seconds();
		double StageStarted = Started;
		double* Stage = &Stats.StoreLookupSeconds;
		auto NextStage = [&](double& Next) {
			const double Now = FPlatformTime::Seconds();
			*Stage += Now - StageStarted; Stage = &Next; StageStarted = Now;
		};
		ON_SCOPE_EXIT {
			const double Now = FPlatformTime::Seconds();
			*Stage += Now - StageStarted; Stats.StoreSeconds += Now - Started;
		};
		const FString InputKey = Key(Text, SourceUnit), Path = CacheRoot() / (InputKey + TEXT(".syntax"));
		if (IFileManager::Get().FileExists(*Path)) return;
		NextStage(Stats.StoreBuildSeconds);
		struct FVisit { const FHjsonNode* Node; bool bExit; };
		TArray<FVisit> Pending{{&Document.GetRoot().Get(), false}};
		TArray<const FHjsonNode*> Nodes; TMap<const FHjsonNode*, uint32> NodeIds;
		TMap<FString, uint32, FDefaultSetAllocator, StateSemantic::TExactStringMapKeyFuncs<uint32>> StringIds;
		TArray<const FString*> Strings; int64 StringBytes = 0;
		auto AddString = [&](const FString& Value) {
			if (!StringIds.Contains(Value)) { StringIds.Add(Value, Strings.Num()); Strings.Add(&Value); StringBytes += 4 + static_cast<int64>(Value.Len()) * sizeof(TCHAR); }
		};
		while (!Pending.IsEmpty()) {
			const FVisit Visit = Pending.Pop(EAllowShrinking::No); const FHjsonNode& Node = *Visit.Node;
			if (!Visit.bExit) {
				if (NodeIds.Contains(Visit.Node)) continue;
				if (Nodes.Num() + Pending.Num() > MaxNodes || StringBytes > MaxBlobBytes) return;
				Pending.Add({Visit.Node, true});
				for (const auto& Child : Node.GetItems()) Pending.Add({&Child.Get(), false});
				for (const auto& Child : Node.GetEntries()) Pending.Add({&Child.Value.Get(), false});
				continue;
			}
			if (!Node.GetProvenance().GetSourceUnit().Equals(SourceUnit, ESearchCase::CaseSensitive)) return;
			NodeIds.Add(Visit.Node, Nodes.Num()); Nodes.Add(Visit.Node);
			AddString(Node.GetProvenance().GetJsonPointer()); AddString(Node.GetLexeme()); AddString(Node.GetText()); AddString(Node.GetProvenance().GetOriginalKey());
			for (const auto& Field : Node.GetEntries()) AddString(Field.Key);
		}
		Stats.StoreBuiltNodes += Nodes.Num();
		NextStage(Stats.StoreSerializeSeconds);
		FWriter Raw; Raw.Text(InputKey);
		for (uint8 D = 0; D < DimensionCount; ++D) {
			const auto Dimension = static_cast<EUeAgentResourceDimension>(D); const int64 A = Before.Consumed(Dimension), B = After.Consumed(Dimension);
			if (B == MAX_int64 || B < A) return; Raw.Pod<int64>(B - A);
		}
		Raw.Pod<uint32>(Strings.Num()); for (const FString* String : Strings) Raw.Text(*String);
		Raw.Pod<uint32>(Nodes.Num());
		for (const FHjsonNode* Node : Nodes) {
			const auto& P = Node->GetProvenance();
			Raw.Pod<uint8>(static_cast<uint8>(Node->GetKind())); Raw.Pod<uint8>(Node->GetBoolean() ? 1 : 0);
			Raw.Pod<int32>(P.GetLine()); Raw.Pod<int32>(P.GetColumn()); Raw.Pod<int64>(P.GetByteStart()); Raw.Pod<int64>(P.GetByteEnd());
			Raw.Pod<uint32>(StringIds.FindChecked(P.GetJsonPointer())); Raw.Pod<uint32>(StringIds.FindChecked(Node->GetLexeme()));
			Raw.Pod<uint32>(StringIds.FindChecked(Node->GetText())); Raw.Pod<uint32>(StringIds.FindChecked(P.GetOriginalKey()));
			Raw.Pod<uint32>(Node->GetItems().Num() + Node->GetEntries().Num());
			for (const auto& Child : Node->GetItems()) Raw.Pod<uint32>(NodeIds.FindChecked(&Child.Get()));
			for (const auto& Child : Node->GetEntries()) { Raw.Pod<uint32>(StringIds.FindChecked(Child.Key)); Raw.Pod<uint32>(NodeIds.FindChecked(&Child.Value.Get())); }
		}
		if (!Raw.bValid) return;
		Stats.StoreRawBytes += Raw.Bytes.Num();
		NextStage(Stats.StoreCompressSeconds);
		// This disposable AST candidate is latency-sensitive. UE's NAME_Zlib
		// wrapper fixes the default compression level and ignores speed flags.
		// Use the same zlib stream at its fast level: existing blobs/readers and
		// exact uncompressed SHA remain compatible; byte admission is unchanged.
		const uLong RawSize = static_cast<uLong>(Raw.Bytes.Num());
		uLongf CompressedSize = compressBound(RawSize);
		if (CompressedSize > MaxBlobBytes - 48) return;
		TArray<uint8> Compressed; Compressed.SetNumUninitialized(static_cast<int32>(CompressedSize));
		if (compress2(Compressed.GetData(), &CompressedSize, Raw.Bytes.GetData(), RawSize, Z_BEST_SPEED) != Z_OK) return;
		NextStage(Stats.StoreSerializeSeconds);
		uint8 Hash[SHA256_DIGEST_LENGTH]; Digest(Raw.Bytes.GetData(), Raw.Bytes.Num(), Hash);
		FWriter Packed; Packed.Pod<uint32>(SyntaxVersion); Packed.Pod<uint32>(sizeof(TCHAR)); Packed.Pod<int64>(Raw.Bytes.Num()); Packed.Raw(Hash, sizeof(Hash)); Packed.Raw(Compressed.GetData(), CompressedSize);
		NextStage(Stats.StoreIoSeconds);
		if (Packed.bValid && WriteCache(Path, Packed.Bytes)) { ++Stats.Stored; Stats.StorePackedBytes += Packed.Bytes.Num(); }
	}
}
