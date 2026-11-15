#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"
#include "HAL/PlatformAtomics.h"

class FJsonValue;
class FJsonObject;

namespace UE::UAI::HjsonWorkspace { class FExportFilePlan; }
namespace UE::UAI::ArtifactPublisher { void RememberVerifiedPlanBytes(const HjsonWorkspace::FExportFilePlan& File); }

namespace UE::UAI::HjsonWorkspace
{
	class FWorkspaceError;
	class FSourceProvenance;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	using FHjsonOperationCheckpoint = TFunction<bool(
		const FString& Stage, FWorkspaceError& OutError)>;

	UEAGENTINTERFACE_API FString EscapeJsonPointerSegment(FString Segment);

	// Shared accounting for semantic/content-graph workspace parsing,
	// verification, composition, and publication.
	UEAGENTINTERFACE_API FUeAgentResourceTracker
	MakeSemanticWorkspaceResourceTracker();

	UEAGENTINTERFACE_API TSharedRef<
		FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeSemanticWorkspaceResourceTrackerOwner();

	enum class EHjsonNodeKind : uint8
	{
		Null,
		Boolean,
		String,
		Integer,
		Decimal,
		Array,
		Object,
	};

	enum class EWorkspaceSourceKind : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	enum class EWorkspaceKind : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	class FHjsonDocument;
	class FHjsonNode;
	struct FHjsonNodeRelocation;

	class UEAGENTINTERFACE_API FWorkspaceError final
	{
	public:
		const FString& GetCode() const;
		const FString& GetSourceUnit() const;
		const FString& GetJsonPointer() const;
		int64 GetObserved() const;
		int32 GetLine() const;
		int32 GetColumn() const;
		bool IsStoppedEarly() const;
		const TArray<FString>& GetCyclePath() const;
		const Contract::FDiagnostic& GetDiagnostic() const { return Diagnostic; }
		void SetFailure(Contract::FDiagnostic Failure, const FString& FallbackSource = FString(),
			const FString& FallbackJsonPointer = TEXT("/"));
		// Collect independent failures without replacing the first error/code.
		void Append(const FWorkspaceError& Error);
		void MarkUnchecked(const FString& Stage, const FString& Source = FString(),
			const FString& Reason = TEXT("prerequisite_failed"), int32 FromLine = 0);
		bool HasErrors() const { return !Diagnostic.IsEmpty(); }
		void Locate(const FSourceProvenance& Source);
		void Locate(const FHjsonDocument& Document);
		// Resolve producer-supplied paths against an original parser subtree.
		// Missing fields point to their existing parent as related input.
		void Locate(const FHjsonNode& Root);
		// At a delegation boundary, qualify paths relative to the delegated value.
		void PrependPath(const FString& PointerPrefix);
		// Input delegation must not rebase native facts or already-bound command input.
		void PrependPath(const FString& PointerPrefix, bool bInputOnly);
		void LocateUnits(const TMap<FString, FSourceProvenance>& Sources);
		void Recode(const FString& InCode, const FString& PublicReason);
		void SetMessage(FString InMessage) { Diagnostic.PublicMessage = MoveTemp(InMessage); }

		void Reset();
		void SetSourceFailure(
			FString InCode,
			FString PublicReason,
			FString InSourceUnit = FString(),
			FString InJsonPointer = TEXT("/"),
			int64 InObserved = 0,
			bool bInStoppedEarly = false,
			TArray<FString> InCyclePath = {},
			int32 InLine = 0,
			int32 InColumn = 0);

	private:
		Contract::FDiagnostic Diagnostic;
	};

	class UEAGENTINTERFACE_API FSourceProvenance final
	{
	public:
		FSourceProvenance(
			FString InSourceUnit,
			FString InJsonPointer,
			int32 InLine,
			int32 InColumn,
			int64 InByteStart,
			int64 InByteEnd,
			FString InOriginalKey);
		// A parser document shares its immutable source name across all nodes.
		// Nodes retain this backing when they outlive the document wrapper.
		FSourceProvenance(
			TSharedRef<const FString> InSourceUnit,
			FString InJsonPointer, int32 InLine, int32 InColumn,
			int64 InByteStart, int64 InByteEnd, FString InOriginalKey);

		const FString& GetSourceUnit() const;
		const TSharedRef<const FString>& GetSourceUnitBacking() const { return SourceUnit; }
		const FString& GetJsonPointer() const;
		int32 GetLine() const;
		int32 GetColumn() const;
		int64 GetByteStart() const;
		int64 GetByteEnd() const;
		const FString& GetOriginalKey() const;

