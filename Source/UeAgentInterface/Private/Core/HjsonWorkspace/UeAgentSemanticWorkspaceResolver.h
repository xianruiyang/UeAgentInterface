#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	struct FSemanticWorkspaceReadState;
	struct FDefinitionProjectionHints;
	struct FWorkspacePublication;
	struct FCommittedWorkspaceDelta;
	class FWorkspaceFileProof;
	class FSemanticProjectionBindings;
	bool HasSemanticWorkspaceReadScope();
	/** Also accepts the legacy full-reader formula only for this exact scoped resolution. */
	/** SourceLoader alone marks a scoped resolution accepted after its current
	 * namespace/leaf/bytes fence. Naming and syntax consumers gain no native or
	 * later publication proof from this handoff. */
	void ClearAcceptedSemanticWorkspace();
	void AcceptResolvedSemanticWorkspace(const FResolvedSemanticWorkspacePtr& Workspace,
		FHjsonOperationCheckpoint AcceptanceCheckpoint = {});
	/** Request-local accepted input, never a new native-state proof. */
	FResolvedSemanticWorkspacePtr GetAcceptedSemanticWorkspace(const FString& WorkspaceRoot);
	const FWorkspacePublication* FindAcceptedWorkspacePublication(const FString& WorkspaceRoot);
	/** Already parsed public documents whose accepted bytes differ from the
	 * original publication. Does not observe files, traverse hash-only unchanged
	 * documents or classify author intent; the Source owner already did that. */
	bool TryVisitAcceptedPublicDocumentByteChanges(const FString& WorkspaceRoot,
		TFunctionRef<bool(const FString&)> Visitor, FWorkspaceError& Error);
	FString GetAcceptedSemanticWorkspaceRoot();
	struct FAcceptedWorkspaceByteSpan
	{
		int64 Begin = -1;
		int64 End = -1;
		bool IsValid() const { return Begin >= 0 && End > Begin; }
		bool Equals(const FAcceptedWorkspaceByteSpan& Other) const { return Begin == Other.Begin && End == Other.End; }
	};
	/** Borrowed fields from the inventory that this resolver actually accepted.
	 * This is an output-projection hint, never a current-native or publish proof. */
	struct FInventoryRowSyntax;
	/** The digest owner outlives its inventory visit/batch. Unlike a borrowed
	 * stack functor this view may be retained until that batch is consumed.
	 * No digest, text or alternate comparison is manufactured by the view. */
	struct FAcceptedWorkspaceContentHashView
	{
		const void* Backing = nullptr;
		bool (*Compare)(const void*, const FString&) = nullptr;
		FString (*ReadCanonical)(const void*) = nullptr;
		bool IsBound() const { return Backing && Compare && ReadCanonical; }
		bool Matches(const FString& Hash) const { return Compare(Backing, Hash); }
		FString Canonical() const { return ReadCanonical(Backing); }
		static FAcceptedWorkspaceContentHashView FromCanonicalText(const FString& Text) {
			return {&Text, [](const void* Data, const FString& Hash) {
				return static_cast<const FString*>(Data)->Equals(Hash, ESearchCase::CaseSensitive);
			}, [](const void* Data) { return *static_cast<const FString*>(Data); }};
		}
	};
	struct FAcceptedWorkspaceInventoryRow
	{
		const FString& UnitId;
		ESemanticUnitKind UnitKind;
		const FString& PublicRef;
		const FString& LogicalPath;
		const FString& DocumentPath;
		const FString& UnitPointer;
		const FString& EnvelopePath;
		const TArray<FString>& ContentFiles;
		/** Expand the original compact digest only for readers which use it. */
		TFunctionRef<FString()> ReadEditHash;
		// Synchronous comparison against the accepted original canonical-row
		// SHA, without expanding the compact digest or copying an all-unit map.
		TFunctionRef<bool(const FString&)> ContentHashEquals;
		// The exact canonical spelling emitted by the existing row SHA owner.
		// Empty for a legacy/noncanonical representation; consumers fall back.
		TFunctionRef<FString()> CanonicalContentHash;
		// Offsets belong to the exact accepted inventory/envelope file hashes.
		// They grant no byte, semantic-content, or current-publication proof.
		FAcceptedWorkspaceByteSpan InventorySpan;
		FAcceptedWorkspaceByteSpan EnvelopeSpan;
		bool bGeneratedProjection = false;
		// Same immutable identity already owned by the actual unit producer.
		TSharedPtr<const FString, ESPMode::ThreadSafe> SharedUnitIdentity;
		int32 InventoryOrdinal = INDEX_NONE;
		bool bPersistedEnvelopeSpan = false;
		// Original parsed storage, not a second identity/path/digest projection.
		TSharedPtr<const FInventoryRowSyntax> SourceSyntax;
		FAcceptedWorkspaceContentHashView ContentHashView;
		// A legacy row visitor's work unit is one row. Producers exposing their
		// actual physical chunk may extend it; absent batching never grows scope.
		bool bEndReadChunk = true;
	};
	class FAcceptedWorkspaceInventory
	{
	public:
		virtual ~FAcceptedWorkspaceInventory() = default;
		virtual int32 Num() const = 0;
		/** Only the exact canonical inventory representation can be retained by
		 * a manifest-only publication. Other rows use the original row writer. */
		virtual bool Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const = 0;
		/** Physical organization from the actual accepted rows/publication, not
		 * from survival of a formatter class or its optional layout memo. */
		virtual TOptional<bool> GetScopedMachineBuckets() const { return {}; }
		virtual void VisitDocuments(TFunctionRef<void(const FString&)> Visitor) const
		{
			TSet<FString> Seen;
			Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (!Seen.Contains(Row.DocumentPath)) { Seen.Add(Row.DocumentPath); Visitor(Row.DocumentPath); }
				return true;
			});
		}
		/** Prepare this document's inventory metadata before author-file observation.
		 * This neither issues rows nor asserts that a document selection is nonempty.
		 * Unsupported producers stop here; no alternate full visitor is selected. */
		virtual bool PrepareDocumentMetadata(const FString& DocumentPath) const { return false; }
		/** Same ordered input contract; producers may isolate their pure parsing
		 * work, then return the first original-document failure after joining. */
		virtual bool PrepareDocumentsMetadata(const TArray<FString>& Paths, FString& FailedDocument) const {
			for (const auto& Path : Paths) if (!PrepareDocumentMetadata(Path)) { FailedDocument = Path; return false; }
			return true;
		}

		/** Direct views of the same formatter/model owner; absent for legacy input. */
		virtual const FWorkspaceLayoutPlan* GetGeneratedLayout() const { return nullptr; }
		virtual bool VisitDocument(const FString& Path,
			TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const { return false; }
		virtual bool VisitUnit(const FString& Id, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const {
			bool bFound = false, bAccepted = false;
			Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (Row.UnitId != Id) return true;
				bFound = true; bAccepted = Visitor(Row); return false;
			});
			return bFound && bAccepted;
		}
		virtual bool VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor,
			bool bRequireEnvelopeSpan = true) const = 0;
		virtual TOptional<FString> FindPublicRef(const FString& Ref) const {
			TOptional<FString> Found;
			Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (Row.PublicRef != Ref) return true;
				Found = Row.UnitId; return false;
			});
			return Found;
		}
		virtual void VisitHashes(TFunctionRef<void(const FString&, const FString&)> Visitor) const {}
		virtual void VisitPaths(TFunctionRef<void(const FString&, const FString&, const FString&, const TArray<FString>&)> Visitor) const {}
		virtual void VisitExpandedDocuments(TFunctionRef<void(const FString&)> Visitor) const {}
		virtual bool HasContentFileDirectory() const { return false; }
		virtual void VisitContentFiles(TFunctionRef<void(const FString&)> Visitor) const {}
		virtual FString GetInventoryPath(const FString& Document) const { return {}; }
		virtual TSharedPtr<const FDefinitionProjectionHints> GetProjectionHints() const { return nullptr; }
	};
	TSharedPtr<const FAcceptedWorkspaceInventory> GetAcceptedWorkspaceInventory(const FString& WorkspaceRoot);
	/** The actual generated-material owner whose metadata the accepted source
	 * consumed. This grants neither current native state nor publication rights. */
	TSharedPtr<const FResolvedWorkspaceUnitBindings> GetAcceptedGeneratedWorkspaceBindings(const FString& WorkspaceRoot);
	/** Existing generated shard bytes bound to the accepted source and the same
	 * producer. This is material reuse, not a current-file/publication fence. */
	TSharedPtr<const TArray<uint8>> GetAcceptedGeneratedInventoryBytes(const FString& WorkspaceRoot, const FString& RelativePath);
	/** Producer material for the next source read. The resolver must observe
	 * the current metadata and public file hashes before using any candidate;
	 * this does not accept source, native state or publication by itself.
	 * VisitRows' projection text must belong to the retained Bindings owner;
	 * envelope paths and digest text are copied during the synchronous visit. */
	void RememberGeneratedSemanticWorkspace(const FString& WorkspaceRoot, FAuthoringModelRef Model,
		TSharedRef<const FResolvedWorkspaceUnitBindings> Bindings, const FExportWorkspacePlan& Plan,
		TFunction<bool(const FString&, FAcceptedWorkspaceByteSpan&)> FindEnvelopeSpan,
		TFunctionRef<void(TFunctionRef<void(const FAcceptedWorkspaceInventoryRow&)>)> VisitRows,
		const FWorkspacePublication& Publication);
	void RetireGeneratedSemanticWorkspace(const FString& WorkspaceRoot);
	/** Explicit full/initialize establishes a continuous OS observation origin.
	 * This is never called as a local-read failure recovery. */
	bool BeginExplicitWorkspaceFileObservation(const FString& WorkspaceRoot, FWorkspaceError& Error,
		bool bInitialPublicationRootReplaced = false);
	/** Explicit complete-input producer, before the file plan is sealed. New
	 * formatter ranges are kept; preserved original bytes use the original
	 * parser once and bind their keys to accepted inventory ordinals. */
	bool PrepareExplicitWorkspaceFileVersionInputs(const FString& WorkspaceRoot, TArray<FExportFilePlan>& Files,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error);
	bool PrepareExplicitWorkspaceFileVersionInputs(const FString& WorkspaceRoot, TArray<FExportFilePlan>& Files,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error, bool bPersistEnvelopeRanges,
		const FAuthoringModel* Model = nullptr, TArray<TPair<FIntPoint, FIntPoint>>* ModelSpans = nullptr);
	/** Same-publication metadata, produced before sealing and transferred only
	 * at publication success. Neither owner retains a Model or native values. */
	bool PrepareCompleteWorkspacePublicationDirectory(const FString& WorkspaceRoot, const FAuthoringModel& Model,
		const FWorkspaceLayoutPlan& Layout, bool bScopedBuckets, FWorkspaceError& Error);
	bool PrepareChangedWorkspacePublicationDirectory(const FString& WorkspaceRoot,
		const FCommittedWorkspaceDelta& Changes, const FWorkspaceLayoutPlan* Layout, FWorkspaceError& Error,
		const TSet<FString>& PlacementOnlyRows = {});
	/** Validate the required handoff before publication becomes visible. */
	bool ValidateWorkspaceFileVersionInputs(const FString& WorkspaceRoot, const FExportWorkspacePlan& Plan,
		FWorkspaceError& Error);
	/** Final publisher hands its actual sealed file versions to the source owner.
	 * No AST, copied model, directory hash scan or native state is produced. */
	bool RememberPublishedWorkspaceFileVersions(const FString& WorkspaceRoot, const FExportWorkspacePlan& Plan,
		const FString& Generation, const FString& ConsumerState, FWorkspaceError& Error);
	/** Merge notifications newer than this request's admitted byte cut into
	 * the existing publication pending-document set; does not consume them. */
	bool TryCollectUnconsumedWorkspaceFileCandidates(const FString& WorkspaceRoot,
		TSet<FString>& InOutPendingDocuments, FWorkspaceError& Error);
	/** Synchronous naming view over the same accepted inventory/baseline. No
	 * copied all-unit hint map and no additional retained cache generation. */
	TSharedPtr<const FSemanticProjectionBindings> GetAcceptedSemanticProjectionBindings(const FString& WorkspaceRoot);
	TSharedPtr<const FSemanticProjectionBindings> GetAcceptedSemanticProjectionBindings(
		const FString& WorkspaceRoot, FAuthoringModelRef CurrentModel,
		const FCommittedWorkspaceDelta* ActualChanges = nullptr, bool bUseCurrentNamingCandidates = false);
	/** Actual accepted directory publication with no pending authored intents.
	 * This is derived from the formal Source, never formatter/lease eligibility.
	 * Full output can use its metadata without reading every old captured value. */
	bool HasAcceptedPublicationInputs(const FString& WorkspaceRoot);
	/** Complete current observation has compared every accepted row and sealed
	 * its real differences. Rebind the existing pure publication directory from
	 * those declarations and the accepted row producer, without a full layout. */
	bool PrepareObservedWorkspacePublicationOrigin(const FString& WorkspaceRoot,
		const FString& TargetKind, const FAuthoringModel& Current, const FCommittedWorkspaceDelta& Differences, FWorkspaceError& Error);
	/** Exact accepted source text digest, not evidence that the path is still current. */
	const FString* FindAcceptedSemanticSourceHash(const FString& WorkspaceRoot, const FString& RelativePath);
	/** Publisher identities/hashes already decoded by this accepted Source owner. */
	FString GetAcceptedSemanticPublisherOwner(const FString& WorkspaceRoot);
	const TMap<FString, FString>* GetAcceptedPublishedFileHashes(const FString& WorkspaceRoot);
	/** Actual raw bytes under the accepted source's current data lease. The publisher
	 * must retain its logical-path/bytes checkpoints before staging and commit. */
	bool HasAcceptedSemanticSourceByteObservation(const FString& WorkspaceRoot, const FString& RelativePath, const FString& ByteHash);
	/** Raw-byte representation of the same accepted/current observation. Never
	 * substitute the canonical text hash when retaining a differently encoded file. */
	bool TryReadAcceptedSemanticSourceByteHash(const FString& WorkspaceRoot, const FString& RelativePath,
		FString& OutHash, FWorkspaceError& Error);
	/** Retain physical encodings only for this request's actual Source inputs.
	 * Published hashes for unrelated files are not current read observations. */
	bool TryUpdateAcceptedRetainedFileHashes(const FString& WorkspaceRoot,
		TMap<FString, FString>& Retained, FWorkspaceError& Error);
	FHjsonDocumentPtr FindAcceptedSemanticSourceDocument(const FString& WorkspaceRoot, const FString& RelativePath);
	/** Read only a required accepted source document when its optional AST is
	 * absent. The same reader and exact accepted byte hash remain authoritative. */
	bool TryReadAcceptedSemanticSourceDocument(const FString& WorkspaceRoot, const FString& RelativePath,
		ContextResource::FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError);
	/** Original captured-value codec for one known envelope/declaration. It
	 * does not resolve author intent, scan membership or construct a Model. */
	bool TryRestoreCapturedSemanticUnit(const FHjsonNode& Envelope, const FSemanticUnit::FDeclarationView& Declaration,
		ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError);
	bool TryRestoreCapturedSemanticUnit(const StateSemantic::FUeAgentCanonicalValue& Envelope,
		const FSemanticUnit::FDeclarationView& Declaration, TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError);
	/** Decoded captured rows belong to this existing synchronous read scope,
	 * not the persistent generated-material owner or a previous Model. */
	const FSemanticUnit* ReadScopedCapturedSemanticUnit(const FResolvedWorkspaceUnitBindings& Owner, const FString& UnitId,
		TFunctionRef<bool(ContextResource::FUeAgentResourceTracker&, TOptional<FSemanticUnit>&, FWorkspaceError&)> Read,
		FWorkspaceError& OutError);
	const FWorkspaceError* FindScopedCapturedSemanticReadFailure(const FResolvedWorkspaceUnitBindings& Owner);
	void RecordScopedCapturedSemanticReadFailure(const FResolvedWorkspaceUnitBindings& Owner, const FWorkspaceError& Error);
	bool ReadScopedCapturedMemberNames(const FResolvedWorkspaceUnitBindings& Owner, const FString& Id, const FString& RootField,
		ContextResource::FUeAgentResourceTracker& Resources, FKeyedMemberNameVisitor Visitor,
		TFunctionRef<bool(FHjsonNodePtr&, FWorkspaceError&)> Read, FWorkspaceError& Error);
	bool TryReadCapturedMemberNameNodes(const TArray<uint8>& Utf8, const FSourceProvenance& Span, const FString& RootField,
		ContextResource::FUeAgentResourceTracker& Resources, FHjsonNodePtr& Out, FWorkspaceError& Error);
	bool TryReadPublishedCapturedMemberNames(const FString& Root, const FString& Path, const FString& Hash,
		const FString& Id, const FString& RootField, ContextResource::FUeAgentResourceTracker& Resources, FHjsonNodePtr& Out, FWorkspaceError& Error);
	bool TryRestoreCapturedSemanticMembers(const StateSemantic::FUeAgentCanonicalValue& Envelope,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error);
	bool TryReadCapturedSemanticMembers(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error);
	bool TryReadPublishedCapturedSemanticMembers(const FString& Root, const FString& Path, const FString& Hash,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error);
	/** Original file/parser reader for one required captured row. Missing raw
	 * backing reads that file only, never the complete semantic workspace. */
	bool TryReadPublishedCapturedSemanticUnit(const FString& Root, const FString& Path, const FString& ExpectedHash,
		const FSemanticUnit::FDeclarationView& Declaration, ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError);
	bool TryCapturedSpanContainsAnyString(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		TFunctionRef<bool(const FString&)> Matches, ContextResource::FUeAgentResourceTracker& Resources,
		bool& OutMatch, FWorkspaceError& Error);
	bool TryCapturedCanonicalContainsAnyString(const StateSemantic::FUeAgentCanonicalValue& Envelope,
		TFunctionRef<bool(const FString&)> Matches, bool& OutMatch, FWorkspaceError& Error);
	bool TryPublishedCapturedContainsAnyString(const FString& Root, const FString& Path, const FString& Hash,
		const FString& UnitId, TFunctionRef<bool(const FString&)> Matches, ContextResource::FUeAgentResourceTracker& Resources,
		bool& OutMatch, FWorkspaceError& Error);
	StateSemantic::FUeAgentCanonicalValuePtr FindAcceptedSemanticSourceCanonical(const FString& WorkspaceRoot, const FString& RelativePath);
	/** Reopens the current logical path and verifies actual UTF-8 bytes against
	 * the accepted input. Non-identical encodings simply do not grant file reuse. */
	bool TryReadAcceptedSemanticSourceBytes(const FString& WorkspaceRoot, const FString& RelativePath,
		TArray<uint8>& OutBytes, FString& OutHash, FWorkspaceError& Error);
	/** Required generated input from the matching accepted publication's owned
	 * file namespace. Verifies current raw bytes at that logical path; does not
	 * add guides/machine metadata to accepted author input or its checkpoint. */
	bool TryReadPublishedWorkspaceBytes(const FString& WorkspaceRoot, const FString& RelativePath,
		const FString& PublishedByteHash, TArray<uint8>& OutBytes, FWorkspaceError& Error);
	/** Recheck the accepted namespace and current leaf/bytes at a later consumer
	 * boundary without resolving or assembling the same model again. */
	bool ValidateAcceptedSemanticWorkspace(const FString& WorkspaceRoot, FUeAgentResourceTracker& Resources, FWorkspaceError& Error);
	/** One-use handoff from the successful domain-entry checkpoint to shared
	 * preparation. An unset result requires the ordinary resolver. This does not
	 * confer a new observation time or remove the later publication checkpoint. */
	TOptional<bool> TryConsumeAcceptedWorkspaceForPrepare(const FWorkspaceSource& Source,
		ESemanticWorkspaceMode Mode, FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace, FWorkspaceError& Error);
	FAuthoringModelPtr FindScopedCommittedBaseline(const FResolvedSemanticWorkspace& Workspace,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error);
	void RememberScopedCommittedBaseline(const FResolvedSemanticWorkspace& Workspace, FAuthoringModelPtr Model,
		TMap<ContextResource::EUeAgentResourceDimension, int64> Charges);
	TSharedPtr<const FDefinitionProjectionHints> GetAcceptedDefinitionProjectionHints(const FString& TargetKind = FString());
	const TMap<ContextResource::EUeAgentResourceDimension, int64>* FindAcceptedSemanticSyntaxCharges(
		const FString& SourceUnit, const FString& Utf8Hash);
	class FWorkspaceReadPathScope;
	/** Exact successful resolver inputs. One synchronous generation owns the
	 * hashes/leases; source acceptance and later publication share that record. */
	class FResolvedWorkspaceSourceProof final
	{
	public:
		struct FState;
		explicit FResolvedWorkspaceSourceProof(TSharedRef<const FState> InState);
		bool HasObservedContent(const FString& Path, const FString& TextHash) const;
		bool HasCurrentByteObservation(const FString& Path, const FString& ByteHash) const;
		bool ValidateFile(const FString& Path, void* CurrentFile, FWorkspaceError& Error) const;
		bool ValidateRemaining(const TArray<FString>& ObservedPaths, const FWorkspaceReadPathScope* PathScope,
			FWorkspaceError& Error) const;
		bool ValidateAll(FWorkspaceError& Error) const;
		bool TryUpdateRetainedFileHashes(TMap<FString, FString>& Retained, FWorkspaceError& Error) const;
		/** Add an actual on-demand reader to this same synchronous checkpoint. */
		bool TryIncludeObservedInput(const FString& Path, const FString& TextHash, FWorkspaceError& Error,
			TSharedPtr<const FWorkspaceFileProof> ActualProof = nullptr) const;
	private:
		TSharedRef<const FState> State;
	};
	/** False without a current successful scoped resolution: old callers keep
	 * their original namespace path rather than manufacturing a proof. */
	bool TryGetResolvedWorkspaceSourceProof(const FString& Root,
		TSharedPtr<const FResolvedWorkspaceSourceProof>& OutProof, FWorkspaceError& Error);
	/** Internal syntax/index proof from actual publisher readback. Semantic row
	 * reuse still requires a previous successful resolver and exact source text. */
	void RememberParsedSemanticManifest(const FString& WorkspaceRoot, const FString& ContentHash,
		const FHjsonDocument& Document, const TArray<uint8>& Utf8,
		const TMap<ContextResource::EUeAgentResourceDimension, int64>& ParseCharges);
	/** Pure encoding facts, not a file-currentness or semantic proof. The byte
	 * hash must be bound by real readback before reuse at another path/time. */
	class FWorkspaceTextEncodingProof final
	{
	public:
		bool Matches(const FString& Hash, int64 Bytes) const { return Bytes == ByteCount && ByteHash.Equals(Hash, ESearchCase::CaseSensitive); }
		int32 GetCharacters() const { return Characters; }
		const FString& GetTextHash() const { return DifferentTextHash.IsEmpty() ? ByteHash : DifferentTextHash; }
		int64 GetAllocatedSize() const { return sizeof(FWorkspaceTextEncodingProof) + 32 + ByteHash.GetAllocatedSize() + DifferentTextHash.GetAllocatedSize(); }
	private:
		FWorkspaceTextEncodingProof() = default;
		FString ByteHash, DifferentTextHash;
		int64 ByteCount = 0;
		int32 Characters = 0;
		friend TSharedRef<const FWorkspaceTextEncodingProof> ObserveWorkspaceTextEncoding(const TArray<uint8>&, const FString&);
		friend TSharedRef<const FWorkspaceTextEncodingProof> MeasureCanonicalWorkspaceTextEncoding(const TArray<uint8>&, const FString&);
	};
	TSharedRef<const FWorkspaceTextEncodingProof> ObserveWorkspaceTextEncoding(const TArray<uint8>& ActualBytes, const FString& ActualBytesHash);
	/** The formatter already produced canonical UTF-8 without a BOM/embedded
	 * terminator. Measure its UTF-16 length without decoding/re-encoding another
	 * complete text. The publisher still binds the exact actual readback bytes. */
	TSharedRef<const FWorkspaceTextEncodingProof> MeasureCanonicalWorkspaceTextEncoding(const TArray<uint8>& CanonicalUtf8, const FString& ByteHash);
	/** Optional bytes-only handoff from a real read at the final published path.
	 * The lease must have been acquired before that read. This grants no parser,
	 * semantic, namespace or publication proof, and retains no file contents. */
	bool RememberObservedSemanticSourceFile(const FString& WorkspaceRoot, const FString& AbsolutePath,
		const TArray<uint8>& ActualBytes, const FString& ActualBytesHash,
		TSharedPtr<const FWorkspaceFileProof> Proof,
	TSharedPtr<const FWorkspaceTextEncodingProof> Encoding = nullptr, bool bPublishedWrite = false);

	/** Shares immutable parses and validated resolutions within one synchronous
	 * import. Reuse rereads source text and rechecks referenced file existence;
	 * a source, mode or content change uses the complete resolver again. */
	class UEAGENTINTERFACE_API FSemanticWorkspaceReadScope final
	{
	public:
		FSemanticWorkspaceReadScope();
		~FSemanticWorkspaceReadScope();
		FSemanticWorkspaceReadScope(const FSemanticWorkspaceReadScope&) = delete;
		FSemanticWorkspaceReadScope& operator=(const FSemanticWorkspaceReadScope&) = delete;
	private:
		TUniquePtr<FSemanticWorkspaceReadState> State;
	};

	/** Bind publication to the actual source of this scope's successful resolve.
	 * The checkpoint retains hashes, not parser/model ownership. */
	UEAGENTINTERFACE_API bool TryMakeSemanticWorkspaceSourceCheckpoint(
		const FString& WorkspaceRoot, FHjsonOperationCheckpoint& OutCheckpoint, FWorkspaceError& OutError);
	/** Release the current thread's pure-data reuse generation at module shutdown. */
	UEAGENTINTERFACE_API void ResetSemanticWorkspaceReuse();

	/** Shared type/value interpretation for a workspace property or a native default template. */
	UEAGENTINTERFACE_API bool TryResolveKeyedPropertyValues(
		const FHjsonNode& Row, const FString& ExpectedType,
		const TArray<FString>& TemplateFields,
		ContextResource::FUeAgentResourceTracker& Resources,
		TMap<FString, StateSemantic::FUeAgentCanonicalValueRef>& OutValues,
		FWorkspaceError& OutError, const TMap<FString, FString>* PublicRefs = nullptr,
		FHjsonNodePtr* OutValueSources = nullptr);
	UEAGENTINTERFACE_API bool TryProjectHjsonCanonicalValue(
		const FHjsonNode& Root, ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
		bool bInterpretSemanticValues = true);

	UEAGENTINTERFACE_API bool TryResolveSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);

	/**
	 * Resolves an authoring workspace after LLM-facing managed units were edited.
	 * The machine manifest remains the immutable inventory/base binding; changed
	 * unit hashes and the resulting semantic fingerprint are recomputed from the
	 * edited units instead of requiring the caller to edit machine-owned hashes.
	 * Editable resolution requires immutable per-unit manifest envelopes; legacy
	 * full-row workspaces without envelopes must be re-exported before editing.
	 */
	UEAGENTINTERFACE_API bool TryResolveEditedSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError,
		TSharedPtr<const FResolvedWorkspaceSourceProof>* OutUnchangedImportProof = nullptr);
}