	private:
		TSharedRef<const FString> SourceUnit;
		FString JsonPointer;
		int32 Line = 1;
		int32 Column = 1;
		int64 ByteStart = 0;
		int64 ByteEnd = 0;
		FString OriginalKey;
	};

	class FHjsonNode;
	using FHjsonNodePtr = TSharedPtr<const FHjsonNode>;
	using FHjsonNodeRef = TSharedRef<const FHjsonNode>;

	class UEAGENTINTERFACE_API FHjsonNode final
	{
	public:
		FHjsonNode(
			EHjsonNodeKind InKind,
			FSourceProvenance InProvenance,
			FString InLexeme,
			FString InText,
			bool bInBoolean,
			TArray<FHjsonNodeRef> InItems,
			TArray<TPair<FString, FHjsonNodeRef>> InEntries);

		// An unchanged subtree keeps its value storage. Only its source location
		// changes; children acquire their new locations when actually requested.
		FHjsonNode(FHjsonNodeRef Unchanged, FSourceProvenance InProvenance);
		const FHjsonNode* GetValueStorageIdentity() const;
		int64 GetOwnedStorageBytes(TArray<const FHjsonNode*>& OutRetainedNodes) const;

		EHjsonNodeKind GetKind() const;
		const FSourceProvenance& GetProvenance() const;
		const FString& GetLexeme() const;
		const FString& GetText() const;
		bool GetBoolean() const;
		const TArray<FHjsonNodeRef>& GetItems() const;
		const TArray<TPair<FString, FHjsonNodeRef>>& GetEntries() const;
		const FHjsonNode* Get(const FString& Key) const;
		const FHjsonNode* Find(const FString& RelativeJsonPointer) const;

	private:
		EHjsonNodeKind Kind = EHjsonNodeKind::Null;
		FSourceProvenance Provenance;
		FString Lexeme;
		FString Text;
		bool bBoolean = false;
		TArray<FHjsonNodeRef> Items;
		TArray<TPair<FString, FHjsonNodeRef>> Entries;
		TSharedPtr<FHjsonNodeRelocation> Relocation;
	};

	class FHjsonDocument;
	using FHjsonDocumentPtr = TSharedPtr<const FHjsonDocument>;
	using FHjsonDocumentRef = TSharedRef<const FHjsonDocument>;

	class UEAGENTINTERFACE_API FHjsonDocument final
	{
	public:
		FHjsonDocument(FString InSourceUnit, FHjsonNodeRef InRoot);
		explicit FHjsonDocument(FHjsonNodeRef InRoot);

		const FString& GetSourceUnit() const;
		int64 GetOwnedSourceUnitAllocatedSize() const { return SourceUnit.GetAllocatedSize(); }
		const FHjsonNodeRef& GetRoot() const;
		const FHjsonNode* Find(const FString& JsonPointer) const;

	private:
		FString SourceUnit;
		FHjsonNodeRef Root;
		bool bSourceFromRoot = false;
	};

	class UEAGENTINTERFACE_API FWorkspaceSource final
	{
	public:
		static FWorkspaceSource Directory(FString Path);
		static FWorkspaceSource SingleFile(FString Path);
		static FWorkspaceSource Inline(
			FString Text,
			FString SourceUnit,
			FString SourceId);

		EWorkspaceSourceKind GetKind() const;
		const FString& GetPath() const;
		const FString& GetText() const;
		const FString& GetSourceUnit() const;
		const FString& GetSourceId() const;

	private:
		FWorkspaceSource(
			EWorkspaceSourceKind InKind,
			FString InPath,
			FString InText,
			FString InSourceUnit,
			FString InSourceId);

		EWorkspaceSourceKind Kind = EWorkspaceSourceKind::Inline;
		FString Path;
		FString Text;
		FString SourceUnit;
		FString SourceId;
	};

	class UEAGENTINTERFACE_API FWorkspacePolicy final
	{
	public:
		FWorkspacePolicy(
			FString InDialectId,
			FString InContractId,
			int32 InContractVersion,
			FString InProfileId,
			FString InEntryUnit,
			TArray<FString> InReferenceFields);

		const FString& GetDialectId() const;
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetProfileId() const;
		const FString& GetEntryUnit() const;
		const TArray<FString>& GetReferenceFields() const;
		bool IsValid() const;

	private:
		FString DialectId;
		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		FString EntryUnit;
		TArray<FString> ReferenceFields;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FWorkspaceUnit final
	{
	public:
		FWorkspaceUnit(
			FString InUnitId,
			FHjsonDocumentRef InDocument,
			TArray<uint8> InCanonicalUtf8,
			FString InContentHash);

		const FString& GetUnitId() const;
		const FHjsonDocumentRef& GetDocument() const;
		const TArray<uint8>& GetCanonicalUtf8() const;
		const FString& GetContentHash() const;

	private:
		FString UnitId;
		FHjsonDocumentRef Document;
		TArray<uint8> CanonicalUtf8;
		FString ContentHash;
	};

	class UEAGENTINTERFACE_API FReferenceEdge final
	{
	public:
		FReferenceEdge(
			FString InSourceUnit,
			FString InJsonPointer,
			FString InTargetUnit);

		const FString& GetSourceUnit() const;
		const FString& GetJsonPointer() const;
		const FString& GetTargetUnit() const;

	private:
		FString SourceUnit;
		FString JsonPointer;
		FString TargetUnit;
	};

	class FCanonicalWorkspace;
	using FCanonicalWorkspacePtr = TSharedPtr<const FCanonicalWorkspace>;
	using FCanonicalWorkspaceRef = TSharedRef<const FCanonicalWorkspace>;

	class UEAGENTINTERFACE_API FCanonicalWorkspace final
	{
	public:
		FCanonicalWorkspace(
			EWorkspaceSourceKind InSourceKind,
			FWorkspacePolicy InPolicy,
			TArray<FWorkspaceUnit> InUnits,
			TArray<FReferenceEdge> InReferenceEdges,
			FString InSemanticFingerprint);

		EWorkspaceSourceKind GetSourceKind() const;
		const FWorkspacePolicy& GetPolicy() const;
		const TArray<FWorkspaceUnit>& GetUnits() const;
		const TArray<FReferenceEdge>& GetReferenceEdges() const;
		const FString& GetSemanticFingerprint() const;

	private:
		EWorkspaceSourceKind SourceKind = EWorkspaceSourceKind::Inline;
		FWorkspacePolicy Policy;
		TArray<FWorkspaceUnit> Units;
		TArray<FReferenceEdge> ReferenceEdges;
		FString SemanticFingerprint;
	};

	struct FHjsonRootHeaderFacts;
	class UEAGENTINTERFACE_API FExportFilePlan final
	{
	public:
		using FFragmentIdentities = TArray<TSharedRef<const FString, ESPMode::ThreadSafe>>;
		FExportFilePlan(
			FString InRelativePath,
			TArray<uint8> InCanonicalUtf8,
			FString InContentHash,
			TArray<FIntPoint> InGeneratedFragmentSpans = {},
			TSharedPtr<const FFragmentIdentities> InGeneratedFragmentIdentities = nullptr);

		const FString& GetRelativePath() const;
		const TArray<uint8>& GetCanonicalUtf8() const;
		/** Weak identity of sealed bytes; not evidence of any validation by itself. */
		TWeakPtr<const TArray<uint8>> GetCanonicalUtf8Identity() const { return CanonicalUtf8; }
		const FString& GetContentHash() const;
		/** Digest handed off by the actual producer of these immutable bytes. */
		bool HasVerifiedCanonicalUtf8() const { return FPlatformAtomics::InterlockedCompareExchange(&bVerifiedCanonicalUtf8, 0, 0) != 0; }
		TSharedPtr<const FHjsonRootHeaderFacts> GetRootHeaderFacts() const { return RootHeaderFacts; }
		/** Formatter output indexed by the corresponding inventory member ordinal.
		 * Bound to this plan's immutable bytes; never inferred from another model. */
		TSharedPtr<const TArray<FIntPoint>> GetGeneratedFragmentSpans() const { return GeneratedFragmentSpans; }
		TSharedPtr<const FFragmentIdentities> GetGeneratedFragmentIdentities() const { return GeneratedFragmentIdentities; }
		/** Seal original-parser ranges beside these same immutable file bytes. */
		FExportFilePlan WithGeneratedFragmentSpans(TArray<FIntPoint> Spans,
			TSharedPtr<const FFragmentIdentities> Identities = nullptr) const {
			FExportFilePlan Result = *this;
			Result.GeneratedFragmentSpans = MakeShared<const TArray<FIntPoint>>(MoveTemp(Spans));
			Result.GeneratedFragmentIdentities = MoveTemp(Identities);
			return Result;
		}
		FExportFilePlan WithGeneratedFragmentIdentities(TSharedPtr<const FFragmentIdentities> Identities) const {
			FExportFilePlan Result = *this; Result.GeneratedFragmentIdentities = MoveTemp(Identities); return Result;
		}
		// Optional source-observation memo only. All files still receive the
		// publisher's real readback, hash, ownership and concurrent-write checks.
		/** Membership in the formatter's physical row or content_ref output. */
		void SetSemanticSourceDocument(bool bDocument) {
			bSemanticSourceDocument = bDocument;
			bSourceObservationNeeded = bDocument || RelativePath.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive);
		}
		bool IsSemanticSourceDocument() const { return bSemanticSourceDocument; }
		bool NeedsSourceObservation() const { return bSourceObservationNeeded; }

	private:
		FString RelativePath;
		// Plans and successful export baselines read the same immutable bytes.
		TSharedRef<const TArray<uint8>> CanonicalUtf8;
		FString ContentHash;
		TSharedPtr<const FHjsonRootHeaderFacts> RootHeaderFacts;
		TSharedPtr<const TArray<FIntPoint>> GeneratedFragmentSpans;
		// Same actual inventory ordinal as the spans. Both machine roles share
		// this identity sequence; it is not another global identity index.
		TSharedPtr<const FFragmentIdentities> GeneratedFragmentIdentities;
		bool bSourceObservationNeeded = true;
		bool bSemanticSourceDocument = false;
		mutable int32 bVerifiedCanonicalUtf8 = 0;
		friend void ArtifactPublisher::RememberVerifiedPlanBytes(const FExportFilePlan& File);
	};

	class FExportWorkspacePlan;
	using FExportWorkspacePlanPtr = TSharedPtr<const FExportWorkspacePlan>;
	using FExportWorkspacePlanRef = TSharedRef<const FExportWorkspacePlan>;

	class UEAGENTINTERFACE_API FExportWorkspacePlan final
	{
	public:
		FExportWorkspacePlan(
			EWorkspaceKind InWorkspaceKind,
			FString InEntryFile,
			FString InSnapshotId,
			FUeAgentStateRevision InSnapshotRevision,
			FString InSnapshotEvidenceId,
			TArray<FExportFilePlan> InFiles,
			FString InPlanFingerprint,
			int64 InObservedTokens, TMap<FString, FString> InRetainedFileHashes = {},
			TOptional<TSet<FString>> InPendingAuthorDocuments = {});

		EWorkspaceKind GetWorkspaceKind() const;
		const FString& GetEntryFile() const;
		const FString& GetSnapshotId() const;
		const FUeAgentStateRevision& GetSnapshotRevision() const;
		const FString& GetSnapshotEvidenceId() const;
		const TArray<FExportFilePlan>& GetFiles() const;
		/** Existing generated files which this publication leaves in place.
		 * These are expected current bytes, not new bodies or an authority to
		 * overwrite them. The original publisher verifies them in its source
		 * namespace/current-byte pass before advancing the same publication. */
		const TMap<FString, FString>& GetRetainedFileHashes() const { return RetainedFileHashes; }
		/** Exact pending-author fact from this plan's actual body producer.
		 * Absence asks the existing pending writer to finish its K documents. */
		const TOptional<TSet<FString>>& GetPendingAuthorDocuments() const { return PendingAuthorDocuments; }
		const FString& GetPlanFingerprint() const;
		int64 GetObservedTokens() const;

	private:
		EWorkspaceKind WorkspaceKind = EWorkspaceKind::Directory;
		FString EntryFile;
		FString SnapshotId;
		FUeAgentStateRevision SnapshotRevision;
		FString SnapshotEvidenceId;
		TArray<FExportFilePlan> Files;
		TMap<FString, FString> RetainedFileHashes;
		TOptional<TSet<FString>> PendingAuthorDocuments;
		FString PlanFingerprint;
		int64 ObservedTokens = 0;
	};

	UEAGENTINTERFACE_API bool TryParseHjsonDocument(
		const FString& Text,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint = nullptr);
	/** Parse one producer-bound UTF-8 span with the same grammar. Provenance
	 * remains in the observed full document, including absolute byte positions. */
	UEAGENTINTERFACE_API bool TryParseHjsonDocumentSpan(
		const FString& Text, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint = nullptr);
	/** Same parser for a sealed formatter UTF-8 span. The caller must bind the
	 * span to that producer's actual bytes; this grants no current-file proof. */
	bool TryParseGeneratedHjsonSpan(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError,
		TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree = nullptr);
	/** Consume a generated fragment without exporting its temporary syntax.
	 * Successful metadata consumers do not need absolute line/column. The same
	 * parser resolves them only for an actual failure, preserving diagnostics. */
	bool TryConsumeGeneratedHjsonSpan(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> Consumer,
		FWorkspaceError& OutError, TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree = nullptr);
	/** The same parser on already addressed bytes, preserving absolute byte
	 * provenance. A real failure may resolve missing line/column separately. */
	struct FHjsonFragmentConsumeTimes {
		double Decode = 0, Parse = 0, Retire = 0;
	};
	bool TryConsumeGeneratedHjsonFragment(TConstArrayView<uint8> Bytes, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> Consumer,
		FWorkspaceError& OutError, TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree = nullptr,
		FHjsonFragmentConsumeTimes* Times = nullptr);
	void RelocateGeneratedHjsonFragmentError(const TArray<uint8>& PrefixBytes, const FSourceProvenance& Span,
		FWorkspaceError& OutError);
	/** Same complete grammar/diagnostics, but materialize only selected
	 * descendants. The root is retained. This is deliberately not a complete
	 * Document or syntax-cache entry. Containing nodes keep their actual source
	 * spans; omitted values are read by the original span parser on demand. */
	bool TryParseHjsonSelectedNodes(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources, FHjsonNodePtr& OutRoot, FWorkspaceError& OutError,
		TFunctionRef<bool(const FString& Pointer, const FString& Key)> IncludeSubtree,
		const FHjsonOperationCheckpoint* Checkpoint = nullptr);

	// A synchronous projection of the same grammar. Selected scalar storage is
	// borrowed from this parse's input/token owner until ConsumeRoot returns. Selection
	// omits only that scalar's AST node; syntax/duplicates are still checked.
	// ConsumeRoot runs only after complete syntax success. Its failure is final.
	struct FHjsonParsedScalar
	{
		EHjsonNodeKind Kind;
		FStringView Text;
		FStringView Lexeme;
		bool bBoolean;
		int32 Line, Column;
		int64 ByteStart, ByteEnd;
		bool bBorrowedText = false, bBorrowedLexeme = false;
	};
	// The original grammar already knows the parent and member/array ordinal.
	// This synchronous view does not require a success-only concatenated path.
	struct FHjsonScalarLocation
	{
		FStringView ParentPointer, Key;
		int32 ArrayIndex = INDEX_NONE;
	};
	bool TryConsumeHjsonScalarProjection(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		TFunctionRef<bool(const FHjsonScalarLocation&, const FHjsonParsedScalar&)> SelectScalar,
		TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> ConsumeRoot,
		FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint = nullptr);

	struct FHjsonDocumentTextChange
	{
		int64 OldBegin = 0, OldEnd = 0, NewBegin = 0, NewEnd = 0;
	};
	struct FHjsonDocumentUpdateMetrics
	{
		int64 DiffBlocks = 0, ReparsedBytes = 0, ReparsedContainers = 0, ReusedSubtrees = 0;
		bool bWholeDocument = false;
		bool bComparedText = false;
		TArray<FHjsonDocumentTextChange> Changes;
	};

	// OldText must be the exact decoded text which produced Previous. The same
	// parser owns changed containers and diagnostics. Missing backing is handled
	// by the caller's ordinary reader for this document, never another model.
	UEAGENTINTERFACE_API bool TryUpdateHjsonDocument(
		const FString& OldText, const FString& NewText, FHjsonDocumentRef Previous,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError, FHjsonDocumentUpdateMetrics* Metrics = nullptr,
		const FHjsonOperationCheckpoint* Checkpoint = nullptr);

	UEAGENTINTERFACE_API bool TryWriteHjsonDocument(
		FHjsonDocumentRef Document,
		FUeAgentResourceTracker& Resources,
		TArray<uint8>& OutCanonicalUtf8,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryProjectHjsonNodeToJsonValue(
		const FHjsonNode& Node,
		FUeAgentResourceTracker& Resources,
		TSharedPtr<FJsonValue>& OutValue,
		FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint = nullptr);

	UEAGENTINTERFACE_API bool TryLoadWorkspace(
		const FWorkspaceSource& Source,
		const FWorkspacePolicy& Policy,
		FUeAgentResourceTracker& Resources,
		FCanonicalWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);


}
