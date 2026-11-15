#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Containers/StringConv.h"
#include "Containers/StringView.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonSyntaxCache.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticPublicReferences.h"
#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceFileProof.h"
#include "Core/HjsonWorkspace/UeAgentCapturedReferenceReadScope.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceChangeObserver.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"

#include "Containers/StringConv.h"
#include "Containers/BitArray.h"
#include "Containers/ChunkedArray.h"
#include "Containers/StaticArray.h"
#include "Algo/BinarySearch.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/PlatformMisc.h"
#include "Core/ArtifactPublisher/UeAgentArtifactGenerationFiles.h"
#include "Core/HjsonWorkspace/UeAgentHjsonValidatedHeader.h"
#endif

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;


	namespace { struct FReferenceLookupProof; using FReferenceLookupProofPtr = TSharedPtr<const FReferenceLookupProof>; }

	struct FPublishedSemanticDirectory;
	struct FSemanticWorkspaceReadState
	{
		struct FDocument
		{
			FString SourceUnit;
			FString ContentHash;
			int64 SourceCharacters = 0;
			int64 RetainedSyntaxBytes = 0;
			FHjsonDocumentPtr Parsed;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
			bool bExactParseCharges = false;
			TSharedPtr<const FString> Text;
			FHjsonDocumentPtr DiffBaseDocument;
			// Derived candidates in the existing document budget, not an author
			// consumption frontier. Pending keys are rechecked against the current
			// captured baseline; this also carries A when a later raw edit changes B.
			TMap<FString, TSet<FString>> KeyedPending;
			TMap<FString, TArray<FReferenceLookupProofPtr>> KeyedReferences;
			FString DiffBaseHash;
			TArray<FHjsonDocumentTextChange> Changes;
			bool bComparedText = false;
		};
		FSemanticWorkspaceReadState* Previous = nullptr;
		TMap<FString, FDocument> Documents;
		// Parser facts for compact inventory readers which intentionally retire
		// their AST. Request-local only; these entries never own Parsed.
		TMap<FString, FDocument> CompactSyntax;
		int32 ReusedPublicSyntax = 0;
		int32 UpdatedSyntaxDocuments = 0, WholeSyntaxDocuments = 0, MissingSyntaxText = 0;
		int64 UpdatedSyntaxBytes = 0, ReusedSyntaxSubtrees = 0, ReusedSyntaxRowDigests = 0;
		int32 ReusedFileObservations = 0, ActualFileReads = 0;
		int32 DirectHashOnlyFiles = 0;
		int64 DirectHashOnlyBytes = 0;
		double FileProofBeginSeconds = 0, FileBodyReadSeconds = 0, FileByteHashSeconds = 0;
		double FileTextEncodingSeconds = 0, FileProofEndSeconds = 0, FileAdmissionSeconds = 0;
		TMap<FString, FString> HashOnlyDocuments;
		TMap<FString, FString> ResolvedHashOnlyDocuments;
		TFunction<const FString*(const FString&)> PublishedSourceHash;
		FString PublisherOwnerId;
		const TMap<FString, FString>* PublishedFileHashes = nullptr; // Backed by PublishedSourceHash's Directory owner.
		// Only one completed resolution is retained, and only for this synchronous
		// import. Reuse requires kernel-backed current bytes or a real reread.
		FString ResolvedRoot;
		TOptional<FWorkspacePublication> ResolvedPublication;
		ESemanticWorkspaceMode ResolvedMode = ESemanticWorkspaceMode::State;
		bool bResolvedManagedEdits = false;
		FResolvedSemanticWorkspacePtr ResolvedWorkspace;
		// Compatibility for the full-reader material produced in this exact
		// synchronous resolution. Never an independently accepted source token.
		FWorkspaceError ResolvedDiagnostics;
		TMap<FString, FHjsonDocumentPtr> ResolvedDocuments;
		TSharedPtr<const FResolvedWorkspaceSourceProof> SourceProof;
		TSharedPtr<const FDefinitionProjectionHints> ResolvedProjectionHints;
		TSharedPtr<const FAcceptedWorkspaceInventory> ResolvedInventory;
		TSharedPtr<const FResolvedWorkspaceUnitBindings> GeneratedBindings;
		TFunction<FUeAgentCanonicalValuePtr(const FString&, const FString&)> GeneratedDocument;
		TFunction<bool(const FString&, FAcceptedWorkspaceByteSpan&, FWorkspaceError&)> PublishedEnvelopeSpan;
		bool bSourceAccepted = false;
		bool bPrepareHandoff = false;
		FHjsonOperationCheckpoint AcceptanceCheckpoint;
		bool bEstablishFileObservation = false;
		TSharedPtr<const FPublishedSemanticDirectory> PreparedPublicationDirectory;
		FString PreparedPublicationRoot;
		FString FileObservationRoot;
		TSet<FString> SelectedFilePaths;
		uint64 SelectedNotificationThrough = 0;
		FResolvedSemanticWorkspacePtr BaselineSource;
		FAuthoringModelPtr CommittedBaseline;
		TMap<EUeAgentResourceDimension, int64> BaselineCharges;
		struct FCapturedRows {
			TMap<FString, int32> Index;
			// UE constructs the chunk's unused slots. Optional supplies those
			// empty slots without inventing an invalid/default semantic unit.
			TChunkedArray<TOptional<FSemanticUnit>> Values;
			FWorkspaceError Error;
			TMap<FString, FHjsonNodePtr> Names;
		};
		TMap<const FResolvedWorkspaceUnitBindings*, TUniquePtr<FCapturedRows>> CapturedRows;
		struct FCapturedDocument {
			struct FEnvelopeSpan {
				int64 Begin = 0, End = 0;
				int32 Line = 0, Column = 0;
			};
			FString ByteHash;
			FWorkspaceFileProofPtr ByteProof;
			// Declaration parsing is temporary. Later value/name readers need the
			// original row location, not another owner of every metadata node.
			TMap<FString, FEnvelopeSpan> Envelopes;
			TArray<uint8> Utf8;
			TOptional<FSourceProvenance> FindOrigin(const FString& Path, const FString& Id) const {
				const auto* Span = Envelopes.Find(Id);
				if (!Span) return {};
				return FSourceProvenance(Path, TEXT("/") + EscapeJsonPointerSegment(Id),
					Span->Line, Span->Column, Span->Begin, Span->End, Id);
			}
		};
		// Synchronous reads only. This is the original envelope grammar with
		// values omitted below their top-level shape, never a complete AST cache.
		TMap<FString, FCapturedDocument> CapturedDocuments;
		int32 CapturedValueRowsRead = 0;
		int32 CapturedDeclarationRows = 0;
		int64 PublicationRowComparisons = 0;
		int32 CapturedMetadataDocuments = 0, CapturedMetadataReleased = 0;
		int32 CapturedMetadataPeak = 0;
		int32 CapturedSpanRows = 0;
		int64 CapturedSpanBytes = 0;
		int32 CapturedByteReads = 0;
		int64 CapturedLiveBytes = 0, CapturedPeakBytes = 0, CapturedRetiredBytes = 0;
		int32 CapturedStringQueries = 0, CapturedRangeReads = 0;
		int64 CapturedRangeReadBytes = 0;
		TSharedPtr<FWorkspaceReadPathScope> CapturedReferencePaths;
		FString CapturedReferenceRoot;
		int32 CapturedReferenceBatchDepth = 0;
		double CapturedReferenceRootSeconds = 0, CapturedReferenceObserveSeconds = 0;
		double CapturedReferenceProofSeconds = 0, CapturedReferenceIoSeconds = 0;
		double CapturedReferenceSyntaxSeconds = 0, CapturedReferenceStringSeconds = 0;
		double CapturedReferenceDecodeSeconds = 0, CapturedReferenceParseSeconds = 0, CapturedReferenceRetireSeconds = 0;
		double CapturedReferenceWholeSeconds = 0;
		int32 PublishedInventoryFilesRead = 0, PublishedInventoryRowsRead = 0;
		bool bPublishedInventoryParallel = false;
		int32 PublishedInventoryDocumentQueries = 0;
		int64 PublishedDeclarationRefIds = 0, MetadataAliasLookups = 0;
		int64 PublishedInventoryDocumentSlots = 0, PublishedInventoryDocumentMatches = 0;
		int64 PublishedInventoryBytesRead = 0, PublishedInventoryScalarNodesSkipped = 0;
		int64 PublishedInventoryBorrowedTokenTexts = 0, PublishedInventoryBorrowedTokenLexemes = 0;
		int64 PublishedInventoryBorrowedKeys = 0, PublishedInventorySpanNodesSkipped = 0;
		int64 InventoryFieldNodesVisited = 0, InventoryCompactDigestViews = 0, InventoryValidatedPathReuses = 0;
		double PublishedInventoryReadSeconds = 0, PublishedInventorySyntaxSeconds = 0;
		double PublishedInventoryRowsSeconds = 0, PublishedInventoryCopySeconds = 0;
		double PublishedInventoryRetireSeconds = 0;
	};
	static thread_local FSemanticWorkspaceReadState* ActiveReadState = nullptr;
	bool HasSemanticWorkspaceReadScope() { return ActiveReadState != nullptr; }
	void ClearAcceptedSemanticWorkspace()
	{
		if (ActiveReadState) { ActiveReadState->bSourceAccepted = false; ActiveReadState->bPrepareHandoff = false; }
	}
	void AcceptResolvedSemanticWorkspace(const FResolvedSemanticWorkspacePtr& Workspace,
		FHjsonOperationCheckpoint AcceptanceCheckpoint)
	{
		if (ActiveReadState) {
			ActiveReadState->bPrepareHandoff = false;
			ActiveReadState->bSourceAccepted = Workspace.IsValid() && ActiveReadState->ResolvedWorkspace == Workspace;
			ActiveReadState->AcceptanceCheckpoint = ActiveReadState->bSourceAccepted ? MoveTemp(AcceptanceCheckpoint) : FHjsonOperationCheckpoint();
		}
	}
	bool ValidateAcceptedSemanticWorkspace(const FString& WorkspaceRoot, FUeAgentResourceTracker& Resources, FWorkspaceError& Error)
	{
		if (ActiveReadState) ActiveReadState->bPrepareHandoff = false;
		if (!GetAcceptedSemanticWorkspace(WorkspaceRoot) || !ActiveReadState->AcceptanceCheckpoint) return false;
		if (!ActiveReadState->AcceptanceCheckpoint(TEXT("before_domain_prepare"), Error)) {
			ActiveReadState->bSourceAccepted = false;
			return false;
		}
		// Match the resolver's validated-resolution charge in this consumer's
		// tracker. Domains may use a fresh tracker; acceptance is not a budget bypass.
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany({{EUeAgentResourceDimension::Files,
			ActiveReadState->ResolvedDocuments.Num() + ActiveReadState->ResolvedHashOnlyDocuments.Num()}},
			TEXT("semantic_workspace:validated_resolution"), ChargeError)) {
			Error.SetFailure(ChargeError);
			return false;
		}
		Error = ActiveReadState->ResolvedDiagnostics;
		ActiveReadState->bPrepareHandoff = true;
		return true;
	}
	TOptional<bool> TryConsumeAcceptedWorkspaceForPrepare(const FWorkspaceSource& Source,
		ESemanticWorkspaceMode Mode, FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace, FWorkspaceError& Error)
	{
		if (!ActiveReadState || !ActiveReadState->bPrepareHandoff) return {};
		auto& State = *ActiveReadState;
		State.bPrepareHandoff = false;
		if (Source.GetKind() != EWorkspaceSourceKind::Directory || !State.bResolvedManagedEdits
			|| Mode != State.ResolvedMode || !GetAcceptedSemanticWorkspace(Source.GetPath())) return {};
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany({{EUeAgentResourceDimension::Files,
			State.ResolvedDocuments.Num() + State.ResolvedHashOnlyDocuments.Num()}},
			TEXT("semantic_workspace:validated_resolution"), ChargeError)) {
			Error.SetFailure(ChargeError); return false;
		}
		OutWorkspace = State.ResolvedWorkspace;
		Error = State.ResolvedDiagnostics;
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=accepted_source_prepare_handoff files=%d current_reopens=0"),
			State.ResolvedDocuments.Num() + State.ResolvedHashOnlyDocuments.Num());
		return true;
	}
	FAuthoringModelPtr FindScopedCommittedBaseline(const FResolvedSemanticWorkspace& Workspace,
		FUeAgentResourceTracker& Resources, FWorkspaceError& Error)
	{
		if (!ActiveReadState || ActiveReadState->ResolvedWorkspace.Get() != &Workspace
			|| ActiveReadState->BaselineSource.Get() != &Workspace || !ActiveReadState->CommittedBaseline) return nullptr;
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany(ActiveReadState->BaselineCharges, TEXT("semantic_layout.compile"), ChargeError)) {
			Error.SetFailure(ChargeError);
			return nullptr;
		}
		return ActiveReadState->CommittedBaseline;
	}
	void RememberScopedCommittedBaseline(const FResolvedSemanticWorkspace& Workspace, FAuthoringModelPtr Model,
		TMap<EUeAgentResourceDimension, int64> Charges)
	{
		if (!ActiveReadState || ActiveReadState->ResolvedWorkspace.Get() != &Workspace) return;
		ActiveReadState->BaselineSource = ActiveReadState->ResolvedWorkspace;
		ActiveReadState->CommittedBaseline = MoveTemp(Model);
		ActiveReadState->BaselineCharges = MoveTemp(Charges);
	}
	TSharedPtr<const FDefinitionProjectionHints> GetAcceptedDefinitionProjectionHints(const FString& TargetKind)
	{
		if (!ActiveReadState || !ActiveReadState->bSourceAccepted || !ActiveReadState->ResolvedProjectionHints) return nullptr;
		if (!TargetKind.IsEmpty()) {
			const auto& Model = ActiveReadState->ResolvedWorkspace->GetModel();
			for (const auto& Target : Model->GetTargetUnitIds()) {
				const auto* Unit = Model->FindUnit(Target);
				if (!Unit || !Unit->GetSemanticTypeRef().Equals(TargetKind, ESearchCase::CaseSensitive)) return nullptr;
			}
		}
		return ActiveReadState->ResolvedProjectionHints;
	}
	FResolvedSemanticWorkspacePtr GetAcceptedSemanticWorkspace(const FString& WorkspaceRoot)
	{
		if (!ActiveReadState || !ActiveReadState->bSourceAccepted) return nullptr;
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		return Root.Equals(ActiveReadState->ResolvedRoot, ESearchCase::IgnoreCase)
			? ActiveReadState->ResolvedWorkspace : nullptr;
	}
	TSharedPtr<const FAcceptedWorkspaceInventory> GetAcceptedWorkspaceInventory(const FString& WorkspaceRoot)
	{
		return GetAcceptedSemanticWorkspace(WorkspaceRoot)
			? ActiveReadState->ResolvedInventory : nullptr;
	}
	bool HasAcceptedPublicationInputs(const FString& WorkspaceRoot)
	{
		const auto Source = GetAcceptedSemanticWorkspace(WorkspaceRoot);
		return Source && Source->GetEditedUnitIds().IsSet() && Source->GetEditedUnitIds()->IsEmpty()
			&& Source->GetStructuralEdits().IsEmpty() && ActiveReadState->ResolvedInventory
			&& ActiveReadState->ResolvedPublication.IsSet() && ActiveReadState->ResolvedPublication->bDirectDataflow;
	}
	const FWorkspacePublication* FindAcceptedWorkspacePublication(const FString& WorkspaceRoot)
	{
		if (!GetAcceptedSemanticWorkspace(WorkspaceRoot)) return nullptr;
		return ActiveReadState->ResolvedPublication.IsSet() ? &ActiveReadState->ResolvedPublication.GetValue() : nullptr;
	}
	bool TryVisitAcceptedPublicDocumentByteChanges(const FString& WorkspaceRoot,
		TFunctionRef<bool(const FString&)> Visitor, FWorkspaceError& Error)
	{
		if (!GetAcceptedSemanticWorkspace(WorkspaceRoot) || !ActiveReadState->PublishedFileHashes)
			return FailWorkspaceDataflowHandoff(Error, TEXT("source.public_document_bytes"),
				TEXT("accepted Source and publisher hashes"), WorkspaceRoot, TEXT("missing"),
				TEXT("same-request source observation"), TEXT("formal Source resolution"));
		const FString Prefix = ActiveReadState->ResolvedRoot + TEXT("/");
		for (const auto& Document : ActiveReadState->ResolvedDocuments) {
			if (!Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
			const FString Path = Document.Key.Mid(Prefix.Len());
			if (Path.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive)) continue;
			const auto* Published = ActiveReadState->PublishedFileHashes->Find(Path);
			if (!Published) continue;
			const auto* Read = ActiveReadState->Documents.Find(Document.Key);
			if (!Read || Read->Parsed != Document.Value)
				return FailWorkspaceDataflowHandoff(Error, TEXT("source.public_document_bytes"), Path,
					WorkspaceRoot, TEXT("accepted document version is not the original read"),
					TEXT("same-version bytes and syntax"), TEXT("formal Source ReadDocument"));
			if (!Read->ContentHash.Equals(*Published, ESearchCase::CaseSensitive) && !Visitor(Path)) return false;
		}
		return true;
	}
	TSharedPtr<const FResolvedWorkspaceUnitBindings> GetAcceptedGeneratedWorkspaceBindings(const FString& WorkspaceRoot)
	{
		return GetAcceptedSemanticWorkspace(WorkspaceRoot) && ActiveReadState->ResolvedInventory
			? ActiveReadState->GeneratedBindings : nullptr;
	}
	TSharedPtr<const FSemanticProjectionBindings> GetAcceptedSemanticProjectionBindings(const FString& WorkspaceRoot)
	{
		const auto Source = GetAcceptedSemanticWorkspace(WorkspaceRoot);
		const auto Inventory = GetAcceptedWorkspaceInventory(WorkspaceRoot);
		if (!Source || !Inventory) return nullptr;
		if (const auto Generated = GetAcceptedGeneratedWorkspaceBindings(WorkspaceRoot)) {
			if (const auto* Projection = Generated->GetGeneratedProjection()) {
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=accepted_projection_view source=generated_owner units=%d expanded_edit_hashes=0"),
					Projection->GetModel().GetUnitCount());
				return TSharedPtr<const FSemanticProjectionBindings>(Generated, Projection);
			}
		}
		class FAcceptedBindings final : public FSemanticProjectionBindings
		{
			FResolvedSemanticWorkspacePtr Source;
			TSharedPtr<const FAcceptedWorkspaceInventory> Inventory;
		public:
			FAcceptedBindings(FResolvedSemanticWorkspacePtr InSource, TSharedPtr<const FAcceptedWorkspaceInventory> InInventory)
				: Source(MoveTemp(InSource)), Inventory(MoveTemp(InInventory)) {}
			const FAuthoringModel& GetModel() const override { return Source->GetModel().Get(); }
			bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)> Visitor) const override
			{
				return Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
					const auto* Before = Source->FindBaselineUnit(Row.UnitId);
					return !Before || Visitor(*Before, Row.PublicRef, Row.LogicalPath, Row.DocumentPath);
				});
			}
		};
		return MakeShared<FAcceptedBindings>(Source, Inventory);
	}
	static bool FindPublishedReference(const FString& Root, const FString& Id, FString& Out);
	static bool IsPublishedHandleReserved(const FString& Root, const FString& Handle,
		const TSet<FString>& Excluded, bool& Out, FWorkspaceError& Error);
	static bool VisitPublishedReferences(const FString& Root, const TSet<FString>& Ids,
		TFunctionRef<bool(const FString&, const FString&)> Visitor, FWorkspaceError& Error);

	TSharedPtr<const FSemanticProjectionBindings> GetAcceptedSemanticProjectionBindings(
		const FString& WorkspaceRoot, FAuthoringModelRef CurrentModel, const FCommittedWorkspaceDelta* ActualChanges,
		bool bUseCurrentNamingCandidates)
	{
		if (CurrentModel->IsOutputScope()) {
			const auto Source = GetAcceptedSemanticWorkspace(WorkspaceRoot);
			const auto Inventory = GetAcceptedWorkspaceInventory(WorkspaceRoot);
			if (!Source || !Inventory) return nullptr;
			class FOutputNames final : public FSemanticProjectionBindings {
				FAuthoringModelRef Current;
				FResolvedSemanticWorkspaceRef Source;
				TSharedRef<const FAcceptedWorkspaceInventory> Inventory;
				TSet<FString> Added;
				TSet<FString> ReferencedIds;
				FString Root;
				TSet<FString> ExcludedHandles;
				bool bCurrentNamingCandidates;
			public:
				FOutputNames(FAuthoringModelRef InCurrent, FResolvedSemanticWorkspaceRef InSource,
					TSharedRef<const FAcceptedWorkspaceInventory> InInventory, const FString& InRoot, const FCommittedWorkspaceDelta* Changes,
					bool bInCurrentNamingCandidates)
					: Current(MoveTemp(InCurrent)), Source(MoveTemp(InSource)), Inventory(MoveTemp(InInventory)), Root(InRoot),
					bCurrentNamingCandidates(bInCurrentNamingCandidates) {
					if (Changes) for (const auto& Change : Changes->Changes) if (!Change.Before && Change.After) Added.Add(Change.UnitId);
					if (bCurrentNamingCandidates && Changes) for (const auto& Change : Changes->Changes)
						if (Change.Before && !Change.After) {
							FString Ref; int32 Colon = INDEX_NONE;
							if (FindPublishedReference(Root, Change.UnitId, Ref) && Ref.FindChar(TEXT(':'), Colon))
								ExcludedHandles.Add(Ref.Mid(Colon + 1).ToLower());
						}
					for (const auto& Declaration : Current->GetDeclarations()) {
						FString Ref; int32 Colon = INDEX_NONE;
						if (FindPublishedReference(Root, Declaration.GetUnitId(), Ref) && Ref.FindChar(TEXT(':'), Colon))
							ExcludedHandles.Add(Ref.Mid(Colon + 1).ToLower());
						// Declared relation edges already identify semantic units. Do
						// not discover refs by walking arbitrary strings/object paths.
						for (const auto& Id : Declaration.GetRelationRefs())
							if (Current->FindUnitIndex(Id) == INDEX_NONE) ReferencedIds.Add(Id);
						if (Declaration.GetUnitKind() != ESemanticUnitKind::OrderingState
							|| Declaration.GetRole() != TEXT("stable_item_order")
							|| Declaration.GetSemanticTypeRef() != TEXT("uai.authoring.ordering/1")) continue;
						const auto* Unit = Current->FindUnit(Declaration.GetUnitId());
						const auto* Items = Unit ? Unit->FindValue(TEXT("item_refs")) : nullptr;
						if (Items && (*Items)->GetKind() == EUeAgentValueKind::Array)
							for (const auto& Item : (*Items)->GetArrayItems()) if (Item->GetKind() == EUeAgentValueKind::String
								&& Current->FindUnitIndex(Item->GetText()) == INDEX_NONE) ReferencedIds.Add(Item->GetText());
					}
				}
				const FAuthoringModel& GetModel() const override { return Current.Get(); }
				bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)> Visitor) const override {
					for (const auto& Declaration : Current->GetDeclarations()) {
						const auto& Id = Declaration.GetUnitId();
						if (Added.Contains(Id)) continue;
						bool bAccepted = true;
						Inventory->VisitUnit(Id, [&](const FAcceptedWorkspaceInventoryRow& Row) {
							// Explicit complete observation uses the same current-unit
							// naming candidates as FPublishedNames, not native Before.
							const auto* Before = bCurrentNamingCandidates ? Current->FindUnit(Id) : Source->FindBaselineUnit(Id);
							bAccepted = !Before || Visitor(*Before, Row.PublicRef, Row.LogicalPath, Row.DocumentPath); return bAccepted;
						});
						if (!bAccepted) return false;
					}
					return true;
				}
				bool IsReservedHandle(const FString& Handle, bool& Out, FWorkspaceError& Error) const override {
					return IsPublishedHandleReserved(Root, Handle, ExcludedHandles, Out, Error);
				}
				bool VisitReferencedPublicReferences(TFunctionRef<bool(const FString&, const FString&)> Visitor, FWorkspaceError& Error) const override {
					return VisitPublishedReferences(Root, ReferencedIds, Visitor, Error);
				}
			};
			return MakeShared<FOutputNames>(MoveTemp(CurrentModel), Source.ToSharedRef(), Inventory.ToSharedRef(), WorkspaceRoot, ActualChanges, bUseCurrentNamingCandidates);
		}
		if (!HasAcceptedPublicationInputs(WorkspaceRoot)) return GetAcceptedSemanticProjectionBindings(WorkspaceRoot);
		const auto Inventory = ActiveReadState->ResolvedInventory;
		const auto Source = GetAcceptedSemanticWorkspace(WorkspaceRoot);
		if (!Source || !Inventory) return nullptr;
		const auto& Scope = Source->GetModel();
		if (Scope->GetContractId() != CurrentModel->GetContractId()
			|| Scope->GetContractVersion() != CurrentModel->GetContractVersion() || Scope->GetProfileId() != CurrentModel->GetProfileId()
			|| Scope->GetTargetUnitIds() != CurrentModel->GetTargetUnitIds()) return nullptr;
		class FPublishedNames final : public FSemanticProjectionBindings {
			FAuthoringModelRef Current;
			FResolvedSemanticWorkspaceRef Source;
			TSharedRef<const FAcceptedWorkspaceInventory> Inventory;
		public:
			FPublishedNames(FAuthoringModelRef InCurrent, FResolvedSemanticWorkspaceRef InSource, TSharedRef<const FAcceptedWorkspaceInventory> InInventory)
				: Current(MoveTemp(InCurrent)), Source(MoveTemp(InSource)), Inventory(MoveTemp(InInventory)) {}
			const FAuthoringModel& GetModel() const override { return Source->GetModel().Get(); }
			bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)> Visitor) const override {
				return Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
					const auto* Unit = Current->FindUnit(Row.UnitId);
					// Old metadata supplies a naming candidate only. The original
					// allocator validates its grammar beside this current unit.
					return !Unit || Unit->GetUnitKind() != Row.UnitKind
						|| Visitor(*Unit, Row.PublicRef, Row.LogicalPath, Row.DocumentPath);
				});
			}
		};
		return MakeShared<FPublishedNames>(MoveTemp(CurrentModel), GetAcceptedSemanticWorkspace(WorkspaceRoot).ToSharedRef(), Inventory.ToSharedRef());
	}
	FString GetAcceptedSemanticWorkspaceRoot()
	{
		return ActiveReadState && ActiveReadState->bSourceAccepted ? ActiveReadState->ResolvedRoot : FString();
	}
	const FString* FindAcceptedSemanticSourceHash(const FString& WorkspaceRoot, const FString& RelativePath)
	{
		if (!GetAcceptedSemanticWorkspace(WorkspaceRoot)) return nullptr;
		const FString Path = FPaths::Combine(ActiveReadState->ResolvedRoot, RelativePath);
		if (const auto* Hash = ActiveReadState->ResolvedHashOnlyDocuments.Find(Path)) return Hash;
		const auto* Document = ActiveReadState->ResolvedDocuments.Find(Path);
		const auto* Read = ActiveReadState->Documents.Find(Path);
		if (Document && Read && *Document == Read->Parsed) return &Read->ContentHash;
		return ActiveReadState->PublishedSourceHash ? ActiveReadState->PublishedSourceHash(RelativePath) : nullptr;
	}
	FString GetAcceptedSemanticPublisherOwner(const FString& WorkspaceRoot)
	{
		return GetAcceptedSemanticWorkspace(WorkspaceRoot) ? ActiveReadState->PublisherOwnerId : FString();
	}
	const TMap<FString, FString>* GetAcceptedPublishedFileHashes(const FString& WorkspaceRoot)
	{
		return GetAcceptedSemanticWorkspace(WorkspaceRoot) ? ActiveReadState->PublishedFileHashes : nullptr;
	}
	FHjsonDocumentPtr FindAcceptedSemanticSourceDocument(const FString& WorkspaceRoot, const FString& RelativePath)
	{
		if (!GetAcceptedSemanticWorkspace(WorkspaceRoot)) return nullptr;
		const auto* Document = ActiveReadState->ResolvedDocuments.Find(FPaths::Combine(ActiveReadState->ResolvedRoot, RelativePath));
		return Document ? *Document : nullptr;
	}
	FUeAgentCanonicalValuePtr FindAcceptedSemanticSourceCanonical(const FString& WorkspaceRoot, const FString& RelativePath)
	{
		const auto* Hash = FindAcceptedSemanticSourceHash(WorkspaceRoot, RelativePath);
		return Hash && ActiveReadState->GeneratedDocument ? ActiveReadState->GeneratedDocument(RelativePath, *Hash) : nullptr;
	}
	const TMap<EUeAgentResourceDimension, int64>* FindAcceptedSemanticSyntaxCharges(
		const FString& SourceUnit, const FString& Utf8Hash)
	{
		if (!ActiveReadState || !ActiveReadState->bSourceAccepted) return nullptr;
		const FString Path = FPaths::Combine(ActiveReadState->ResolvedRoot, SourceUnit);
		const auto* Document = ActiveReadState->Documents.Find(Path);
		const auto* Resolved = ActiveReadState->ResolvedDocuments.Find(Path);
		// The hash describes the exact UTF-8 encoding of the text parsed by the
		// resolver. It grants syntax/charges only, never current file contents.
		if (Document && Resolved && Document->Parsed == *Resolved && Document->bExactParseCharges
			&& Document->SourceUnit.Equals(SourceUnit, ESearchCase::CaseSensitive)
			&& Document->ContentHash.Equals(Utf8Hash, ESearchCase::CaseSensitive)) return &Document->ParseCharges;
		const auto* Compact = ActiveReadState->CompactSyntax.Find(Path);
		const auto* ResolvedHash = ActiveReadState->ResolvedHashOnlyDocuments.Find(Path);
		return Compact && ResolvedHash && Compact->bExactParseCharges
			&& Compact->SourceUnit.Equals(SourceUnit, ESearchCase::CaseSensitive)
			&& Compact->ContentHash.Equals(*ResolvedHash, ESearchCase::CaseSensitive)
			&& Compact->ContentHash.Equals(Utf8Hash, ESearchCase::CaseSensitive)
			? &Compact->ParseCharges : nullptr;
	}
	using FCompletedDocumentRead = FSemanticWorkspaceReadState::FDocument;
	// One successful resolver's public documents. Private baseline envelopes
	// keep their existing bounded typed/row proofs instead of retaining a second AST.
	static thread_local TMap<FString, FCompletedDocumentRead> PreviousDocumentReads;
	struct FObservedSourceFile
	{
		FString ContentHash;
		int64 SourceCharacters = 0;
		FWorkspaceFileProofPtr Proof;
		// Canonical UTF-8 shares the existing digest rather than copying it.
		FString DifferentByteHash;
		bool bByteHashObserved = false;
	// Lifetime only. A final write awaits its real watcher event (MAX_uint64),
	// then its acknowledgement. Neither value establishes file identity or bytes.
	uint64 KeepUntilNotification = 0;
		const FString& GetByteHash() const { return DifferentByteHash.IsEmpty() ? ContentHash : DifferentByteHash; }
	};
	// Source observations contain only digest/size and the bounded kernel lease.
	// They do not retain source text, additional ASTs or another model generation.
	static thread_local FString ObservedSourceRoot;
	static thread_local TMap<FString, FObservedSourceFile> ObservedSourceFiles;
static void StoreObservedSourceFile(const FString& Path, FObservedSourceFile File, bool bPublishedWrite = false)
{
	if (bPublishedWrite) File.KeepUntilNotification = MAX_uint64;
	else if (const auto* Old = ObservedSourceFiles.Find(Path); Old && Old->Proof && Old->Proof == File.Proof)
		File.KeepUntilNotification = Old->KeepUntilNotification;
	ObservedSourceFiles.Add(Path, MoveTemp(File));
}
	struct FPublishedSourceFileVersion
	{
		FString Hash;
		FString ComparisonHash;
		// Only current comparison bytes, directly shared from the real formatter.
		// This is not an AST or a strong reference to its model/layout producer.
		TSharedPtr<const TArray<uint8>> ComparisonBytes;
		TSharedPtr<const TArray<FIntPoint>> FragmentSpans;
		TSharedPtr<const FExportFilePlan::FFragmentIdentities> FragmentIdentities;
	bool bSemanticSourceDocument = false;
	};
// A public candidate still needs an actual inventory row or declared content_ref.
// Guides and machine records have their own readers and cannot own edit units.
static bool IsPublicSemanticFileCandidate(const FString& Path)
{
	return Path.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase) && !Path.StartsWith(TEXT("."))
		&& !Path.StartsWith(TEXT("examples/"), ESearchCase::CaseSensitive);
}
	static thread_local int64 PublishedComparisonBytes = 0;
	static constexpr int64 SourceVersionRetentionBudget = 512LL * 1024 * 1024;
	/** The actual published directory, with no Model, unit values, native
	 * bindings, AST or producer closure. Original compiler/placement/member
	 * storage is shared. Flat current-row changes never retain a generation
	 * chain or construct another complete identity index. */
	struct FPublishedSemanticDirectory
	{
		using FIdentity = TSharedRef<const FString, ESPMode::ThreadSafe>;
		struct FChangedRow {
			FSemanticUnit::FDeclarationView Declaration;
			FString Document, PublicRef;
		};
		TSharedRef<const TArray<FSemanticUnit::FDeclarationView>> OriginalDeclarations;
		TSharedRef<const FCompiledAuthoringStructure> OriginalIndex;
		FWorkspacePlacementView OriginalDocuments, OriginalPublicRefs;
		TMap<FString, FChangedRow> ChangedRows;
		TSet<FString> RemovedOriginalRows;
		TSharedPtr<const TSet<FString>> OriginalHandles;
		FDefinitionProjectionHints DefinitionHints;
		TSet<FString> AddedHandles, RemovedOriginalHandles;
		int32 UnitCount;
		bool bScopedBuckets;

		FPublishedSemanticDirectory(const FAuthoringModel& Model, const FWorkspaceLayoutPlan& Layout, bool bScoped)
			: OriginalDeclarations(Model.GetSharedDeclarations()), OriginalIndex(Model.GetSharedUnitIndex()),
			OriginalDocuments(Layout.GetPhysicalPlacements()), OriginalPublicRefs(Layout.GetMachineManifest().PublicRefMapping),
				DefinitionHints(Layout.GetDefinitionProjectionHints()), UnitCount(Model.GetUnitCount()), bScopedBuckets(bScoped) {}
		FPublishedSemanticDirectory(const FAuthoringModel& Current,
			TSharedRef<const FWorkspacePlacementStorage> Placements, const FDefinitionProjectionHints& Names,
			TSharedRef<const TSet<FString>> Handles, int32 Count, bool bScoped)
			: OriginalDeclarations(Current.GetSharedDeclarations()), OriginalIndex(Current.GetSharedUnitIndex()),
			OriginalDocuments(Placements, true), OriginalPublicRefs(Placements, FWorkspacePlacementView::EColumn::PublicRef),
			OriginalHandles(Handles), DefinitionHints(Names), UnitCount(Count), bScopedBuckets(bScoped) {}

		TOptional<FSemanticUnit::FDeclarationView> FindDeclaration(const FString& Id) const {
			if (RemovedOriginalRows.Contains(Id)) return {};
			if (const auto* Changed = ChangedRows.Find(Id)) return Changed->Declaration;
			const int32 Index = FindCompiledUnitIndex(OriginalIndex.Get(), Id);
			return OriginalDeclarations->IsValidIndex(Index)
				? TOptional<FSemanticUnit::FDeclarationView>((*OriginalDeclarations)[Index]) : TOptional<FSemanticUnit::FDeclarationView>();
		}
		TOptional<FString> FindDocument(const FString& Id) const {
			if (RemovedOriginalRows.Contains(Id)) return {};
			if (const auto* Changed = ChangedRows.Find(Id)) return Changed->Document;
			const auto* Path = OriginalDocuments.Find(Id); return Path ? TOptional<FString>(*Path) : TOptional<FString>();
		}
		TOptional<FString> FindPublicRef(const FString& Id) const {
			if (RemovedOriginalRows.Contains(Id)) return {};
			if (const auto* Changed = ChangedRows.Find(Id)) return Changed->PublicRef;
			const auto* Ref = OriginalPublicRefs.FindReference(Id); return Ref ? TOptional<FString>(*Ref) : TOptional<FString>();
		}
		TOptional<FString> FindUnitByPublicRef(const FString& Ref) const {
			for (const auto& Definition : DefinitionHints.Definitions) if (Definition.Value.PublicRef == Ref) return Definition.Key;
			for (const auto& Changed : ChangedRows) if (Changed.Value.PublicRef == Ref) return Changed.Key;
			// An explicit cross-unit public reference needs an identity lookup.
			// Use the allocator's existing pure name rows, never inventory text
			// or unit values, and do not build another global inverse index.
			for (const auto& Before : OriginalPublicRefs) if (!RemovedOriginalRows.Contains(Before.Key)
				&& !ChangedRows.Contains(Before.Key) && Before.Value == Ref) return Before.Key;
			return {};
		}
		static FString Handle(const FString& Ref) {
			int32 Colon = INDEX_NONE; return Ref.FindChar(TEXT(':'), Colon) ? Ref.Mid(Colon + 1).ToLower() : FString();
		}
		bool ContainsHandle(const FString& Value) const {
			const FString Key = Value.ToLower();
			return AddedHandles.Contains(Key) || (!RemovedOriginalHandles.Contains(Key) && OriginalHandles && OriginalHandles->Contains(Key));
		}
		void RemoveHandle(const FString& Ref) {
			const FString Key = Handle(Ref); if (Key.IsEmpty()) return;
			AddedHandles.Remove(Key); if (OriginalHandles && OriginalHandles->Contains(Key)) RemovedOriginalHandles.Add(Key);
		}
		void AddHandle(const FString& Ref) {
			const FString Key = Handle(Ref); if (Key.IsEmpty()) return;
			RemovedOriginalHandles.Remove(Key); if (!OriginalHandles || !OriginalHandles->Contains(Key)) AddedHandles.Add(Key);
		}
		bool VisitDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&, int32)> Visitor) const {
			int32 Position = 0;
			for (const auto& Before : *OriginalDeclarations) {
				if (RemovedOriginalRows.Contains(Before.GetUnitId())) continue;
				const auto* Changed = ChangedRows.Find(Before.GetUnitId());
				if (!Visitor(Changed ? Changed->Declaration : Before, Position++)) return false;
			}
			for (const auto& Changed : ChangedRows) if (FindCompiledUnitIndex(OriginalIndex.Get(), Changed.Key) == INDEX_NONE
				&& !Visitor(Changed.Value.Declaration, Position++)) return false;
			return true;
		}
	};
	struct FWorkspaceSourceObservation
	{
		FileChanges::FObserver Observer;
		FString Generation;
		TSharedPtr<const FPublishedSemanticDirectory> PublishedDirectory;
		TMap<FString, FPublishedSourceFileVersion> Files;
		TSet<FString> PendingDocuments;
		TSet<FString> PublishedCreatedDirectories;
		bool bPublishedOrigin = false;
	uint64 AcknowledgedNotifications = 0;
		int64 RetainedBytes = 0;
		~FWorkspaceSourceObservation() { PublishedComparisonBytes -= RetainedBytes; }
	};
	static thread_local TMap<FString, TSharedPtr<FWorkspaceSourceObservation>> WorkspaceSourceObservations;
static bool AcknowledgeSourceNotifications(FWorkspaceSourceObservation& Session, uint64 Through, FileChanges::FError& Error)
{
	if (!Session.Observer.Acknowledge(Through, Error)) return false;
	Session.AcknowledgedNotifications = FMath::Max(Session.AcknowledgedNotifications, Through);
	return true;
}
	static FString NormalizeObservationRoot(const FString& Input)
	{
		FString Root = FPaths::ConvertRelativePathToFull(Input); FPaths::NormalizeDirectoryName(Root); return Root;
	}
	static TSharedPtr<const FPublishedSemanticDirectory> FindPublicationDirectory(const FString& Root)
	{
		const auto Session = WorkspaceSourceObservations.FindRef(NormalizeObservationRoot(Root));
		return Session ? Session->PublishedDirectory : nullptr;
	}
	bool PrepareObservedWorkspacePublicationOrigin(const FString& WorkspaceRoot,
		const FString& TargetKind, const FAuthoringModel& Current, const FCommittedWorkspaceDelta& Differences, FWorkspaceError& Error)
	{
		const double Started = FPlatformTime::Seconds();
		const FString Root = NormalizeObservationRoot(WorkspaceRoot);
		const auto Session = WorkspaceSourceObservations.FindRef(Root);
		const auto Inventory = GetAcceptedWorkspaceInventory(Root);
		const auto* Publication = FindAcceptedWorkspacePublication(Root);
		const auto Names = GetAcceptedDefinitionProjectionHints(TargetKind);
		if (!Session || !Inventory || !Publication || !Publication->ScopedMachineBuckets.IsSet())
			return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_origin"),
				TEXT("accepted row directory, publication and explicit observation owner"), Root, TEXT("missing"),
				TEXT("complete observation's same-generation input"), TEXT("formal Source / publication entry"));
		if (Session->PublishedDirectory && Session->Generation == Publication->Generation) return true;
		if (!Names) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_origin"),
			TEXT("definition naming facts"), Root, TEXT("missing"), TEXT("accepted publication names"), TEXT("Source definition binding owner"));
		auto Placements = MakeShared<FWorkspacePlacementStorage>();
		Placements->UnitIndex = Current.GetSharedUnitIndex(); Placements->Locations.SetNumZeroed(Current.GetUnitCount());
		TMap<FString, TSharedRef<FWorkspacePlacementDocument>> Documents;
		auto Handles = MakeShared<TSet<FString>>();
		TMap<FString, const FCommittedUnitChange*> Changed;
		TMap<FString, FPublishedSemanticDirectory::FChangedRow> BeforeRows;
		struct FMachineVersion {
			FString Hash;
			TSharedPtr<TArray<FIntPoint>> Spans;
			TSharedPtr<FExportFilePlan::FFragmentIdentities> Identities;
			bool bInherited = false;
		};
		TMap<FString, FMachineVersion> MachineVersions;
		for (const auto& Change : Differences.Changes) Changed.Add(Change.UnitId, &Change);
		int32 AcceptedRows = 0;
		if (!Inventory->Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
			++AcceptedRows;
		Session->Files.FindOrAdd(Row.DocumentPath).bSemanticSourceDocument = true;
		for (const FString& Content : Row.ContentFiles) Session->Files.FindOrAdd(Content).bSemanticSourceDocument = true;
			const auto* Change = Changed.Find(Row.UnitId);
			if (Change && (*Change)->Before)
				BeforeRows.Add(Row.UnitId, {(*Change)->Before->GetDeclaration(), Row.DocumentPath, Row.PublicRef});
			const FString Handle = FPublishedSemanticDirectory::Handle(Row.PublicRef); if (!Handle.IsEmpty()) Handles->Add(Handle);
			const int32 Index = Current.FindUnitIndex(Row.UnitId);
			auto* Version = MachineVersions.Find(Row.EnvelopePath);
			if (!Version) {
				const auto* Expected = ActiveReadState->PublishedFileHashes ? ActiveReadState->PublishedFileHashes->Find(Row.EnvelopePath) : nullptr;
				if (!Expected) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_spans"), Row.EnvelopePath,
					Root, TEXT("missing accepted baseline byte hash"), TEXT("same-generation owner bytes"), TEXT("Source publication owner"));
				FMachineVersion New; New.Hash = *Expected;
				const auto* Existing = Session->Files.Find(Row.EnvelopePath);
				New.bInherited = Existing && Session->Generation == Publication->Generation && Existing->Hash == *Expected
					&& Existing->FragmentSpans && Existing->FragmentIdentities;
				if (!New.bInherited) { New.Spans = MakeShared<TArray<FIntPoint>>(); New.Identities = MakeShared<FExportFilePlan::FFragmentIdentities>(); }
				Version = &MachineVersions.Add(Row.EnvelopePath, MoveTemp(New));
			}
			if (!Version->bInherited) {
				if (!Row.EnvelopeSpan.IsValid() || Row.EnvelopeSpan.End > MAX_int32 || Row.InventoryOrdinal != Version->Spans->Num())
					return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_spans"), Row.UnitId,
						Row.EnvelopePath, TEXT("missing persisted byte span or physical ordinal"),
						TEXT("same-row envelope_begin/end and ordinal"), TEXT("original inventory row formatter / Source parser"));
				TSharedPtr<const FString, ESPMode::ThreadSafe> Identity;
				if (Index != INDEX_NONE) Identity = Current.GetDeclarations()[Index].GetSharedUnitId();
				else if (Change && (*Change)->Before) Identity = (*Change)->Before->GetSharedUnitId();
				if (!Identity) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_spans"), Row.UnitId, Root,
					TEXT("no actual immutable unit identity"), TEXT("current or removed Before identity"), TEXT("complete observation comparison"));
				Version->Spans->Add(FIntPoint(static_cast<int32>(Row.EnvelopeSpan.Begin), static_cast<int32>(Row.EnvelopeSpan.End)));
				Version->Identities->Add(Identity.ToSharedRef());
			}
			if (Index == INDEX_NONE) {
				if (Change && (*Change)->Before) return true; // Deleted origin rows are held by their real Before below.
				return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_origin"), Row.UnitId, Root,
					TEXT("accepted identity has neither current declaration nor a real removed Before"),
					TEXT("complete observation's original difference"), TEXT("observed row comparison"));
			}
			auto* Document = Documents.Find(Row.DocumentPath);
			if (!Document) Document = &Documents.Add(Row.DocumentPath, MakeShared<FWorkspacePlacementDocument>(MakeShared<const FString>(Row.DocumentPath)));
			const auto& Declaration = Current.GetDeclarations()[Index];
			Placements->Locations[Index] = {&Document->Get(), (*Document)->Rows.Num()};
			(*Document)->Rows.Add({Declaration.GetSharedUnitId(), Row.LogicalPath, FString(), false, Row.PublicRef}); return true;
		})) return false;
		for (const auto& Document : Documents) Placements->Files.Add(Document.Key, FWorkspaceFileMembers(Document.Value));
		int32 ProducedVersionRows = 0;
		for (auto& Pair : MachineVersions) if (!Pair.Value.bInherited) {
			auto& Version = Session->Files.FindOrAdd(Pair.Key);
			const int64 Before = (Version.FragmentSpans ? Version.FragmentSpans->GetAllocatedSize() : 0)
				+ (Version.FragmentIdentities ? Version.FragmentIdentities->GetAllocatedSize() : 0);
			const int64 After = Pair.Value.Spans->GetAllocatedSize() + Pair.Value.Identities->GetAllocatedSize();
			if (After > SourceVersionRetentionBudget - PublishedComparisonBytes + Before)
				return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_spans"), Pair.Key, Root,
					TEXT("accepted row coordinates exceed the existing Source version owner capacity"),
					TEXT("original byte span owner"), TEXT("explicit publication input"));
			ProducedVersionRows += Pair.Value.Spans->Num();
			Version.Hash = Pair.Value.Hash; Version.FragmentSpans = MoveTemp(Pair.Value.Spans); Version.FragmentIdentities = MoveTemp(Pair.Value.Identities);
			PublishedComparisonBytes += After - Before; Session->RetainedBytes += After - Before;
		}
		auto Origin = MakeShared<FPublishedSemanticDirectory>(Current, Placements, *Names, Handles,
			AcceptedRows, Publication->ScopedMachineBuckets.GetValue());
		Origin->ChangedRows = MoveTemp(BeforeRows);
		for (const auto& Change : Differences.Changes) {
			if (!Change.Before) { if (Change.After) Origin->RemovedOriginalRows.Add(Change.UnitId); continue; }
			if (!Origin->ChangedRows.Contains(Change.UnitId)) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.observed_origin"), Change.UnitId, Root,
				TEXT("real Before has no accepted row"), TEXT("accepted destination address"), TEXT("original inventory producer"));
		}
		Session->PublishedDirectory = MoveTemp(Origin);
		Session->Generation = Publication->Generation; // Accepted origin only; bPublishedOrigin/through are not advanced here.
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=observed_publication_origin accepted_rows=%d documents=%d span_buckets=%d produced_span_rows=%d current_index_reused=true layout_planned=false baseline_values_decoded=0 model_retained=false unit_values_retained=false native_retained=false ast_retained=false total_s=%.6f"),
			AcceptedRows, Documents.Num(), MachineVersions.Num(), ProducedVersionRows, FPlatformTime::Seconds() - Started);
		return true;
	}
	static bool FindPublishedReference(const FString& Root, const FString& Id, FString& Out)
	{
		const auto Directory = FindPublicationDirectory(Root);
		const auto Ref = Directory ? Directory->FindPublicRef(Id) : TOptional<FString>();
		if (!Ref) return false;
		Out = Ref.GetValue(); return true;
	}
	static bool IsPublishedHandleReserved(const FString& Root, const FString& Handle,
		const TSet<FString>& Excluded, bool& Out, FWorkspaceError& Error)
	{
		const auto Directory = FindPublicationDirectory(Root);
		if (!Directory) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.naming"),
			TEXT("same-generation published handle ownership"), Root, TEXT("missing"),
			TEXT("value-free publication directory"), TEXT("original layout allocator / final publisher"));
		Out = !Excluded.Contains(Handle.ToLower()) && Directory->ContainsHandle(Handle); return true;
	}
	static bool VisitPublishedReferences(const FString& Root, const TSet<FString>& Ids,
		TFunctionRef<bool(const FString&, const FString&)> Visitor, FWorkspaceError& Error)
	{
		for (const auto& Id : Ids) {
			FString Ref;
			if (!FindPublishedReference(Root, Id, Ref)) return FailWorkspaceDataflowHandoff(Error,
				TEXT("publication.reference"), Id, Root, TEXT("missing published public_ref"),
				TEXT("same-generation referenced identity"), TEXT("original layout allocator / final publisher"));
			if (!Visitor(Id, Ref)) return false;
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=scoped_output_reference_bindings requested=%d published=%d member_values_read=0 namespace_text_rows=0"), Ids.Num(), Ids.Num());
		return true;
	}
	bool PrepareCompleteWorkspacePublicationDirectory(const FString& WorkspaceRoot, const FAuthoringModel& Model,
		const FWorkspaceLayoutPlan& Layout, bool bScopedBuckets, FWorkspaceError& Error)
	{
		// A pure plan-only caller owns no Source session. The final publisher
		// separately requires the real read scope and prepared directory.
		if (!ActiveReadState) return true;
		const auto Namespace = Layout.GetSharedProjection()->ReferenceNamespace;
		if (Model.IsOutputScope() || !Namespace || !Namespace->Handles)
			return FailWorkspaceDataflowHandoff(Error, TEXT("publication.complete_directory"),
				TEXT("completed declarations/placements/allocator handles"), WorkspaceRoot, TEXT("producer handoff absent"),
				TEXT("explicit complete-input formatter result"), TEXT("composition / original layout allocator"));
		auto Next = MakeShared<FPublishedSemanticDirectory>(Model, Layout, bScopedBuckets);
		Next->OriginalHandles = Namespace->Handles;
		ActiveReadState->PreparedPublicationDirectory = MoveTemp(Next);
		ActiveReadState->PreparedPublicationRoot = WorkspaceRoot.IsEmpty() ? ActiveReadState->FileObservationRoot : NormalizeObservationRoot(WorkspaceRoot);
		return true;
	}
	bool PrepareChangedWorkspacePublicationDirectory(const FString& WorkspaceRoot,
	const FCommittedWorkspaceDelta& Changes, const FWorkspaceLayoutPlan* Layout, FWorkspaceError& Error,
	const TSet<FString>& PlacementOnlyRows)
	{
		const auto Before = FindPublicationDirectory(WorkspaceRoot);
		if (!ActiveReadState || !Before) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.changed_directory"),
			TEXT("accepted destination publication directory"), WorkspaceRoot, TEXT("missing"),
			TEXT("same-generation value-free declarations and names"), TEXT("original complete publication / previous local publication"));
		// The property dispatcher already proved unchanged declaration/type and
		// membership. Values and newly formatted spans change, this directory does not.
		ActiveReadState->PreparedPublicationRoot = NormalizeObservationRoot(WorkspaceRoot);
		if (!Layout) { ActiveReadState->PreparedPublicationDirectory = Before; return true; }
		auto Next = MakeShared<FPublishedSemanticDirectory>(*Before);
		for (const auto& Change : Changes.Changes) {
			const auto Previous = Next->FindDeclaration(Change.UnitId);
			const auto PreviousRef = Next->FindPublicRef(Change.UnitId);
			if (PreviousRef) Next->RemoveHandle(PreviousRef.GetValue());
			if (!Change.After) {
				if (Previous) --Next->UnitCount;
				Next->ChangedRows.Remove(Change.UnitId);
				Next->DefinitionHints.Definitions.Remove(Change.UnitId);
				if (FindCompiledUnitIndex(Next->OriginalIndex.Get(), Change.UnitId) != INDEX_NONE) Next->RemovedOriginalRows.Add(Change.UnitId);
				continue;
			}
			const auto* Path = Layout->GetPhysicalPlacements().Find(Change.UnitId);
			const auto* Ref = Layout->GetMachineManifest().PublicRefMapping.FindReference(Change.UnitId);
			if (!Path || !Ref) return FailWorkspaceDataflowHandoff(Error, TEXT("publication.changed_directory"),
				TEXT("actual row placement/public_ref: ") + Change.UnitId, WorkspaceRoot, TEXT("missing scoped formatter output"),
				TEXT("same-request row placement"), TEXT("original scoped layout allocator"));
			if (!Previous) ++Next->UnitCount;
			Next->RemovedOriginalRows.Remove(Change.UnitId);
			Next->ChangedRows.Add(Change.UnitId, {Change.After->GetDeclaration(), *Path, *Ref});
			Next->AddHandle(*Ref);
		}
	// A surviving member may move when another member enters or leaves its
	// physical directory. Its formatter changed the address, not the declaration.
	for (const FString& Id : PlacementOnlyRows) {
		const auto Declaration = Before->FindDeclaration(Id);
		const auto PreviousPath = Before->FindDocument(Id);
		const auto PreviousRef = Before->FindPublicRef(Id);
		const auto* Path = Layout->GetPhysicalPlacements().Find(Id);
		const auto* Ref = Layout->GetMachineManifest().PublicRefMapping.FindReference(Id);
		if (!Declaration || !PreviousPath || !PreviousRef || !Path || !Ref)
			return FailWorkspaceDataflowHandoff(Error, TEXT("publication.changed_directory"), Id, WorkspaceRoot,
				TEXT("missing accepted row or final formatter address"), TEXT("surviving written row placement"),
				TEXT("accepted publication directory and scoped layout allocator"));
		if (*PreviousPath == *Path && *PreviousRef == *Ref) continue;
		if (*PreviousRef != *Ref) {
			Next->RemoveHandle(*PreviousRef);
			Next->AddHandle(*Ref);
		}
		Next->ChangedRows.Add(Id, {Declaration.GetValue(), *Path, *Ref});
	}
		for (const auto& Binding : Layout->GetDefinitionProjectionHints().Definitions) Next->DefinitionHints.Definitions.Add(Binding.Key, Binding.Value);
		ActiveReadState->PreparedPublicationDirectory = MoveTemp(Next);
		return true;
	}
	static bool FailFileObservation(FWorkspaceError& Error, const FString& Root, const FileChanges::FError& Native)
	{
		Error.SetSourceFailure(TEXT("workspace_file_observation_unavailable"), FString::Printf(
			TEXT("OS file selection stopped at %s (code=%d, Win32=%u, path=%s). A continuous observation origin must be explicitly established; no directory/hash/alternate reader was selected."),
			Native.Stage.c_str(), static_cast<int32>(Native.Code), Native.NativeCode, Native.Path.c_str()), Root);
		return false;
	}
	bool BeginExplicitWorkspaceFileObservation(const FString& WorkspaceRoot, FWorkspaceError& Error, bool bInitialPublicationRootReplaced)
	{
		const FString Root = NormalizeObservationRoot(WorkspaceRoot);
		if (!ActiveReadState) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_scope_missing"), TEXT("Explicit observation establishment requires the formal source read scope."), Root); return false;
		}
		if (bInitialPublicationRootReplaced && (!ActiveReadState->bEstablishFileObservation || ActiveReadState->FileObservationRoot != Root)) {
			Error.SetSourceFailure(TEXT("workspace_initial_observation_scope_missing"), TEXT("Only the explicit initial directory publication may bind its newly published root before final byte verification."), Root); return false;
		}
		ActiveReadState->bEstablishFileObservation = true; ActiveReadState->FileObservationRoot = Root;
		if (bInitialPublicationRootReplaced) WorkspaceSourceObservations.Remove(Root);
		if (const auto Existing = WorkspaceSourceObservations.FindRef(Root)) {
			FileChanges::FSnapshot Current;
			if (Existing->Observer.Snapshot(Current)) return true;
			// This call is the user's explicit full/initialize action, never a
			// missing-cache branch in import. Retire only this lost session.
			WorkspaceSourceObservations.Remove(Root);
		}
		if (!IFileManager::Get().MakeDirectory(*Root, true)) {
			Error.SetSourceFailure(TEXT("workspace_observation_directory_failed"), TEXT("Could not create/open the explicitly initialized observation directory."), Root); return false;
		}
		auto Next = MakeShared<FWorkspaceSourceObservation>(); FileChanges::FError Native;
		if (!Next->Observer.Start(std::wstring(*Root, Root.Len()), Native)) return FailFileObservation(Error, Root, Native);
	// The replacement observer cannot acknowledge events from an older root.
	if (ObservedSourceRoot == Root)
		for (auto& Read : ObservedSourceFiles) Read.Value.KeepUntilNotification = 0;
		WorkspaceSourceObservations.Add(Root, MoveTemp(Next));
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=explicit_establish root=%s source=Windows notifications=true directory_hash_scan=false"), *Root);
		return true;
	}
	namespace
	{
		bool ParseCapturedEnvelopeDirectory(const FString& Text, const FString& Path,
			FSemanticWorkspaceReadState::FCapturedDocument& Read, FUeAgentResourceTracker& Resources, FWorkspaceError& Error,
			bool bCaptureLineColumns = true);

		FString Sha256(const TArray<uint8>& Bytes);
		bool BuildExplicitPublicationSpanFiles(TArray<FExportFilePlan>& Files, const FAcceptedWorkspaceInventory& Inventory,
			TFunctionRef<const FString*(const FString&)> AcceptedHash, FUeAgentResourceTracker& Resources, FWorkspaceError& Error,
			int32* OutParsedFiles = nullptr, bool bPersistEnvelopeRanges = false,
			const FAuthoringModel* Model = nullptr, TArray<TPair<FIntPoint, FIntPoint>>* ModelSpans = nullptr)
		{
			const double Started = FPlatformTime::Seconds();
			TMap<FString, TArray<FIntPoint>> Established;
			TMap<FString, TMap<int32, FPublishedSemanticDirectory::FIdentity>> OrdinalIdentities;
			TSet<FString> PreservedInventories, UpgradeInventoryFiles;
			int64 ParsedBytes = 0; int32 ParsedFiles = 0, BoundRows = 0, PassedEnvelopeFiles = 0;
			if (OutParsedFiles) *OutParsedFiles = 0;
			for (const auto& File : Files) {
				const FString& Path = File.GetRelativePath();
				const bool bInventory = Path.StartsWith(TEXT(".uai/inventory/"));
				if (!bInventory && !Path.StartsWith(TEXT(".uai/baselines/"))) continue;
				const bool bPrepared = File.GetGeneratedFragmentSpans() && File.GetGeneratedFragmentIdentities();
				const auto* Expected = AcceptedHash(Path);
				// Newly formatted records already carry their own baseline ranges.
				// Only preserved original metadata can require the explicit upgrade.
				if (bPrepared && (!bPersistEnvelopeRanges || !bInventory || !Expected || *Expected != File.GetContentHash())) continue;
				if (!Expected || *Expected != File.GetContentHash()) {
					Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_bytes_unbound"),
						TEXT("Stage=explicit_file_versions; missing same-byte accepted machine file. Producer=original machine formatter or accepted publication bytes; no alternate reader selected."), Path); return false;
				}
				OrdinalIdentities.FindOrAdd(Path.Replace(TEXT("/inventory/"), TEXT("/baselines/")));
				if (bInventory) {
					PreservedInventories.Add(Path);
					if (bPersistEnvelopeRanges) Established.FindOrAdd(Path.Replace(TEXT("/inventory/"), TEXT("/baselines/")));
					}
				if (!File.GetGeneratedFragmentSpans()) Established.Add(Path);
			}
			if (OrdinalIdentities.IsEmpty()) return true;
			const auto Store = [&](const FString& Path, int32 Ordinal, FAcceptedWorkspaceByteSpan Span) {
				auto* Ranges = Established.Find(Path); if (!Ranges) return true;
				if (Ordinal < 0 || !Span.IsValid() || Span.End > MAX_int32) {
					Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_input_missing"),
						TEXT("Stage=explicit_file_versions; accepted row has no valid original range/ordinal. Producer=accepted inventory and original machine-file parser."), Path); return false;
					}
				if (Ranges->Num() <= Ordinal) Ranges->SetNumZeroed(Ordinal + 1);
				(*Ranges)[Ordinal] = FIntPoint(static_cast<int32>(Span.Begin), static_cast<int32>(Span.End));
				return true;
				};
			if (!Inventory.Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
				const FString Shard = Row.EnvelopePath.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
				auto* Identities = OrdinalIdentities.Find(Row.EnvelopePath); if (!Identities) return true;
				if (Row.InventoryOrdinal < 0 || Identities->Contains(Row.InventoryOrdinal)) {
					Error.SetSourceFailure(TEXT("workspace_publication_row_identity_invalid"), TEXT("The accepted physical row has no unique original ordinal."), Row.UnitId); return false;
				}
				Identities->Add(Row.InventoryOrdinal, Row.SharedUnitIdentity ? Row.SharedUnitIdentity.ToSharedRef()
					: MakeShared<const FString, ESPMode::ThreadSafe>(Row.UnitId));
				if (bPersistEnvelopeRanges && PreservedInventories.Contains(Shard) && !Row.bPersistedEnvelopeSpan) UpgradeInventoryFiles.Add(Shard);
				if (Row.EnvelopeSpan.IsValid() && !Store(Row.EnvelopePath, Row.InventoryOrdinal, Row.EnvelopeSpan)) return false;
				++BoundRows;
				return Store(Shard, Row.InventoryOrdinal, Row.InventorySpan);
				})) return false;
			for (const auto& File : Files) {
				const FString& Path = File.GetRelativePath();
				auto* Ranges = Established.Find(Path);
				if (!Ranges || !Path.StartsWith(TEXT(".uai/baselines/"))) continue;
				const auto& Identities = OrdinalIdentities.FindChecked(Path);
				if (const auto Existing = File.GetGeneratedFragmentSpans()) *Ranges = *Existing;
				if (Ranges->Num() == Identities.Num() && !Ranges->ContainsByPredicate([](FIntPoint Span) { return Span.X < 0 || Span.Y <= Span.X; })) {
					++PassedEnvelopeFiles; continue;
					}
				// This explicit complete-input producer has no prior ranges for
				// these accepted bytes. Its only consumer needs byte boundaries,
				// never per-row line/column or a retained batch of envelope maps.
				FSemanticWorkspaceReadState::FCapturedDocument Read;
				const auto& Bytes = File.GetCanonicalUtf8();
				const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
				if (!ParseCapturedEnvelopeDirectory(FString(Text.Length(), Text.Get()), Path, Read, Resources, Error, false)) return false;
				ParsedBytes += Bytes.Num(); ++ParsedFiles; if (OutParsedFiles) *OutParsedFiles = ParsedFiles;
				if (Read.Envelopes.Num() != Identities.Num()) {
					Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_coverage_missing"),
						TEXT("Stage=explicit_file_versions; the original machine-file rows do not match the accepted inventory membership."), Path); return false;
					}
				for (const auto& Identity : Identities) {
					const auto* Origin = Read.Envelopes.Find(*Identity.Value);
					if (!Origin) {
						Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_unit_missing"),
							TEXT("Stage=explicit_file_versions; accepted unit is absent from the preserved machine bytes. No new identity or range was invented."), *Identity.Value); return false;
						}
					if (!Store(Path, Identity.Key, {Origin->Begin, Origin->End})) return false;
					}
				}
			TMap<FString, TSharedPtr<const FExportFilePlan::FFragmentIdentities>> BoundIdentities;
			for (const auto& Bucket : OrdinalIdentities) {
				auto Identities = MakeShared<FExportFilePlan::FFragmentIdentities>(); Identities->Reserve(Bucket.Value.Num());
				for (int32 Ordinal = 0; Ordinal < Bucket.Value.Num(); ++Ordinal) {
					const auto* Id = Bucket.Value.Find(Ordinal);
					if (!Id) { Error.SetSourceFailure(TEXT("workspace_publication_row_identity_missing"), TEXT("The accepted row ordinals are not contiguous."), Bucket.Key); return false; }
					Identities->Add(*Id);
				}
				BoundIdentities.Add(Bucket.Key, MoveTemp(Identities));
			}
			int64 UpgradeByteDelta = 0; int32 UpgradedRows = 0;
			for (auto& File : Files) {
				const FString Path = File.GetRelativePath();
				if (!UpgradeInventoryFiles.Contains(Path)) continue;
				const FString EnvelopePath = Path.Replace(TEXT("/inventory/"), TEXT("/baselines/"));
				const auto& Identities = BoundIdentities.FindChecked(EnvelopePath);
				TArray<FUeAgentCanonicalValuePtr> Metadata; Metadata.SetNum(Identities->Num());
				if (!Inventory.VisitBucket(EnvelopePath, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (!Metadata.IsValidIndex(Row.InventoryOrdinal) || *(*Identities)[Row.InventoryOrdinal] != Row.UnitId) {
						Error.SetSourceFailure(TEXT("workspace_publication_row_identity_invalid"), TEXT("Explicit inventory upgrade requires the original row identity and ordinal."), Row.UnitId); return false;
						}
					return TryBuildPublishedInventoryMetadata(Row, Resources, Metadata[Row.InventoryOrdinal], Error);
					}, false)) return false;
				TArray<FUeAgentCanonicalValueRef> Rows; Rows.Reserve(Metadata.Num());
				for (const auto& Row : Metadata) {
					if (!Row) { Error.SetSourceFailure(TEXT("workspace_publication_row_identity_missing"), TEXT("Explicit inventory upgrade did not receive every original ordinal."), Path); return false; }
					Rows.Add(Row.ToSharedRef());
					}
				TArray<uint8> Bytes; TArray<FIntPoint> Spans;
				if (!TrySerializePublishedInventoryWithEnvelopeSpans(Rows, Established.FindChecked(EnvelopePath), Resources, Bytes, Spans, Error, Path)) return false;
				UpgradeByteDelta += Bytes.Num() - File.GetCanonicalUtf8().Num(); UpgradedRows += Rows.Num();
				const FString Hash = Sha256(Bytes);
				File = FExportFilePlan(Path, MoveTemp(Bytes), Hash, MoveTemp(Spans), Identities);
				Established.Remove(Path);
				}
			for (auto& File : Files) if (const auto* Identities = BoundIdentities.Find(File.GetRelativePath().Replace(TEXT("/inventory/"), TEXT("/baselines/")))) {
				auto* Ranges = Established.Find(File.GetRelativePath());
				const auto Existing = File.GetGeneratedFragmentSpans();
				const auto* ActualRanges = Ranges ? Ranges : Existing.Get();
				if (!ActualRanges || ActualRanges->Num() != (*Identities)->Num()) {
					Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_coverage_missing"),
						TEXT("Stage=explicit_file_versions; preserved machine ranges do not cover the accepted row ordinals. Producer must supply this file's complete version input."), File.GetRelativePath()); return false;
					}
				for (int32 I = 0; I < ActualRanges->Num(); ++I) {
					const auto Span = (*ActualRanges)[I];
					if (Span.X < 0 || Span.Y <= Span.X || Span.Y > File.GetCanonicalUtf8().Num()) {
						Error.SetSourceFailure(TEXT("workspace_explicit_publication_span_coverage_missing"), TEXT("The actual machine row range is outside its own file bytes."), File.GetRelativePath()); return false;
						}
					if (Model && ModelSpans) {
						const int32 Index = Model->FindUnitIndex(*(*(*Identities))[I]);
						if (ModelSpans->IsValidIndex(Index)) {
							if (File.GetRelativePath().StartsWith(TEXT(".uai/inventory/"))) (*ModelSpans)[Index].Key = Span;
							else (*ModelSpans)[Index].Value = Span;
							}
						}
					}
				if (Ranges) File = File.WithGeneratedFragmentSpans(MoveTemp(*Ranges), *Identities);
				else File = File.WithGeneratedFragmentIdentities(*Identities);
				}
			UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=explicit_file_version_inputs parsed_files=%d parsed_bytes=%lld bound_rows=%d machine_files=%d passed_envelope_files=%d produced_line_columns=0 envelope_maps=one_file body_copies=0 native_capture=false total_s=%.6f"),
				ParsedFiles, ParsedBytes, BoundRows, Established.Num(), PassedEnvelopeFiles, FPlatformTime::Seconds() - Started);
			if (bPersistEnvelopeRanges) UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=explicit_inventory_range_upgrade files=%d rows=%d persistent_byte_delta=%lld separate_index=false"),
				UpgradeInventoryFiles.Num(), UpgradedRows, UpgradeByteDelta);
			return true;
		}
	}
	bool PrepareExplicitWorkspaceFileVersionInputs(const FString& WorkspaceRoot, TArray<FExportFilePlan>& Files,
		FUeAgentResourceTracker& Resources, FWorkspaceError& Error)
	{
		return PrepareExplicitWorkspaceFileVersionInputs(WorkspaceRoot, Files, Resources, Error, false);
	}
	bool PrepareExplicitWorkspaceFileVersionInputs(const FString& WorkspaceRoot, TArray<FExportFilePlan>& Files,
		FUeAgentResourceTracker& Resources, FWorkspaceError& Error, bool bPersistEnvelopeRanges,
		const FAuthoringModel* Model, TArray<TPair<FIntPoint, FIntPoint>>* ModelSpans)
	{
		if (!ActiveReadState || !ActiveReadState->bEstablishFileObservation) return true;
		const bool bNeedsOriginalRanges = Files.ContainsByPredicate([](const FExportFilePlan& File) {
			return (!File.GetGeneratedFragmentSpans() || !File.GetGeneratedFragmentIdentities()) && (File.GetRelativePath().StartsWith(TEXT(".uai/baselines/"))
				|| File.GetRelativePath().StartsWith(TEXT(".uai/inventory/"))); });
		const bool bNeedsPreservedUpgrade = bPersistEnvelopeRanges && Files.ContainsByPredicate([&](const FExportFilePlan& File) {
			if (!File.GetRelativePath().StartsWith(TEXT(".uai/inventory/"))) return false;
			const auto* Accepted = FindAcceptedSemanticSourceHash(WorkspaceRoot, File.GetRelativePath());
			return Accepted && *Accepted == File.GetContentHash(); });
		if (!bNeedsOriginalRanges && !bNeedsPreservedUpgrade) return true;
		const auto Inventory = GetAcceptedWorkspaceInventory(WorkspaceRoot);
		if (!Inventory) {
			Error.SetSourceFailure(TEXT("workspace_explicit_publication_inventory_missing"),
				TEXT("Stage=explicit_file_versions; preserved machine bytes require their accepted inventory producer before plan sealing."), WorkspaceRoot); return false;
			}
		return BuildExplicitPublicationSpanFiles(Files, *Inventory,
			[&](const FString& Path) { return FindAcceptedSemanticSourceHash(WorkspaceRoot, Path); }, Resources, Error,
			nullptr, bPersistEnvelopeRanges, Model, ModelSpans);
	}
	bool ValidateWorkspaceFileVersionInputs(const FString& WorkspaceRoot, const FExportWorkspacePlan& Plan, FWorkspaceError& Error)
	{
		const FString Root = NormalizeObservationRoot(WorkspaceRoot);
		if (!WorkspaceSourceObservations.Contains(Root)) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_origin_missing"),
				TEXT("Stage=publication.file_versions; missing explicit Windows observation owner before publication."), Root); return false;
			}
		const auto Session = WorkspaceSourceObservations.FindRef(Root);
		if (ActiveReadState && ActiveReadState->PreparedPublicationDirectory && ActiveReadState->PreparedPublicationRoot != Root)
			return FailWorkspaceDataflowHandoff(Error, TEXT("publication.directory"), TEXT("matching destination workspace"), Root,
				ActiveReadState->PreparedPublicationRoot, Root, TEXT("composition publication producer"));
		if (!ActiveReadState || (!ActiveReadState->PreparedPublicationDirectory && !Session->PublishedDirectory))
			return FailWorkspaceDataflowHandoff(Error, TEXT("publication.directory"),
				TEXT("same-generation declarations/placements/names"), Root, TEXT("missing"),
				TEXT("sealed publication metadata"), TEXT("complete or changed-row publication producer"));
		for (const auto& File : Plan.GetFiles()) if ((File.GetRelativePath().StartsWith(TEXT(".uai/baselines/")) || File.GetRelativePath().StartsWith(TEXT(".uai/inventory/")))
			&& (!File.GetGeneratedFragmentSpans() || !File.GetGeneratedFragmentIdentities()
				|| File.GetGeneratedFragmentSpans()->Num() != File.GetGeneratedFragmentIdentities()->Num())) {
			Error.SetSourceFailure(TEXT("workspace_publication_spans_missing"),
				TEXT("Stage=publication.file_versions; machine file has no matching row identities/ranges bound to its plan bytes. Producer=original row formatter or explicit complete-input version preparation; no parser recovery selected."), File.GetRelativePath()); return false;
			}
		for (const auto& File : Plan.GetRetainedFileHashes()) {
			const auto* Version = Session->Files.Find(File.Key);
		if (Version && Version->bSemanticSourceDocument) {
				// A real raw comparison version can precede newer author spelling.
				// Neither this hash nor the visible file proves author consumption.
				if (!Version || !Version->ComparisonBytes || Version->ComparisonHash.IsEmpty())
					return FailWorkspaceDataflowHandoff(Error, TEXT("publication.file_versions"), File.Key, Root,
						TEXT("retained public document has no exact old raw input"), TEXT("same observer's comparison bytes"),
						TEXT("explicit Source byte observation / actual public formatter"));
			} else if (File.Key.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive)) {
				if (!Version || Version->Hash != File.Value || !Version->FragmentSpans || !Version->FragmentIdentities
					|| Version->FragmentSpans->Num() != Version->FragmentIdentities->Num())
					return FailWorkspaceDataflowHandoff(Error, TEXT("publication.file_versions"), File.Key, Root,
						TEXT("retained baseline has no matching row identities/spans"), TEXT("accepted same-byte persisted row coordinates"),
						TEXT("original inventory parser / observed origin producer"));
			}
		}
		return true;
	}
	bool RememberPublishedWorkspaceFileVersions(const FString& WorkspaceRoot, const FExportWorkspacePlan& Plan,
		const FString& Generation, const FString& ConsumerState, FWorkspaceError& Error)
	{
		const FString Root = NormalizeObservationRoot(WorkspaceRoot);
		const auto Session = WorkspaceSourceObservations.FindRef(Root);
		if (!Session) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_origin_missing"), TEXT("The publication has no explicitly established Windows observation owner."), Root); return false;
		}
		FWorkspacePublication Publication; Publication.bDirectDataflow = true; Publication.Generation = Generation;
		TSet<FString> Pending;
		if (!TryReadPublishedPendingAuthorDocuments(ConsumerState, Publication, Pending)) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_pending_missing"), TEXT("The final publisher did not provide its same-generation pending document set."), Root); return false;
		}
		TSet<FString> Included;
		for (const auto& File : Plan.GetRetainedFileHashes()) {
			Included.Add(File.Key);
			// Preserve a real previous comparison version; this registers only a
			// missing expected-name record, never new author-consumption evidence.
			if (!Session->Files.Contains(File.Key)) Session->Files.Add(File.Key, {File.Value});
		}
		for (const auto& File : Plan.GetFiles()) {
			const FString& Path = File.GetRelativePath(); Included.Add(Path);
			if (!Session->Files.Contains(Path)) {
				FString Parent = FPaths::GetPath(Path);
				while (!Parent.IsEmpty()) { Session->PublishedCreatedDirectories.Add(Parent); Parent = FPaths::GetPath(Parent); }
			}
			auto& Version = Session->Files.FindOrAdd(Path); Version.Hash = File.GetContentHash();
		Version.bSemanticSourceDocument = File.IsSemanticSourceDocument();
			const int64 OldSpanBytes = (Version.FragmentSpans ? Version.FragmentSpans->GetAllocatedSize() : 0)
				+ (Version.FragmentIdentities ? Version.FragmentIdentities->GetAllocatedSize() : 0);
			auto Spans = File.GetGeneratedFragmentSpans();
			auto Identities = File.GetGeneratedFragmentIdentities();
			const int64 NewSpanBytes = (Spans ? Spans->GetAllocatedSize() : 0) + (Identities ? Identities->GetAllocatedSize() : 0);
			if (NewSpanBytes > SourceVersionRetentionBudget - PublishedComparisonBytes + OldSpanBytes) {
				Error.SetSourceFailure(TEXT("workspace_publication_spans_not_retained"),
					TEXT("The formatter's required publication spans exceed the existing Source ownership budget. The publication requires an explicit smaller supported organization; no parser recovery was selected."), Path); return false;
			}
			PublishedComparisonBytes += NewSpanBytes - OldSpanBytes; Session->RetainedBytes += NewSpanBytes - OldSpanBytes;
			Version.FragmentSpans = MoveTemp(Spans);
			Version.FragmentIdentities = MoveTemp(Identities);
		if (Version.bSemanticSourceDocument
				&& (!Pending.Contains(Path) || !Version.ComparisonBytes)) {
				const int64 Before = Version.ComparisonBytes ? Version.ComparisonBytes->GetAllocatedSize() : 0;
				auto Bytes = File.GetCanonicalUtf8Identity().Pin(); const int64 After = Bytes ? Bytes->GetAllocatedSize() : 0;
				if (After > SourceVersionRetentionBudget - PublishedComparisonBytes + Before) Bytes.Reset();
				const int64 Retained = Bytes ? After : 0;
				PublishedComparisonBytes += Retained - Before; Session->RetainedBytes += Retained - Before;
				Version.ComparisonBytes = MoveTemp(Bytes); Version.ComparisonHash = Version.ComparisonBytes ? Version.Hash : FString();
			}
		}
		for (auto It = Session->Files.CreateIterator(); It; ++It) if (!Included.Contains(It.Key())) {
			const int64 Bytes = (It.Value().ComparisonBytes ? It.Value().ComparisonBytes->GetAllocatedSize() : 0)
				+ (It.Value().FragmentSpans ? It.Value().FragmentSpans->GetAllocatedSize() : 0)
				+ (It.Value().FragmentIdentities ? It.Value().FragmentIdentities->GetAllocatedSize() : 0);
			PublishedComparisonBytes -= Bytes; Session->RetainedBytes -= Bytes; It.RemoveCurrent();
		}
		if (ActiveReadState && ActiveReadState->PreparedPublicationDirectory) Session->PublishedDirectory = ActiveReadState->PreparedPublicationDirectory;
		Session->Generation = Generation; Session->PendingDocuments = MoveTemp(Pending); Session->bPublishedOrigin = true;
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=publication_directory units=%d changed_declarations=%d removed_original_declarations=%d model_retained=false unit_values_retained=false native_retained=false ast_retained=false namespace_text_rows=0"),
			Session->PublishedDirectory ? Session->PublishedDirectory->UnitCount : 0,
			Session->PublishedDirectory ? Session->PublishedDirectory->ChangedRows.Num() : 0,
			Session->PublishedDirectory ? Session->PublishedDirectory->RemovedOriginalRows.Num() : 0);
		if (ActiveReadState && ActiveReadState->FileObservationRoot == Root && ActiveReadState->SelectedNotificationThrough) {
			FileChanges::FError Native;
		if (!AcknowledgeSourceNotifications(*Session, ActiveReadState->SelectedNotificationThrough, Native)) return FailFileObservation(Error, Root, Native);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=publication_versions changed_files=%d known_files=%d pending_documents=%d source_version_retained_bytes=%lld source_version_budget=%lld ast_built=0 bytes_copied=0"),
			Plan.GetFiles().Num(), Session->Files.Num(), Session->PendingDocuments.Num(), Session->RetainedBytes, SourceVersionRetentionBudget);
		return true;
	}
	bool TryCollectUnconsumedWorkspaceFileCandidates(const FString& WorkspaceRoot, TSet<FString>& Pending, FWorkspaceError& Error)
	{
		const FString Root = NormalizeObservationRoot(WorkspaceRoot);
		const auto Session = WorkspaceSourceObservations.FindRef(Root);
		if (!Session || !ActiveReadState || ActiveReadState->FileObservationRoot != Root) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_scope_missing"), TEXT("Pending publication requires the same request's Windows notification cut. No directory scan was selected."), Root); return false;
		}
		FileChanges::FSnapshot Changes;
		if (!Session->Observer.Snapshot(Changes)) return FailFileObservation(Error, Root, Changes.Error);
		int32 Added = 0;
		for (const auto& Change : Changes.Candidates) {
			if (Change.LastSequence <= ActiveReadState->SelectedNotificationThrough) continue;
			FString Path(Change.Path.c_str()); Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Change.Directory) {
				if (Change.Changes == FileChanges::Modified || Session->PublishedCreatedDirectories.Contains(Path)) continue;
				Error.SetSourceFailure(TEXT("workspace_pending_directory_change"), TEXT("A newer directory change has no explicit child bindings. Native results remain pending; no recursive reader was selected."), Path); return false;
			}
		const auto* Published = Session->Files.Find(Path);
		if (Published ? !Published->bSemanticSourceDocument : !IsPublicSemanticFileCandidate(Path)) continue;
			const auto* Actual = ObservedSourceFiles.Find(Root / Path);
			if (Actual && Actual->bByteHashObserved && Actual->Proof && Actual->Proof->IsCurrent()) {
				if (Published && Actual->GetByteHash() == Published->Hash) continue;
				const auto* Accepted = FindAcceptedSemanticSourceHash(Root, Path);
				if (Accepted && Actual->ContentHash == *Accepted) continue;
			}
			if (!Pending.Contains(Path)) { Pending.Add(Path); ++Added; }
		}
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=unconsumed_after_cut candidates=%llu added_pending=%d pending_documents=%d consumed=false"),
			static_cast<uint64>(Changes.Candidates.size()), Added, Pending.Num());
		return true;
	}
	static bool SelectWorkspaceChangedFiles(const FString& Root, FWorkspaceError& Error)
	{
		if (!ActiveReadState) return false;
		if (ActiveReadState->FileObservationRoot == Root && ActiveReadState->SelectedNotificationThrough) return true;
		const auto Session = WorkspaceSourceObservations.FindRef(Root);
		if (!Session || !Session->bPublishedOrigin) {
			Error.SetSourceFailure(TEXT("workspace_file_observation_origin_missing"),
				TEXT("Windows file selection has no continuous published origin in this Editor session. Explicit full export must establish it; no file/hash scan was selected."), Root); return false;
		}
		FileChanges::FSnapshot Changes;
		if (!Session->Observer.Snapshot(Changes)) return FailFileObservation(Error, Root, Changes.Error);
		ActiveReadState->FileObservationRoot = Root;
		ActiveReadState->SelectedNotificationThrough = Changes.ThroughSequence;
		ActiveReadState->SelectedFilePaths = Session->PendingDocuments;
		uint64 AcknowledgeThrough = Changes.ThroughSequence;
		int32 IgnoredDirectoryMetadata = 0;
		for (const auto& Change : Changes.Candidates) {
			FString Path(Change.Path.c_str()); Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Change.Directory) {
				// A directory timestamp/attribute is not document contents or
				// membership. Recursive collection already observes child names.
				if (Change.Changes == FileChanges::Modified) { ++IgnoredDirectoryMetadata; continue; }
				if ((Change.Changes & FileChanges::Added) && !(Change.Changes & ~(FileChanges::Added | FileChanges::Modified))
					&& Session->PublishedCreatedDirectories.Contains(Path)) continue;
				// A directory event is not a trustworthy list of affected files.
				// Do not turn this missing input into a recursive enumeration.
				Error.SetSourceFailure(TEXT("workspace_file_selection_directory_change"),
					FString::Printf(TEXT("Path=%s; actions=%u. A directory was created, removed or moved without a matching publication producer. This Source consumer has no explicit child bindings; no recursive reader was selected."),
						*Path, Change.Changes), Root / Path); return false;
			}
			const auto* Published = Session->Files.Find(Path);
			if (!Published && (!Path.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)
				|| !IFileManager::Get().FileExists(*(Root / Path)))) continue; // e.g. editor's already-removed temporary save file
		auto* Actual = ObservedSourceFiles.Find(Root / Path);
			if (Published && Actual && Actual->bByteHashObserved && Actual->GetByteHash() == Published->Hash
			&& Actual->Proof && Actual->Proof->IsCurrent()) {
			if (Actual->KeepUntilNotification) Actual->KeepUntilNotification = Change.LastSequence;
			continue;
		}
			ActiveReadState->SelectedFilePaths.Add(Path);
			AcknowledgeThrough = FMath::Min<uint64>(AcknowledgeThrough, Change.LastSequence - 1);
		}
		FileChanges::FError Native;
	if (AcknowledgeThrough && !AcknowledgeSourceNotifications(*Session, AcknowledgeThrough, Native)) return FailFileObservation(Error, Root, Native);
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=select candidates=%llu selected_paths=%d pending_documents=%d ignored_directory_metadata=%d records=%llu acknowledged_system_through=%llu hash_directory_scan=false"),
			static_cast<uint64>(Changes.Candidates.size()), ActiveReadState->SelectedFilePaths.Num(), Session->PendingDocuments.Num(), IgnoredDirectoryMetadata,
			Changes.NotificationRecords, AcknowledgeThrough);
		return true;
	}

	FSemanticWorkspaceReadScope::FSemanticWorkspaceReadScope()
		: State(MakeUnique<FSemanticWorkspaceReadState>())
	{
		State->Previous = ActiveReadState;
		ActiveReadState = State.Get();
	}

	FSemanticWorkspaceReadScope::~FSemanticWorkspaceReadScope()
	{
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=publication_value_consumers decoded_rows=%d lifetime=synchronous_source_scope"), State->CapturedValueRowsRead);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=captured_string_consumers queries=%d range_reads=%d range_bytes=%lld typed_units_produced=0"),
			State->CapturedStringQueries, State->CapturedRangeReads, State->CapturedRangeReadBytes);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=captured_string_costs root_s=%.6f leaf_open_s=%.6f lease_s=%.6f range_io_s=%.6f fragment_s=%.6f string_query_s=%.6f required_whole_observation_s=%.6f timing=exclusive_components"),
			State->CapturedReferenceRootSeconds, State->CapturedReferenceObserveSeconds, State->CapturedReferenceProofSeconds,
			State->CapturedReferenceIoSeconds, State->CapturedReferenceSyntaxSeconds, State->CapturedReferenceStringSeconds,
			State->CapturedReferenceWholeSeconds);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=captured_fragment_costs decode_s=%.6f parse_s=%.6f retire_s=%.6f timing=fragment_subcomponents"),
			State->CapturedReferenceDecodeSeconds, State->CapturedReferenceParseSeconds, State->CapturedReferenceRetireSeconds);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=published_inventory_consumers files=%d rows=%d bytes=%lld read_hash_decode_s=%.6f syntax_s=%.6f rows_s=%.6f handoff_s=%.6f retire_s=%.6f source_row_hashes=0 source_backing_copies=0 optional_proofs=0 scalar_nodes_skipped=%lld timing=%s"),
			State->PublishedInventoryFilesRead, State->PublishedInventoryRowsRead, State->PublishedInventoryBytesRead,
			State->PublishedInventoryReadSeconds, State->PublishedInventorySyntaxSeconds,
			State->PublishedInventoryRowsSeconds, State->PublishedInventoryCopySeconds, State->PublishedInventoryRetireSeconds, State->PublishedInventoryScalarNodesSkipped,
			State->bPublishedInventoryParallel ? TEXT("main_io_plus_worker_elapsed_components") : TEXT("exclusive_components"));
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=inventory_projected_token_storage borrowed_decoded_texts=%lld borrowed_raw_lexemes=%lld borrowed_object_keys=%lld span_integer_nodes_skipped=%lld lifetime=input_or_token_until_same_row_consumer"),
			State->PublishedInventoryBorrowedTokenTexts, State->PublishedInventoryBorrowedTokenLexemes,
			State->PublishedInventoryBorrowedKeys, State->PublishedInventorySpanNodesSkipped);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=published_inventory_document_selection queries=%d actual_row_slots=%lld matching_rows=%lld addressing=owned_shard_range"),
			State->PublishedInventoryDocumentQueries, State->PublishedInventoryDocumentSlots, State->PublishedInventoryDocumentMatches);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=inventory_schema_fields field_nodes_visited=%lld compact_digest_views=%lld validated_path_reuses=%lld field_binding=one_row_stack no_retained_lookup=true"),
			State->InventoryFieldNodesVisited, State->InventoryCompactDigestViews, State->InventoryValidatedPathReuses);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=declaration_consumers rows=%d absolute_positions=on_error temporary_ast_retained=0 row_lookup_comparisons=%lld published_reference_ids=%lld metadata_alias_lookups=%lld"),
			State->CapturedDeclarationRows, State->PublicationRowComparisons, State->PublishedDeclarationRefIds, State->MetadataAliasLookups);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=captured_directory_lifetime metadata_documents=%d released_metadata=%d live_metadata=%d peak_metadata=%d span_rows=%d span_bytes=%lld byte_reads=%d live_bytes=%lld peak_bytes=%lld retired_bytes=%lld lifetime=synchronous_source_scope"),
			State->CapturedMetadataDocuments, State->CapturedMetadataReleased,
			State->CapturedMetadataDocuments - State->CapturedMetadataReleased, State->CapturedMetadataPeak,
			State->CapturedSpanRows, State->CapturedSpanBytes, State->CapturedByteReads,
			State->CapturedLiveBytes, State->CapturedPeakBytes, State->CapturedRetiredBytes);
		ActiveReadState = State->Previous;
		const double Started = FPlatformTime::Seconds();
		const int32 Documents = State->Documents.Num(), ResolvedDocuments = State->ResolvedDocuments.Num();
		State.Reset();
	if (!ActiveReadState) {
		// Accepted checkpoints and guards own their real leases independently.
		// Named writes may also be needed to classify a later notification.
		const auto Session = WorkspaceSourceObservations.FindRef(ObservedSourceRoot);
		const int32 Before = ObservedSourceFiles.Num();
		for (auto It = ObservedSourceFiles.CreateIterator(); It; ++It) {
			const auto& File = It.Value();
			if (!Session || File.KeepUntilNotification <= Session->AcknowledgedNotifications
				|| !File.Proof || !File.Proof->IsLeasePending()) It.RemoveCurrent();
		}
		UE_LOG(LogTemp, Display, TEXT("UAI source observation phase=source_lease_release released=%d awaiting_notifications=%d pending_file_proofs=%d"),
			Before - ObservedSourceFiles.Num(), ObservedSourceFiles.Num(), GetWorkspaceFileProofUsage().PendingOperations);
	}
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_read_scope_release documents=%d resolved_documents=%d total_s=%.6f"),
			Documents, ResolvedDocuments, FPlatformTime::Seconds() - Started);
	}

	namespace
	{
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");
		const FString EmbeddedPath =
			TEXT("workspace.hjson");
		void ComputeSha256(const uint8* Bytes, int64 Num, uint8 (&Digest)[SHA256_DIGEST_LENGTH])
		{
			const uint8 Empty = 0;
			SHA256(Num == 0 ? &Empty : Bytes, static_cast<size_t>(Num), Digest);
		}
	}

		// Inventory hashes retain the original wire text contract. Only the exact
		// spelling emitted by our SHA owner has a compact representation; older,
		// malformed or differently-cased text remains verbatim for diagnostics and
		// the existing FString comparison/material semantics.
		struct FInventoryDigest
		{
			uint8 Bytes[SHA256_DIGEST_LENGTH] = {};
			FString Original;
			bool bCompact = false;
			static bool DecodeView(FStringView Text, uint8* Out)
			{
				if (Text.Len() != 7 + SHA256_DIGEST_LENGTH * 2
					|| !Text.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive)) return false;
				auto Nibble = [](TCHAR C) { return C >= TEXT('0') && C <= TEXT('9') ? C - TEXT('0')
					: C >= TEXT('a') && C <= TEXT('f') ? C - TEXT('a') + 10 : -1; };
				for (int32 I = 0; I < SHA256_DIGEST_LENGTH; ++I) {
					const int32 High = Nibble(Text[7 + I * 2]), Low = Nibble(Text[8 + I * 2]);
					if (High < 0 || Low < 0) return false;
					Out[I] = static_cast<uint8>((High << 4) | Low);
				}
				return true;
			}
			static bool Decode(const FString& Text, uint8* Out) { return DecodeView(FStringView(Text), Out); }
			void Set(FString Text)
			{
				bCompact = Decode(Text, Bytes);
				Original = bCompact ? FString() : MoveTemp(Text);
			}
			void SetView(FStringView Text)
			{
				bCompact = DecodeView(Text, Bytes);
				Original = bCompact ? FString() : FString(Text);
			}
			void SetFromBytes(const uint8* Data, int64 Num)
			{
				ComputeSha256(Data, Num, Bytes); Original.Empty(); bCompact = true;
			}
			bool IsEmpty() const { return !bCompact && Original.IsEmpty(); }
			int64 GetAllocatedSize() const { return Original.GetAllocatedSize(); }
			FString ToString() const
			{
				if (!bCompact) return Original;
				static const TCHAR Hex[] = TEXT("0123456789abcdef");
				FString Text(TEXT("sha256:")); Text.Reserve(7 + SHA256_DIGEST_LENGTH * 2);
				for (uint8 Byte : Bytes) { Text.AppendChar(Hex[Byte >> 4]); Text.AppendChar(Hex[Byte & 15]); }
				return Text;
			}
			bool Matches(const FString& Text) const
			{
				uint8 Other[SHA256_DIGEST_LENGTH];
				if (bCompact && Decode(Text, Other)) return FMemory::Memcmp(Bytes, Other, sizeof(Bytes)) == 0;
				return ToString() == Text;
			}
			FAcceptedWorkspaceContentHashView View() const {
				return {this, [](const void* Data, const FString& Hash) {
					return static_cast<const FInventoryDigest*>(Data)->Matches(Hash);
				}, [](const void* Data) {
					const auto& Digest = *static_cast<const FInventoryDigest*>(Data);
					return Digest.bCompact ? Digest.ToString() : FString();
				}};
			}
			void UpdateTextHash(SHA256_CTX& Hash) const
			{
				if (!bCompact) { SHA256_Update(&Hash, *Original, Original.Len() * sizeof(TCHAR)); return; }
				TCHAR Text[7 + SHA256_DIGEST_LENGTH * 2];
				FMemory::Memcpy(Text, TEXT("sha256:"), 7 * sizeof(TCHAR));
				static const TCHAR Hex[] = TEXT("0123456789abcdef");
				for (int32 I = 0; I < SHA256_DIGEST_LENGTH; ++I) {
					Text[7 + I * 2] = Hex[Bytes[I] >> 4]; Text[8 + I * 2] = Hex[Bytes[I] & 15];
				}
				// HashRowInputs historically hashes TCHAR text without a terminator.
				SHA256_Update(&Hash, Text, sizeof(Text));
			}
		};

		struct FInventoryRowSyntax
		{
			TSharedPtr<const FString, ESPMode::ThreadSafe> UnitIdentity;
			const FString& GetUnitId() const { return *UnitIdentity; }
			FString PublicRef;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::OpaqueExtension;
			FString LogicalPath;
			FString DocumentPath;
			FString UnitPointer;
			// Generated projections already own these strings in immutable layout
			// and row containers. Keep their owner, rather than another all-row
			// copy. Parsed/legacy input continues to own the fields above.
			TSharedPtr<const FResolvedWorkspaceUnitBindings> GeneratedOwner;
			const FString* GeneratedPublicRef = nullptr;
			const FString* GeneratedLogicalPath = nullptr;
			const FString* GeneratedDocumentPath = nullptr;
			const FString* GeneratedUnitPointer = nullptr;
			const TArray<FString>* GeneratedContentFiles = nullptr;
			const FString& GetPublicRef() const { return GeneratedPublicRef ? *GeneratedPublicRef : PublicRef; }
			const FString& GetLogicalPath() const { return GeneratedLogicalPath ? *GeneratedLogicalPath : LogicalPath; }
			const FString& GetDocumentPath() const { return GeneratedDocumentPath ? *GeneratedDocumentPath : DocumentPath; }
			const FString& GetUnitPointer() const { return GeneratedUnitPointer ? *GeneratedUnitPointer : UnitPointer; }
			FInventoryDigest ContentHash;
			FInventoryDigest EditHash;
			FAcceptedWorkspaceByteSpan StoredEnvelopeSpan;
			TSharedPtr<const FString> EnvelopeRef;
			const FString& GetEnvelopeRef() const { static const FString Empty; return EnvelopeRef ? *EnvelopeRef : Empty; }
			TArray<FString> ContentFiles;
			const TArray<FString>& GetContentFiles() const { return GeneratedContentFiles ? *GeneratedContentFiles : ContentFiles; }
			int64 OwnedProjectionBytes() const {
				int64 Bytes = PublicRef.GetAllocatedSize() + LogicalPath.GetAllocatedSize()
					+ DocumentPath.GetAllocatedSize() + UnitPointer.GetAllocatedSize() + ContentFiles.GetAllocatedSize();
				for (const auto& Path : ContentFiles) Bytes += Path.GetAllocatedSize();
				return Bytes;
			}
		};
	namespace
	{
		class FCompactBaselineHashes final : public FResolvedWorkspaceBaselineHashes
		{
		public:
			struct FRow {
				TSharedPtr<const FString, ESPMode::ThreadSafe> Id;
				FInventoryDigest Hash;
				FRow(TSharedPtr<const FString, ESPMode::ThreadSafe> InId, const FInventoryDigest& InHash)
					: Id(MoveTemp(InId)), Hash(InHash) {}
			};
			TArray<FRow> Rows;
			void CopyTo(TMap<FString, FString>& Out) const override {
				for (const auto& Row : Rows) Out.Add(*Row.Id, Row.Hash.ToString());
			}
			int64 GetAllocatedSize() const {
				int64 Bytes = sizeof(FCompactBaselineHashes) + 32 + Rows.GetAllocatedSize();
				for (const auto& Row : Rows) Bytes += Row.Hash.GetAllocatedSize();
				return Bytes;
			}
		};
		struct FInventoryRow
		{
			FInventoryRow(FInventoryRowSyntax InSyntax, const FHjsonNode* InSource, const FHjsonNode* InEnvelope)
				: Syntax(MakeShared<const FInventoryRowSyntax>(MoveTemp(InSyntax))), SourceNode(InSource), Envelope(InEnvelope) {
				GeneratedEnvelopeSpan = Syntax->StoredEnvelopeSpan;
				}
			FInventoryRow(TSharedRef<const FInventoryRowSyntax> InSyntax, const FHjsonNode* InSource, const FHjsonNode* InEnvelope)
				: Syntax(MoveTemp(InSyntax)), SourceNode(InSource), Envelope(InEnvelope) {
				GeneratedEnvelopeSpan = Syntax->StoredEnvelopeSpan;
				}
			// A cached inventory shard owns one immutable identity/path/hash record.
			// A read copies only this handle and its local source/baseline bindings.
			TSharedRef<const FInventoryRowSyntax> Syntax;
			FString DocumentPathOverride;
			FString LogicalPathOverride;
			const FString& GetDocumentPath() const { return DocumentPathOverride.IsEmpty() ? Syntax->GetDocumentPath() : DocumentPathOverride; }
			const FString& GetLogicalPath() const { return LogicalPathOverride.IsEmpty() ? Syntax->GetLogicalPath() : LogicalPathOverride; }
			const FHjsonNode* SourceNode = nullptr;
			const FHjsonNode* Envelope = nullptr;
			FHjsonNodePtr RetainedSourceNode;
			// Split inventory success paths consume Syntax. Keep exact source bytes
			// once per shard for diagnostics, not another per-row AST of those fields.
			TOptional<FSourceProvenance> SourceOrigin;
			// Generated rows need the byte span on the normal path. Build their
			// JSON pointer only if the diagnostic-source consumer materializes it.
			int32 GeneratedInventoryOrdinal = INDEX_NONE;
			TSharedPtr<const TArray<uint8>> SourceUtf8;
			FInventoryDigest SourceRowHash;
			FAcceptedWorkspaceByteSpan GeneratedEnvelopeSpan;
			bool bGeneratedProjection = false;
		};
		class FResolvedWorkspaceInventory final : public FAcceptedWorkspaceInventory
		{
		public:
			struct FRow {
				TSharedRef<const FInventoryRowSyntax> Syntax;
				FAcceptedWorkspaceByteSpan InventorySpan, EnvelopeSpan;
				bool bGeneratedProjection = false;
				int32 InventoryOrdinal = INDEX_NONE;
			};
			TArray<FRow> Rows;
			TOptional<bool> ScopedMachineBuckets;
			TOptional<bool> GetScopedMachineBuckets() const override { return ScopedMachineBuckets; }
			TSharedPtr<const FDefinitionProjectionHints> ProjectionHints;
			TSharedPtr<const FDefinitionProjectionHints> GetProjectionHints() const override { return ProjectionHints; }
			int32 Num() const override { return Rows.Num(); }
			bool PrepareDocumentMetadata(const FString&) const override { return true; } // These exact rows are already parsed.
			bool VisitRows(TFunctionRef<bool(const FRow&)> Selected,
				TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const {
				const int32 Count = Rows.Num();
				for (int32 Index = 0; Index < Count; ++Index) {
					const auto Entry = Rows[Index];
					if (!Selected(Entry)) continue;
					const auto Row = Entry.Syntax;
					const auto MatchesContentHash = [&](const FString& Hash) { return Row->ContentHash.Matches(Hash); };
					const auto CanonicalContentHash = [&]() { return Row->ContentHash.bCompact ? Row->ContentHash.ToString() : FString(); };
					const auto EditHash = [&]() { return Row->EditHash.ToString(); };
					if (!Visitor({Row->GetUnitId(), Row->UnitKind, Row->GetPublicRef(),
						Row->GetLogicalPath(), Row->GetDocumentPath(), Row->GetUnitPointer(), Row->GetEnvelopeRef(), Row->GetContentFiles(),
						EditHash, MatchesContentHash, CanonicalContentHash, Entry.InventorySpan, Entry.EnvelopeSpan,
						Entry.bGeneratedProjection, Row->UnitIdentity, Entry.InventoryOrdinal, Row->StoredEnvelopeSpan.IsValid(), Row,
						Row->ContentHash.View(), Index + 1 == Count || Rows[Index + 1].Syntax->GetEnvelopeRef() != Row->GetEnvelopeRef()})) return false;
				}
				return true;
			}
			bool VisitRange(int32 First, int32 Count, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const {
				for (int32 Index = First; Index < First + Count; ++Index) {
					// A synchronous consumer may ask this directory for another shard.
					// Keep the immutable syntax and scalar span data independent of
					// subsequent growth of Rows, not a reference into its array.
					const auto Entry = Rows[Index]; const auto Row = Entry.Syntax;
					const auto Edit = [&]() { return Row->EditHash.ToString(); };
					const auto Matches = [&](const FString& Hash) { return Row->ContentHash.Matches(Hash); };
					const auto Content = [&]() { return Row->ContentHash.bCompact ? Row->ContentHash.ToString() : FString(); };
					if (!Visitor({Row->GetUnitId(), Row->UnitKind, Row->GetPublicRef(), Row->GetLogicalPath(), Row->GetDocumentPath(),
						Row->GetUnitPointer(), Row->GetEnvelopeRef(), Row->GetContentFiles(), Edit, Matches, Content,
						Entry.InventorySpan, Entry.EnvelopeSpan, Entry.bGeneratedProjection, Row->UnitIdentity, Entry.InventoryOrdinal,
						Row->StoredEnvelopeSpan.IsValid(), Row, Row->ContentHash.View(),
						Index + 1 == First + Count || Rows[Index + 1].Syntax->GetEnvelopeRef() != Row->GetEnvelopeRef()})) return false;
				}
				return true;
			}
			bool Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
				return VisitRows([](const FRow&) { return true; }, Visitor);
			}
			bool VisitDocument(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
				return VisitRows([&](const FRow& Row) { return Row.Syntax->GetDocumentPath() == Path; }, Visitor);
			}
			bool VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor,
				bool bRequireEnvelopeSpan = true) const override {
				// Explicit materialized inputs have only these metadata rows. The
				// current directory source retains its original bucket producer below;
				// it is not wrapped in this view or sent through a default full Visit.
				return VisitRows([&](const FRow& Row) { return Row.Syntax->GetEnvelopeRef() == Path; }, Visitor);
				}
			FString GetInventoryPath(const FString& Document) const override {
				if (ScopedMachineBuckets.IsSet()) return GetPublishedMachineEnvelopePath(Document, ScopedMachineBuckets.GetValue())
					.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
				for (const auto& Row : Rows) if (Row.Syntax->GetDocumentPath() == Document)
					return Row.Syntax->GetEnvelopeRef().Replace(TEXT("/baselines/"), TEXT("/inventory/"));
				return {};
			}
			void VisitHashes(TFunctionRef<void(const FString&, const FString&)> Visitor) const override {
				for (const auto& Row : Rows) Visitor(Row.Syntax->GetUnitId(), Row.Syntax->ContentHash.ToString());
			}
			void VisitPaths(TFunctionRef<void(const FString&, const FString&, const FString&, const TArray<FString>&)> Visitor) const override {
				for (const auto& Row : Rows) Visitor(Row.Syntax->GetUnitId(), Row.Syntax->GetDocumentPath(), Row.Syntax->GetEnvelopeRef(), Row.Syntax->GetContentFiles());
			}
			void VisitExpandedDocuments(TFunctionRef<void(const FString&)> Visitor) const override {
				TSet<FString> Seen;
				for (const auto& Row : Rows) if (!Row.Syntax->GetContentFiles().IsEmpty() && !Seen.Contains(Row.Syntax->GetDocumentPath())) {
					Seen.Add(Row.Syntax->GetDocumentPath()); Visitor(Row.Syntax->GetDocumentPath());
				}
			}
			bool HasContentFileDirectory() const override { return true; }
			void VisitContentFiles(TFunctionRef<void(const FString&)> Visitor) const override {
				TSet<FString> Seen;
				for (const auto& Row : Rows) for (const auto& Path : Row.Syntax->GetContentFiles())
					if (!Seen.Contains(Path)) { Seen.Add(Path); Visitor(Path); }
			}
			void CompletePhysicalOrganization() {
				if (Rows.IsEmpty()) return;
				bool bScoped = true, bPrefix = true;
				for (const auto& Row : Rows) {
					bScoped &= Row.Syntax->GetEnvelopeRef() == GetPublishedMachineEnvelopePath(Row.Syntax->GetDocumentPath(), true);
					bPrefix &= Row.Syntax->GetEnvelopeRef() == GetPublishedMachineEnvelopePath(Row.Syntax->GetDocumentPath(), false);
				}
				if (bScoped) ScopedMachineBuckets = true;
				else if (bPrefix) ScopedMachineBuckets = false;
			}
		};

		struct FBaselineParseProof
		{
			FString ContentHash;
			TMap<EUeAgentResourceDimension, int64> Charges;
			bool bExact = false;
		};

		struct FRowInputDigest
		{
			uint8 Bytes[SHA256_DIGEST_LENGTH] = {};
			bool operator==(const FRowInputDigest& Other) const
			{
				return FMemory::Memcmp(Bytes, Other.Bytes, sizeof(Bytes)) == 0;
			}
			bool operator!=(const FRowInputDigest& Other) const { return !(*this == Other); }
			friend uint32 GetTypeHash(const FRowInputDigest& Value) {
				uint32 Prefix = 0; FMemory::Memcpy(&Prefix, Value.Bytes, sizeof(Prefix)); return Prefix;
			}
		};
		struct FReferenceLookupProof
		{
			FString Key;
			TOptional<FString> Result;
		};
		struct FResolvedRowProof;
		struct FGeneratedSourceCandidate;
		using FResolvedRowProofPtr = TSharedPtr<const FResolvedRowProof>;
		struct FResolvedRowsProof;
		struct FResolvedRowGroup;
		struct FResolvedGroupIndex;
		struct FInventoryShardRange
		{
			FString Path, ContentHash, MembershipHash;
			int32 First = 0, Count = 0;
		};
		struct FReadRowProof
		{
			const FHjsonNode* AuthoredStorage = nullptr;
			TWeakPtr<const FHjsonDocument> AuthoredDocument;
			FRowInputDigest Input;
			FRowInputDigest Authored;
			FRowInputDigest Envelope;
			FAcceptedWorkspaceByteSpan EnvelopeSpan;
			FHjsonDocumentPtr ReboundSource;
			bool bValid = false;
		};
		struct FParsedSource
		{
			FHjsonDocumentPtr Envelope;
			FHjsonDocumentPtr ManifestDocument;
			TArray<FHjsonDocumentPtr> UnitDocuments;
			const FHjsonNode* Manifest = nullptr;
			TArray<const FHjsonNode*> UnitRows;
			TArray<FHjsonNodeRef> AssembledRows;
			TArray<const FHjsonNode*> UnitEnvelopes;
			TArray<FString> ManagedPaths;
			TSet<FString> ContentReferenceUnits;
			TMap<FString, FBaselineParseProof> BaselineProofs;
			TMap<FString, FString> InventoryHashes;
			TMap<FString, FString> PublicRefs;
			FString ReferenceMapHash;
			TMap<FString, FString> DocumentHashes;
			TArray<FReadRowProof> RowProofs;
			// Only changed, unexpanded rows carry the already produced edit digest
			// to the semantic reader. Exact rows exit before captured-value ASTs.
			TMap<int32, FString> EarlyEditDigests;
			int32 EarlyExactEditRows = 0;
			TArray<FResolvedRowProofPtr> ReusableRows;
			TSharedPtr<const FGeneratedSourceCandidate> GeneratedSource;
			bool bSparseGeneratedInventory = false;
			TArray<FIntPoint> SparseManagedSlices;
			TMap<FString, FString> FingerprintPublishedHashes;
			FWorkspaceFileProofPtr FingerprintOwnerProof;
			TSet<FString> FingerprintRequiredDocuments;
			TArray<FString> FingerprintRowPaths;
			TArray<FString> FingerprintInterfacePaths;
			bool bCanonicalInputFingerprint = false;
			int32 SparseManagedPrefix = 0;
			int32 SparseManagedTail = 0;
			TBitArray<> GeneratedRows;
			TSharedPtr<const FResolvedRowsProof> PriorRowsOwner;
			TArray<FInventoryShardRange> InventoryRanges;
			TSharedPtr<const FResolvedGroupIndex> GroupIndex;
			TMap<int32, TSharedPtr<const FResolvedRowGroup>> ReusedGroups;
			// Exact historical path order is needed only by an old record whose
			// input fingerprint really has to be compared. Normal source admission
			// consumes the file namespace directly, without a per-unit path visit.
		};

		// A row's projected source root is shallow, but owning its children would
		// independently retain the public parser tree after its bounded owner retires.
		// Keep the exact root shape/provenance and weak children instead. A current
		// accepted workspace pins the children for its consumers; reuse without that
		// backing fails normally and resolves the current source again.
		struct FResolvedRowShape
		{
			int32 ItemCount = 0;
			TArray<FString> FieldNames;
			int64 GetAllocatedSize() const {
				int64 Bytes = sizeof(FResolvedRowShape) + 32 + FieldNames.GetAllocatedSize();
				for (const FString& Name : FieldNames) Bytes += Name.GetAllocatedSize();
				return Bytes;
			}
		};
		struct FResolvedShapeCandidate { uint32 Hash = 0; TWeakPtr<const FResolvedRowShape> Shape; };
		// Names/order only; no document, node or provenance is retained by this
		// bounded weak index. A row always owns its own exact weak child sequence.
		static thread_local TUniquePtr<TStaticArray<FResolvedShapeCandidate, 256>> ResolvedShapeCandidates;
		TSharedRef<const FResolvedRowShape> SealResolvedShape(const FHjsonNode& Root)
		{
			const auto& Fields = Root.GetEntries();
			const int32 Items = Root.GetItems().Num();
			uint32 Hash = GetTypeHash(Items);
			int64 Characters = 0;
			for (const auto& Field : Fields) { Hash = HashCombineFast(Hash, GetTypeHash(Field.Key)); Characters += Field.Key.Len(); }
			const bool bIndexable = Items <= 64 && Fields.Num() <= 64 && Characters <= 4096;
			TSharedPtr<const FResolvedRowShape> Previous;
			FResolvedShapeCandidate* Slot = nullptr;
			if (bIndexable) {
				if (!ResolvedShapeCandidates) ResolvedShapeCandidates = MakeUnique<TStaticArray<FResolvedShapeCandidate, 256>>();
				Slot = &(*ResolvedShapeCandidates)[Hash % 256]; Previous = Slot->Shape.Pin();
				if (Previous && Slot->Hash == Hash && Previous->ItemCount == Items && Previous->FieldNames.Num() == Fields.Num()) {
					bool bEqual = true;
					for (int32 I = 0; I < Fields.Num(); ++I)
						if (!Previous->FieldNames[I].Equals(Fields[I].Key, ESearchCase::CaseSensitive)) { bEqual = false; break; }
					if (bEqual) return Previous.ToSharedRef();
				}
			}
			auto Result = MakeShared<FResolvedRowShape>(); Result->ItemCount = Items;
			Result->FieldNames.Reserve(Fields.Num());
			for (const auto& Field : Fields) Result->FieldNames.Add(Field.Key);
			if (Slot && !Previous) { Slot->Hash = Hash; Slot->Shape = Result; }
			return Result;
		}
		class FResolvedRowSource final
		{
		public:
			explicit FResolvedRowSource(const FHjsonDocumentRef& Document)
				: Kind(Document->GetRoot()->GetKind()), Provenance(Document->GetRoot()->GetProvenance()),
				  Lexeme(Document->GetRoot()->GetLexeme()), Text(Document->GetRoot()->GetText()),
				  bBoolean(Document->GetRoot()->GetBoolean()), Shape(SealResolvedShape(Document->GetRoot().Get())), LastDocument(Document)
			{
				if (!Document->GetSourceUnit().Equals(Provenance.GetSourceUnit(), ESearchCase::CaseSensitive))
					SourceOverride = Document->GetSourceUnit();
				Children.Reserve(Shape->ItemCount + Shape->FieldNames.Num());
				for (const auto& Item : Document->GetRoot()->GetItems()) Children.Add(Item);
				for (const auto& Field : Document->GetRoot()->GetEntries()) Children.Add(Field.Value);
			}
			const FString& GetSourceUnit() const { return SourceOverride.IsSet() ? SourceOverride.GetValue() : Provenance.GetSourceUnit(); }
			const FString& GetSharedSourceName() const { return Provenance.GetSourceUnit(); }
			bool IsAvailable() const {
				for (const auto& Child : Children) if (!Child.IsValid()) return false;
				return true;
			}
			bool RetainCurrentBacking(TArray<FHjsonNodeRef>& Out) const {
				for (const auto& Child : Children) { const auto Pin = Child.Pin(); if (!Pin) return false; Out.Add(Pin.ToSharedRef()); }
				return true;
			}
			FHjsonDocumentPtr Pin() const {
				if (const auto Existing = LastDocument.Pin()) return Existing;
				TArray<FHjsonNodeRef> PinnedItems;
				TArray<TPair<FString, FHjsonNodeRef>> PinnedFields;
				PinnedItems.Reserve(Shape->ItemCount); PinnedFields.Reserve(Shape->FieldNames.Num());
				for (int32 I = 0; I < Children.Num(); ++I) {
					const auto Child = Children[I].Pin(); if (!Child) return nullptr;
					if (I < Shape->ItemCount) PinnedItems.Add(Child.ToSharedRef());
					else PinnedFields.Emplace(Shape->FieldNames[I - Shape->ItemCount], Child.ToSharedRef());
				}
				const FHjsonNodeRef Root = MakeShared<FHjsonNode>(Kind, Provenance, Lexeme, Text, bBoolean,
					MoveTemp(PinnedItems), MoveTemp(PinnedFields));
				const FHjsonDocumentRef Document = SourceOverride.IsSet()
					? MakeShared<FHjsonDocument>(SourceOverride.GetValue(), Root) : MakeShared<FHjsonDocument>(Root);
				LastDocument = Document;
				return Document;
			}
			int64 GetAllocatedSize() const {
				int64 Bytes = sizeof(FResolvedRowSource) + 32 + Lexeme.GetAllocatedSize() + Text.GetAllocatedSize()
					+ Provenance.GetJsonPointer().GetAllocatedSize() + Provenance.GetOriginalKey().GetAllocatedSize()
					+ Children.GetAllocatedSize();
				if (SourceOverride.IsSet()) Bytes += SourceOverride->GetAllocatedSize();
				return Bytes;
			}
			const FResolvedRowShape& GetShape() const { return Shape.Get(); }
		private:
			EHjsonNodeKind Kind;
			FSourceProvenance Provenance;
			FString Lexeme, Text;
			bool bBoolean = false;
			TOptional<FString> SourceOverride;
			TSharedRef<const FResolvedRowShape> Shape;
			TArray<TWeakPtr<const FHjsonNode>> Children;
			mutable TWeakPtr<const FHjsonDocument> LastDocument;
		};

		using FResolvedResourceCharges = TMap<EUeAgentResourceDimension, int64>;
		struct FResolvedChargeCandidate
		{
			uint32 Hash = 0;
			TWeakPtr<const FResolvedResourceCharges> Charges;
		};
		// Only exact successful resource deltas are shared. The weak table is
		// fixed-size and holds neither a model nor source/native objects.
		static thread_local TUniquePtr<TStaticArray<FResolvedChargeCandidate, 1024>> ResolvedChargeCandidates;
		TSharedPtr<const FResolvedResourceCharges> SealResolvedCharges(FResolvedResourceCharges&& Charges)
		{
			uint32 Hash = 0;
			for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D)
				Hash = HashCombineFast(Hash, GetTypeHash(Charges.FindRef(static_cast<EUeAgentResourceDimension>(D))));
			if (!ResolvedChargeCandidates)
				ResolvedChargeCandidates = MakeUnique<TStaticArray<FResolvedChargeCandidate, 1024>>();
			auto& Slot = (*ResolvedChargeCandidates)[Hash % 1024];
			const auto Previous = Slot.Charges.Pin();
			if (Previous && Slot.Hash == Hash && Previous->Num() == Charges.Num()) {
				bool bEqual = true;
				for (const auto& Pair : Charges) {
					const int64* Value = Previous->Find(Pair.Key);
					if (!Value || *Value != Pair.Value) { bEqual = false; break; }
				}
				if (bEqual) return Previous;
			}
			const auto Result = MakeShared<const FResolvedResourceCharges>(MoveTemp(Charges));
			if (!Previous) { Slot.Hash = Hash; Slot.Charges = Result; }
			return Result;
		}

		// Only the resolver produces this successful semantic proof. The optional
		// cache does not retain another parser generation through source bindings.
		struct FResolvedRowProof
		{
			const FHjsonNode* AuthoredStorage = nullptr;
			TWeakPtr<const FHjsonDocument> AuthoredDocument;
			FRowInputDigest InputHash;
			FRowInputDigest AuthoredHash;
			FRowInputDigest EnvelopeHash;
			FAcceptedWorkspaceByteSpan EnvelopeSpan;
			TSharedPtr<const FString> EnvelopeRef;
			const FString& GetEnvelopeRef() const { static const FString Empty; return EnvelopeRef ? *EnvelopeRef : Empty; }
			TArray<FReferenceLookupProofPtr> ReferenceLookups;
			bool bCompleteReferenceLookups = false;
			TOptional<FSemanticUnit> Unit;
			TOptional<FSemanticUnit> Baseline;
			// Usually empty: retain the original lookup spelling only when unit
			// normalization makes it differ from the inventory's existing key.
			FString LookupKeyOverride;
			const FString& GetLookupKey() const { return LookupKeyOverride.IsEmpty() ? Unit->GetUnitId() : LookupKeyOverride; }
			TOptional<FSourceProvenance> Source;
			TSharedPtr<const FResolvedRowSource> SourceDocument;
			TSharedPtr<const TSet<FString>> KeyedRoots;
			TSharedPtr<const FResolvedResourceCharges> Charges;
			// Only complete successful rows enter this proof. Failed diagnostics
			// remain with the current resolver result, never in the reuse generation.
			bool bEdited = false;
			int64 RetainedBytes = 0;
		};
		struct FResolvedRowKeyFuncs : BaseKeyFuncs<FResolvedRowProofPtr, FString, false>
		{
			static KeyInitType GetSetKey(ElementInitType Row) { return Row->GetLookupKey(); }
			// Preserve the previous FString map key semantics. The key is owned by
			// the sealed unit held by this row; no second long UnitId is allocated.
			static bool Matches(KeyInitType A, KeyInitType B) { return A == B; }
			static uint32 GetKeyHash(KeyInitType Key) { return GetTypeHash(Key); }
		};
		struct FResolvedRowPosition
		{
			TSharedRef<const FString, ESPMode::ThreadSafe> UnitId;
			FIntPoint Position;
		};
		struct FResolvedRowPositionKeyFuncs : BaseKeyFuncs<FResolvedRowPosition, FString, false>
		{
			static KeyInitType GetSetKey(ElementInitType Row) { return Row.UnitId.Get(); }
			static bool Matches(KeyInitType A, KeyInitType B) { return A == B; }
			static uint32 GetKeyHash(KeyInitType Key) { return GetTypeHash(Key); }
		};
		struct FResolvedGroupIndex
		{
			TArray<FInventoryShardRange> Shards;
			TArray<FString> Documents;
			TMap<FString, TArray<int32>> DocumentGroups, BaselineGroups;
			TSet<FResolvedRowPosition, FResolvedRowPositionKeyFuncs> RowPositions;
			const FIntPoint* FindPosition(const FString& Id) const
			{
				const auto* Row = RowPositions.Find(Id);
				return Row ? &Row->Position : nullptr;
			}
			const FIntPoint& FindPositionChecked(const FString& Id) const
			{
				const auto* Row = RowPositions.Find(Id);
				check(Row);
				return Row->Position;
			}
			bool OwnsIdentity(const FString& Id) const
			{
				const auto* Row = RowPositions.Find(Id);
				return Row && &Row->UnitId.Get() == &Id;
			}
			TSharedRef<const FString, ESPMode::ThreadSafe> BindIdentity(TSharedRef<const FString, ESPMode::ThreadSafe> Id) const
			{
				const auto* Row = RowPositions.Find(Id.Get());
				return Row && Row->UnitId->Equals(Id.Get(), ESearchCase::CaseSensitive) ? Row->UnitId : Id;
			}
			TArray<TArray<FString>> FirstManagedPaths;
			int64 ChargeBytes = 0;
		};
		struct FResolvedRowGroup
		{
			FString InventoryHash;
			TArray<FResolvedRowProofPtr> Rows;
			TArray<FSemanticUnit> Units;
			// Borrowed only after the current document identities have been pinned.
			// No group owns another complete public-document AST or old generation.
			TArray<const FHjsonNode*> Nodes;
			TArray<FReferenceLookupProofPtr> Lookups;
			int64 ReferenceEdges = 0;
			TMap<EUeAgentResourceDimension, int64> Charges;
			TArray<FString> EditedIds;
			TMap<FString, TSharedPtr<const TSet<FString>>> KeyedRoots;
			int64 ChargeBytes = 0;
		};
		constexpr int64 ResolvedGroupBudget = 64LL * 1024 * 1024;
		constexpr int64 ResolvedRowsBudget = 512LL * 1024 * 1024;
		struct FResolvedRowsProof
		{
			FString Root;
			FString ReferenceMapHash;
			ESemanticWorkspaceMode Mode = ESemanticWorkspaceMode::State;
			bool bManagedEdits = false;
			TSet<FResolvedRowProofPtr, FResolvedRowKeyFuncs> Rows;
			// The complete inventory index already owns key lookup/uniqueness.
			// Keep sparse candidates in its exact positions instead of rebuilding
			// a second hash set and reserving an imaginary node per grouped row.
			TArray<FResolvedRowProofPtr> IndexedRows;
			int32 IndexedRowCount = 0;
			TMap<FString, FBaselineParseProof> Baselines;
			TMap<FString, FString> DocumentHashes;
			TSharedPtr<const FResolvedGroupIndex> GroupIndex;
			TArray<TSharedPtr<const FResolvedRowGroup>> Groups;
			TMap<FString, TWeakPtr<const FHjsonDocument>> GroupDocuments;
			TSharedPtr<const FCompiledAuthoringReuseProof> CompilerProof;
			int64 GroupChargeBytes = 0;
			int32 SparsePosition(const FString& Id) const
			{
				const auto* Position = GroupIndex ? GroupIndex->FindPosition(Id) : nullptr;
				return Position && GroupIndex->Shards.IsValidIndex(Position->X)
					? GroupIndex->Shards[Position->X].First + Position->Y : INDEX_NONE;
			}
			void InitializeSparseRows()
			{
				if (!GroupIndex || GroupIndex->Shards.IsEmpty()) return;
				const auto& Last = GroupIndex->Shards.Last();
				const int64 Count = static_cast<int64>(Last.First) + Last.Count;
				if (Count > 0 && Count <= MAX_int32 && Count <= ResolvedRowsBudget / sizeof(FResolvedRowProofPtr))
					IndexedRows.SetNum(static_cast<int32>(Count));
			}
			void AddSparseRow(FResolvedRowProofPtr Row)
			{
				const int32 Position = SparsePosition(Row->GetLookupKey());
				if (IndexedRows.IsValidIndex(Position)) {
					if (!IndexedRows[Position]) ++IndexedRowCount;
					IndexedRows[Position] = MoveTemp(Row);
				} else Rows.Add(MoveTemp(Row));
			}
			void RemoveSparseRow(const FString& Id)
			{
				Rows.Remove(Id);
				const int32 Position = SparsePosition(Id);
				if (IndexedRows.IsValidIndex(Position) && IndexedRows[Position]) {
					IndexedRows[Position].Reset(); --IndexedRowCount;
				}
			}
			int64 SparseAllocatedBytes() const { return Rows.GetAllocatedSize() + IndexedRows.GetAllocatedSize(); }
			int32 SparseRowCount() const { return Rows.Num() + IndexedRowCount; }
			void ShrinkSparseRows()
			{
				Rows.Shrink();
				if (IndexedRowCount == 0) IndexedRows.Empty();
			}
			FResolvedRowProofPtr FindRow(const FString& Id) const
			{
				if (const auto* Row = Rows.Find(Id)) return *Row;
				const auto* Position = GroupIndex ? GroupIndex->FindPosition(Id) : nullptr;
				const int32 Sparse = Position && GroupIndex->Shards.IsValidIndex(Position->X)
					? GroupIndex->Shards[Position->X].First + Position->Y : INDEX_NONE;
				if (IndexedRows.IsValidIndex(Sparse) && IndexedRows[Sparse]) return IndexedRows[Sparse];
				return Position && Groups.IsValidIndex(Position->X) && Groups[Position->X]
					&& Groups[Position->X]->Rows.IsValidIndex(Position->Y)
					? Groups[Position->X]->Rows[Position->Y] : nullptr;
			}
			template <typename Visitor> void VisitRows(Visitor&& Visit) const
			{
				for (const auto& Row : Rows) Visit(Row->GetLookupKey(), *Row);
				for (const auto& Row : IndexedRows) if (Row) Visit(Row->GetLookupKey(), *Row);
				for (const auto& Group : Groups) if (Group)
					for (const auto& Row : Group->Rows) Visit(Row->Unit->GetUnitId(), *Row);
			}
		};
		static thread_local TSharedPtr<const FResolvedRowsProof> PreviousResolvedRows;
		// A view over this successful resolver generation, not another set of maps.
		// Sparse overrides remain in FResolvedSemanticWorkspace for parsed/rebound rows.
		class FResolvedRowBindings final : public FResolvedWorkspaceUnitBindings
		{
		public:
			explicit FResolvedRowBindings(TSharedRef<const FResolvedRowsProof> InRows, bool bPinCurrentBacking = true) : Rows(MoveTemp(InRows)) {
				if (bPinCurrentBacking) Rows->VisitRows([&](const FString&, const FResolvedRowProof& Row) {
					if (Row.SourceDocument && !Row.SourceDocument->RetainCurrentBacking(CurrentSourceBacking)) bComplete = false;
				});
			}
			bool IsComplete() const { return bComplete; }
			const FSourceProvenance* FindSource(const FString& Id) const override {
				const auto Row = Rows->FindRow(Id);
				return Row && Row->Source.IsSet() ? &Row->Source.GetValue() : nullptr;
			}
			const FSemanticUnit* FindBaseline(const FString& Id) const override {
				const auto Row = Rows->FindRow(Id);
				return Row && Row->Baseline.IsSet() ? &Row->Baseline.GetValue() : nullptr;
			}
			FString ReadBaselineHash(const FString& Id) const override {
				const auto Row = Rows->FindRow(Id);
				return Row && Row->Baseline.IsSet() ? Row->Baseline->GetPublicRowHash() : FString();
			}
			FHjsonDocumentPtr FindSourceDocument(const FString& Id) const override {
				if (const auto* Existing = CurrentDocuments.Find(Id)) return *Existing;
				const auto Row = Rows->FindRow(Id);
				const auto Document = Row && Row->SourceDocument ? Row->SourceDocument->Pin() : nullptr;
				// FindValueNode returns a borrowed node, including the projected root.
				// Its materialized wrapper must therefore live with this result view.
				if (Document) CurrentDocuments.Add(Id, Document);
				return Document;
			}
			void CopySources(TMap<FString, FSourceProvenance>& Out) const override {
				Rows->VisitRows([&](const FString& Id, const FResolvedRowProof& Row) { if (Row.Source.IsSet()) Out.Add(Id, Row.Source.GetValue()); });
			}
			void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const override {
				Rows->VisitRows([&](const FString& Id, const FResolvedRowProof& Row) { if (Row.Baseline.IsSet()) Out.Add(Id, Row.Baseline.GetValue()); });
			}
		private:
			TSharedRef<const FResolvedRowsProof> Rows;
			TArray<FHjsonNodeRef> CurrentSourceBacking;
			mutable TMap<FString, FHjsonDocumentPtr> CurrentDocuments;
			bool bComplete = true;
		};
		struct FManifestIndexProof
		{
			FString Root;
			FString ContentHash;
			FString ReferenceMapHash;
			FHjsonDocumentPtr Header;
			TArray<uint8> Utf8;
			TArray<FInventoryRow> Inventory;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
		};
		static thread_local TSharedPtr<const FManifestIndexProof> PreviousManifestIndex;
		struct FInventoryDocumentProof
		{
			FString ContentHash;
			FString MembershipHash;
			FString IdentityHash;
			TArray<FInventoryRow> Rows;
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
			bool bExactParseCharges = false;
			int64 RetainedBytes = 0;
		};
		static thread_local FString InventoryProofRoot;
		static thread_local TMap<FString, TSharedPtr<const FInventoryDocumentProof>> InventoryProofs;
		struct FInventoryIdentityShard
		{
			FString ContentHash;
			FString IdentityHash;
			int32 RowCount = 0;
		};
		struct FInventoryIdentityProof
		{
			FString Root;
			TMap<FString, FInventoryIdentityShard> Shards;
			FString ReferenceMapHash;
		};
		static thread_local TSharedPtr<const FInventoryIdentityProof> PreviousInventoryIdentities;

		struct FGeneratedSourceCandidate {
			FString Root;
			FWorkspacePublication Publication;
			FAuthoringModelPtr Model;
			TSharedPtr<const FResolvedWorkspaceUnitBindings> Bindings;
			TSharedPtr<const FAcceptedWorkspaceInventory> Inventory;
			TMap<FString, FString> FileHashes;
			// Same-publication author input is independent of the published file
			// hash. An unchanged file can still contain an unapplied author edit.
			TSet<FString> PendingAuthorDocuments;
			FString ReferenceMapHash;
			TFunction<bool(const FString&, FAcceptedWorkspaceByteSpan&)> FindEnvelopeSpan;
		};
		static thread_local TSharedPtr<const FGeneratedSourceCandidate> GeneratedSourceCandidate;
		class FGeneratedBaselineHashes final : public FResolvedWorkspaceBaselineHashes
		{
			TSharedRef<const FAcceptedWorkspaceInventory> Inventory;
		public:
			explicit FGeneratedBaselineHashes(TSharedRef<const FAcceptedWorkspaceInventory> InInventory) : Inventory(MoveTemp(InInventory)) {}
			void CopyTo(TMap<FString, FString>& Out) const override {
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=baseline_hash_map_materialization rows=%d"), Inventory->Num());
				Inventory->VisitHashes([&](const FString& Id, const FString& Hash) { Out.Add(Id, Hash); });
			}
		};

		class FLayeredSourceBindings final : public FResolvedWorkspaceUnitBindings {
			TSharedPtr<const FResolvedWorkspaceUnitBindings> Parsed, Generated;
		public:
			FLayeredSourceBindings(TSharedPtr<const FResolvedWorkspaceUnitBindings> InParsed,
				TSharedPtr<const FResolvedWorkspaceUnitBindings> InGenerated) : Parsed(MoveTemp(InParsed)), Generated(MoveTemp(InGenerated)) {}
			const FSourceProvenance* FindSource(const FString& Id) const override {
				if (Parsed) if (const auto* Source = Parsed->FindSource(Id)) return Source;
				return Generated->FindSource(Id);
			}
			const FSemanticUnit* FindBaseline(const FString& Id) const override {
				if (Parsed) if (const auto* Unit = Parsed->FindBaseline(Id)) return Unit;
				return Generated->FindBaseline(Id);
			}
			bool TryBaselineContainsAnyString(const FString& Id, TFunctionRef<bool(const FString&)> Matches,
				TOptional<bool>& Out, FWorkspaceError& Error) const override {
				if (Parsed) {
					if (!Parsed->TryBaselineContainsAnyString(Id, Matches, Out, Error)) return false;
					if (Out.IsSet()) return true;
				}
				return Generated->TryBaselineContainsAnyString(Id, Matches, Out, Error);
			}
			FString ReadBaselineHash(const FString& Id) const override {
				if (Parsed) { FString Hash = Parsed->ReadBaselineHash(Id); if (!Hash.IsEmpty()) return Hash; }
				return Generated->ReadBaselineHash(Id);
			}
			bool TryReadBaselineMembers(const FString& Id, const FString& RootField, const TSet<FString>& Keys,
				FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const override {
				if (Parsed) {
					if (!Parsed->TryReadBaselineMembers(Id, RootField, Keys, Resources, Out, Error)) return false;
					if (Out.IsSet()) return true;
				}
				return Generated->TryReadBaselineMembers(Id, RootField, Keys, Resources, Out, Error);
			}
			bool VisitBaselineMemberNames(const FString& Id, const FString& RootField, FKeyedMemberNameVisitor Visitor,
				FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const override {
				return Generated->VisitBaselineMemberNames(Id, RootField, Visitor, Resources, Error);
			}
			FHjsonDocumentPtr FindSourceDocument(const FString& Id) const override {
				if (Parsed) if (auto Document = Parsed->FindSourceDocument(Id)) return Document;
				return Generated->FindSourceDocument(Id);
			}
			void CopySources(TMap<FString, FSourceProvenance>& Out) const override { Generated->CopySources(Out); if (Parsed) Parsed->CopySources(Out); }
			void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const override { Generated->CopyBaselines(Out); if (Parsed) Parsed->CopyBaselines(Out); }
		};

		bool SameRowProvenance(const FSourceProvenance& A, const FSourceProvenance& B)
		{
			return A.GetSourceUnit().Equals(B.GetSourceUnit(), ESearchCase::CaseSensitive)
				&& A.GetJsonPointer().Equals(B.GetJsonPointer(), ESearchCase::CaseSensitive)
				&& A.GetLine() == B.GetLine() && A.GetColumn() == B.GetColumn()
				&& A.GetByteStart() == B.GetByteStart() && A.GetByteEnd() == B.GetByteEnd()
				&& A.GetOriginalKey().Equals(B.GetOriginalKey(), ESearchCase::CaseSensitive);
		}

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& SourceUnit = FString(),
			const FString& JsonPointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_resolve"));
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, JsonPointer);
			if (ActiveReadState)
				if (const auto* Document = ActiveReadState->Documents.Find(SourceUnit); Document && Document->Parsed)
					if (const auto* Node = Document->Parsed->Find(JsonPointer)) OutError.Locate(Node->GetProvenance());
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}

		bool FailAtNode(FWorkspaceError& OutError, FUeAgentStateSemanticError Error, const FHjsonNode& Node)
		{
			const auto& Source = Node.GetProvenance();
			OutError.SetFailure(MoveTemp(Error), Source.GetSourceUnit(), Source.GetJsonPointer());
			OutError.Locate(Source);
			if (ActiveReadState)
				if (const auto* Document = ActiveReadState->Documents.Find(Source.GetSourceUnit()); Document && Document->Parsed)
					OutError.Locate(*Document->Parsed);
			return false;
		}

		bool FailNode(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Node, TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true)
		{
			return FailAtNode(OutError, HjsonNodeFailure(Code, Reason, TEXT("workspace_resolve"),
				Node, MoveTemp(Expected), bPublicActual), Node);
		}

		bool FailField(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Object, const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true)
		{
			const auto* Node = Object.Get(Field);
			return FailAtNode(OutError, HjsonFieldFailure(Code, Reason, TEXT("workspace_resolve"),
				Object, Field, MoveTemp(Expected), bPublicActual), Node ? *Node : Object);
		}

		bool FailCapturedField(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FHjsonNode& Object, const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = HjsonFieldFailure(Code, Reason, TEXT("workspace_resolve"), Object, Field, MoveTemp(Expected), false);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.Source.SourceFile.Reset();
			Diagnostic.Source.JsonPath.Reset();
			Diagnostic.Source.Line = Diagnostic.Source.Column = 0;
			Diagnostic.PropertyPath = Field;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		struct FNodeDifference
		{
			const FHjsonNode* Actual = nullptr;
			const FHjsonNode* Expected = nullptr;
			bool bCount = false;
		};

		bool EquivalentNode(const FHjsonNode& Left, const FHjsonNode& Right, FNodeDifference* Difference = nullptr)
		{
			const auto Reject = [&](const FHjsonNode* Expected = nullptr, bool bCount = false)
			{
				if (Difference) *Difference = {&Left, Expected, bCount};
				return false;
			};
			if (Left.GetKind() != Right.GetKind())
			{
				return Reject(&Right);
			}
			switch (Left.GetKind())
			{
			case EHjsonNodeKind::Null:
				return true;
			case EHjsonNodeKind::Boolean:
				return Left.GetBoolean() == Right.GetBoolean() || Reject(&Right);
			case EHjsonNodeKind::String:
				return Left.GetText().Equals(Right.GetText(), ESearchCase::CaseSensitive) || Reject(&Right);
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return Left.GetLexeme().Equals(Right.GetLexeme(), ESearchCase::CaseSensitive) || Reject(&Right);
			case EHjsonNodeKind::Array:
			{
				const TArray<FHjsonNodeRef>& L = Left.GetItems();
				const TArray<FHjsonNodeRef>& R = Right.GetItems();
				if (L.Num() != R.Num())
				{
					return Reject(&Right, true);
				}
				for (int32 Index = 0; Index < L.Num(); ++Index)
				{
					if (!EquivalentNode(L[Index].Get(), R[Index].Get(), Difference))
					{
						return false;
					}
				}
				return true;
			}
			case EHjsonNodeKind::Object:
			{
				const TArray<TPair<FString, FHjsonNodeRef>>& L =
					Left.GetEntries();
				const TArray<TPair<FString, FHjsonNodeRef>>& R =
					Right.GetEntries();
				if (L.Num() != R.Num())
				{
					return Reject(&Right, true);
				}
				for (const TPair<FString, FHjsonNodeRef>& Entry : L)
				{
					const FHjsonNode* Other = Right.Get(Entry.Key);
					if (!Other)
					{
						if (Difference) *Difference = {&Entry.Value.Get(), nullptr, false};
						return false;
					}
					if (!EquivalentNode(Entry.Value.Get(), *Other, Difference))
					{
						return false;
					}
				}
				return true;
			}
			default:
				return Reject(&Right);
			}
		}

		bool FailMetadataDifference(FWorkspaceError& OutError, const FString& Reason, const FNodeDifference& Difference)
		{
			auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_envelope_mismatch"), Reason,
				TEXT("workspace_resolve"), *Difference.Actual, nullptr, false);
			Diagnostic.ExpectedEvidence = HjsonDiagnosticValue(Difference.Expected);
			Diagnostic.ExpectedValuePresent = Difference.Expected != nullptr;
			if (Difference.bCount)
			{
				const auto Count = [](const FHjsonNode& Node)
				{ return Node.GetKind() == EHjsonNodeKind::Array ? Node.GetItems().Num() : Node.GetEntries().Num(); };
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Count(*Difference.Actual));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Count(*Difference.Expected));
			}
			return FailAtNode(OutError, MoveTemp(Diagnostic), *Difference.Actual);
		}

		const FHjsonNode* ResolveEnvelopeField(
			const FHjsonNode& Row,
			const FHjsonNode* Envelope,
			const FString& Key,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* RowValue = Row.Get(Key);
			const FHjsonNode* EnvelopeValue =
				Envelope == nullptr ? nullptr : Envelope->Get(Key);
			FNodeDifference Difference;
			if (RowValue != nullptr && EnvelopeValue != nullptr
				&& !EquivalentNode(*RowValue, *EnvelopeValue, &Difference))
			{
				FailMetadataDifference(OutError, FString::Printf(TEXT("Metadata field '%s' differs from its captured workspace value."), *Key), Difference);
				return nullptr;
			}
			return RowValue != nullptr ? RowValue : EnvelopeValue;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			std::initializer_list<TPair<
				EUeAgentResourceDimension,
				int64>> Charges,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			if (!Resources.CollectsUsage()) return true;
			FUeAgentResourceChargeError ResourceError;
			TMap<EUeAgentResourceDimension, int64> Values;
			for (const TPair<
				EUeAgentResourceDimension,
				int64>& ChargeValue : Charges)
			{
				Values.Add(
					ChargeValue.Key,
					ChargeValue.Value);
			}
			return Resources.TryChargeMany(
					Values,
					Path,
					ResourceError)
				|| FailResource(OutError, ResourceError);
		}

		FString Sha256(const uint8* Bytes, const int64 Num)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			ComputeSha256(Bytes, Num, Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			return Sha256(Bytes.GetData(), Bytes.Num());
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256(Bytes);
		}

		// Same terminated-text SHA as Sha256Text, fed in the original grammar's
		// order. Every variable field is separated by ASCII punctuation, so UTF-16
		// surrogate pairs cannot straddle fields. Preserve the historical first-NUL
		// cutoff as well, rather than silently changing a workspace fingerprint.
		struct FWorkspaceTextHash
		{
			SHA256_CTX State;
			bool bTerminated = false;
			FWorkspaceTextHash() { SHA256_Init(&State); }
			void Append(const FString& Text) {
				if (bTerminated) return;
				const int32 Length = FCString::Strlen(*Text);
				const FTCHARToUTF8 Utf8(*Text, Length);
				if (Utf8.Length()) SHA256_Update(&State, Utf8.Get(), Utf8.Length());
				bTerminated = Length != Text.Len();
			}
			void Append(const TCHAR* Text) {
				if (bTerminated) return;
				const FTCHARToUTF8 Utf8(Text);
				if (Utf8.Length()) SHA256_Update(&State, Utf8.Get(), Utf8.Length());
			}
			void Append(const FInventoryDigest& Digest) {
				if (bTerminated) return;
				if (!Digest.bCompact) { Append(Digest.Original); return; }
				ANSICHAR Text[7 + SHA256_DIGEST_LENGTH * 2];
				FMemory::Memcpy(Text, "sha256:", 7);
				static const ANSICHAR Hex[] = "0123456789abcdef";
				for (int32 I = 0; I < SHA256_DIGEST_LENGTH; ++I) {
					Text[7 + I * 2] = Hex[Digest.Bytes[I] >> 4]; Text[8 + I * 2] = Hex[Digest.Bytes[I] & 15];
				}
				SHA256_Update(&State, Text, sizeof(Text));
			}
			FString Finish() {
				uint8 Digest[SHA256_DIGEST_LENGTH]; SHA256_Final(Digest, &State);
				static const TCHAR Hex[] = TEXT("0123456789abcdef");
				FString Result(TEXT("sha256:")); Result.Reserve(7 + SHA256_DIGEST_LENGTH * 2);
				for (const uint8 Byte : Digest) { Result.AppendChar(Hex[Byte >> 4]); Result.AppendChar(Hex[Byte & 15]); }
				return Result;
			}
		};

		// Internal proof of parser input, independent of source coordinates. This
		// does not replace the public semantic/edit hashes or normalize decimals.
		FRowInputDigest HashRowInputNode(const FHjsonNode& Root)
		{
			SHA256_CTX Hash;
			SHA256_Init(&Hash);
			const auto AddText = [&](const FString& Text) {
				const int32 Length = Text.Len();
				SHA256_Update(&Hash, &Length, sizeof(Length));
				SHA256_Update(&Hash, *Text, Length * sizeof(TCHAR));
			};
			TArray<const FHjsonNode*> Stack{&Root};
			while (!Stack.IsEmpty())
			{
				const FHjsonNode& Node = *Stack.Pop(EAllowShrinking::No);
				const uint8 Kind = static_cast<uint8>(Node.GetKind());
				SHA256_Update(&Hash, &Kind, sizeof(Kind));
				switch (Node.GetKind())
				{
				case EHjsonNodeKind::String: AddText(Node.GetText()); break;
				case EHjsonNodeKind::Integer:
				case EHjsonNodeKind::Decimal: AddText(Node.GetLexeme()); break;
				case EHjsonNodeKind::Boolean: {
					const uint8 Value = Node.GetBoolean() ? 1 : 0;
					SHA256_Update(&Hash, &Value, sizeof(Value)); break;
				}
				case EHjsonNodeKind::Array: {
					const int32 Count = Node.GetItems().Num();
					SHA256_Update(&Hash, &Count, sizeof(Count));
					for (int32 I = Count - 1; I >= 0; --I) Stack.Add(&Node.GetItems()[I].Get());
					break;
				}
				case EHjsonNodeKind::Object: {
					const int32 Count = Node.GetEntries().Num();
					SHA256_Update(&Hash, &Count, sizeof(Count));
					for (const auto& Field : Node.GetEntries()) AddText(Field.Key);
					for (int32 I = Count - 1; I >= 0; --I) Stack.Add(&Node.GetEntries()[I].Value.Get());
					break;
				}
				case EHjsonNodeKind::Null: break;
				}
			}
			FRowInputDigest Result;
			SHA256_Final(Result.Bytes, &Hash);
			return Result;
		}

		FRowInputDigest HashRowInputs(const FInventoryDigest& InventoryHash,
			const FRowInputDigest& Authored, const FRowInputDigest& Envelope)
		{
			SHA256_CTX Hash;
			SHA256_Init(&Hash);
			InventoryHash.UpdateTextHash(Hash);
			SHA256_Update(&Hash, Authored.Bytes, sizeof(Authored.Bytes));
			SHA256_Update(&Hash, Envelope.Bytes, sizeof(Envelope.Bytes));
			FRowInputDigest Result;
			SHA256_Final(Result.Bytes, &Hash);
			return Result;
		}

		struct FReferenceProofPool
		{
			TMap<FString, FReferenceLookupProofPtr> Lookups;
			TSet<const FReferenceLookupProof*> RetainedLookupIdentities;
			int64 BackingBytes = 0;
			int64 Edges = 0;
			static bool SameBinding(const FReferenceLookupProof& A, const FReferenceLookupProof& B)
			{
				return A.Key.Equals(B.Key, ESearchCase::CaseSensitive) && A.Result.IsSet() == B.Result.IsSet()
					&& (!A.Result.IsSet() || A.Result.GetValue().Equals(B.Result.GetValue(), ESearchCase::CaseSensitive));
			}
			bool TryAdoptGroup(const FResolvedRowGroup& Group, const bool bCommit = true)
			{
				if (Group.ReferenceEdges > 2 * 1024 * 1024 - Edges) return false;
				int64 AddedBytes = 0;
				for (const auto& Lookup : Group.Lookups) {
					if (!Lookup) return false;
					if (const auto* Existing = Lookups.Find(Lookup->Key)) {
						if (!SameBinding(**Existing, *Lookup)) return false;
					}
					if (!RetainedLookupIdentities.Contains(Lookup.Get())) AddedBytes += sizeof(FReferenceLookupProof) + 128
						+ 2LL * (Lookup->Key.Len() + 1) * sizeof(TCHAR)
						+ (Lookup->Result.IsSet() ? (Lookup->Result.GetValue().Len() + 1LL) * sizeof(TCHAR) : 0);
				}
				if (BackingBytes + AddedBytes > 16LL * 1024 * 1024) return false;
				if (!bCommit) return true;
				for (const auto& Lookup : Group.Lookups) {
					Lookups.FindOrAdd(Lookup->Key, Lookup); RetainedLookupIdentities.Add(Lookup.Get());
				}
				BackingBytes += AddedBytes; Edges += Group.ReferenceEdges;
				return true;
			}
			// Equal bindings may have distinct immutable objects after a changed row
			// is reparsed. Charge every retained object identity once, rather than
			// rewriting an unchanged group merely to intern equal values again.
			bool TryAdopt(const FResolvedRowProof& Row)
			{
				if (!Row.bCompleteReferenceLookups || Edges + Row.ReferenceLookups.Num() > 2 * 1024 * 1024) return false;
				int64 AddedBytes = 0;
				for (const auto& Lookup : Row.ReferenceLookups) {
					if (!Lookup) return false;
					if (const auto* Existing = Lookups.Find(Lookup->Key)) {
						if (!SameBinding(**Existing, *Lookup)) return false;
					}
					if (!RetainedLookupIdentities.Contains(Lookup.Get())) {
						AddedBytes += sizeof(FReferenceLookupProof) + 128
							+ 2LL * (Lookup->Key.Len() + 1) * sizeof(TCHAR)
							+ (Lookup->Result.IsSet() ? (Lookup->Result.GetValue().Len() + 1LL) * sizeof(TCHAR) : 0);
					}
				}
				if (BackingBytes + AddedBytes > 16LL * 1024 * 1024) return false;
				for (const auto& Lookup : Row.ReferenceLookups) {
					Lookups.FindOrAdd(Lookup->Key, Lookup); RetainedLookupIdentities.Add(Lookup.Get());
				}
				BackingBytes += AddedBytes;
				Edges += Row.ReferenceLookups.Num();
				return true;
			}
			FReferenceLookupProofPtr Observe(const FString& Key, const FString* Result)
			{
				if (const auto* Existing = Lookups.Find(Key)) return *Existing;
				const int64 Bytes = sizeof(FReferenceLookupProof) + 128
					+ 2LL * (Key.Len() + 1) * sizeof(TCHAR)
					+ (Result ? (Result->Len() + 1LL) * sizeof(TCHAR) : 0);
				if (BackingBytes + Bytes > 16LL * 1024 * 1024) return nullptr;
				auto Proof = MakeShared<FReferenceLookupProof>();
				Proof->Key = Key;
				if (Result) Proof->Result = *Result;
				BackingBytes += Bytes;
				Lookups.Add(Key, Proof);
				RetainedLookupIdentities.Add(&Proof.Get());
				return Proof;
			}
		};
		class FGeneratedReferenceLookup
		{
			const FWorkspacePlacementView* Mapping;
			const FAcceptedWorkspaceInventory* Inventory = nullptr;
			mutable TMap<FString, TSharedPtr<const FString>> Resolved;
			// View pairs are temporary, but both strings belong to the immutable
			// document retained by the generated Layout for this entire resolve.
			struct FEntry { uint32 Hash; const FString* PublicRef; const FString* UnitId; };
			mutable TArray<FEntry> Entries;
			mutable bool bBuilt = false;
		public:
			explicit FGeneratedReferenceLookup(const FWorkspacePlacementView* InMapping) : Mapping(InMapping) {}
			explicit FGeneratedReferenceLookup(const FAcceptedWorkspaceInventory* InInventory)
				: Mapping(InInventory && InInventory->GetGeneratedLayout()
					? &InInventory->GetGeneratedLayout()->GetMachineManifest().PublicRefMapping : nullptr), Inventory(InInventory) {}
			const FString* Find(const FString& Ref) const {
				if (!Mapping && Inventory) {
					// Generic strings are not reference-lookup requests. Only the
					// original public-reference grammar may touch a cold directory.
					const int32 Colon = Ref.Find(TEXT(":"), ESearchCase::CaseSensitive);
					if (Colon <= 0) return nullptr;
					bool bReferenceKind = false;
					for (uint8 Kind = 0; Kind <= static_cast<uint8>(ESemanticUnitKind::OpaqueExtension); ++Kind)
						bReferenceKind |= Ref.Left(Colon) == LexToString(static_cast<ESemanticUnitKind>(Kind)).Replace(TEXT("_state"), TEXT(""));
					if (!bReferenceKind) return nullptr;
					if (const auto* Previous = Resolved.Find(Ref)) return Previous->Get();
					const auto Id = Inventory->FindPublicRef(Ref);
					auto& Value = Resolved.Add(Ref);
					if (Id.IsSet()) Value = MakeShared<const FString>(Id.GetValue());
					return Value.Get();
				}
				if (!Mapping) return nullptr;
				if (!bBuilt) {
					Entries.Reserve(Mapping->Num());
					for (const auto& Pair : *Mapping) Entries.Add({GetTypeHash(Pair.Value), &Pair.Value, &Pair.Key});
					Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Hash < B.Hash; });
					bBuilt = true;
				}
				const uint32 Hash = GetTypeHash(Ref);
				int32 First = 0, Last = Entries.Num();
				while (First < Last) { const int32 Mid = First + (Last - First) / 2; if (Entries[Mid].Hash < Hash) First = Mid + 1; else Last = Mid; }
				for (int32 I = First; I < Entries.Num() && Entries[I].Hash == Hash; ++I)
					if (*Entries[I].PublicRef == Ref) return Entries[I].UnitId;
				return nullptr;
			}
		};
		struct FRowReferenceRecorder
		{
			const TMap<FString, FString>* Map = nullptr;
			FReferenceProofPool* Pool = nullptr;
			const FGeneratedReferenceLookup* Generated = nullptr;
			TArray<FReferenceLookupProofPtr> Lookups;
			bool bComplete = true;
			void Observe(const FString& Key, const FString* Result)
			{
				if (!bComplete) return;
				const auto Proof = Pool->Observe(Key, Result);
				if (!Proof || Pool->Edges >= 2 * 1024 * 1024) { bComplete = false; Lookups.Reset(); return; }
				if (!Lookups.Contains(Proof)) { Lookups.Add(Proof); ++Pool->Edges; }
			}
		};
		static thread_local FRowReferenceRecorder* ActiveRowReferenceRecorder = nullptr;
		const FString* FindObservedPublicRef(const TMap<FString, FString>* Map, const FString& Key)
		{
			const FString* Result = Map ? Map->Find(Key) : nullptr;
			if (ActiveRowReferenceRecorder && Map == ActiveRowReferenceRecorder->Map) {
				if (!Result && ActiveRowReferenceRecorder->Generated) Result = ActiveRowReferenceRecorder->Generated->Find(Key);
				ActiveRowReferenceRecorder->Observe(Key, Result);
			}
			return Result;
		}

		bool ReferenceLookupSetMatches(const TArray<FReferenceLookupProofPtr>& Lookups,
			const TMap<FString, FString>& Current, const bool bSameReferenceMap,
			TMap<const FReferenceLookupProof*, bool>& Checked)
		{
			if (bSameReferenceMap) return true;
			for (const auto& Lookup : Lookups)
			{
				bool* Match = Checked.Find(Lookup.Get());
				if (!Match) {
					const FString* Value = Current.Find(Lookup->Key);
					Match = &Checked.Add(Lookup.Get(), Value
						? Lookup->Result.IsSet() && Value->Equals(Lookup->Result.GetValue(), ESearchCase::CaseSensitive)
						: !Lookup->Result.IsSet());
				}
				if (!*Match) return false;
			}
			return true;
		}
		bool ReferenceLookupsMatch(const FResolvedRowProof& Row,
			const TMap<FString, FString>& Current, const bool bSameReferenceMap,
			TMap<const FReferenceLookupProof*, bool>& Checked)
		{
			return bSameReferenceMap || (Row.bCompleteReferenceLookups
				&& ReferenceLookupSetMatches(Row.ReferenceLookups, Current, false, Checked));
		}

		// Pure successful material, separate from the optional heavy typed-row
		// proof. No AST, unit, source binding, native object or file lease survives
		// here. Bump the private version for parser/material/charge changes.
		constexpr uint32 DerivedRowVersion = 3;
		struct FDerivedRowMaterial
		{
			FRowInputDigest Key;
			int32 Position = INDEX_NONE;
			uint8 Content[SHA256_DIGEST_LENGTH] = {}, Baseline[SHA256_DIGEST_LENGTH] = {}, Edit[SHA256_DIGEST_LENGTH] = {};
			uint8 Flags = 0; // valid content, captured baseline, edit material
			static FString Text(const uint8* Bytes) {
				FInventoryDigest Digest; FMemory::Memcpy(Digest.Bytes, Bytes, sizeof(Digest.Bytes));
				Digest.bCompact = true; return Digest.ToString();
			}
		};
		void AddDerivedKeyText(SHA256_CTX& Hash, const FString& Text)
		{
			const int32 Length = Text.Len(); SHA256_Update(&Hash, &Length, sizeof(Length));
			SHA256_Update(&Hash, *Text, static_cast<int64>(Length) * sizeof(TCHAR));
		}
		FRowInputDigest DerivedRowKey(const FReadRowProof& Read, const FInventoryRow& Inventory,
			const FHjsonNode& Row, const FHjsonNode* Envelope, const FRowReferenceRecorder& References)
		{
			SHA256_CTX Hash; SHA256_Init(&Hash);
			SHA256_Update(&Hash, Read.Input.Bytes, sizeof(Read.Input.Bytes));
			AddDerivedKeyText(Hash, Inventory.Syntax->GetUnitId());
			const auto Kind = Inventory.Syntax->UnitKind; SHA256_Update(&Hash, &Kind, sizeof(Kind));
			AddDerivedKeyText(Hash, Inventory.Syntax->GetUnitPointer());
			AddDerivedKeyText(Hash, Row.GetProvenance().GetSourceUnit());
			AddDerivedKeyText(Hash, Row.GetProvenance().GetJsonPointer());
			AddDerivedKeyText(Hash, Envelope ? Envelope->GetProvenance().GetSourceUnit() : FString());
			AddDerivedKeyText(Hash, Envelope ? Envelope->GetProvenance().GetJsonPointer() : FString());
			const int32 Count = References.Lookups.Num(); SHA256_Update(&Hash, &Count, sizeof(Count));
			for (const auto& Lookup : References.Lookups) {
				AddDerivedKeyText(Hash, Lookup->Key);
				const uint8 Present = Lookup->Result.IsSet() ? 1 : 0; SHA256_Update(&Hash, &Present, sizeof(Present));
				if (Present) AddDerivedKeyText(Hash, Lookup->Result.GetValue());
			}
			FRowInputDigest Result; SHA256_Final(Result.Bytes, &Hash); return Result;
		}
		class FDerivedRowMaterials
		{
			static constexpr int32 BlockRows = 4096;
			static constexpr int64 PendingBudget = 64LL * 1024 * 1024;
			FString Scope;
			FString Group;
			bool bDirty = false;
			TMap<FRowInputDigest, FDerivedRowMaterial> Rows;
			TSet<FRowInputDigest> ObservedRows;
			TArray<FHjsonDerivedBlob> Pending;
			int64 PendingBytes = 0;
			void Account(const int64 Scratch = 0) {
				PeakBytes = FMath::Max(PeakBytes, static_cast<int64>(sizeof(*this) + Scope.GetAllocatedSize()
					+ Group.GetAllocatedSize() + Rows.GetAllocatedSize() + ObservedRows.GetAllocatedSize() + Pending.GetAllocatedSize()) + PendingBytes + Scratch);
			}
			FString BlockKey() const { return Scope + TEXT("|") + Group; }
			void Seal()
			{
				// Retire rows not observed in this current group. Old membership is
				// never an authority for the current inventory or semantic model.
				if (ObservedRows.Num() != Rows.Num()) bDirty = true;
				if (!bDirty) return;
				const double Started = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { SealSeconds += FPlatformTime::Seconds() - Started; };
				FHjsonDerivedBlob Blob; Blob.Key = BlockKey();
				const int32 Count = ObservedRows.Num();
				constexpr int32 RecordBytes = sizeof(int32) + 1 + 4 * SHA256_DIGEST_LENGTH;
				Blob.Bytes.Reserve(2 * sizeof(uint32) + Count * RecordBytes);
				auto Raw = [&](const void* Data, int32 Size) { Blob.Bytes.Append(static_cast<const uint8*>(Data), Size); };
				Raw(&DerivedRowVersion, sizeof(DerivedRowVersion)); Raw(&Count, sizeof(Count));
				for (const auto& Key : ObservedRows) { const auto& Row = Rows.FindChecked(Key);
					Raw(&Row.Position, sizeof(Row.Position)); Raw(&Row.Flags, sizeof(Row.Flags)); Raw(Row.Key.Bytes, sizeof(Row.Key.Bytes));
					Raw(Row.Content, sizeof(Row.Content)); Raw(Row.Baseline, sizeof(Row.Baseline)); Raw(Row.Edit, sizeof(Row.Edit));
				}
				Account(Blob.Bytes.GetAllocatedSize() + Blob.Key.GetAllocatedSize());
				const int64 Bytes = Blob.Bytes.GetAllocatedSize() + Blob.Key.GetAllocatedSize();
				if (Bytes <= PendingBudget - PendingBytes) {
					PendingBytes += Bytes; Pending.Add(MoveTemp(Blob)); SealedRows += Count;
				} else RejectedRows += Count;
				bDirty = false; Account();
			}
			void Select(const FString& StableGroup)
			{
				if (StableGroup == Group) return;
				Seal(); Group = StableGroup; Rows.Reset(); ObservedRows.Reset();
				const double Started = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { LoadSeconds += FPlatformTime::Seconds() - Started; };
				TArray<uint8> Bytes;
				if (!TryRestoreHjsonDerivedBlob(BlockKey(), Bytes)) { ++BlockMisses; Account(); return; }
				LoadedBytes += Bytes.Num(); Account(Bytes.GetAllocatedSize());
				int32 Offset = 0; bool bValid = true;
				auto Raw = [&](void* Data, int32 Size) {
					if (!bValid || Size > Bytes.Num() - Offset) { bValid = false; return; }
					FMemory::Memcpy(Data, Bytes.GetData() + Offset, Size); Offset += Size;
				};
				uint32 Version = 0; int32 Count = 0;
				Raw(&Version, sizeof(Version)); Raw(&Count, sizeof(Count));
				bValid = bValid && Version == DerivedRowVersion && Count >= 0 && Count <= BlockRows;
				for (int32 I = 0; I < Count && bValid; ++I) {
					FDerivedRowMaterial Row;
					Raw(&Row.Position, sizeof(Row.Position)); Raw(&Row.Flags, sizeof(Row.Flags)); Raw(Row.Key.Bytes, sizeof(Row.Key.Bytes));
					Raw(Row.Content, sizeof(Row.Content)); Raw(Row.Baseline, sizeof(Row.Baseline)); Raw(Row.Edit, sizeof(Row.Edit));
					bValid = bValid && Row.Position >= 0 && (Row.Flags & 1) && !(Row.Flags & ~7);
					if (bValid && !Rows.Contains(Row.Key)) Rows.Add(Row.Key, Row); else bValid = false;
				}
				if (!bValid || Offset != Bytes.Num()) {
					Rows.Reset(); ++BlockRejected;
				} else { ++BlockHits; LoadedRows += Count; }
				Account(Bytes.GetAllocatedSize());
			}
		public:
			int64 Queries = 0, Hits = 0, Misses = 0, LoadedRows = 0, SealedRows = 0, RejectedRows = 0;
			int64 RelocatedHits = 0;
			int64 RowHashHits = 0, BaselineHashHits = 0, EditHashHits = 0;
			int64 LoadedBytes = 0, PeakBytes = 0, BlockHits = 0, BlockMisses = 0, BlockRejected = 0;
			double KeySeconds = 0, LookupSeconds = 0, LoadSeconds = 0, SealSeconds = 0, StoreSeconds = 0, ChargeSeconds = 0;
			FHjsonDerivedStoreStats StoreStats;
			explicit FDerivedRowMaterials(FString InScope) : Scope(MoveTemp(InScope)) {}
			const FDerivedRowMaterial* Find(const FString& StableGroup, const int32 Index, const FRowInputDigest& Key) {
				++Queries; Select(StableGroup);
				const double Started = FPlatformTime::Seconds();
				ON_SCOPE_EXIT { LookupSeconds += FPlatformTime::Seconds() - Started; };
				if (const auto* Row = Rows.Find(Key)) {
					ObservedRows.Add(Key); ++Hits; if (Row->Position != Index) ++RelocatedHits; return Row;
				}
				++Misses; return nullptr;
			}
			void Remember(const FString& StableGroup, const int32 Index, const FDerivedRowMaterial& Row) {
				Select(StableGroup);
				if (!Rows.Contains(Row.Key) && Rows.Num() >= BlockRows) { ++RejectedRows; return; }
				auto& Stored = Rows.Add(Row.Key, Row); Stored.Position = Index;
				ObservedRows.Add(Row.Key); bDirty = true;
			}
			void Finish() {
				Seal(); const double Started = FPlatformTime::Seconds();
				StoreStats = RememberHjsonDerivedBlobs(Pending);
				StoreSeconds = FPlatformTime::Seconds() - Started;
				Account(StoreStats.ScratchBytes); Pending.Empty(); PendingBytes = 0; Rows.Empty(); ObservedRows.Empty();
			}
		};

		bool TryRebindRowSource(const FResolvedRowProof& Prior, const FHjsonNode& Current,
			FHjsonDocumentPtr& OutSource)
		{
			if (!Prior.SourceDocument) { OutSource.Reset(); return true; }
			const auto PriorDocument = Prior.SourceDocument->Pin();
			if (!PriorDocument) return false;
			if (!Prior.Source.IsSet()) return false;
			const FString& RootPointer = Current.GetProvenance().GetJsonPointer();
			auto FindCurrent = [&](const FString& Pointer) -> const FHjsonNode* {
				if (Pointer == RootPointer) return &Current;
				if (RootPointer == TEXT("/")) return Current.Find(Pointer);
				return Pointer.StartsWith(RootPointer + TEXT("/"), ESearchCase::CaseSensitive)
					? Current.Find(Pointer.Mid(RootPointer.Len())) : nullptr;
			};
			TFunction<bool(const FHjsonNodeRef&, int32, FHjsonNodePtr&)> Rebind;
			Rebind = [&](const FHjsonNodeRef& Node, const int32 Depth, FHjsonNodePtr& Result) {
				if (Depth > 128 || Node->GetProvenance().GetSourceUnit()
					!= Prior.Source->GetSourceUnit()) return false;
				const auto* Found = FindCurrent(Node->GetProvenance().GetJsonPointer());
				if (!Found) return false;
				if (Node->GetValueStorageIdentity() == Found->GetValueStorageIdentity()
					|| HashRowInputNode(Node.Get()) == HashRowInputNode(*Found)) {
					Result = MakeShared<FHjsonNode>(*Found); return true;
				}
				TArray<FHjsonNodeRef> Items;
				TArray<TPair<FString, FHjsonNodeRef>> Fields;
				for (const auto& Item : Node->GetItems()) {
					FHjsonNodePtr Child; if (!Rebind(Item, Depth + 1, Child)) return false;
					Items.Add(Child.ToSharedRef());
				}
				for (const auto& Field : Node->GetEntries()) {
					FHjsonNodePtr Child; if (!Rebind(Field.Value, Depth + 1, Child)) return false;
					Fields.Emplace(Field.Key, Child.ToSharedRef());
				}
				Result = MakeShared<FHjsonNode>(Node->GetKind(), Found->GetProvenance(), Node->GetLexeme(),
					Node->GetText(), Node->GetBoolean(), MoveTemp(Items), MoveTemp(Fields));
				return true;
			};
			FHjsonNodePtr Root;
			if (!Rebind(PriorDocument->GetRoot(), 0, Root)) return false;
			OutSource = MakeShared<FHjsonDocument>(Current.GetProvenance().GetSourceUnit(), Root.ToSharedRef());
			return true;
		}

		FString HashReferenceMap(const TArray<FInventoryRow>& Inventory)
		{
			TArray<const FInventoryRow*> Sorted;
			for (const auto& Row : Inventory) Sorted.Add(&Row);
			Sorted.Sort([](const FInventoryRow& A, const FInventoryRow& B) { return A.Syntax->GetPublicRef() < B.Syntax->GetPublicRef(); });
			FString Material;
			for (const auto* Row : Sorted)
				Material += FString::Printf(TEXT("%d:%s%d:%s"), Row->Syntax->GetPublicRef().Len(), *Row->Syntax->GetPublicRef(), Row->Syntax->GetUnitId().Len(), *Row->Syntax->GetUnitId());
			return Sha256Text(Material);
		}

		bool TryCanonicalValue(const FHjsonNode& Root, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefToUnitId = nullptr,
			bool bInterpretTypedValues = true, bool* OutHasContentReference = nullptr);

		bool TrySerializeEditNode(
			const FHjsonNode& Node,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError,
			TArray<uint8>& OutBytes, bool* OutHasContentReference = nullptr)
		{
			OutBytes.Reset();
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalValue(Node, Resources, Value, OutError, nullptr, false, OutHasContentReference))
			{
				return false;
			}
			if (!Value.IsValid()) {
				OutError.SetSourceFailure(TEXT("semantic_edit_value_missing"), TEXT("The edit node has no canonical value to serialize."),
					Node.GetProvenance().GetSourceUnit(), Node.GetProvenance().GetJsonPointer());
				OutError.Locate(Node.GetProvenance()); return false;
			}
			if (!TrySerializeSemanticHjsonObject(*Value, OutBytes, OutError, Node.GetProvenance().GetSourceUnit())) {
				OutError.Locate(Node.GetProvenance()); return false;
			}
			return true;
		}

		const FHjsonNode* RequiredNode(
			const FHjsonNode& Object,
			const FString& Key,
			FWorkspaceError& OutError, const FString& ExpectedType = FString())
		{
			const FHjsonNode* Result = Object.Get(Key);
			if (Result == nullptr)
			{
				FailField(OutError, TEXT("semantic_workspace_manifest_field_missing"),
					FString::Printf(TEXT("Required workspace metadata field '%s' is missing."), *Key), Object, Key,
					ExpectedType.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(ExpectedType), false);
			}
			return Result;
		}

		bool ReadMetadataStringText(FStringView Text, bool bRequired, FStringView& OutValue)
		{
			OutValue = Text.TrimStartAndEnd();
			return !bRequired || !OutValue.IsEmpty();
		}

		bool ReadMetadataStringNode(const FHjsonNode& Object, FStringView Key,
			const FHjsonNode* Node, bool bRequired, FStringView& OutValue, FWorkspaceError& OutError)
		{
			OutValue = {};
			if (!Node) {
				if (!bRequired) return true;
				const FString Name(Key);
				return FailField(OutError, TEXT("semantic_workspace_manifest_field_missing"),
					FString::Printf(TEXT("Required workspace metadata field '%s' is missing."), *Name), Object, Name,
					MakeShared<FJsonValueString>(TEXT("string")), false);
			}
			if (Node->GetKind() != EHjsonNodeKind::String)
			{
				const FString Name(Key);
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					bRequired ? FString::Printf(TEXT("Workspace metadata field '%s' must be a string."), *Name)
						: FString::Printf(TEXT("Optional workspace metadata field '%s' must be a string when present."), *Name), *Node,
					MakeShared<FJsonValueString>(TEXT("string")), false);
			}
			if (!ReadMetadataStringText(Node->GetText(), bRequired, OutValue))
			{
				const FString Name(Key);
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Workspace metadata field '%s' must not be empty."), *Name), *Node,
					MakeShared<FJsonValueString>(TEXT("non-empty string")));
			}
			return true;
		}

		bool ReadOwnedMetadataString(const FHjsonNode& Object, FStringView Key,
			const FHjsonNode* Node, bool bRequired, FString& OutValue, FWorkspaceError& OutError)
		{
			if (!bRequired) OutValue.Reset();
			FStringView Value;
			const bool bRead = ReadMetadataStringNode(Object, Key, Node, bRequired, Value, OutError);
			// Preserve the old output-on-error behavior: a required empty string
			// clears its output; a missing/non-string required field leaves it.
			if (bRead || (Node && Node->GetKind() == EHjsonNodeKind::String)) OutValue = FString(Value);
			return bRead;
		}

		bool RequiredString(const FHjsonNode& Object, const FString& Key, FString& OutValue, FWorkspaceError& OutError)
		{ return ReadOwnedMetadataString(Object, FStringView(Key), Object.Get(Key), true, OutValue, OutError); }

		bool OptionalString(const FHjsonNode& Object, const FString& Key, FString& OutValue, FWorkspaceError& OutError)
		{ return ReadOwnedMetadataString(Object, FStringView(Key), Object.Get(Key), false, OutValue, OutError); }

		bool RequiredInteger(
			const FHjsonNode& Object,
			const FString& Key,
			int64 Minimum,
			int64& OutValue,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Node =
				RequiredNode(Object, Key, OutError, TEXT("integer"));
			if (Node == nullptr)
			{
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::Integer
				|| !LexTryParseString(
					OutValue,
					*Node->GetLexeme())
				|| OutValue < Minimum)
			{
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					FString::Printf(TEXT("Workspace metadata field '%s' must be a signed 64-bit integer at least %lld."), *Key, Minimum), *Node,
					MakeShared<FJsonValueString>(FString::Printf(TEXT("int64 >= %lld"), Minimum)), false);
			}
			return true;
		}

		bool ReadStringArray(
			const FHjsonNode& Object,
			const FString& Key,
			bool bRequired,
			TArray<FString>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (Node == nullptr)
			{
				if (!bRequired) return true;
				RequiredNode(Object, Key, OutError, TEXT("array"));
				return false;
			}
			if (Node->GetKind() != EHjsonNodeKind::Array)
			{
				return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					TEXT("The workspace metadata field must be an array."), *Node, MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			TSet<FString> Seen;
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
						TEXT("Each entry in this workspace metadata array must be a string."), Item.Get(), MakeShared<FJsonValueString>(TEXT("string")), false);
				}
				FString Value = Item->GetText().TrimStartAndEnd();
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return FailNode(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
						Value.IsEmpty() ? TEXT("Workspace metadata array entries must not be empty.")
							: TEXT("This workspace metadata array entry duplicates an earlier entry."), Item.Get(),
						MakeShared<FJsonValueString>(TEXT("non-empty unique string")), false);
				}
				Seen.Add(Value);
				OutValues.Add(MoveTemp(Value));
			}
			return true;
		}

		bool ParseUnitKind(
			FStringView Text,
			ESemanticUnitKind& OutKind)
		{
			// This is the fixed enum vocabulary from the original codec, not a
			// memo keyed by a document/model or a cache admission condition.
			static const TArray<FString> Names = [] {
				TArray<FString> Result;
				for (uint8 Index = 0; Index <= static_cast<uint8>(ESemanticUnitKind::OpaqueExtension); ++Index)
					Result.Add(LexToString(static_cast<ESemanticUnitKind>(Index)));
				return Result;
			}();
			for (int32 Index = 0; Index < Names.Num(); ++Index)
			{
				// FString's previous operator== is explicitly case insensitive.
				if (Text.Equals(FStringView(Names[Index]), ESearchCase::IgnoreCase))
				{
					OutKind = static_cast<ESemanticUnitKind>(Index);
					return true;
				}
			}
			return false;
		}
		bool ParseUnitKind(const FString& Text, ESemanticUnitKind& OutKind)
		{ return ParseUnitKind(FStringView(Text), OutKind); }

		TSharedPtr<FJsonValue> UnitKindChoices()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (uint8 Index = 0; Index <= static_cast<uint8>(ESemanticUnitKind::OpaqueExtension); ++Index)
				Values.Add(MakeShared<FJsonValueString>(LexToString(static_cast<ESemanticUnitKind>(Index))));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}

		bool ParseMode(
			const FString& Text,
			ESemanticWorkspaceMode& OutMode)
		{
			for (uint8 Index = 0;
				Index <= static_cast<uint8>(
					ESemanticWorkspaceMode::EditBundle);
				++Index)
			{
				const ESemanticWorkspaceMode Candidate =
					static_cast<ESemanticWorkspaceMode>(Index);
				if (LexToString(Candidate) == Text)
				{
					OutMode = Candidate;
					return true;
				}
			}
			return false;
		}

		bool ParseStructuralEditKind(
			const FString& Text,
			EStructuralEditKind& OutKind)
		{
			for (uint8 Index = 0;
				Index <= static_cast<uint8>(
					EStructuralEditKind::SetOrder);
				++Index)
			{
				const EStructuralEditKind Candidate =
					static_cast<EStructuralEditKind>(Index);
				if (LexToString(Candidate) == Text)
				{
					OutKind = Candidate;
					return true;
				}
			}
			return false;
		}

		bool FailTypedValue(
			FWorkspaceError& OutError,
			const FHjsonNode& Node,
			const FString& Reason,
			TSharedPtr<FJsonValue> Expected = nullptr,
			const FString& Code =
				TEXT("semantic_workspace_typed_value_invalid"))
		{
			return FailNode(OutError, Code, Reason, Node, MoveTemp(Expected));
		}

		bool FailTypedValidation(FWorkspaceError& OutError, Contract::FDiagnostic Error, const FHjsonNode& Root)
		{
			const FString& Path = Error.PropertyPath.IsEmpty() ? Error.SemanticPointer : Error.PropertyPath;
			FString Pointer = Path.StartsWith(TEXT("/")) ? Path : TEXT("/") + Path;
			Pointer.ReplaceInline(TEXT("."), TEXT("/"));
			Pointer.ReplaceInline(TEXT("["), TEXT("/"));
			Pointer.ReplaceInline(TEXT("]"), TEXT(""));
			if (const auto* Node = Path.IsEmpty() ? nullptr : Root.Find(Pointer))
				return FailAtNode(OutError, MoveTemp(Error), *Node);
			Error.SourceKind = TEXT("related_input");
			return FailAtNode(OutError, MoveTemp(Error), Root);
		}

		bool ReadTypedString(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue,
			FWorkspaceError& OutError, bool bPublicActual = true)
		{
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::String)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must be a string."), *Key), Object, Key,
					MakeShared<FJsonValueString>(TEXT("string")), bPublicActual);
			}
			OutValue = Node->GetText().TrimStartAndEnd();
			return !OutValue.IsEmpty()
				|| FailNode(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must not be empty."), *Key), *Node,
					MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicActual);
		}

		bool ReadTypedStringArray(
			const FHjsonNode& Object,
			const FString& Key,
			TArray<FString>& OutValues,
			FWorkspaceError& OutError, bool bPublicActual = true)
		{
			OutValues.Reset();
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					FString::Printf(TEXT("Typed value field '%s' must be an array."), *Key), Object, Key,
					MakeShared<FJsonValueString>(TEXT("array")), bPublicActual);
			}
			for (const FHjsonNodeRef& Item : Node->GetItems())
			{
				if (Item->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_typed_value_invalid"),
						FString::Printf(TEXT("Each element of '%s' must be a string."), *Key), Item.Get(),
						MakeShared<FJsonValueString>(TEXT("string")), bPublicActual);
				}
				OutValues.Add(Item->GetText().TrimStartAndEnd());
			}
			return true;
		}

		bool ParseTypedTemporal(
			const FHjsonNode& Object,
			const bool bRequireMarker,
			FUeAgentTemporalPoint& OutValue,
			FWorkspaceError& OutError)
		{
			if (Object.GetKind() != EHjsonNodeKind::Object)
			{
				return FailTypedValue(OutError, Object, TEXT("A temporal value must be an object."), MakeShared<FJsonValueString>(TEXT("object")));
			}
			if (bRequireMarker)
			{
				FString Marker;
				if (!ReadTypedString(
						Object,
						TEXT("$uai"),
						Marker,
						OutError))
				{
					return false;
				}
				if (Marker != TEXT("temporal")) return FailTypedValue(OutError, *Object.Get(TEXT("$uai")),
					TEXT("A temporal value requires $uai to be temporal."), MakeShared<FJsonValueString>(TEXT("temporal")));
			}
			FString ClockId;
			FString Tick;
			FString RateNumerator;
			FString RateDenominator;
			FString State;
			if (!ReadTypedString(
					Object,
					TEXT("clock_id"),
					ClockId,
					OutError, false)
				|| !ReadTypedString(
					Object,
					TEXT("tick"),
					Tick,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("rate_numerator"),
					RateNumerator,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("rate_denominator"),
					RateDenominator,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("state"),
					State,
					OutError))
			{
				return false;
			}
			OutValue = FUeAgentTemporalPoint(
				MoveTemp(ClockId),
				MoveTemp(Tick),
				MoveTemp(RateNumerator),
				MoveTemp(RateDenominator),
				MoveTemp(State));
			return OutValue.IsValid()
				|| FailTypedValidation(OutError, OutValue.GetValidationDiagnostic(), Object);
		}

		bool ParseSpatialQuantityKind(
			const FString& Text,
			EUeAgentSpatialQuantityKind& OutKind)
		{
			if (Text == TEXT("point"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Point;
				return true;
			}
			if (Text == TEXT("direction"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Direction;
				return true;
			}
			if (Text == TEXT("normal"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Normal;
				return true;
			}
			if (Text == TEXT("transform"))
			{
				OutKind = EUeAgentSpatialQuantityKind::Transform;
				return true;
			}
			return false;
		}

		bool ParseTypedSpatial(
			const FHjsonNode& Object,
			FUeAgentSpatialValue& OutValue,
			FWorkspaceError& OutError)
		{
			if (Object.GetKind() != EHjsonNodeKind::Object)
			{
				return FailTypedValue(OutError, Object, TEXT("A spatial value must be an object."), MakeShared<FJsonValueString>(TEXT("object")));
			}
			FString Marker;
			FString Subject;
			FString QuantityText;
			if (!ReadTypedString(
					Object,
					TEXT("$uai"),
					Marker,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("subject"),
					Subject,
					OutError)
				|| !ReadTypedString(
					Object,
					TEXT("quantity_kind"),
					QuantityText,
					OutError))
			{
				return false;
			}
			EUeAgentSpatialQuantityKind QuantityKind;
			if (Marker != TEXT("spatial")) return FailTypedValue(OutError, *Object.Get(TEXT("$uai")),
				TEXT("A spatial value requires $uai to be spatial."), MakeShared<FJsonValueString>(TEXT("spatial")));
			if (!ParseSpatialQuantityKind(
					QuantityText,
					QuantityKind))
			{
				return FailTypedValue(OutError, *Object.Get(TEXT("quantity_kind")),
					TEXT("The spatial quantity_kind must be point, direction, normal, or transform."),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(TEXT("point")), MakeShared<FJsonValueString>(TEXT("direction")),
						MakeShared<FJsonValueString>(TEXT("normal")), MakeShared<FJsonValueString>(TEXT("transform"))}));
			}
			const FHjsonNode* FrameNode = Object.Get(TEXT("frame"));
			if (!FrameNode
				|| FrameNode->GetKind() != EHjsonNodeKind::Object)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("A spatial value requires a frame object."), Object, TEXT("frame"), MakeShared<FJsonValueString>(TEXT("object")));
			}
			FString FrameId;
			FString BasisFrameId;
			FString Unit;
			FString BasisEvidence;
			if (!ReadTypedString(
					*FrameNode,
					TEXT("frame_id"),
					FrameId,
					OutError, false)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_frame_id"),
					BasisFrameId,
					OutError, false)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("unit"),
					Unit,
					OutError)
				|| !ReadTypedString(
					*FrameNode,
					TEXT("basis_evidence"),
					BasisEvidence,
					OutError, false))
			{
				return false;
			}
			const FHjsonNode* DimensionNode =
				FrameNode->Get(TEXT("dimension"));
			int64 Dimension = 0;
			if (!DimensionNode
				|| DimensionNode->GetKind() != EHjsonNodeKind::Integer
				|| !LexTryParseString(
					Dimension,
					*DimensionNode->GetLexeme())
				|| Dimension < 1
				|| Dimension > MAX_int32)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("The spatial frame dimension must be an integer from 1 through 2147483647."), *FrameNode, TEXT("dimension"),
					MakeShared<FJsonValueString>(TEXT("integer from 1 through 2147483647")));
			}
			const FHjsonNode* BasisNode = FrameNode->Get(TEXT("basis"));
			TArray<TArray<FString>> Basis;
			if (!BasisNode
				|| BasisNode->GetKind() != EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
					TEXT("The spatial frame basis must be an array of rows."), *FrameNode, TEXT("basis"), MakeShared<FJsonValueString>(TEXT("array")));
			}
			for (const FHjsonNodeRef& RowNode : BasisNode->GetItems())
			{
				if (RowNode->GetKind() != EHjsonNodeKind::Array)
				{
					return FailTypedValue(OutError, RowNode.Get(), TEXT("Each spatial frame basis row must be an array."), MakeShared<FJsonValueString>(TEXT("array")));
				}
				TArray<FString> Row;
				for (const FHjsonNodeRef& Cell : RowNode->GetItems())
				{
					if (Cell->GetKind() != EHjsonNodeKind::String)
					{
						return FailTypedValue(OutError, Cell.Get(), TEXT("Each spatial frame basis cell must be a numeric string."), MakeShared<FJsonValueString>(TEXT("numeric string")));
					}
					Row.Add(Cell->GetText().TrimStartAndEnd());
				}
				Basis.Add(MoveTemp(Row));
			}
			TArray<FString> Origin;
			TArray<FString> Components;
			TArray<FString> ConversionEvidence;
			if (!ReadTypedStringArray(
					*FrameNode,
					TEXT("origin"),
					Origin,
					OutError)
				|| !ReadTypedStringArray(
					Object,
					TEXT("components"),
					Components,
					OutError)
				|| !ReadTypedStringArray(
					Object,
					TEXT("conversion_evidence"),
					ConversionEvidence,
					OutError, false))
			{
				return false;
			}
			const FHjsonNode* FrameObserved =
				FrameNode->Get(TEXT("observed_at"));
			const FHjsonNode* ValueObserved =
				Object.Get(TEXT("observed_at"));
			FUeAgentTemporalPoint FrameTime(TEXT("invalid"), TEXT("0"));
			FUeAgentTemporalPoint ValueTime(TEXT("invalid"), TEXT("0"));
			if (!FrameObserved) return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
				TEXT("The spatial frame requires observed_at."), *FrameNode, TEXT("observed_at"), MakeShared<FJsonValueString>(TEXT("temporal object")));
			if (!ValueObserved) return FailField(OutError, TEXT("semantic_workspace_typed_value_invalid"),
				TEXT("The spatial value requires observed_at."), Object, TEXT("observed_at"), MakeShared<FJsonValueString>(TEXT("temporal object")));
			if (!ParseTypedTemporal(
					*FrameObserved,
					false,
					FrameTime,
					OutError)
				|| !ParseTypedTemporal(
					*ValueObserved,
					false,
					ValueTime,
					OutError))
			{
				return false;
			}
			FUeAgentSpatialFrame Frame(
				MoveTemp(FrameId),
				MoveTemp(BasisFrameId),
				static_cast<int32>(Dimension),
				MoveTemp(Basis),
				MoveTemp(Origin),
				MoveTemp(Unit),
				MoveTemp(BasisEvidence),
				MoveTemp(FrameTime));
			if (!Frame.IsValid())
			{
				return FailTypedValidation(OutError, Frame.GetValidationDiagnostic(), *FrameNode);
			}
			OutValue = FUeAgentSpatialValue(
				MoveTemp(Subject),
				QuantityKind,
				MoveTemp(Frame),
				MoveTemp(Components),
				MoveTemp(ValueTime),
				MoveTemp(ConversionEvidence));
			return OutValue.IsValid()
				|| FailTypedValidation(OutError, OutValue.GetValidationDiagnostic(), Object);
		}

		bool TryTypedCanonicalValue(
			const FHjsonNode& Root,
			FUeAgentResourceTracker& Resources,
			bool& bOutHandled,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			bOutHandled = false;
			if (Root.GetKind() != EHjsonNodeKind::Object)
			{
				return true;
			}
			const FHjsonNode* MarkerNode = Root.Get(TEXT("$uai"));
			if (!MarkerNode)
			{
				return true;
			}
			bOutHandled = true;
			if (MarkerNode->GetKind() != EHjsonNodeKind::String)
			{
				return FailTypedValue(OutError, *MarkerNode, TEXT("The $uai typed-value marker must be a string."), MakeShared<FJsonValueString>(TEXT("string")));
			}
			FUeAgentStateSemanticError StateError;
			if (MarkerNode->GetText() == TEXT("decimal"))
			{
				FString Lexeme;
				if (!ReadTypedString(
						Root,
						TEXT("value"),
						Lexeme,
						OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TryDecimal(
						Lexeme,
						Resources,
						OutValue,
						StateError))
				{
					return FailAtNode(OutError, MoveTemp(StateError), Root);
				}
				return true;
			}
			if (MarkerNode->GetText() == TEXT("temporal"))
			{
				FUeAgentTemporalPoint Temporal(TEXT("invalid"), TEXT("0"));
				if (!ParseTypedTemporal(
						Root,
						true,
						Temporal,
						OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TryTemporal(
						Temporal,
						Resources,
						OutValue,
						StateError))
				{
					return FailAtNode(OutError, MoveTemp(StateError), Root);
				}
				return true;
			}
			if (MarkerNode->GetText() == TEXT("spatial"))
			{
				FUeAgentSpatialValue Spatial(
					TEXT("invalid"),
					EUeAgentSpatialQuantityKind::Point,
					FUeAgentSpatialFrame(
						TEXT("invalid"),
						TEXT("invalid"),
						1,
						{{TEXT("1")}},
						{TEXT("0")},
						TEXT("invalid"),
						TEXT("invalid"),
						FUeAgentTemporalPoint(
							TEXT("invalid"),
							TEXT("0"))),
					{TEXT("0")},
					FUeAgentTemporalPoint(TEXT("invalid"), TEXT("0")));
				if (!ParseTypedSpatial(Root, Spatial, OutError))
				{
					return false;
				}
				if (!FUeAgentCanonicalValue::TrySpatial(
						Spatial,
						Resources,
						OutValue,
						StateError))
				{
					return FailAtNode(OutError, MoveTemp(StateError), Root);
				}
				return true;
			}
			return FailTypedValue(
				OutError,
				*MarkerNode,
				TEXT("The $uai marker must be decimal, temporal, or spatial."),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("decimal")),
					MakeShared<FJsonValueString>(TEXT("temporal")), MakeShared<FJsonValueString>(TEXT("spatial"))}),
				TEXT("semantic_workspace_typed_value_kind_unsupported"));
		}

		bool TryCanonicalValue(
			const FHjsonNode& Root,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError,
			const TMap<FString, FString>* PublicRefToUnitId,
			const bool bInterpretTypedValues, bool* OutHasContentReference)
		{
			if (OutHasContentReference) *OutHasContentReference = false;
			struct FVisit
			{
				const FHjsonNode* Node = nullptr;
				bool bExpanded = false;
			};
			TArray<FVisit> Stack;
			TMap<const FHjsonNode*, FUeAgentCanonicalValuePtr>
				Converted;
			Stack.Add({&Root, false});
			while (!Stack.IsEmpty())
			{
				const FVisit Visit =
					Stack.Pop(EAllowShrinking::No);
				if (Converted.Contains(Visit.Node))
				{
					continue;
				}
				if (OutHasContentReference && !Visit.bExpanded && Visit.Node->GetKind() == EHjsonNodeKind::Object
					&& Visit.Node->GetEntries().Num() == 1) {
					const auto* Reference = Visit.Node->Get(TEXT("content_ref"));
					if (Reference && Reference->GetKind() == EHjsonNodeKind::String) *OutHasContentReference = true;
				}
				if (bInterpretTypedValues && !Visit.bExpanded
					&& Visit.Node->GetKind()
						== EHjsonNodeKind::Object)
				{
					bool bHandled = false;
					FUeAgentCanonicalValuePtr TypedValue;
					if (!TryTypedCanonicalValue(
							*Visit.Node,
							Resources,
							bHandled,
							TypedValue,
							OutError))
					{
						return false;
					}
					if (bHandled)
					{
						Converted.Add(Visit.Node, TypedValue);
						continue;
					}
				}
				if (!Visit.bExpanded
					&& (Visit.Node->GetKind()
						== EHjsonNodeKind::Array
						|| Visit.Node->GetKind()
							== EHjsonNodeKind::Object))
				{
					Stack.Add({Visit.Node, true});
					if (Visit.Node->GetKind()
						== EHjsonNodeKind::Array)
					{
						const TArray<FHjsonNodeRef>& Items =
							Visit.Node->GetItems();
						for (int32 Index = Items.Num() - 1;
							Index >= 0;
							--Index)
						{
							Stack.Add(
								{&Items[Index].Get(), false});
						}
					}
					else
					{
						const TArray<TPair<
							FString,
							FHjsonNodeRef>>& Entries =
								Visit.Node->GetEntries();
						for (int32 Index = Entries.Num() - 1;
							Index >= 0;
							--Index)
						{
							Stack.Add(
								{&Entries[Index].Value.Get(), false});
						}
					}
					continue;
				}

				FUeAgentCanonicalValuePtr Value;
				FUeAgentStateSemanticError StateError;
				bool bCreated = false;
				switch (Visit.Node->GetKind())
				{
				case EHjsonNodeKind::Null:
					bCreated = FUeAgentCanonicalValue::TryNull(
						Resources,
						Value,
						StateError);
					break;
				case EHjsonNodeKind::Boolean:
					bCreated =
						FUeAgentCanonicalValue::TryBoolean(
							Visit.Node->GetBoolean(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::String:
				{
					const FString* Internal = FindObservedPublicRef(PublicRefToUnitId, Visit.Node->GetText());
					bCreated =
						FUeAgentCanonicalValue::TryString(
							Internal == nullptr
								? Visit.Node->GetText() : *Internal,
							Resources,
							Value,
							StateError);
					break;
				}
				case EHjsonNodeKind::Integer:
					bCreated =
						FUeAgentCanonicalValue::TryInteger(
							Visit.Node->GetLexeme(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::Decimal:
					bCreated =
						FUeAgentCanonicalValue::TryDecimal(
							Visit.Node->GetLexeme(),
							Resources,
							Value,
							StateError);
					break;
				case EHjsonNodeKind::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const FHjsonNodeRef& Item :
						Visit.Node->GetItems())
					{
						Items.Add(
							Converted.FindChecked(
								&Item.Get()).ToSharedRef());
					}
					bCreated =
						FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							Value,
							StateError);
					break;
				}
				case EHjsonNodeKind::Object:
				{
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						FHjsonNodeRef>& Item :
						Visit.Node->GetEntries())
					{
						const FString* InternalKey = FindObservedPublicRef(PublicRefToUnitId, Item.Key);
						Items.Emplace(
							InternalKey == nullptr ? Item.Key : *InternalKey,
							Converted.FindChecked(
								&Item.Value.Get())
								.ToSharedRef());
					}
					bCreated =
						FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							Value,
							StateError);
					break;
				}
				default:
					break;
				}
				if (!bCreated || !Value.IsValid())
				{
					if (StateError.IsEmpty()) StateError = Contract::FDiagnostic::Failure(
						TEXT("semantic_workspace_unit_value_invalid"),
						TEXT("The HJSON node could not be represented as a semantic value."), TEXT("workspace_resolve"));
					return FailAtNode(OutError, MoveTemp(StateError), *Visit.Node);
				}
				Converted.Add(Visit.Node, MoveTemp(Value));
			}
			OutValue = Converted.FindChecked(&Root);
			return OutValue.IsValid();
		}

		int64 KeyedDocumentMetadataBytes(const FSemanticWorkspaceReadState::FDocument& Document)
		{
			int64 Bytes = Document.KeyedPending.GetAllocatedSize() + Document.KeyedReferences.GetAllocatedSize();
			for (const auto& Scope : Document.KeyedPending) {
				Bytes += Scope.Key.GetAllocatedSize() + Scope.Value.GetAllocatedSize();
				for (const auto& Key : Scope.Value) Bytes += Key.GetAllocatedSize();
			}
			for (const auto& Scope : Document.KeyedReferences) {
				Bytes += Scope.Key.GetAllocatedSize() + Scope.Value.GetAllocatedSize();
				for (const auto& Reference : Scope.Value) Bytes += sizeof(FReferenceLookupProof) + 32
					+ Reference->Key.GetAllocatedSize() + (Reference->Result.IsSet() ? Reference->Result->GetAllocatedSize() : 0);
			}
			return Bytes;
		}

		struct FKeyedMemberSelection {
			FSemanticWorkspaceReadState::FDocument* Document = nullptr;
			FString Pointer;
			TSet<FString> Keys;
			bool bPartial = false;
			bool bRequirePartial = false;
		};
		bool HasIndependentNativeMembers(const FKeyedItemProjectionContract& Contract)
		{
			return (Contract.RootField == TEXT("actors") || Contract.RootField == TEXT("components"))
				&& Contract.ReferenceFields.IsEmpty() && Contract.CollectionFieldContracts.IsEmpty()
				&& Contract.RecordSchemaField.IsEmpty() && Contract.CapturedRecordIdentityField.IsEmpty();
		}

		FKeyedMemberSelection SelectKeyedMembers(const FHjsonNode& Visible, const FHjsonNode& Captured,
			const FKeyedItemProjectionContract& Contract,
			const TMap<FString, FString>& PublicRefs)
		{
			FKeyedMemberSelection Result;
			Result.bRequirePartial = HasIndependentNativeMembers(Contract);
			// These native member records have no cross-collection reference or
			// dynamic record-schema consumer. Other contracts keep their reader.
			if (!ActiveReadState || !HasIndependentNativeMembers(Contract)
				|| Visible.GetKind() != EHjsonNodeKind::Object) return Result;
			for (auto& Pair : ActiveReadState->Documents) {
				auto& Current = Pair.Value;
				if (Current.SourceUnit != Visible.GetProvenance().GetSourceUnit()) continue;
				Result.Document = &Current; Result.Pointer = Visible.GetProvenance().GetJsonPointer();
				const auto* Previous = PreviousDocumentReads.Find(Pair.Key);
				const auto* Pending = Previous ? Previous->KeyedPending.Find(Result.Pointer) : nullptr;
				const auto* References = Previous ? Previous->KeyedReferences.Find(Result.Pointer) : nullptr;
				if (!Current.bComparedText || !Current.DiffBaseDocument) return Result;
				// Unchanged text can acquire a different meaning when a readable
				// reference is rebound. Carry the original owner's actual lookups,
				// including misses, rather than trusting the raw range alone.
				if (References && Previous && Current.DiffBaseHash == Previous->ContentHash) for (const auto& Reference : *References) {
					const auto* Value = FindObservedPublicRef(&PublicRefs, Reference->Key);
					if (Value ? !Reference->Result.IsSet() || *Value != Reference->Result.GetValue()
						: Reference->Result.IsSet()) return Result;
				}
				const auto* Old = Current.DiffBaseDocument->Find(Result.Pointer);
				if (!Old || Old->GetKind() != EHjsonNodeKind::Object) return Result;
				if (Pending && Previous && Current.DiffBaseHash == Previous->ContentHash) Result.Keys = *Pending;
				const auto Touched = [&](const FHjsonNode& Node, bool bOld, bool& bValid) {
					const auto& Source = Node.GetProvenance();
					const int64 Begin = Source.GetByteStart(), End = Source.GetByteEnd();
					if (Begin < 0 || End < Begin) { bValid = false; return false; }
					int32 Low = 0, High = Current.Changes.Num();
					while (Low < High) {
						const int32 Mid = Low + (High - Low) / 2;
						const auto& Change = Current.Changes[Mid];
						if ((bOld ? Change.OldEnd : Change.NewEnd) < Begin) Low = Mid + 1; else High = Mid;
					}
					return Low < Current.Changes.Num() && (bOld ? Current.Changes[Low].OldBegin : Current.Changes[Low].NewBegin) <= End;
				};
				// Only identities and parser spans are visited for the unchanged
				// members. Their values are not converted, compared or hashed.
				TSet<FString> OldKeys, NewKeys;
				bool bValid = true;
				for (const auto& Item : Old->GetEntries()) {
					OldKeys.Add(Item.Key);
					if (Touched(Item.Value.Get(), true, bValid)) Result.Keys.Add(Item.Key);
				}
				for (const auto& Item : Visible.GetEntries()) {
					NewKeys.Add(Item.Key);
					if (!OldKeys.Contains(Item.Key) || Touched(Item.Value.Get(), false, bValid)) Result.Keys.Add(Item.Key);
				}
				for (const auto& Key : OldKeys) if (!NewKeys.Contains(Key)) Result.Keys.Add(Key);
				// The latest raw is not the published membership baseline. A native
				// create may have published another readable alias since that raw.
				// Compare identities only, so deleting that published alias is not
				// lost when it never occurred in the previous author text.
				TSet<FString> CapturedKeys;
				for (const auto& Item : Captured.GetEntries()) {
					CapturedKeys.Add(Item.Key);
					if (!NewKeys.Contains(Item.Key)) Result.Keys.Add(Item.Key);
				}
				for (const auto& Key : NewKeys) if (!CapturedKeys.Contains(Key)) Result.Keys.Add(Key);
				Result.bPartial = bValid;
				return Result;
			}
			return Result;
		}

		bool TryCanonicalKeyedMembers(const FHjsonNode& Items, const FKeyedMemberSelection& Selection,
			FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out, FWorkspaceError& Error,
			const TMap<FString, FString>* PublicRefs, bool bInterpret)
		{
			if (!Selection.bPartial) {
				if (Selection.bRequirePartial) return FailNode(Error, TEXT("workspace_keyed_range_selection_missing"),
					TEXT("The local member input has no matching old tree/range selection. No complete canonical-value producer was selected."), Items);
				return TryCanonicalValue(Items, Resources, Out, Error, PublicRefs, bInterpret);
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Values;
			for (const auto& Item : Items.GetEntries()) if (Selection.Keys.Contains(Item.Key)) {
				FUeAgentCanonicalValuePtr Value;
				if (!TryCanonicalValue(Item.Value.Get(), Resources, Value, Error, PublicRefs, bInterpret)) return false;
				Values.Emplace(Item.Key, Value.ToSharedRef());
			}
			FUeAgentStateSemanticError Failure;
			if (!FUeAgentCanonicalValue::TryObject(Values, Resources, Out, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
			return true;
		}

		bool TryResolveNativeKeyedMembers(const FHjsonNode& VisibleItems, const FHjsonNode& CapturedItems,
			const FKeyedMemberSelection& Selection, const FKeyedItemProjectionContract& Contract,
			const TMap<FString, FString>& PublicRefs, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out, bool& bChanged, int32& VisibleCount, int32& CapturedCount, FWorkspaceError& Error)
		{
			if (HasIndependentNativeMembers(Contract) && !Selection.bPartial)
				return FailNode(Error, TEXT("workspace_keyed_range_selection_missing"),
					TEXT("The member reader is missing its matching old tree/range selection. No complete desired-value resolver was selected."), VisibleItems);
			// The identity directory is needed for removals. Value conversion and
			// the existing keyed resolver operate one member at a time; a cold raw
			// miss does not create two complete typed desired/captured collections.
			TMap<FString, const FHjsonNode*> Before;
			for (const auto& Item : CapturedItems.GetEntries())
				if (!Selection.bPartial || Selection.Keys.Contains(Item.Key)) Before.Add(Item.Key, &Item.Value.Get());
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Operations;
			FWorkspaceError Errors;
			const auto Resolve = [&](const FString& Key, const FHjsonNode* Visible, const FHjsonNode* Captured) {
				FWorkspaceError Local;
				const auto Run = [&]() {
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> VisibleRow, CapturedRow;
					FUeAgentCanonicalValuePtr Value;
					if (Visible) {
						if (!TryCanonicalValue(*Visible, Resources, Value, Local,
							Contract.bInterpretSemanticValues ? &PublicRefs : nullptr, Contract.bInterpretSemanticValues)) return false;
						VisibleRow.Emplace(Key, Value.ToSharedRef()); ++VisibleCount;
					}
					if (Captured) {
						if (!TryCanonicalValue(*Captured, Resources, Value, Local, nullptr, Contract.bInterpretSemanticValues)) return false;
						CapturedRow.Emplace(Key, Value.ToSharedRef()); ++CapturedCount;
					}
					FUeAgentCanonicalValuePtr VisibleValue, CapturedValue, Resolved;
					FUeAgentStateSemanticError Failure;
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(VisibleRow), Resources, VisibleValue, Failure)
						|| !FUeAgentCanonicalValue::TryObject(MoveTemp(CapturedRow), Resources, CapturedValue, Failure)) {
						Local.SetFailure(MoveTemp(Failure)); return false;
					}
					bool bMemberChanged = false;
					if (!TryResolveKeyedItemDesiredState(*VisibleValue, *CapturedValue, VisibleItems.GetProvenance().GetSourceUnit(),
						Contract, nullptr, Resources, Resolved, bMemberChanged, Local, nullptr,
						EKeyedItemInputKind::VisibleDesiredState, nullptr, &VisibleItems)) return false;
					for (const auto& Operation : Resolved->GetObjectItems()) Operations.Add(Operation);
					return true;
				};
				if (!Run()) {
					// Original resolver diagnostics already carry their member path.
					// Canonical conversion failures retain the exact original node.
					Errors.Append(Local);
				}
			};
			for (const auto& Item : VisibleItems.GetEntries()) {
				if (Selection.bPartial && !Selection.Keys.Contains(Item.Key)) continue;
				const auto* Captured = Before.Find(Item.Key);
				Resolve(Item.Key, &Item.Value.Get(), Captured ? *Captured : nullptr);
				Before.Remove(Item.Key);
			}
			for (const auto& Item : Before) Resolve(Item.Key, nullptr, Item.Value);
			if (Errors.HasErrors()) { Error = MoveTemp(Errors); return false; }
			Operations.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
			bChanged = !Operations.IsEmpty();
			FUeAgentStateSemanticError Failure;
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Operations), Resources, Out, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
			return true;
		}

		bool ParseSemanticUnit(
			const FHjsonNode& Row,
			const FHjsonNode* Envelope,
			TSharedRef<const FString, ESPMode::ThreadSafe> InventoryIdentity,
			const FString& InventoryKind,
			const FString& InventoryPointer,
			const TMap<FString, FString>& PublicRefToUnitId,
			FUeAgentResourceTracker& Resources,
			TOptional<FSemanticUnit>& OutUnit,
			TOptional<bool>& OutManagedSemanticChanged,
			TSet<FString>& OutResolvedKeyedOperationRoots,
			FHjsonNodePtr& OutValueSources,
			FWorkspaceError& OutError, bool bDeclarationOnly = false, const FAuthoringModel* PublishedModel = nullptr)
		{
			const FString& InventoryUnitId = InventoryIdentity.Get();
			OutManagedSemanticChanged.Reset();
			OutResolvedKeyedOperationRoots.Reset();
			OutUnit.Reset();
			OutValueSources.Reset();
			if (Row.GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_unit_document_invalid"), TEXT("A semantic unit document row must be an object."),
					Row.GetProvenance().GetSourceUnit(),
					Row.GetProvenance().GetJsonPointer());
			}
			FString UnitId;
			FString KindText;
			FString SemanticTypeRef;
			FString Role;
			FString OwnerRef;
			FString FieldSetRef;
			FString OrderingRef;
			FString LayoutRef;
			FString SourceProvenance;
			FString TargetIdentityKey;
				auto RequiredResolvedString =
				[&](const FString& Key, FString& Value)
			{
				const FHjsonNode* Node = ResolveEnvelopeField(
					Row, Envelope, Key, OutError);
				if (Node == nullptr)
				{
					if (!OutError.HasErrors()) FailField(OutError, TEXT("semantic_workspace_manifest_field_missing"),
						FString::Printf(TEXT("Required semantic metadata field '%s' is missing."), *Key),
						Row, Key, MakeShared<FJsonValueString>(TEXT("string")), false);
					return false;
				}
				return RequiredString(Row.Get(Key) ? Row : *Envelope, Key, Value, OutError);
			};
			auto OptionalResolvedString =
				[&](const FString& Key, FString& Value)
			{
				const FHjsonNode* Node = ResolveEnvelopeField(
					Row, Envelope, Key, OutError);
				if (Node == nullptr)
				{
					return OutError.GetCode().IsEmpty();
				}
				return OptionalString(Row.Get(Key) ? Row : *Envelope, Key, Value, OutError);
			};
			auto ReadResolvedArray =
				[&](const FString& Key, TArray<FString>& Values)
			{
				const FHjsonNode* RowValue = Row.Get(Key);
				const FHjsonNode* EnvelopeValue =
					Envelope == nullptr ? nullptr : Envelope->Get(Key);
				FNodeDifference Difference;
				if (RowValue != nullptr && EnvelopeValue != nullptr
					&& !EquivalentNode(*RowValue, *EnvelopeValue, &Difference))
				{
					return FailMetadataDifference(OutError,
						FString::Printf(TEXT("Metadata array '%s' differs from its captured workspace value."), *Key), Difference);
				}
				if (RowValue != nullptr)
				{
					return ReadStringArray(Row, Key, false, Values, OutError);
				}
				if (EnvelopeValue != nullptr)
				{
					return ReadStringArray(
						*Envelope, Key, false, Values, OutError);
				}
				return true;
			};
			const FHjsonNode* KindNode = Row.Get(TEXT("unit_kind"));
			if (KindNode != nullptr)
			{
				if (KindNode->GetKind() != EHjsonNodeKind::String)
				{
					return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"),
						TEXT("The semantic unit_kind field must be a string."), *KindNode, MakeShared<FJsonValueString>(TEXT("string")));
				}
				KindText = KindNode->GetText().TrimStartAndEnd();
				if (!InventoryKind.IsEmpty()
					&& KindText != InventoryKind)
				{
					return FailNode(OutError, TEXT("semantic_workspace_envelope_mismatch"),
						TEXT("The authored unit kind differs from the workspace inventory."), *KindNode, MakeShared<FJsonValueString>(InventoryKind));
				}
			}
			else
			{
				KindText = InventoryKind;
			}
			const FHjsonNode* UnitIdNode = Row.Get(TEXT("unit_id"));
			if (UnitIdNode == nullptr)
			{
				UnitId = InventoryUnitId;
			}
			else if (UnitIdNode->GetKind() != EHjsonNodeKind::String)
			{
				return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"), TEXT("The semantic unit_id field must be a string."),
					*UnitIdNode, MakeShared<FJsonValueString>(TEXT("string")), false);
			}
			else
			{
				UnitId = UnitIdNode->GetText().TrimStartAndEnd();
				if (!InventoryUnitId.IsEmpty() && UnitId != InventoryUnitId)
				{
					auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_envelope_mismatch"),
						TEXT("The authored unit identity differs from the workspace inventory."), TEXT("workspace_resolve"), *UnitIdNode, nullptr, false);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryUnitId);
					return FailAtNode(OutError, MoveTemp(Diagnostic), *UnitIdNode);
				}
			}
			if (UnitId.IsEmpty()) return FailField(OutError, TEXT("semantic_workspace_unit_document_invalid"),
				TEXT("The semantic unit identity is empty in both the document and its inventory."), Row, TEXT("unit_id"),
				MakeShared<FJsonValueString>(TEXT("non-empty string")), false);
			if (!RequiredResolvedString(TEXT("semantic_type_ref"), SemanticTypeRef)
				|| !RequiredResolvedString(TEXT("role"), Role)
				|| !OptionalResolvedString(TEXT("owner_ref"), OwnerRef)
				|| !OptionalResolvedString(TEXT("field_set_ref"), FieldSetRef)
				|| !OptionalResolvedString(TEXT("ordering_ref"), OrderingRef)
				|| !OptionalResolvedString(TEXT("layout_ref"), LayoutRef)
				|| !OptionalResolvedString(TEXT("source_provenance"), SourceProvenance)
				|| !OptionalResolvedString(TEXT("target_identity_key"), TargetIdentityKey))
			{
				return false;
			}
			ESemanticUnitKind UnitKind;
			if (!ParseUnitKind(KindText, UnitKind))
			{
				auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_unit_kind_invalid"),
					TEXT("The semantic unit kind is unsupported."), TEXT("workspace_resolve"), KindNode ? Row : (Envelope ? *Envelope : Row),
					TEXT("unit_kind"), UnitKindChoices());
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(KindText);
				Diagnostic.ActualValuePresent = true;
				return FailAtNode(OutError, MoveTemp(Diagnostic), KindNode ? *KindNode : Row);
			}
			const FHjsonNode* Definition = Row.Get(TEXT("definition"));
			const bool bKeyedProperty = UnitKind == ESemanticUnitKind::PropertySet
				&& Row.Get(TEXT("values")) == nullptr && Envelope && Envelope->Get(TEXT("captured_type"));
			TArray<FString> CategoryPath;
			if (!ReadResolvedArray(TEXT("category_path"), CategoryPath)) return false;
			if (bKeyedProperty && !Row.Get(TEXT("category_path")) && !Envelope->Get(TEXT("category_path"))) {
				// Older envelopes omitted native category segments. The inventory's
				// successfully resolved public address still proves the exact rendered
				// category; retain that spelling rather than guessing current metadata.
				const FString Prefix(TEXT("/categories/"));
				if (InventoryPointer.StartsWith(Prefix, ESearchCase::CaseSensitive)) {
					const int32 End = InventoryPointer.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Prefix.Len());
					if (End != INDEX_NONE) {
						const FString Encoded = InventoryPointer.Mid(Prefix.Len(), End - Prefix.Len());
						FString Rendered = Encoded.Replace(TEXT("~1"), TEXT("/"), ESearchCase::CaseSensitive);
						Rendered.ReplaceInline(TEXT("~0"), TEXT("~"), ESearchCase::CaseSensitive);
						if (EscapeJsonPointerSegment(Rendered).Equals(Encoded, ESearchCase::CaseSensitive))
							CategoryPath.Add(MoveTemp(Rendered));
					}
				}
			}
			if (!bDeclarationOnly && !bKeyedProperty
				&& (Definition || (UnitKind == ESemanticUnitKind::ItemState && !FieldSetRef.IsEmpty())))
			{
				const FString* ResolvedDefinition = Definition && Definition->GetKind() == EHjsonNodeKind::String
					? FindObservedPublicRef(&PublicRefToUnitId, Definition->GetText()) : nullptr;
				if (UnitKind != ESemanticUnitKind::ItemState || FieldSetRef.IsEmpty() || !Definition
					|| Definition->GetKind() != EHjsonNodeKind::String
					|| !(ResolvedDefinition ? *ResolvedDefinition : Definition->GetText()).Equals(FieldSetRef, ESearchCase::CaseSensitive))
					return Fail(OutError, TEXT("semantic_workspace_item_definition_read_only"), TEXT("The item's definition reference must retain its captured field-set definition."),
						Row.GetProvenance().GetSourceUnit(), TEXT("/definition"));
			}
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			if (!bDeclarationOnly) {
			const FHjsonNode* ValuesNode = Row.Get(TEXT("values"));
			const FHjsonNode* CapturedValues =
				Envelope == nullptr ? nullptr : Envelope->Get(TEXT("captured_values"));
			const bool bGraphProjection =
				UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr
				&& IsGraphAuthoringStructureType(SemanticTypeRef,
					CapturedValues && CapturedValues->Get(TEXT("nodes")) && CapturedValues->Get(TEXT("edges")));
			// Decomposed document collections carry semantic-id#document-path.
			// Their native capture and contract belong to composition, not to a
			// global root-name match (e.g. Sequence and UMG both have "bindings").
			const bool bDocumentCollectionProjection = UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr && SemanticTypeRef.Contains(TEXT("#"));
			TArray<const FKeyedItemProjectionContract*> KeyedItemContracts;
			for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
				if (!bGraphProjection && !bDocumentCollectionProjection && (Row.Get(Contract.RootField) != nullptr
					|| (CapturedValues && CapturedValues->Get(Contract.RootField))))
				{
					KeyedItemContracts.Add(&Contract);
				}
			const bool bKeyedItemProjection =
				UnitKind == ESemanticUnitKind::StructureState
				&& ValuesNode == nullptr
				&& !KeyedItemContracts.IsEmpty();
			const bool bActorComponentItemProjection =
				UnitKind == ESemanticUnitKind::ItemState
				&& Role == TEXT("actor_component_hierarchy")
				&& (SemanticTypeRef.EndsWith(TEXT(".actor.target"))
					|| SemanticTypeRef.EndsWith(TEXT(".component.target")));
			if (!bGraphProjection && !bKeyedProperty
				&& !bKeyedItemProjection && !bActorComponentItemProjection
				&& ValuesNode == nullptr)
			{
				ValuesNode = CapturedValues;
			}
			if (!bGraphProjection && !bKeyedProperty && !bKeyedItemProjection
				&& !bActorComponentItemProjection
				&& (ValuesNode == nullptr
				|| ValuesNode->GetKind() != EHjsonNodeKind::Object))
			{
				return FailField(OutError, TEXT("semantic_workspace_unit_values_invalid"),
					TEXT("The semantic unit requires a values object or a captured values object."),
					Row.Get(TEXT("values")) || !Envelope ? Row : *Envelope,
					Row.Get(TEXT("values")) || !Envelope ? TEXT("values") : TEXT("captured_values"),
					MakeShared<FJsonValueString>(TEXT("object")), false);
			}
			// Inspect only structural owner records, never reflected business values.
			// Keep the original nodes until every independent seed has been checked.
			if (UnitKind == ESemanticUnitKind::StructureState)
			{
				FWorkspaceError SeedErrors;
				auto CheckPayload = [&SeedErrors](const FHjsonNode* Owner, const TCHAR* Field)
				{
					const FHjsonNode* Payload = Owner ? Owner->Get(Field) : nullptr;
					if (!Payload) return;
					FWorkspaceError Error;
					if (!UeAgentDetailsAuthoringService::ValidatePropertyPayload(*Payload, Error)) SeedErrors.Append(Error);
				};
				auto CheckOwner = [&CheckPayload](const FHjsonNode& Owner)
				{
					CheckPayload(&Owner, TEXT("properties"));
					CheckPayload(&Owner, TEXT("property_changes"));
					CheckPayload(Owner.Get(TEXT("slot")), TEXT("properties"));
					if (const FHjsonNode* Renderers = Owner.Get(TEXT("renderers")); Renderers && Renderers->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Renderer : Renderers->GetEntries()) CheckPayload(&Renderer.Value.Get(), TEXT("properties"));
				};
				auto CheckCollection = [&CheckOwner](const FHjsonNode* Collection)
				{
					if (!Collection || Collection->GetKind() != EHjsonNodeKind::Object) return;
					for (const auto& Entry : Collection->GetEntries()) CheckOwner(Entry.Value.Get());
				};
				auto CheckStructure = [&CheckCollection](const FHjsonNode* Structure)
				{
					if (!Structure || Structure->GetKind() != EHjsonNodeKind::Object) return;
					for (const TCHAR* Field : {TEXT("nodes"), TEXT("widgets"), TEXT("items"), TEXT("goals")})
						CheckCollection(Structure->Get(Field));
					const FHjsonNode* Emitters = Structure->Get(TEXT("emitters"));
					CheckCollection(Emitters);
					if (Emitters && Emitters->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Entry : Emitters->GetEntries()) CheckCollection(Entry.Value->Get(TEXT("renderers")));
					const FHjsonNode* Tree = Structure->Get(TEXT("widget_tree"));
					if (Tree) CheckCollection(Tree->Get(TEXT("widgets")));
					const FHjsonNode* Graphs = Structure->Get(TEXT("graphs"));
					if (Graphs && Graphs->GetKind() == EHjsonNodeKind::Object)
						for (const auto& Graph : Graphs->GetEntries()) CheckCollection(Graph.Value->Get(TEXT("nodes")));
				};
				if (bKeyedItemProjection)
				{
					for (const auto* Contract : KeyedItemContracts)
					{
						// Typed record properties belong to their row schema. They are
						// not reflected UObject property documents or creation seeds.
						if (Contract->RecordSchemaField.IsEmpty() && Contract->bInterpretSemanticValues)
							CheckCollection(Row.Get(Contract->RootField));
					}
				}
				else
				{
					const FHjsonNode* Structure = bDocumentCollectionProjection || bGraphProjection ? &Row : ValuesNode;
					if (!Structure) Structure = &Row;
					CheckStructure(Structure);
					CheckStructure(Structure->Get(TEXT("provider_extension")));
				}
				if (SeedErrors.HasErrors()) { OutError = MoveTemp(SeedErrors); return false; }
			}
			if (bDocumentCollectionProjection)
			{
				for (const auto& Field : CapturedValues->GetEntries())
				{
					const FHjsonNode* Edited = Row.Get(Field.Key);
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalValue(Edited ? *Edited : Field.Value.Get(), Resources, Value,
						OutError, &PublicRefToUnitId)) return false;
					Values.Add(Field.Key, Value.ToSharedRef());
				}
			}
			else if (bGraphProjection)
			{
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_graph_capture_missing"),
						TEXT("The graph has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				FUeAgentCanonicalValuePtr CapturedRoot;
				FUeAgentCanonicalValuePtr VisibleRoot;
				if (!TryCanonicalValue(
						*CapturedValues, Resources, CapturedRoot, OutError)
					|| !TryCanonicalValue(
						Row, Resources, VisibleRoot, OutError,
						&PublicRefToUnitId)
					|| !TryResolveGraphAuthoringProjection(
						*VisibleRoot,
						*CapturedRoot,
						Row.GetProvenance().GetSourceUnit(),
						Resources,
						Values,
						OutError, &Row, &OutValueSources))
				{
					return false;
				}
			}
			else if (bKeyedItemProjection)
			{
				auto IsCollectionRoot = [&](const FString& Name) {
					return KeyedItemContracts.ContainsByPredicate([&](const auto* Contract) { return Contract->RootField == Name; });
				};
				auto IsDeclaredRootField = [&](const FString& Name) {
					return KeyedItemContracts.ContainsByPredicate([&](const auto* Contract) {
						return Contract->RootField == Name || Contract->RecordSchemaField == Name
							|| Contract->IsReadOnlyRootField(Name) || Contract->IsEditableRootField(Name);
					});
				};
				auto ResolveCollection = [&](const FKeyedItemProjectionContract& Contract, FWorkspaceError& OutError) -> bool {
					const FHjsonNode* CapturedItems = CapturedValues == nullptr
						? nullptr : CapturedValues->Get(Contract.RootField);
					const FHjsonNode* VisibleItems = Row.Get(Contract.RootField);
					if (CapturedItems == nullptr)
					{
						return FailCapturedField(OutError, TEXT("semantic_workspace_keyed_item_capture_missing"),
							TEXT("The collection has no captured items; export the workspace again."),
							CapturedValues ? *CapturedValues : (Envelope ? *Envelope : Row),
							CapturedValues ? Contract.RootField : TEXT("captured_values"));
					}
					FUeAgentCanonicalValuePtr Captured;
					FUeAgentCanonicalValuePtr Visible;
					FUeAgentCanonicalValuePtr Resolved;
					FUeAgentCanonicalValuePtr RecordSchema;
					bool bSemanticChanged = false;
					bool bRootChanged = false;
					for (const auto& Field : Row.GetEntries())
						if (Contract.IsEditableRootField(Field.Key) && !CapturedValues->Get(Field.Key))
							return FailCapturedField(OutError, TEXT("keyed_item_root_field_not_captured"),
								FString::Printf(TEXT("Editable root field '%s' has no captured baseline."), *Field.Key), *CapturedValues, Field.Key);
					if (!Contract.ReadOnlyRootFields.IsEmpty() || !Contract.EditableRootFields.IsEmpty())
						for (const auto& Field : Row.GetEntries())
							if (!IsDeclaredRootField(Field.Key))
							{
								auto Diagnostic = HjsonNodeFailure(TEXT("keyed_item_root_field_unknown"),
									FString::Printf(TEXT("Root field '%s' is not declared by this collection contract."), *Field.Key),
									TEXT("workspace_resolve"), Field.Value.Get(), nullptr, false);
								Diagnostic.ActualValue = MakeShared<FJsonValueString>(Field.Key);
								return FailAtNode(OutError, MoveTemp(Diagnostic), Field.Value.Get());
							}
					if (!Contract.RecordSchemaField.IsEmpty())
					{
						const FHjsonNode* CapturedSchema = CapturedValues->Get(Contract.RecordSchemaField);
						const FHjsonNode* VisibleSchema = Row.Get(Contract.RecordSchemaField);
						FUeAgentCanonicalValuePtr VisibleSchemaValue;
						FUeAgentSemanticComparison SchemaComparison;
						FUeAgentStateSemanticError SchemaError;
						if (!CapturedSchema) return FailCapturedField(OutError, TEXT("keyed_record_schema_read_only"),
							TEXT("The record schema has no captured baseline; export the workspace again."), *CapturedValues, Contract.RecordSchemaField);
						if (!VisibleSchema) return FailField(OutError, TEXT("keyed_record_schema_read_only"),
							TEXT("The required read-only record schema was removed."), Row, Contract.RecordSchemaField);
						if (!TryCanonicalValue(*CapturedSchema, Resources, RecordSchema, OutError)
							|| !TryCanonicalValue(*VisibleSchema, Resources, VisibleSchemaValue, OutError)) return false;
						if (!TryCompareValues(RecordSchema.ToSharedRef(), VisibleSchemaValue.ToSharedRef(),
							VisibleSchema->GetProvenance().GetJsonPointer(), Resources, SchemaComparison, SchemaError))
							return FailAtNode(OutError, MoveTemp(SchemaError), *VisibleSchema);
						if (!SchemaComparison.IsEquivalent())
							return FailAtNode(OutError, MakeComparisonDiagnostic(SchemaComparison, TEXT("keyed_record_schema_read_only"),
								TEXT("The record schema is read-only and differs from its captured value."), TEXT("workspace_resolve")), *VisibleSchema);
					}
					for (const auto& Field : CapturedValues->GetEntries())
					{
						if (IsCollectionRoot(Field.Key)) continue;
						FUeAgentCanonicalValuePtr Value;
						if (!TryCanonicalValue(*Field.Value, Resources, Value, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
						if (Contract.IsReadOnlyRootField(Field.Key))
						{
							const FHjsonNode* VisibleField = Row.Get(Field.Key);
							FUeAgentCanonicalValuePtr VisibleValue;
							FUeAgentSemanticComparison Comparison;
							FUeAgentStateSemanticError Error;
							if (!VisibleField)
								return FailField(OutError, TEXT("keyed_item_root_field_read_only"),
									FString::Printf(TEXT("Required read-only root field '%s' was removed."), *Field.Key), Row, Field.Key);
							if (!TryCanonicalValue(*VisibleField, Resources, VisibleValue, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
							if (!TryCompareValues(Value.ToSharedRef(), VisibleValue.ToSharedRef(),
								VisibleField->GetProvenance().GetJsonPointer(), Resources, Comparison, Error))
							{
								return FailAtNode(OutError, MoveTemp(Error), *VisibleField);
							}
							if (!Comparison.IsEquivalent())
								return FailAtNode(OutError, MakeComparisonDiagnostic(Comparison, TEXT("keyed_item_root_field_read_only"),
									TEXT("A read-only root field differs from its captured value."), TEXT("workspace_resolve")), *VisibleField);
						}
						if (Contract.IsEditableRootField(Field.Key))
						{
							if (const FHjsonNode* VisibleField = Row.Get(Field.Key))
							{
								FUeAgentCanonicalValuePtr VisibleValue;
								FUeAgentSemanticComparison Comparison;
								FUeAgentStateSemanticError Error;
								if (!TryCanonicalValue(*VisibleField, Resources, VisibleValue, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
								if (!TryCompareValues(Value.ToSharedRef(), VisibleValue.ToSharedRef(),
									VisibleField->GetProvenance().GetJsonPointer(), Resources, Comparison, Error))
								{
									return FailAtNode(OutError, MoveTemp(Error), *VisibleField);
								}
								bRootChanged |= !Comparison.IsEquivalent();
								Value = MoveTemp(VisibleValue);
							}
						}
						Values.Add(Field.Key, Value.ToSharedRef());
					}
					const auto MemberSelection = VisibleItems ? SelectKeyedMembers(*VisibleItems, *CapturedItems, Contract, PublicRefToUnitId) : FKeyedMemberSelection();
					int32 InterpretedVisible = 0, InterpretedCaptured = 0;
					const bool bStreamMembers = VisibleItems && VisibleItems->GetKind() == EHjsonNodeKind::Object
						&& CapturedItems->GetKind() == EHjsonNodeKind::Object && HasIndependentNativeMembers(Contract);
					if (bStreamMembers) {
						if (!TryResolveNativeKeyedMembers(*VisibleItems, *CapturedItems, MemberSelection, Contract, PublicRefToUnitId,
							Resources, Resolved, bSemanticChanged, InterpretedVisible, InterpretedCaptured, OutError)) return false;
					} else {
					if (!TryCanonicalKeyedMembers(*CapturedItems, MemberSelection, Resources, Captured, OutError, nullptr, Contract.bInterpretSemanticValues)) return false;
					// Absent fields retain captured state. Only an explicitly supplied
					// collection participates in desired-state item removal/creation.
					if (!VisibleItems) Resolved = Captured;
					else if (VisibleItems == nullptr || !TryCanonicalKeyedMembers(
							*VisibleItems, MemberSelection, Resources, Visible, OutError,
							Contract.bInterpretSemanticValues ? &PublicRefToUnitId : nullptr, Contract.bInterpretSemanticValues)
						|| !TryResolveKeyedItemDesiredState(
							*Visible, *Captured, Row.GetProvenance().GetSourceUnit(),
							Contract, RecordSchema.Get(), Resources, Resolved, bSemanticChanged, OutError, nullptr,
							EKeyedItemInputKind::VisibleDesiredState, nullptr, VisibleItems))
					{
						return false;
					}
					InterpretedVisible = Visible ? Visible->GetObjectItems().Num() : 0;
					InterpretedCaptured = Captured->GetObjectItems().Num();
					}
					if (MemberSelection.Document && Resolved->GetKind() == EUeAgentValueKind::Object) {
						TSet<FString> Pending;
						for (const auto& Operation : Resolved->GetObjectItems()) Pending.Add(Operation.Key);
						MemberSelection.Document->KeyedPending.Add(MemberSelection.Pointer, MoveTemp(Pending));
						if (ActiveRowReferenceRecorder && ActiveRowReferenceRecorder->bComplete)
							MemberSelection.Document->KeyedReferences.Add(MemberSelection.Pointer, ActiveRowReferenceRecorder->Lookups);
						else MemberSelection.Document->KeyedReferences.Remove(MemberSelection.Pointer);
						UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=keyed_source_members root=%s complete_identities=%d interpreted_visible=%d interpreted_captured=%d actual_operations=%d range_candidates=%s"),
							*Contract.RootField, VisibleItems->GetEntries().Num(), InterpretedVisible,
							InterpretedCaptured, Resolved->GetObjectItems().Num(), MemberSelection.bPartial ? TEXT("true") : TEXT("false"));
					}
					Values.Add(Contract.RootField, Resolved.ToSharedRef());
					if (VisibleItems && (!Contract.bIdentityFromMapKey || Contract.bEmitOperationRecords))
						OutResolvedKeyedOperationRoots.Add(Contract.RootField);
					OutManagedSemanticChanged = OutManagedSemanticChanged.Get(false) || bSemanticChanged || bRootChanged;
					return true;
					};
				FWorkspaceError CollectionErrors;
				for (const auto* Contract : KeyedItemContracts)
				{
					FWorkspaceError Error;
					if (!ResolveCollection(*Contract, Error))
					{
						const FHjsonNode* Source = Row.Get(Error.GetSourceUnit());
						if (!Source) Source = Row.Get(Contract->RootField);
						Error.Locate(Source ? Source->GetProvenance() : Row.GetProvenance());
						CollectionErrors.Append(Error);
					}
				}
				if (CollectionErrors.HasErrors()) { OutError = MoveTemp(CollectionErrors); return false; }
			}
			else if (bActorComponentItemProjection)
			{
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_captured_values_missing"),
						TEXT("The actor or component has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				FUeAgentCanonicalValuePtr Captured;
				FUeAgentCanonicalValuePtr Visible;
				FUeAgentCanonicalValuePtr Resolved;
				bool bSemanticChanged = false;
				if (!TryCanonicalValue(*CapturedValues, Resources, Captured, OutError)
					|| !TryCanonicalValue(
						Row, Resources, Visible, OutError,
						&PublicRefToUnitId)
					|| !TryResolveCapturedItemDesiredState(
						*Visible, *Captured, Row.GetProvenance().GetSourceUnit(),
						GetActorComponentHierarchyItemVisibleFieldPolicy(),
						Resources, Resolved, bSemanticChanged, OutError))
				{
					return false;
				}
				for (const auto& Field : Resolved->GetObjectItems())
				{
					Values.Add(Field.Key, Field.Value);
				}
				OutManagedSemanticChanged = bSemanticChanged;
			}
			else if (bKeyedProperty)
			{
				const FHjsonNode* CapturedType =
					Envelope == nullptr ? nullptr : Envelope->Get(TEXT("captured_type"));
				if (CapturedType == nullptr
					|| CapturedType->GetKind() != EHjsonNodeKind::String)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_property_type_missing"),
						TEXT("The property has no captured type string; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_type"), MakeShared<FJsonValueString>(TEXT("string")));
				}
				if (CapturedValues == nullptr
					|| CapturedValues->GetKind() != EHjsonNodeKind::Object)
				{
					return FailCapturedField(OutError, TEXT("semantic_workspace_captured_values_missing"),
						TEXT("The property has no captured values object; export the workspace again."), Envelope ? *Envelope : Row,
						TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
				}
				TArray<FString> TemplateFields;
				for (const auto& Field : CapturedValues->GetEntries())
					TemplateFields.Add(Field.Key);
				if (!TryResolveKeyedPropertyValues(Row, CapturedType->GetText(), TemplateFields,
					Resources, Values, OutError, &PublicRefToUnitId, &OutValueSources)) return false;
				// The optional catalog reference cannot select a different property's schema.
				const FHjsonNode* CapturedDefinition = Envelope->Get(TEXT("captured_definition"));
				// This is a captured readable selection, not an inventory public-ref.
				// Cross-document model references are resolved separately to unit IDs.
				if (Definition && (Definition->GetKind() != EHjsonNodeKind::String
					|| (CapturedDefinition && !Definition->GetText().Equals(
						CapturedDefinition->GetText(), ESearchCase::CaseSensitive))))
					return FailNode(OutError, TEXT("semantic_workspace_property_definition_mismatch"),
						TEXT("The authored property definition differs from the captured definition."), *Definition,
						CapturedDefinition ? HjsonDiagnosticValue(CapturedDefinition) : TSharedPtr<FJsonValue>());
			}
			if (!bDocumentCollectionProjection && !bGraphProjection && !bKeyedItemProjection
				&& !bActorComponentItemProjection
				&& !bKeyedProperty)
			{
				for (const TPair<FString, FHjsonNodeRef>& Item : ValuesNode->GetEntries())
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryCanonicalValue(
							Item.Value.Get(), Resources, Value, OutError,
							&PublicRefToUnitId))
					{
						return false;
					}
					if (Values.Contains(Item.Key))
					{
						auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_unit_values_invalid"),
							TEXT("The semantic values contain a duplicate field name."), TEXT("workspace_resolve"), Item.Value.Get());
						Diagnostic.ActualValue = MakeShared<FJsonValueString>(Item.Key);
						return FailAtNode(OutError, MoveTemp(Diagnostic), Item.Value.Get());
					}
					Values.Add(Item.Key, Value.ToSharedRef());
				}
			}
			} // Value decoding is not a declaration-directory consumer.
			TArray<FString> ItemRefs;
			TArray<FString> RelationRefs;
			TArray<FString> DependencyRefs;
			TArray<FString> ProfileVisibility;
			if (!ReadResolvedArray(TEXT("item_refs"), ItemRefs)
				|| !ReadResolvedArray(TEXT("relation_refs"), RelationRefs)
				|| !ReadResolvedArray(TEXT("dependency_refs"), DependencyRefs)
				|| !ReadResolvedArray(TEXT("profile_visibility"), ProfileVisibility))
			{
				return false;
			}
			// The machine envelope emits the declaration's internal IDs verbatim.
			// Confirm that provenance before interpreting any string as an author alias.
			const auto PublishedDeclaration = PublishedModel && Envelope
				&& (!ItemRefs.IsEmpty() || !RelationRefs.IsEmpty() || !DependencyRefs.IsEmpty())
				? PublishedModel->FindDeclaration(InventoryUnitId) : TOptional<FSemanticUnit::FDeclarationView>();
			auto ResolvePublicRefs = [&](const TCHAR* Field, TArray<FString>& Values, const TArray<FString>* Published)
			{
				if (Published && !Row.Get(Field) && Values.Num() == Published->Num()) {
					bool bSame = true;
					for (int32 Index = 0; Index < Values.Num(); ++Index) {
						if (!Values[Index].Equals((*Published)[Index], ESearchCase::CaseSensitive)) { bSame = false; break; }
					}
					if (bSame) {
						if (ActiveReadState) ActiveReadState->PublishedDeclarationRefIds += Values.Num();
						return;
					}
				}
				if (ActiveReadState) ActiveReadState->MetadataAliasLookups += Values.Num();
				for (FString& Value : Values)
				{
					if (const FString* Internal = FindObservedPublicRef(&PublicRefToUnitId, Value))
					{
						Value = *Internal;
					}
				}
			};
			ResolvePublicRefs(TEXT("item_refs"), ItemRefs, PublishedDeclaration ? &PublishedDeclaration->GetItemRefs() : nullptr);
			ResolvePublicRefs(TEXT("relation_refs"), RelationRefs, PublishedDeclaration ? &PublishedDeclaration->GetRelationRefs() : nullptr);
			ResolvePublicRefs(TEXT("dependency_refs"), DependencyRefs, PublishedDeclaration ? &PublishedDeclaration->GetDependencyRefs() : nullptr);
			const auto Identity = UnitId.Equals(InventoryUnitId, ESearchCase::CaseSensitive)
				? InventoryIdentity : MakeShared<const FString, ESPMode::ThreadSafe>(MoveTemp(UnitId));
			OutUnit.Emplace(
				Identity,
				UnitKind,
				MoveTemp(OwnerRef),
				MoveTemp(SemanticTypeRef),
				MoveTemp(Role),
				MoveTemp(FieldSetRef),
				MoveTemp(Values),
				MoveTemp(ItemRefs),
				MoveTemp(RelationRefs),
				MoveTemp(OrderingRef),
				MoveTemp(LayoutRef),
				MoveTemp(DependencyRefs),
				MoveTemp(ProfileVisibility),
				MoveTemp(SourceProvenance),
				MoveTemp(TargetIdentityKey), MoveTemp(CategoryPath));
			if (!OutUnit->IsValid())
			{
				auto Diagnostic = OutUnit->GetValidationDiagnostic();
				OutUnit.Reset();
				return FailAtNode(OutError, MoveTemp(Diagnostic), Row);
			}
			return true;
		}

		bool SafeRelativePath(
			FString Value,
			FString& OutPath)
		{
			Value.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Value.IsEmpty()
				|| Value.StartsWith(TEXT("/"))
				|| Value.Contains(TEXT(":")))
			{
				return false;
			}
			TArray<FString> Segments;
			Value.ParseIntoArray(Segments, TEXT("/"), false);
			if (Segments.IsEmpty())
			{
				return false;
			}
			for (const FString& Segment : Segments)
			{
				if (Segment.IsEmpty()
					|| Segment == TEXT(".")
					|| Segment == TEXT(".."))
				{
					return false;
				}
			}
			OutPath = FString::Join(Segments, TEXT("/"));
			return true;
		}

		bool IsReparsePoint(const FString& Path)
		{
			const ESymlinkResult Result =
				FPlatformFileManager::Get()
					.GetPlatformFile()
					.IsSymlink(*Path);
			return Result == ESymlinkResult::Symlink;
		}
		// BufferToString and its terminated UTF-8 output are the identity for
		// these actual bytes. Excluding NUL preserves the original text hash's
		// first-terminator rule; BOM/UTF-16/non-ASCII retain their encoding owner.
		bool IsNonNullAsciiWorkspaceText(const TArray<uint8>& Bytes)
		{
			for (const uint8 Byte : Bytes) if (Byte == 0 || Byte >= 128) return false;
			return true;
		}

		bool ObserveSourceFile(const FString& Path, bool bNeedText, FString& OutText,
			FString& OutHash, int64& OutCharacters, TArray<uint8>* OutUtf8 = nullptr, FString* OutByteHash = nullptr,
			int64* OutDecodedUtf8Bytes = nullptr, FWorkspaceError* OutFailure = nullptr)
		{
			if (OutDecodedUtf8Bytes) *OutDecodedUtf8Bytes = INDEX_NONE;
			TSharedPtr<FWorkspaceSourceObservation> PublicationSession;
			const FString* PublishedHash = nullptr; FString Relative;
			bool bNeedPublishedRaw = false;
			if (ActiveReadState && ActiveReadState->bEstablishFileObservation && ActiveReadState->PublishedFileHashes
			&& Path.StartsWith(ActiveReadState->FileObservationRoot + TEXT("/"), ESearchCase::CaseSensitive)
			&& IsPublicSemanticFileCandidate(Path.Mid(ActiveReadState->FileObservationRoot.Len() + 1))) {
				Relative = Path.Mid(ActiveReadState->FileObservationRoot.Len() + 1);
				PublicationSession = WorkspaceSourceObservations.FindRef(ActiveReadState->FileObservationRoot);
				PublishedHash = ActiveReadState->PublishedFileHashes->Find(Relative);
				const auto* Version = PublicationSession ? PublicationSession->Files.Find(Relative) : nullptr;
				bNeedPublishedRaw = PublicationSession && PublishedHash && (!Version || !Version->ComparisonBytes
					|| (!PublicationSession->PendingDocuments.Contains(Relative) && Version->ComparisonHash != *PublishedHash));
			}
			const auto* Previous = ObservedSourceFiles.Find(Path);
			if (!bNeedText && !OutUtf8 && !bNeedPublishedRaw && Previous && Previous->Proof && Previous->Proof->IsCurrent())
			{
				OutHash = Previous->ContentHash; OutCharacters = Previous->SourceCharacters;
				if (OutByteHash && Previous->bByteHashObserved) *OutByteHash = Previous->GetByteHash();
				if (ActiveReadState) ++ActiveReadState->ReusedFileObservations;
				return true;
			}
			const double ProofBeginStarted = FPlatformTime::Seconds();
			auto Observation = TryBeginWorkspaceFileObservation(Path);
			const double BodyReadStarted = FPlatformTime::Seconds();
			if (ActiveReadState) ActiveReadState->FileProofBeginSeconds += BodyReadStarted - ProofBeginStarted;
			TArray<uint8> LocalBytes;
			TArray<uint8>& Bytes = OutUtf8 ? *OutUtf8 : LocalBytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
			const double ByteHashStarted = FPlatformTime::Seconds();
			if (ActiveReadState) ActiveReadState->FileBodyReadSeconds += ByteHashStarted - BodyReadStarted;
			FInventoryDigest BytesHash; BytesHash.SetFromBytes(Bytes.GetData(), Bytes.Num());
			const FString ActualByteHash = BytesHash.ToString();
			if (OutByteHash) *OutByteHash = ActualByteHash;
			const double TextEncodingStarted = FPlatformTime::Seconds();
			if (ActiveReadState) ActiveReadState->FileByteHashSeconds += TextEncodingStarted - ByteHashStarted;
			if (!bNeedText && PublishedHash && ActualByteHash == *PublishedHash && IsNonNullAsciiWorkspaceText(Bytes)) {
				OutText.Reset(); OutHash = ActualByteHash; OutCharacters = Bytes.Num();
				if (OutDecodedUtf8Bytes) *OutDecodedUtf8Bytes = Bytes.Num();
				++ActiveReadState->DirectHashOnlyFiles; ActiveReadState->DirectHashOnlyBytes += Bytes.Num();
			} else {
				FFileHelper::BufferToString(OutText, Bytes.GetData(), Bytes.Num());
				const FTCHARToUTF8 CanonicalUtf8(*OutText);
				if (OutDecodedUtf8Bytes) *OutDecodedUtf8Bytes = CanonicalUtf8.Length();
				OutHash = CanonicalUtf8.Length() == Bytes.Num()
					&& (Bytes.IsEmpty() || FMemory::Memcmp(CanonicalUtf8.Get(), Bytes.GetData(), Bytes.Num()) == 0)
					? ActualByteHash : Sha256Text(OutText);
				OutCharacters = OutText.Len();
			}
			if (ActiveReadState) ++ActiveReadState->ActualFileReads;
			const double ProofEndStarted = FPlatformTime::Seconds();
			if (ActiveReadState) ActiveReadState->FileTextEncodingSeconds += ProofEndStarted - TextEncodingStarted;
			if (Observation && !Observation->IsCurrent()) Observation.Reset();
			const double AdmissionStarted = FPlatformTime::Seconds();
			if (ActiveReadState) ActiveReadState->FileProofEndSeconds += AdmissionStarted - ProofEndStarted;
			// The limit bounds optional retained observations, not the complete
			// current read. Keep existing proofs when a new path cannot be admitted.
			if (ObservedSourceFiles.Num() < 16384 || ObservedSourceFiles.Contains(Path))
				StoreObservedSourceFile(Path, {OutHash, OutCharacters, MoveTemp(Observation),
					OutHash == ActualByteHash ? FString() : ActualByteHash, true});
			if (bNeedPublishedRaw && ActualByteHash == *PublishedHash) {
				auto& Version = PublicationSession->Files.FindOrAdd(Relative);
				const int64 Before = Version.ComparisonBytes ? Version.ComparisonBytes->GetAllocatedSize() : 0;
				const int64 After = Bytes.GetAllocatedSize();
				if (After > SourceVersionRetentionBudget - PublishedComparisonBytes + Before) {
					if (OutFailure) FailWorkspaceDataflowHandoff(*OutFailure, TEXT("source.published_raw"), Relative, Path,
						TEXT("exact comparison bytes cannot be held by the existing Source version owner"),
						TEXT("same-publication old raw input for Myers"), TEXT("explicit Source byte observation"));
					return false;
				}
				// Transfer the bytes already read for this exact public hash. This is
				// a raw document version, never the author-consumption baseline.
				Version.ComparisonBytes = OutUtf8 ? MakeShared<const TArray<uint8>>(Bytes)
					: MakeShared<const TArray<uint8>>(MoveTemp(LocalBytes));
				Version.Hash = *PublishedHash; Version.ComparisonHash = *PublishedHash;
				PublishedComparisonBytes += After - Before; PublicationSession->RetainedBytes += After - Before;
			}
			if (ActiveReadState) ActiveReadState->FileAdmissionSeconds += FPlatformTime::Seconds() - AdmissionStarted;
			return true;
		}

		bool ReadDocument(
			const FString& Path,
			const FString& SourceUnit,
			FUeAgentResourceTracker& Resources,
			FHjsonDocumentPtr& OutDocument,
			FWorkspaceError& OutError, FString* OutContentHash = nullptr, bool bRetainSyntax = true)
		{
			if (IsReparsePoint(Path))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"), TEXT("The document or one of its ancestor directories is a reparse point."),
					SourceUnit);
			}
			if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::Files, 1}},
					TEXT("semantic_workspace:") + SourceUnit,
					OutError))
			{
				return false;
			}
			FString CachePath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(CachePath);
			const auto* Cached = ActiveReadState ? ActiveReadState->Documents.Find(CachePath) : nullptr;
			const FCompletedDocumentRead* PreviousDocument = ActiveReadState ? PreviousDocumentReads.Find(CachePath) : nullptr;
			const bool bCachedSource = Cached && Cached->SourceUnit.Equals(SourceUnit, ESearchCase::CaseSensitive)
				&& (SourceUnit != ManifestPath || Cached->Parsed->GetRoot()->Get(TEXT("unit_inventory"))
					|| Cached->Parsed->GetRoot()->Get(TEXT("unit_inventory_files")));
			const bool bPreviousSource = PreviousDocument && PreviousDocument->SourceUnit.Equals(SourceUnit, ESearchCase::CaseSensitive);
			FString Text, ContentHash;
			int64 SourceCharacters = 0;
			if (!ObserveSourceFile(CachePath, !bCachedSource && !bPreviousSource, Text, ContentHash, SourceCharacters, nullptr, nullptr, nullptr, &OutError))
			{
				if (OutError.HasErrors()) return false;
				return Fail(
					OutError,
					TEXT("workspace_source_read_failed"), TEXT("The semantic workspace document could not be read."),
					SourceUnit);
			}
			if (OutContentHash) *OutContentHash = ContentHash;
			if (bCachedSource && Cached->ContentHash == ContentHash)
			{
				OutDocument = Cached->Parsed;
				OutError.Reset();
				return true;
			}
			const auto BeforeParse = Resources.Snapshot();
			TMap<EUeAgentResourceDimension, int64> ParseCharges;
			bool bExactParseCharges = true;
			const bool bReuseDocument = bPreviousSource
				&& PreviousDocument->ContentHash == ContentHash;
			FHjsonDocumentUpdateMetrics UpdateMetrics;
			FHjsonDocumentPtr DiffBaseDocument;
			FString DiffBaseHash;
			if (bReuseDocument) {
				++ActiveReadState->ReusedPublicSyntax;
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(PreviousDocument->ParseCharges, TEXT("semantic_workspace:actual_public_document"), ResourceError))
					return FailResource(OutError, ResourceError);
				OutDocument = PreviousDocument->Parsed;
				ParseCharges = PreviousDocument->ParseCharges;
			} else {
				// A digest-only observation may have been made by another reader in
				// this same scope. A changed document still needs its actual text.
				if (Text.IsEmpty() && SourceCharacters > 0) {
					FString ActualHash; int64 ActualCharacters = 0;
					if (!ObserveSourceFile(CachePath, true, Text, ActualHash, ActualCharacters) || ActualHash != ContentHash)
						return Fail(OutError, TEXT("workspace_source_changed_during_read"),
							TEXT("The source document changed while it was being parsed."), SourceUnit);
				}
				if (bRetainSyntax && SourceUnit != ManifestPath && (!ActiveReadState || !ActiveReadState->bEstablishFileObservation)) {
					auto OldDocument = bCachedSource && Cached->Text ? Cached->Parsed
						: bPreviousSource && PreviousDocument->Text ? PreviousDocument->Parsed : FHjsonDocumentPtr();
					auto OldText = bCachedSource && Cached->Text ? Cached->Text
						: bPreviousSource ? PreviousDocument->Text : TSharedPtr<const FString>();
					DiffBaseHash = bCachedSource && Cached->Text ? Cached->ContentHash
						: bPreviousSource ? PreviousDocument->ContentHash : FString();
					const bool bPublicDocument = IsPublicSemanticFileCandidate(SourceUnit);
					if (bPublicDocument && ActiveReadState && !ActiveReadState->bEstablishFileObservation) {
						const auto Session = WorkspaceSourceObservations.FindRef(ActiveReadState->FileObservationRoot);
						const auto* Published = Session ? Session->Files.Find(SourceUnit) : nullptr;
						if (!Published || !Published->ComparisonBytes) return Fail(OutError,
							TEXT("workspace_incremental_origin_missing"), TEXT("The selected document has no matching published raw version. The publication producer must supply it; no current-document Build was selected."), SourceUnit);
						if (!OldDocument || !OldText || DiffBaseHash != Published->ComparisonHash) {
							FString PublishedText; const auto& Bytes = *Published->ComparisonBytes;
							FFileHelper::BufferToString(PublishedText, Bytes.GetData(), Bytes.Num());
							// Initial construction of this known old version is a formal
							// parser input, not a retry parsing the changed document.
							const double OldBuildStarted = FPlatformTime::Seconds();
							if (!TryParseHjsonDocument(PublishedText, SourceUnit, Resources, OldDocument, OutError)) return false;
							UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_old_version_build document=%s bytes=%d total_s=%.6f input=exact_published_raw current_document_build=false"),
								*SourceUnit, Bytes.Num(), FPlatformTime::Seconds() - OldBuildStarted);
							OldText = MakeShared<const FString>(MoveTemp(PublishedText)); DiffBaseHash = Published->ComparisonHash;
						}
					}
					DiffBaseDocument = OldDocument;
					if (OldDocument && OldText) {
						if (!TryUpdateHjsonDocument(*OldText, Text, OldDocument.ToSharedRef(), Resources, OutDocument, OutError, &UpdateMetrics)) return false;
					} else {
						if (bPublicDocument && (!ActiveReadState || !ActiveReadState->bEstablishFileObservation))
							return Fail(OutError, TEXT("workspace_incremental_syntax_origin_missing"), TEXT("The selected existing document is missing its old text/tree input; no whole-document retry was selected."), SourceUnit);
						UpdateMetrics.bWholeDocument = true;
						UpdateMetrics.ReparsedBytes = FTCHARToUTF8(*Text).Length();
						if (!TryParseHjsonDocument(Text, SourceUnit, Resources, OutDocument, OutError)) return false;
					}
					if (ActiveReadState) {
						++ActiveReadState->UpdatedSyntaxDocuments;
						ActiveReadState->WholeSyntaxDocuments += UpdateMetrics.bWholeDocument ? 1 : 0;
						ActiveReadState->MissingSyntaxText += OldText ? 0 : 1;
						ActiveReadState->UpdatedSyntaxBytes += UpdateMetrics.ReparsedBytes;
						ActiveReadState->ReusedSyntaxSubtrees += UpdateMetrics.ReusedSubtrees;
					}
				} else {
					// This input is already the complete decoded metadata file, not
					// an unknown fragment within it. The original one-use parser
					// supplies the root without a span-location scan, text copy, or
					// complete syntax archive. A failed parse has no alternate route.
					FHjsonNodePtr Metadata;
					if (!TryParseHjsonSelectedNodes(Text, SourceUnit, Resources, Metadata, OutError,
						[](const FString&, const FString&) { return true; })) return false;
					OutDocument = MakeShared<FHjsonDocument>(Metadata.ToSharedRef());
				}
			}
			if (!bReuseDocument) {
				const auto After = Resources.Snapshot();
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 A = BeforeParse.Consumed(Dimension), B = After.Consumed(Dimension);
					if (B == MAX_int64 || B < A) { bExactParseCharges = false; break; }
					if (B > A) ParseCharges.Add(Dimension, B - A);
				}
			}
			if (ActiveReadState)
			{
				auto& Stored = ActiveReadState->Documents.FindOrAdd(CachePath);
				Stored.SourceUnit = SourceUnit;
				Stored.ContentHash = ContentHash;
				Stored.SourceCharacters = SourceCharacters;
				Stored.RetainedSyntaxBytes = bReuseDocument ? PreviousDocument->RetainedSyntaxBytes : 0;
				Stored.Parsed = OutDocument;
				Stored.ParseCharges = MoveTemp(ParseCharges);
				Stored.bExactParseCharges = bExactParseCharges;
				Stored.bComparedText = bReuseDocument || UpdateMetrics.bComparedText;
				Stored.DiffBaseHash = bReuseDocument ? PreviousDocument->DiffBaseHash : DiffBaseHash;
				Stored.DiffBaseDocument = bReuseDocument ? PreviousDocument->DiffBaseDocument : DiffBaseDocument;
				Stored.Changes = bReuseDocument ? PreviousDocument->Changes : MoveTemp(UpdateMetrics.Changes);
				if (bPreviousSource) {
					Stored.KeyedPending = PreviousDocument->KeyedPending;
					Stored.KeyedReferences = PreviousDocument->KeyedReferences;
				}
				Stored.Text = bReuseDocument ? PreviousDocument->Text
					: bRetainSyntax ? MakeShared<const FString>(MoveTemp(Text)) : TSharedPtr<const FString>();
			}
			return true;
		}

		bool EnsureInventorySource(FInventoryRow& Row, FWorkspaceError& OutError);

		// Same Engine hash/equality as FString. All buffers belong to immutable
		// row syntax (or a moved row override) until these local sets are retired;
		// no semantic rename/override mutation runs during inventory discovery.
		using FInventoryIdentitySet = TSet<FStringView>;
		bool AddInventoryIdentity(const FInventoryRow& Row, FInventoryIdentitySet& UnitIds,
			FInventoryIdentitySet& PublicRefs, FInventoryIdentitySet& LogicalPaths, FWorkspaceError& OutError,
			TFunctionRef<FHjsonNodePtr()>* DiagnosticSource = nullptr)
		{
			const FStringView UnitId(Row.Syntax->GetUnitId()), PublicRef(Row.Syntax->GetPublicRef()), LogicalPath(Row.GetLogicalPath());
			if (UnitIds.Contains(UnitId) || PublicRefs.Contains(PublicRef)
				|| LogicalPaths.Contains(LogicalPath)) {
				FInventoryRow DiagnosticRow = Row;
				if (DiagnosticSource) {
					DiagnosticRow.RetainedSourceNode = (*DiagnosticSource)();
					DiagnosticRow.SourceNode = DiagnosticRow.RetainedSourceNode.Get();
				}
				if (!EnsureInventorySource(DiagnosticRow, OutError)) return false;
			if (UnitIds.Contains(UnitId))
				return FailField(OutError, TEXT("semantic_workspace_unit_duplicate"),
					TEXT("The workspace inventory contains a duplicate unit identity."), *DiagnosticRow.SourceNode, TEXT("unit_id"),
					MakeShared<FJsonValueString>(TEXT("unique unit identity")), false);
			if (PublicRefs.Contains(PublicRef))
				return FailField(OutError, TEXT("semantic_workspace_public_ref_duplicate"),
					TEXT("The workspace inventory contains a duplicate public reference."), *DiagnosticRow.SourceNode, TEXT("public_ref"),
					MakeShared<FJsonValueString>(TEXT("unique public reference")));
			if (LogicalPaths.Contains(LogicalPath))
				return FailField(OutError, TEXT("semantic_workspace_inventory_path_duplicate"),
					TEXT("The workspace inventory contains a duplicate logical path."), *DiagnosticRow.SourceNode, TEXT("logical_path"),
					MakeShared<FJsonValueString>(TEXT("unique logical path")));
			}
			UnitIds.Add(UnitId); PublicRefs.Add(PublicRef); LogicalPaths.Add(LogicalPath);
			return true;
		}

		// These are the existing inventory string fields, borrowed from the same
		// grammar's token owner until the synchronous row compiler completes.
		// They are not another inventory index, cache or retained syntax tree.
		struct FInventoryParsedStrings
		{
			struct FValue {
				FStringView Text, Lexeme;
				bool bPresent = false;
				int32 Line = 0, Column = 0;
				int64 ByteStart = 0, ByteEnd = 0;
			};
			static const TArray<FString>& Names() {
				static const TArray<FString> Values {TEXT("unit_id"), TEXT("public_ref"), TEXT("unit_kind"),
					TEXT("logical_path"), TEXT("document_path"), TEXT("unit_pointer"), TEXT("content_hash"),
					TEXT("edit_hash"), TEXT("envelope_ref")};
				return Values;
			}
			FValue Values[9];
			struct FSpanValue {
				FStringView Lexeme;
				bool bPresent = false;
				int32 Line = 0, Column = 0;
				int64 ByteStart = 0, ByteEnd = 0;
			};
			FSpanValue SpanValues[2];
			bool HasProjectedSpan() const { return SpanValues[0].bPresent || SpanValues[1].bPresent; }
			FHjsonNodeRef RestoreSpan(const FHjsonNode& Span) const {
				auto Items = Span.GetItems(); const auto& Source = Span.GetProvenance();
				for (int32 Index = 0; Index < 2; ++Index) if (SpanValues[Index].bPresent) {
					const auto& Value = SpanValues[Index];
					Items.Add(MakeShared<FHjsonNode>(EHjsonNodeKind::Integer,
						FSourceProvenance(Source.GetSourceUnit(), Source.GetJsonPointer() + TEXT("/") + ::LexToString(Index),
							Value.Line, Value.Column, Value.ByteStart, Value.ByteEnd, FString()),
						FString(Value.Lexeme), FString(), false, TArray<FHjsonNodeRef>{}, TArray<TPair<FString, FHjsonNodeRef>>{}));
				}
				Items.Sort([](const auto& A, const auto& B) { return A->GetProvenance().GetByteStart() < B->GetProvenance().GetByteStart(); });
				return MakeShared<FHjsonNode>(Span.GetKind(), Source, Span.GetLexeme(), Span.GetText(), Span.GetBoolean(),
					MoveTemp(Items), Span.GetEntries());
			}
			static int32 FieldIndex(FStringView Key) {
				const auto& Keys = Names();
				int32 Candidate = INDEX_NONE;
				switch (Key.Len()) {
				case 7: Candidate = 0; break;
				case 9: Candidate = Key[0] == TEXT('u') ? 2 : 7; break;
				case 10: Candidate = 1; break;
				case 12:
					switch (Key[0]) { case TEXT('l'): Candidate = 3; break; case TEXT('u'): Candidate = 5; break;
					case TEXT('c'): Candidate = 6; break; case TEXT('e'): Candidate = 8; break; }
					break;
				case 13: Candidate = 4; break;
				}
				if (Candidate != INDEX_NONE && Key.Equals(Keys[Candidate], ESearchCase::CaseSensitive)) return Candidate;
				return INDEX_NONE;
			}
			const FValue* Find(FStringView Key) const {
				const int32 Index = FieldIndex(Key);
				return Index != INDEX_NONE && Values[Index].bPresent ? &Values[Index] : nullptr;
			}
			FHjsonNodeRef MakeField(const FHjsonNode& Item, int32 Index) const {
				const auto& Value = Values[Index]; const auto& Source = Item.GetProvenance();
				const auto& Key = Names()[Index];
				return MakeShared<FHjsonNode>(EHjsonNodeKind::String,
					FSourceProvenance(Source.GetSourceUnit(), Source.GetJsonPointer() + TEXT("/") + Key,
						Value.Line, Value.Column, Value.ByteStart, Value.ByteEnd, Key),
					FString(Value.Lexeme), FString(Value.Text), false, TArray<FHjsonNodeRef>{}, TArray<TPair<FString, FHjsonNodeRef>>{});
			}
			FHjsonNodePtr MakeDiagnosticItem(const FHjsonNode& Item) const {
				auto Entries = Item.GetEntries();
				if (HasProjectedSpan()) for (auto& Entry : Entries)
					if (Entry.Key.Equals(TEXT("envelope_span"), ESearchCase::CaseSensitive)) { Entry.Value = RestoreSpan(Entry.Value.Get()); break; }
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index)
					if (Values[Index].bPresent) Entries.Emplace(Names()[Index], MakeField(Item, Index));
				Entries.Sort([](const auto& A, const auto& B) { return A.Value->GetProvenance().GetByteStart() < B.Value->GetProvenance().GetByteStart(); });
				return MakeShared<FHjsonNode>(Item.GetKind(), Item.GetProvenance(), Item.GetLexeme(), Item.GetText(),
					Item.GetBoolean(), Item.GetItems(), MoveTemp(Entries));
			}
		};

		bool ReadInventorySpanInteger(FStringView Lexeme, int64& OutValue) {
			TStringBuilder<64> Terminated;
			Terminated.Append(Lexeme);
			return LexTryParseString(OutValue, Terminated.ToString());
		}

		struct FInventoryItemFields
		{
			const FInventoryParsedStrings* ParsedStrings = nullptr;
			const FHjsonNode *UnitId = nullptr, *PublicRef = nullptr, *UnitKind = nullptr, *LogicalPath = nullptr;
			const FHjsonNode *DocumentPath = nullptr, *UnitPointer = nullptr, *ContentHash = nullptr, *EditHash = nullptr;
			const FHjsonNode *EnvelopeSpan = nullptr, *ContentFiles = nullptr, *Envelope = nullptr, *EnvelopeRef = nullptr;
			explicit FInventoryItemFields(const FHjsonNode& Item, const FInventoryParsedStrings* InStrings = nullptr)
				: ParsedStrings(InStrings)
			{
				for (const auto& Entry : Item.GetEntries()) {
					if (ActiveReadState) ++ActiveReadState->InventoryFieldNodesVisited;
					const FStringView Key(Entry.Key);
					const auto Bind = [&](FStringView Name, const FHjsonNode*& Destination) {
						// Preserve Get's first exact spelling, including constructed
						// nodes whose duplicate keys never came through the parser.
						if (!Destination && Key.Equals(Name, ESearchCase::CaseSensitive)) Destination = &Entry.Value.Get();
					};
					switch (Key.Len()) {
					case 7: Bind(TEXT("unit_id"), UnitId); break;
					case 8: Bind(TEXT("envelope"), Envelope); break;
					case 9:
						if (Key[0] == TEXT('u')) Bind(TEXT("unit_kind"), UnitKind);
						else if (Key[0] == TEXT('e')) Bind(TEXT("edit_hash"), EditHash);
						break;
					case 10: Bind(TEXT("public_ref"), PublicRef); break;
					case 12:
						switch (Key[0]) {
						case TEXT('l'): Bind(TEXT("logical_path"), LogicalPath); break;
						case TEXT('c'): Bind(TEXT("content_hash"), ContentHash); break;
						case TEXT('u'): Bind(TEXT("unit_pointer"), UnitPointer); break;
						case TEXT('e'): Bind(TEXT("envelope_ref"), EnvelopeRef); break;
						}
						break;
					case 13:
						switch (Key[0]) {
						case TEXT('d'): Bind(TEXT("document_path"), DocumentPath); break;
						case TEXT('e'): Bind(TEXT("envelope_span"), EnvelopeSpan); break;
						case TEXT('c'): Bind(TEXT("content_files"), ContentFiles); break;
						}
						break;
					}
				}
			}
			bool ReadString(const FHjsonNode& Item, FStringView Key, const FHjsonNode* Node,
				bool bRequired, FStringView& OutValue, FWorkspaceError& Error) const {
				const auto* Parsed = ParsedStrings ? ParsedStrings->Find(Key) : nullptr;
				if (!Parsed) return ReadMetadataStringNode(Item, Key, Node, bRequired, OutValue, Error);
				if (ReadMetadataStringText(Parsed->Text, bRequired, OutValue)) return true;
				const auto DiagnosticNode = ParsedStrings->MakeField(Item, FInventoryParsedStrings::FieldIndex(Key));
				return ReadMetadataStringNode(Item, Key, &DiagnosticNode.Get(), bRequired, OutValue, Error);
			}
			bool ReadOwnedString(const FHjsonNode& Item, FStringView Key, const FHjsonNode* Node,
				bool bRequired, FString& OutValue, FWorkspaceError& Error) const {
				if (!bRequired) OutValue.Reset();
				FStringView Value;
				const bool bRead = ReadString(Item, Key, Node, bRequired, Value, Error);
				if (bRead || (Node && Node->GetKind() == EHjsonNodeKind::String)
					|| (ParsedStrings && ParsedStrings->Find(Key))) OutValue = FString(Value);
				return bRead;
			}
			FStringView RawString(FStringView Key, const FHjsonNode* Node) const {
				if (const auto* Parsed = ParsedStrings ? ParsedStrings->Find(Key) : nullptr) return Parsed->Text;
				return Node->GetText();
			}
			FHjsonNodePtr DiagnosticItem(const FHjsonNode& Item) const {
				return ParsedStrings ? ParsedStrings->MakeDiagnosticItem(Item) : FHjsonNodePtr();
			}
			bool FailItemField(FWorkspaceError& Error, const FString& Code, const FString& Reason,
				const FHjsonNode& Item, const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr) const {
				const auto Diagnostic = DiagnosticItem(Item);
				return FailField(Error, Code, Reason, Diagnostic ? *Diagnostic : Item, Field, MoveTemp(Expected));
			}
		};

		bool ParseInventory(
			const FHjsonNode& Manifest,
			TArray<FInventoryRow>& OutRows,
			FWorkspaceError& OutError, const TArray<FInventoryParsedStrings>* ParsedStrings = nullptr)
		{
			OutRows.Reset();
			const FHjsonNode* Inventory =
				Manifest.Get(TEXT("unit_inventory"));
			if (Inventory == nullptr
				|| Inventory->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
					TEXT("Workspace metadata requires a unit_inventory array."), Manifest, TEXT("unit_inventory"), MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			FWorkspaceError Diagnostics;
			FInventoryIdentitySet UnitIds;
			FInventoryIdentitySet PublicRefs;
			FInventoryIdentitySet LogicalPaths;
			// A baseline path is shared by many rows in one inventory document.
			// The temporary lookup dies with this parse; accepted syntax/proofs own
			// only the exact immutable spelling they actually consume.
			TMap<uint32, TArray<TSharedPtr<const FString>>> EnvelopePaths;
			for (int32 ItemIndex = 0; ItemIndex < Inventory->GetItems().Num(); ++ItemIndex)
			{
				const FHjsonNodeRef& Item = Inventory->GetItems()[ItemIndex];
				OutError.Reset();
				auto ReadItem = [&]() -> bool
				{
					if (Item->GetKind()
						!= EHjsonNodeKind::Object)
					{
						return FailNode(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("Each workspace inventory entry must be an object."), Item.Get(), MakeShared<FJsonValueString>(TEXT("object")), false);
					}
					const FInventoryItemFields Fields(Item.Get(),
						ParsedStrings && ParsedStrings->IsValidIndex(ItemIndex) ? &(*ParsedStrings)[ItemIndex] : nullptr);
					FInventoryRowSyntax Row;
					FString UnitId;
					FStringView ContentHashText, EditHashText, EnvelopeRef, KindText;
					const FHjsonNode* SourceNode = &Item.Get();
					if (!Fields.ReadOwnedString(Item.Get(), TEXT("unit_id"), Fields.UnitId, true, UnitId, OutError)
						|| !Fields.ReadOwnedString(Item.Get(), TEXT("public_ref"), Fields.PublicRef, true, Row.PublicRef, OutError)
						|| !Fields.ReadString(Item.Get(), TEXT("unit_kind"), Fields.UnitKind, true, KindText, OutError)
						|| !Fields.ReadOwnedString(Item.Get(), TEXT("logical_path"), Fields.LogicalPath, true, Row.LogicalPath, OutError)
						|| !Fields.ReadOwnedString(Item.Get(), TEXT("document_path"), Fields.DocumentPath, false, Row.DocumentPath, OutError)
						|| !Fields.ReadOwnedString(Item.Get(), TEXT("unit_pointer"), Fields.UnitPointer, false, Row.UnitPointer, OutError)
						|| !Fields.ReadString(Item.Get(), TEXT("content_hash"), Fields.ContentHash, true, ContentHashText, OutError)
						|| !Fields.ReadString(Item.Get(), TEXT("edit_hash"), Fields.EditHash, false, EditHashText, OutError))
					{
						return false;
					}
					Row.ContentHash.SetView(ContentHashText);
					Row.EditHash.SetView(EditHashText);
					if (ActiveReadState) ActiveReadState->InventoryCompactDigestViews += int64(Row.ContentHash.bCompact) + int64(Row.EditHash.bCompact);
					if (const auto* Span = Fields.EnvelopeSpan) {
						const auto* Projection = Fields.ParsedStrings;
						const bool bProjected = Projection && Projection->HasProjectedSpan();
						const bool bIntegers = bProjected
							? Span->GetKind() == EHjsonNodeKind::Array && Span->GetItems().IsEmpty()
								&& Projection->SpanValues[0].bPresent && Projection->SpanValues[1].bPresent
								&& ReadInventorySpanInteger(Projection->SpanValues[0].Lexeme, Row.StoredEnvelopeSpan.Begin)
								&& ReadInventorySpanInteger(Projection->SpanValues[1].Lexeme, Row.StoredEnvelopeSpan.End)
							: Span->GetKind() == EHjsonNodeKind::Array && Span->GetItems().Num() == 2
								&& Span->GetItems()[0]->GetKind() == EHjsonNodeKind::Integer
								&& Span->GetItems()[1]->GetKind() == EHjsonNodeKind::Integer
								&& ReadInventorySpanInteger(Span->GetItems()[0]->GetLexeme(), Row.StoredEnvelopeSpan.Begin)
								&& ReadInventorySpanInteger(Span->GetItems()[1]->GetLexeme(), Row.StoredEnvelopeSpan.End);
						if (!bIntegers || !Row.StoredEnvelopeSpan.IsValid() || Row.StoredEnvelopeSpan.End > MAX_int32) {
							const FHjsonNodePtr Diagnostic = bProjected ? FHjsonNodePtr(Projection->RestoreSpan(*Span)) : FHjsonNodePtr();
							return FailNode(OutError, TEXT("semantic_workspace_envelope_span_invalid"),
								TEXT("envelope_span must contain the original baseline UTF-8 byte begin/end as two ordered nonnegative integers."), Diagnostic ? *Diagnostic : *Span);
							}
						}
					if (Row.DocumentPath.IsEmpty())
					{
						Row.DocumentPath = Row.LogicalPath;
					}
					if (Row.DocumentPath.IsEmpty())
					{
						return Fields.FailItemField(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The inventory entry has neither a document path nor a logical path."), Item.Get(),
							TEXT("document_path"), MakeShared<FJsonValueString>(TEXT("non-empty logical document path")));
					}
					if (Row.UnitPointer.IsEmpty())
					{
						Row.UnitPointer = TEXT("/unit");
					}
					if (const FHjsonNode* ContentFiles = Fields.ContentFiles)
					{
						if (ContentFiles->GetKind() != EHjsonNodeKind::Array)
							return FailNode(OutError, TEXT("semantic_workspace_content_files_invalid"),
								TEXT("The inventory content_files field must be an array."), *ContentFiles, MakeShared<FJsonValueString>(TEXT("array")), false);
						for (const auto& File : ContentFiles->GetItems())
						{
							if (File->GetKind() != EHjsonNodeKind::String)
								return FailNode(OutError, TEXT("semantic_workspace_content_files_invalid"),
									TEXT("Each inventory content_files entry must be a string."), File.Get(), MakeShared<FJsonValueString>(TEXT("string")), false);
							Row.ContentFiles.Add(File->GetText());
						}
					}
					const FHjsonNode* Envelope = Fields.Envelope;
					if (!Fields.ReadString(Item.Get(), TEXT("envelope_ref"), Fields.EnvelopeRef, false, EnvelopeRef, OutError)) return false;
					if (!EnvelopeRef.IsEmpty())
					{
						if (Envelope)
							return Fields.FailItemField(OutError, TEXT("semantic_workspace_envelope_ref_invalid"),
								TEXT("A captured envelope must be inline or reference a managed baseline document, exclusively."),
								Item.Get(), TEXT("envelope_ref"));
						// This same parser already owns the validated normalized spelling.
						// A hash only finds a candidate; exact original text proves reuse.
						const FStringView Raw = Fields.RawString(TEXT("envelope_ref"), Fields.EnvelopeRef);
						if (EnvelopeRef.Len() == Raw.Len()) if (const auto* Candidates = EnvelopePaths.Find(GetTypeHash(Raw)))
							for (const auto& Candidate : *Candidates)
							if (FStringView(*Candidate).Equals(Raw, ESearchCase::CaseSensitive)) { Row.EnvelopeRef = Candidate; break; }
						if (Row.EnvelopeRef) {
							if (ActiveReadState) ++ActiveReadState->InventoryValidatedPathReuses;
						} else {
							FString Safe;
							if (!SafeRelativePath(FString(EnvelopeRef), Safe)
								|| !Safe.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive)
								|| !Safe.EndsWith(TEXT(".hjson"), ESearchCase::CaseSensitive))
								return Fields.FailItemField(OutError, TEXT("semantic_workspace_envelope_ref_invalid"),
									TEXT("A captured envelope must be inline or reference a managed baseline document, exclusively."),
									Item.Get(), TEXT("envelope_ref"));
							auto& Candidates = EnvelopePaths.FindOrAdd(GetTypeHash(Safe));
							for (const auto& Candidate : Candidates)
								if (Candidate->Equals(Safe, ESearchCase::CaseSensitive)) { Row.EnvelopeRef = Candidate; break; }
							if (!Row.EnvelopeRef) { Row.EnvelopeRef = MakeShared<const FString>(MoveTemp(Safe)); Candidates.Add(Row.EnvelopeRef); }
						}
					}
					if (Envelope != nullptr
						&& Envelope->GetKind() != EHjsonNodeKind::Object)
					{
						return FailNode(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The captured inventory envelope must be an object."), *Envelope, MakeShared<FJsonValueString>(TEXT("object")), false);
					}
					if (!ParseUnitKind(KindText, Row.UnitKind))
					{
						return Fields.FailItemField(OutError, TEXT("semantic_workspace_inventory_invalid"),
							TEXT("The inventory unit_kind is unsupported."), Item.Get(), TEXT("unit_kind"), UnitKindChoices());
					}
					Row.UnitIdentity = MakeShared<const FString, ESPMode::ThreadSafe>(MoveTemp(UnitId));
					FInventoryRow BoundRow(MoveTemp(Row), SourceNode, Envelope);
					const auto MakeDiagnosticSource = [&]() { return Fields.DiagnosticItem(Item.Get()); };
					TFunctionRef<FHjsonNodePtr()> DiagnosticSource(MakeDiagnosticSource);
					OutRows.Add(MoveTemp(BoundRow));
					if (!AddInventoryIdentity(OutRows.Last(), UnitIds, PublicRefs, LogicalPaths, OutError,
						Fields.ParsedStrings ? &DiagnosticSource : nullptr)) {
						OutRows.RemoveAt(OutRows.Num() - 1, 1, EAllowShrinking::No); return false;
					}
					return true;
				};
				if (!ReadItem()) Diagnostics.Append(OutError);
			}
			OutError = MoveTemp(Diagnostics);
			return !OutError.HasErrors();
		}

		const FHjsonNode* FindEntry(
			const FHjsonNode& Object,
			const FString& Key)
		{
			return Object.GetKind() == EHjsonNodeKind::Object
				? Object.Get(Key)
				: nullptr;
		}

		bool MaterializeInventorySource(FInventoryRow& Row, const FSourceProvenance& Origin, const TArray<uint8>& Utf8,
			FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			FHjsonDocumentPtr Document;
			if (!TryParseGeneratedHjsonSpan(Utf8, Origin, Resources, Document, OutError)) return false;
			Row.RetainedSourceNode = Document->GetRoot();
			Row.SourceNode = Row.RetainedSourceNode.Get();
			Row.Envelope = Row.SourceNode->Get(TEXT("envelope"));
			return true;
		}

		bool MaterializeInventoryRow(FInventoryRow& Row, const FManifestIndexProof& Proof,
			FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
		{
			if (!Row.Syntax->GetEnvelopeRef().IsEmpty()) return true;
			return MaterializeInventorySource(Row, Row.SourceNode->GetProvenance(), Proof.Utf8, Resources, OutError);
		}

		bool EnsureInventorySource(FInventoryRow& Row, FWorkspaceError& OutError)
		{
			if (Row.SourceNode) return true;
			if (!Row.SourceOrigin.IsSet() || !Row.SourceUtf8)
				return Fail(OutError, TEXT("semantic_workspace_inventory_source_invalid"),
					TEXT("The validated inventory row has no diagnostic raw/span backing. Producer=accepted inventory reader; no whole-document diagnostic reader was selected."),
					Row.SourceOrigin.IsSet() ? Row.SourceOrigin->GetSourceUnit() : Row.GetDocumentPath());
			// The whole validated shard's original parse charges were already
			// replayed. Reconstructing its diagnostic view is not a second input.
			FUeAgentResourceTracker DiagnosticResources;
			if (Row.GeneratedInventoryOrdinal != INDEX_NONE) {
				const auto& Span = Row.SourceOrigin.GetValue();
				const FSourceProvenance Origin(Span.GetSourceUnit(), TEXT("/unit_inventory/") + FString::FromInt(Row.GeneratedInventoryOrdinal),
					Span.GetLine(), Span.GetColumn(), Span.GetByteStart(), Span.GetByteEnd(), Span.GetOriginalKey());
				return MaterializeInventorySource(Row, Origin, *Row.SourceUtf8, DiagnosticResources, OutError);
			}
			return MaterializeInventorySource(Row, Row.SourceOrigin.GetValue(), *Row.SourceUtf8, DiagnosticResources, OutError);
		}

		FString InventoryMembershipHash(const TArray<FInventoryRow>& Rows)
		{
			SHA256_CTX Context; SHA256_Init(&Context);
			const auto Add = [&](const FString& Text) {
				const int64 Length = Text.Len();
				SHA256_Update(&Context, &Length, sizeof(Length));
				SHA256_Update(&Context, *Text, Text.Len() * sizeof(TCHAR));
			};
			for (const auto& Row : Rows) {
				Add(Row.Syntax->GetUnitId()); Add(Row.GetDocumentPath()); Add(Row.GetLogicalPath());
				Add(Row.Syntax->GetUnitPointer()); Add(Row.Syntax->GetEnvelopeRef());
				const int64 Count = Row.Syntax->GetContentFiles().Num(); SHA256_Update(&Context, &Count, sizeof(Count));
				for (const auto& Path : Row.Syntax->GetContentFiles()) Add(Path);
			}
			uint8 Hash[SHA256_DIGEST_LENGTH]; SHA256_Final(Hash, &Context);
			FString Hex; Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Hash) Hex += FString::Printf(TEXT("%02x"), Byte);
			return Hex;
		}

		FString InventoryIdentityHash(const TArray<FInventoryRow>& Rows)
		{
			// Exact, ordered identity tuples. This small proof must not keep all
			// parsed inventory trees alive just to validate uniqueness/ref lookup.
			SHA256_CTX Context; SHA256_Init(&Context);
			const auto Add = [&](const FString& Text) {
				const int64 Length = Text.Len();
				SHA256_Update(&Context, &Length, sizeof(Length));
				SHA256_Update(&Context, *Text, Text.Len() * sizeof(TCHAR));
			};
			for (const auto& Row : Rows) {
				Add(Row.Syntax->GetUnitId()); Add(Row.Syntax->GetPublicRef()); Add(Row.GetLogicalPath());
			}
			uint8 Hash[SHA256_DIGEST_LENGTH]; SHA256_Final(Hash, &Context);
			FString Hex; Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Hash) Hex += FString::Printf(TEXT("%02x"), Byte);
			return Hex;
		}

		TSharedPtr<const FResolvedGroupIndex> MakeResolvedGroupIndex(const FParsedSource& Parsed,
			const TArray<FInventoryRow>& Inventory)
		{
			if (Parsed.InventoryRanges.IsEmpty()) return {};
			// The optional index is charged below, separately from the complete
			// successful rows. Expanded unit count is not an asset-size boundary.
			if (static_cast<int64>(Inventory.Num()) * sizeof(FResolvedRowPosition) > ResolvedGroupBudget) return {};
			auto Index = MakeShared<FResolvedGroupIndex>(); Index->Shards = Parsed.InventoryRanges;
			Index->FirstManagedPaths.SetNum(Index->Shards.Num());
			TSet<FString> Managed, Sources;
			int32 ExpectedFirst = 0;
			for (int32 Group = 0; Group < Index->Shards.Num(); ++Group) {
				const auto& Shard = Index->Shards[Group];
				if (Shard.First != ExpectedFirst || Shard.Count < 0 || Shard.Count > Inventory.Num() - Shard.First) return {};
				ExpectedFirst += Shard.Count;
				const auto AddManaged = [&](const FString& Path) {
					if (!Path.IsEmpty() && !Managed.Contains(Path)) {
						Managed.Add(Path); Index->FirstManagedPaths[Group].Add(Path);
					}
				};
				for (int32 Offset = 0; Offset < Shard.Count; ++Offset) {
					const auto& Row = Inventory[Shard.First + Offset];
					// A content expansion or noncanonical path keeps the original reader.
					if (!Row.Syntax->GetContentFiles().IsEmpty() || !Row.DocumentPathOverride.IsEmpty()
						|| !Row.LogicalPathOverride.IsEmpty()) return {};
					Index->RowPositions.Add({Row.Syntax->UnitIdentity.ToSharedRef(), FIntPoint(Group, Offset)});
					Sources.Add(Row.GetDocumentPath());
					Index->DocumentGroups.FindOrAdd(Row.GetDocumentPath()).AddUnique(Group);
					if (!Row.Syntax->GetEnvelopeRef().IsEmpty()) {
						Index->BaselineGroups.FindOrAdd(Row.Syntax->GetEnvelopeRef()).AddUnique(Group);
						AddManaged(Row.Syntax->GetEnvelopeRef());
					}
					AddManaged(Row.GetDocumentPath());
				}
			}
			if (ExpectedFirst != Inventory.Num() || Index->RowPositions.Num() != Inventory.Num()) return {};
			Index->Documents = Sources.Array(); Index->Documents.Sort();
			int64 Bytes = sizeof(FResolvedGroupIndex) + Index->Shards.GetAllocatedSize()
				+ Index->Documents.GetAllocatedSize() + Index->RowPositions.GetAllocatedSize()
				+ Index->DocumentGroups.GetAllocatedSize() + Index->BaselineGroups.GetAllocatedSize()
				+ Index->FirstManagedPaths.GetAllocatedSize();
			for (const auto& Shard : Index->Shards) Bytes += Shard.Path.GetAllocatedSize()
				+ Shard.ContentHash.GetAllocatedSize() + Shard.MembershipHash.GetAllocatedSize();
			for (const auto& Path : Index->Documents) Bytes += Path.GetAllocatedSize();
			for (const auto& Row : Index->RowPositions) Bytes += sizeof(FString) + 32 + Row.UnitId->GetAllocatedSize();
			for (const auto* Map : {&Index->DocumentGroups, &Index->BaselineGroups})
				for (const auto& Pair : *Map) Bytes += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
			for (const auto& Paths : Index->FirstManagedPaths) {
				Bytes += Paths.GetAllocatedSize(); for (const auto& Path : Paths) Bytes += Path.GetAllocatedSize();
			}
			Index->ChargeBytes = Bytes;
			if (Bytes > ResolvedGroupBudget) return {};
			return Index;
		}

		bool MatchesResolvedGroupIndex(const FResolvedGroupIndex& Index, const TArray<FInventoryShardRange>& Ranges)
		{
			if (Index.Shards.Num() != Ranges.Num()) return false;
			for (int32 I = 0; I < Ranges.Num(); ++I) {
				const auto& A = Index.Shards[I]; const auto& B = Ranges[I];
				if (A.Path != B.Path || A.First != B.First || A.Count != B.Count || A.MembershipHash != B.MembershipHash) return false;
			}
			return true;
		}

		void CompleteResolvedRowGroups(FResolvedRowsProof& Result, const FParsedSource& Parsed,
			const TArray<FInventoryRow>& Inventory)
		{
			if (!Result.GroupIndex || !ActiveReadState) return;
			const auto& Index = *Result.GroupIndex;
			for (const FString& Path : Index.Documents) {
				const auto* Document = ActiveReadState->Documents.Find(FPaths::Combine(Result.Root, Path));
				if (Document) Result.GroupDocuments.Add(Path, Document->Parsed);
			}
			int64 Bytes = Index.ChargeBytes + Result.Groups.GetAllocatedSize() + Result.GroupDocuments.GetAllocatedSize();
			for (const auto& Pair : Result.GroupDocuments) Bytes += Pair.Key.GetAllocatedSize();
			for (const auto& Group : Result.Groups) if (Group) Bytes += Group->ChargeBytes;
			if (Bytes > ResolvedGroupBudget) {
				for (const auto& Group : Result.Groups) if (Group)
					for (const auto& Row : Group->Rows) Result.AddSparseRow(Row);
				// The index passed its own complete 64 MiB admission. Rows may
				// borrow its charged identity backing, so shedding optional group
				// arrays must not also retire that owner while the rows survive.
				Result.Groups.Empty(); Result.GroupDocuments.Empty();
				Result.GroupChargeBytes = Index.ChargeBytes;
				return;
			}
			for (int32 GroupIndex = 0; GroupIndex < Index.Shards.Num(); ++GroupIndex) {
				if (Result.Groups[GroupIndex]) continue;
				const auto& Shard = Parsed.InventoryRanges[GroupIndex];
				if (Shard.Count <= 0) continue;
				auto Group = MakeShared<FResolvedRowGroup>(); Group->InventoryHash = Shard.ContentHash;
				TSet<const FReferenceLookupProof*> Lookups;
				bool bComplete = true;
				for (int32 Offset = 0; Offset < Shard.Count; ++Offset) {
					const auto& Row = Inventory[Shard.First + Offset];
					const FResolvedRowProofPtr Proof = Result.FindRow(Row.Syntax->GetUnitId());
					const auto* Document = ActiveReadState->Documents.Find(FPaths::Combine(Result.Root, Row.GetDocumentPath()));
					const FHjsonNode* Node = Document && Document->Parsed ? Document->Parsed->Find(Row.Syntax->GetUnitPointer()) : nullptr;
					if (!Proof || !Proof->Unit.IsSet() || !Proof->Source.IsSet() || !Proof->SourceDocument || !Proof->SourceDocument->IsAvailable()
						|| !Proof->bCompleteReferenceLookups || !Node
						|| Parsed.ContentReferenceUnits.Contains(Row.Syntax->GetUnitId())
						|| !SameRowProvenance(Proof->Source.GetValue(), Node->GetProvenance())) { bComplete = false; break; }
					Group->Rows.Add(Proof); Group->Units.Add(Proof->Unit.GetValue()); Group->Nodes.Add(Node);
					Group->ReferenceEdges += Proof->ReferenceLookups.Num();
					for (const auto& Lookup : Proof->ReferenceLookups) if (!Lookups.Contains(Lookup.Get())) {
						Lookups.Add(Lookup.Get()); Group->Lookups.Add(Lookup);
					}
					if (Proof->Charges) for (const auto& Charge : *Proof->Charges) {
						int64& Total = Group->Charges.FindOrAdd(Charge.Key);
						if (Charge.Value < 0) { bComplete = false; break; }
						Total += FMath::Min(Charge.Value, MAX_int64 - Total);
					}
					if (!bComplete) break;
					if (Proof->bEdited) Group->EditedIds.Add(Row.Syntax->GetUnitId());
					if (Proof->KeyedRoots) Group->KeyedRoots.Add(Row.Syntax->GetUnitId(), Proof->KeyedRoots);
				}
				if (!bComplete) continue;
				int64 Charge = sizeof(FResolvedRowGroup) + Group->InventoryHash.GetAllocatedSize()
					+ Group->Rows.GetAllocatedSize() + Group->Units.GetAllocatedSize() + Group->Nodes.GetAllocatedSize()
					+ Group->Lookups.GetAllocatedSize() + Group->Charges.GetAllocatedSize()
					+ Group->EditedIds.GetAllocatedSize() + Group->KeyedRoots.GetAllocatedSize();
				for (const auto& Id : Group->EditedIds) Charge += Id.GetAllocatedSize();
				for (const auto& Pair : Group->KeyedRoots) {
					// The group refers to the same immutable set retained by its row.
					Charge += Pair.Key.GetAllocatedSize();
				}
				Group->ChargeBytes = Charge;
				if (Bytes + Charge > ResolvedGroupBudget) continue;
				Bytes += Charge; Result.Groups[GroupIndex] = Group;
				for (const auto& Row : Group->Rows) Result.RemoveSparseRow(Row->GetLookupKey());
			}
			Result.ShrinkSparseRows(); Result.GroupChargeBytes = Bytes;
		}

		int64 RetainedHjsonTreesBytes(TArray<const FHjsonNode*> PendingNodes, TSet<const FString*>* SourceNames = nullptr)
		{
			int64 Bytes = 0;
			TSet<const FHjsonNode*> Visited;
			while (!PendingNodes.IsEmpty()) {
				const auto* Node = PendingNodes.Pop(EAllowShrinking::No);
				if (Visited.Contains(Node)) continue;
				Visited.Add(Node);
				if (SourceNames) SourceNames->Add(&Node->GetProvenance().GetSourceUnit());
				// Do not force lazy source relocation merely to measure optional
				// retention. Count the actual shared storage once instead.
				Bytes += Node->GetOwnedStorageBytes(PendingNodes);
			}
			return Bytes;
		}

		int64 RetainedCanonicalNodeBytes(const FUeAgentCanonicalValue& Value)
		{
			int64 Bytes = sizeof(FUeAgentCanonicalValue) + 32 + Value.GetText().GetAllocatedSize()
				+ Value.GetArrayItems().GetAllocatedSize() + Value.GetObjectItems().GetAllocatedSize();
			for (const auto& Child : Value.GetObjectItems()) Bytes += Child.Key.GetAllocatedSize();
			return Bytes;
		}
		struct FResolvedRowCost
		{
			int64 Rows = 0, SourceBytes = 0, UnitBytes = 0, CanonicalBytes = 0, ProofBytes = 0;
			int64 Total() const { return SourceBytes + UnitBytes + CanonicalBytes + ProofBytes; }
			void Add(const FResolvedRowCost& Other) {
				Rows += Other.Rows; SourceBytes += Other.SourceBytes; UnitBytes += Other.UnitBytes;
				CanonicalBytes += Other.CanonicalBytes; ProofBytes += Other.ProofBytes;
			}
		};
		int64 RetainedResolvedRowBytes(const FResolvedRowProof& Row, FResolvedRowCost& Costs)
		{
			int64 Bytes = sizeof(FResolvedRowProof) + 32 // Shared control; current sparse containers are charged separately.
				+ Row.ReferenceLookups.GetAllocatedSize() + Row.LookupKeyOverride.GetAllocatedSize();
			// Rows now borrows its key from this retained immutable unit. Its
			// actual UnitId allocation is charged by the declaration visitor.
			// The edge array is owned by this row; lookup objects are shared by
			// the resolver pool and charged once at generation admission below.
			if (Row.KeyedRoots) {
				Bytes += sizeof(TSet<FString>) + 32 + Row.KeyedRoots->GetAllocatedSize();
				for (const FString& Root : *Row.KeyedRoots) Bytes += Root.GetAllocatedSize();
			}
			FResolvedRowCost Cost;
			Cost.Rows = 1; Cost.ProofBytes = Bytes;
			if (Row.Source.IsSet()) Cost.SourceBytes += Row.Source->GetJsonPointer().GetAllocatedSize()
				+ Row.Source->GetOriginalKey().GetAllocatedSize();
			if (Row.SourceDocument) {
				Cost.SourceBytes += Row.SourceDocument->GetAllocatedSize();
			}
			TArray<const FUeAgentCanonicalValue*> Pending;
			TSet<const FUeAgentCanonicalValue*> Visited;
			auto AddUnit = [&](const FSemanticUnit& Unit) {
				Cost.UnitBytes += Unit.GetStorageAllocatedSize();
				Unit.VisitValues([&](const FString&, const FUeAgentCanonicalValueRef& Value) { Pending.Add(&Value.Get()); return true; });
			};
			if (Row.Unit.IsSet()) AddUnit(Row.Unit.GetValue());
			if (Row.Baseline.IsSet() && (!Row.Unit.IsSet() || !Row.Unit->SharesStorageWith(Row.Baseline.GetValue()))) AddUnit(Row.Baseline.GetValue());
			while (!Pending.IsEmpty()) {
				const auto* Value = Pending.Pop(EAllowShrinking::No);
				if (Visited.Contains(Value)) continue;
				Visited.Add(Value);
				// Semantic parser JSON rows use these owned representations. An
				// extension with additional backing is resolved normally, not retained
				// with an incomplete memory charge.
				if (Value->GetTemporal() || Value->GetSpatial()) return MAX_int64;
				Cost.CanonicalBytes += RetainedCanonicalNodeBytes(*Value);
				for (const auto& Child : Value->GetArrayItems()) Pending.Add(&Child.Get());
				for (const auto& Child : Value->GetObjectItems()) Pending.Add(&Child.Value.Get());
			}
			// The current cache owns these exact node identities through its units.
			// Admission below charges each node's complete allocation once and rolls
			// back newly introduced identities when a whole group is rejected.
			Costs.Add(Cost);
			return Cost.Total() - Cost.CanonicalBytes;
		}

		int64 RetainedInventoryRowsBytes(const TArray<FInventoryRow>& Rows)
		{
			int64 Bytes = Rows.GetAllocatedSize();
			TArray<const FHjsonNode*> PendingNodes;
			TSet<const FString*> EnvelopePaths;
			TSet<const FString*> SourceNames;
			for (const auto& Row : Rows) {
				Bytes += sizeof(FInventoryRowSyntax) + 32 + Row.SourceRowHash.GetAllocatedSize()
					+ Row.DocumentPathOverride.GetAllocatedSize() + Row.LogicalPathOverride.GetAllocatedSize()
					+ Row.Syntax->OwnedProjectionBytes() + sizeof(FString) + 32
					+ Row.Syntax->ContentHash.GetAllocatedSize() + Row.Syntax->EditHash.GetAllocatedSize();
				Bytes += Row.Syntax->GetUnitId().GetAllocatedSize();
				if (Row.Syntax->EnvelopeRef && !EnvelopePaths.Contains(Row.Syntax->EnvelopeRef.Get())) {
					EnvelopePaths.Add(Row.Syntax->EnvelopeRef.Get());
					Bytes += sizeof(FString) + 32 + Row.Syntax->EnvelopeRef->GetAllocatedSize();
				}
				if (Row.SourceOrigin.IsSet()) {
					Bytes += Row.SourceOrigin->GetJsonPointer().GetAllocatedSize() + Row.SourceOrigin->GetOriginalKey().GetAllocatedSize();
					const auto* Name = &Row.SourceOrigin->GetSourceUnit();
					if (!SourceNames.Contains(Name)) { SourceNames.Add(Name); Bytes += sizeof(FString) + 32 + Name->GetAllocatedSize(); }
				}
				if (Row.RetainedSourceNode) PendingNodes.Add(Row.RetainedSourceNode.Get());
			}
			return Bytes + RetainedHjsonTreesBytes(MoveTemp(PendingNodes));
		}

		TSharedPtr<const FInventoryDocumentProof> MakeInventoryDocumentProof(const FString& Root,
			const FString& Path, const FString& ContentHash, const FHjsonDocument& Document,
			const TArray<uint8>& Utf8, const TMap<EUeAgentResourceDimension, int64>& ParseCharges,
			FWorkspaceError& OutError, bool bExactParseCharges = true)
		{
			auto Proof = MakeShared<FInventoryDocumentProof>();
			if (!ParseInventory(*Document.GetRoot(), Proof->Rows, OutError)) return {};
			Proof->ContentHash = ContentHash; Proof->ParseCharges = ParseCharges;
			Proof->bExactParseCharges = bExactParseCharges;
			Proof->MembershipHash = InventoryMembershipHash(Proof->Rows);
			Proof->IdentityHash = InventoryIdentityHash(Proof->Rows);
			const auto& Nodes = Document.GetRoot()->Get(TEXT("unit_inventory"))->GetItems();
			TSharedPtr<const TArray<uint8>> SourceBytes;
			for (int32 Index = 0; Index < Proof->Rows.Num(); ++Index) {
				auto& Row = Proof->Rows[Index];
				const auto& Span = Row.SourceNode->GetProvenance();
				if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) {
					Fail(OutError, TEXT("semantic_workspace_inventory_source_invalid"),
						TEXT("The parsed inventory row has no valid source byte range."), Path); return {};
				}
				Row.SourceRowHash.SetFromBytes(Utf8.GetData() + Span.GetByteStart(), Span.GetByteEnd() - Span.GetByteStart());
				Row.GeneratedInventoryOrdinal = Index;
				if (!Row.Syntax->GetEnvelopeRef().IsEmpty()) {
					if (!SourceBytes) SourceBytes = MakeShared<const TArray<uint8>>(Utf8);
					Row.SourceOrigin = Span;
					Row.SourceUtf8 = SourceBytes;
					Row.SourceNode = nullptr;
				} else Row.RetainedSourceNode = Nodes[Index];
			}
			int64 Bytes = sizeof(FInventoryDocumentProof) + 128 + Path.GetAllocatedSize()
				+ Proof->ContentHash.GetAllocatedSize() + Proof->MembershipHash.GetAllocatedSize() + Proof->IdentityHash.GetAllocatedSize()
				+ Proof->ParseCharges.GetAllocatedSize() + Document.GetSourceUnit().GetAllocatedSize();
			// Inline envelopes still own their AST. Split rows share one immutable
			// UTF-8 shard; the syntax/locations and that backing are all budgeted.
			if (SourceBytes) Bytes += sizeof(TArray<uint8>) + 32 + SourceBytes->GetAllocatedSize();
			Bytes += RetainedInventoryRowsBytes(Proof->Rows);
			Proof->RetainedBytes = Bytes;
			if (InventoryProofRoot != Root) { InventoryProofs.Reset(); InventoryProofRoot = Root; PreviousManifestIndex.Reset(); PreviousInventoryIdentities.Reset(); }
			for (const auto& Existing : InventoryProofs) if (Existing.Key != Path) {
				Bytes += Existing.Value->RetainedBytes;
			}
			// Admit a shard only if it fits. Clearing all earlier shards here made
			// every scan over 200k rows evict its own useful prefix on every call.
			if (Bytes <= 512ll * 1024 * 1024 && (InventoryProofs.Contains(Path) || InventoryProofs.Num() < 256))
				InventoryProofs.Add(Path, Proof);
			else InventoryProofs.Remove(Path);
			return Proof;
		}

		bool MatchInventoryIdentityShards(const FString& Root, const FHjsonNode& Files,
			TMap<FString, FString>& ExpectedHashes)
		{
			// This proof concerns only inventory uniqueness and the public-ref table.
			// Publication generation, header revision and unrelated row values are not
			// its inputs. Every shard still needs an actual current byte observation.
			if (!PreviousInventoryIdentities || PreviousInventoryIdentities->Root != Root
				|| PreviousInventoryIdentities->Shards.Num() != Files.GetItems().Num()) return false;
			for (const auto& File : Files.GetItems()) {
				FString Path;
				if (File->GetKind() != EHjsonNodeKind::String || !SafeRelativePath(File->GetText(), Path)
					|| ExpectedHashes.Contains(Path)) return false;
				const auto* Before = PreviousInventoryIdentities->Shards.Find(Path);
				if (!Before) return false;
				const FString FullPath = FPaths::Combine(Root, Path);
				FString Text, Hash; int64 Characters = 0;
				if (IsReparsePoint(FullPath) || !ObserveSourceFile(FullPath, false, Text, Hash, Characters)) return false;
				if (Hash != Before->ContentHash) {
					const auto Current = InventoryProofRoot == Root ? InventoryProofs.FindRef(Path) : nullptr;
					if (!Current || Hash != Current->ContentHash || Before->RowCount != Current->Rows.Num()
						|| Before->IdentityHash != Current->IdentityHash) return false;
				}
				ExpectedHashes.Add(Path, MoveTemp(Hash));
			}
			return true;
		}

		bool ReadInventoryDocuments(const FString& Root, const FHjsonNode& Manifest,
			FUeAgentResourceTracker& Resources, FParsedSource& Parsed, TArray<FInventoryRow>& Rows,
			FWorkspaceError& OutError, bool bRetainSyntax = false)
		{
			const auto* Files = Manifest.Get(TEXT("unit_inventory_files"));
			if (!Files || Files->GetKind() != EHjsonNodeKind::Array || Manifest.Get(TEXT("unit_inventory")))
				return FailField(OutError, TEXT("semantic_workspace_inventory_invalid"),
					TEXT("Workspace metadata requires inline unit_inventory or unit_inventory_files, exclusively."), Manifest, TEXT("unit_inventory_files"));
			TSet<FString> Paths;
			FInventoryIdentitySet UnitIds, PublicRefs, LogicalPaths;
			if (PreviousInventoryIdentities && PreviousInventoryIdentities->Root != Root) PreviousInventoryIdentities.Reset();
			TMap<FString, FString> ExpectedIdentityHashes;
			const bool bGeneratedIdentity = Parsed.GeneratedSource && !Parsed.GeneratedSource->ReferenceMapHash.IsEmpty();
			bool bSameIdentities = bGeneratedIdentity || MatchInventoryIdentityShards(Root, *Files, ExpectedIdentityHashes);
			if (bGeneratedIdentity) for (const auto& File : Parsed.GeneratedSource->FileHashes)
				if (File.Key.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) ExpectedIdentityHashes.Add(File.Key, File.Value);
			// The original compiler, naming allocator and layout proved unit/ref/path
			// uniqueness. Actual corresponding metadata bytes bind that same proof.
			// Its conservative file digest avoids rebuilding a sorted all-row text
			// solely for cache equality; this is not a native conflict identity.
			const FString ReusableReferenceHash = bGeneratedIdentity ? Parsed.GeneratedSource->ReferenceMapHash
				: bSameIdentities ? PreviousInventoryIdentities->ReferenceMapHash : FString();
			auto IdentityProof = MakeShared<FInventoryIdentityProof>(); IdentityProof->Root = Root;
			int32 ReusedFiles = 0;
			for (const auto& File : Files->GetItems()) {
				FString Path;
				if (File->GetKind() != EHjsonNodeKind::String || !SafeRelativePath(File->GetText(), Path)
					|| !Path.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)
					|| !Path.EndsWith(TEXT(".hjson"), ESearchCase::CaseSensitive) || Paths.Contains(Path))
					return FailNode(OutError, TEXT("semantic_workspace_inventory_ref_invalid"),
						TEXT("Inventory files must be unique managed relative paths under .uai/inventory/."), *File);
				Paths.Add(Path);
				const FString FullPath = FPaths::Combine(Root, Path);
				if (IsReparsePoint(FullPath)) return Fail(OutError, TEXT("workspace_reparse_point_rejected"),
					TEXT("The inventory document or an ancestor is a reparse point."), Path);
				FString Text, Hash; int64 SourceCharacters = 0;
				if (!ObserveSourceFile(FullPath, false, Text, Hash, SourceCharacters)) return Fail(OutError,
					TEXT("workspace_source_read_failed"), TEXT("The inventory document could not be read."), Path);
				if (bSameIdentities && Hash != ExpectedIdentityHashes.FindRef(Path)) {
					// A writer invalidated the preflight observation. Revalidate the
					// already-consumed prefix before continuing the original order.
					bSameIdentities = false;
					Parsed.GeneratedSource.Reset();
					for (const auto& Row : Rows)
						if (!AddInventoryIdentity(Row, UnitIds, PublicRefs, LogicalPaths, OutError)) return false;
				}
				auto Proof = InventoryProofRoot == Root ? InventoryProofs.FindRef(Path) : nullptr;
				if (Proof && Proof->ContentHash == Hash) {
					if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, OutError)) return false;
					FUeAgentResourceChargeError Error;
					if (!Resources.TryChargeMany(Proof->ParseCharges, TEXT("semantic_workspace:validated_inventory"), Error)) return FailResource(OutError, Error);
					++ReusedFiles;
				} else {
					if (Text.IsEmpty() && SourceCharacters > 0) {
						FString ActualHash; int64 ActualCharacters = 0;
						if (!ObserveSourceFile(FullPath, true, Text, ActualHash, ActualCharacters) || ActualHash != Hash)
							return Fail(OutError, TEXT("workspace_source_changed_during_read"),
								TEXT("The inventory document changed while this workspace was being resolved."), Path);
					}
					const auto Before = Resources.Snapshot();
					FHjsonDocumentPtr Document; FString ParsedHash;
					if (!ReadDocument(FullPath, Path, Resources, Document, OutError, &ParsedHash, bRetainSyntax)) return false;
					if (ParsedHash != Hash) return Fail(OutError, TEXT("workspace_source_changed_during_read"),
						TEXT("The inventory document changed while this workspace was being resolved."), Path);
					TMap<EUeAgentResourceDimension, int64> Charges;
					bool bExactCharges = true;
					const auto After = Resources.Snapshot();
					for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
						const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
						const int64 A = Before.Consumed(Dimension), B = After.Consumed(Dimension);
						if (B == MAX_int64 || B < A) { bExactCharges = false; break; }
						const int64 Delta = B - A;
						if (Dimension != EUeAgentResourceDimension::Files && Delta > 0) Charges.Add(Dimension, Delta);
					}
					// A same-scope ReadDocument hit does not parse/charge twice. Its
					// stored parser facts, not this zero local delta, describe syntax.
					if (ActiveReadState) if (const auto* Actual = ActiveReadState->Documents.Find(FullPath)) {
						if (Actual->Parsed == Document && Actual->ContentHash.Equals(Hash, ESearchCase::CaseSensitive)) {
							Charges = Actual->ParseCharges; bExactCharges = Actual->bExactParseCharges;
						}
					}
					const FTCHARToUTF8 Converted(*Text); TArray<uint8> Utf8;
					Utf8.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
					Proof = MakeInventoryDocumentProof(Root, Path, Hash, *Document, Utf8, Charges, OutError, bExactCharges);
					if (!Proof) return false;
				}
				Parsed.InventoryRanges.Add({Path, Hash, Proof->MembershipHash, Rows.Num(), Proof->Rows.Num()});
				for (const auto& Row : Proof->Rows) {
					Rows.Add(Row);
					// Borrow the output row's stable buffer, including a local path
					// override. A non-retained shard proof may be released next.
					if (!bSameIdentities && !AddInventoryIdentity(Rows.Last(), UnitIds, PublicRefs, LogicalPaths, OutError)) {
						Rows.RemoveAt(Rows.Num() - 1, 1, EAllowShrinking::No); return false;
					}
				}
				IdentityProof->Shards.Add(Path, {Proof->ContentHash, Proof->IdentityHash, Proof->Rows.Num()});
				Parsed.ManagedPaths.Add(Path);
				Parsed.InventoryHashes.Add(Path, Hash);
				if (ActiveReadState) {
					ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
					if (Proof->bExactParseCharges) {
						auto& Syntax = ActiveReadState->CompactSyntax.FindOrAdd(FullPath);
						Syntax.SourceUnit = Path; Syntax.ContentHash = Hash;
						Syntax.ParseCharges = Proof->ParseCharges; Syntax.bExactParseCharges = true;
					} else ActiveReadState->CompactSyntax.Remove(FullPath);
					// Rows own their compact fields and exact diagnostic backing (or
					// the inline envelope subtree). The complete parser document is
					// not another success consumer; keep only the observed-file proof.
					ActiveReadState->Documents.Remove(FullPath);
				}
			}
			Parsed.ReferenceMapHash = bSameIdentities && !ReusableReferenceHash.IsEmpty()
				? ReusableReferenceHash : HashReferenceMap(Rows);
			IdentityProof->ReferenceMapHash = Parsed.ReferenceMapHash;
			int64 IdentityBytes = sizeof(FInventoryIdentityProof) + IdentityProof->Root.GetAllocatedSize()
				+ IdentityProof->ReferenceMapHash.GetAllocatedSize() + IdentityProof->Shards.GetAllocatedSize();
			for (const auto& Pair : IdentityProof->Shards)
				IdentityBytes += Pair.Key.GetAllocatedSize() + Pair.Value.ContentHash.GetAllocatedSize() + Pair.Value.IdentityHash.GetAllocatedSize();
			if (IdentityBytes <= 1024ll * 1024) PreviousInventoryIdentities = MoveTemp(IdentityProof);
			else PreviousInventoryIdentities.Reset();
			int64 RetainedRows = 0, RetainedBytes = 0;
			for (auto It = InventoryProofs.CreateIterator(); It; ++It) {
				if (!Paths.Contains(It.Key())) It.RemoveCurrent();
				else { RetainedRows += It.Value()->Rows.Num(); RetainedBytes += It.Value()->RetainedBytes; }
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=inventory_documents files=%d reused_syntax=%d rows=%d identities_reused=%s retained_rows=%lld retained_bytes=%lld identity_proof_bytes=%lld"),
				Paths.Num(), ReusedFiles, Rows.Num(), bSameIdentities ? TEXT("true") : TEXT("false"), RetainedRows, RetainedBytes, IdentityBytes);
			return true;
		}

		// The accepted published directory consumes compact syntax and actual row
		// locations, not the generic resolver's per-row source hash, copied UTF-8
		// diagnostic backing, group hashes or optional proof-retention accounting.
		// Keep the original grammar/row validator; transfer only the actual fields.
		bool ReadPublishedInventoryInput(const FString& Root, const FString& Path, const FString& ExpectedHash,
			FUeAgentResourceTracker& Resources, FString& Text, int64& SourceBytes, FWorkspaceError& Error)
		{
			const FString FullPath = Root / Path;
			if (IsReparsePoint(FullPath)) return Fail(Error, TEXT("workspace_reparse_point_rejected"),
				TEXT("The accepted inventory document or an ancestor is a reparse point."), Path);
			if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, Error)) return false;
			FString TextHash, ByteHash; int64 Characters = 0, DecodedUtf8Bytes = INDEX_NONE; TArray<uint8> Bytes;
			double Started = FPlatformTime::Seconds();
			if (!ObserveSourceFile(FullPath, true, Text, TextHash, Characters, &Bytes, &ByteHash, &DecodedUtf8Bytes))
				return Fail(Error, TEXT("workspace_source_read_failed"), TEXT("The accepted inventory bytes could not be read."), Path);
			if (ActiveReadState) {
				++ActiveReadState->PublishedInventoryFilesRead; ActiveReadState->PublishedInventoryBytesRead += Bytes.Num();
				ActiveReadState->PublishedInventoryReadSeconds += FPlatformTime::Seconds() - Started;
			}
			if (ByteHash != ExpectedHash) return Fail(Error, TEXT("workspace_source_changed"),
				TEXT("The required inventory differs from its accepted publication."), Path);
			// The compact inventory's persisted positions address this complete
			// UTF-8 file. Reuse the decoder's actual byte count; a different
			// encoding is not permission to reinterpret its byte coordinates.
			if (DecodedUtf8Bytes != Bytes.Num()) {
				Error.SetSourceFailure(TEXT("hjson_source_span_invalid"),
					TEXT("The requested source span is outside the observed document."), Path);
				return false;
			}
			if (ActiveReadState) ActiveReadState->HashOnlyDocuments.Add(FullPath, TextHash);
			SourceBytes = Bytes.Num(); return true;
		}

		bool ParsePublishedInventoryInput(const FString& Text, const FString& Path, int64 SourceBytes,
			FUeAgentResourceTracker& Resources, TArray<FResolvedWorkspaceInventory::FRow>& OutRows, FWorkspaceError& Error)
		{
			TArray<FInventoryParsedStrings> ParsedStrings;
			const auto SelectScalar = [&](const FHjsonScalarLocation& Location, const FHjsonParsedScalar& Scalar) {
				static constexpr TCHAR SpanSuffix[] = TEXT("/envelope_span");
				const bool bSpan = Location.ArrayIndex >= 0 && Location.ArrayIndex < 2
					&& Scalar.Kind == EHjsonNodeKind::Integer && Location.ParentPointer.EndsWith(SpanSuffix, ESearchCase::CaseSensitive);
				const int32 Field = !bSpan && Scalar.Kind == EHjsonNodeKind::String && Location.ArrayIndex == INDEX_NONE
					? FInventoryParsedStrings::FieldIndex(Location.Key) : INDEX_NONE;
				if (!bSpan && Field == INDEX_NONE) return false;
				static constexpr TCHAR Prefix[] = TEXT("/unit_inventory/");
				const FStringView RowPointer = bSpan ? Location.ParentPointer.LeftChop(UE_ARRAY_COUNT(SpanSuffix) - 1) : Location.ParentPointer;
				if (!RowPointer.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
				int32 Cursor = UE_ARRAY_COUNT(Prefix) - 1, RowIndex = 0;
				const int32 IndexStart = Cursor;
				while (Cursor < RowPointer.Len() && RowPointer[Cursor] >= TEXT('0') && RowPointer[Cursor] <= TEXT('9')) {
					const int32 Digit = RowPointer[Cursor++] - TEXT('0');
					// Actual array ordinals originate in the parser's int32 counter.
					if (RowIndex > (MAX_int32 - Digit) / 10) return false;
					RowIndex = RowIndex * 10 + Digit;
					}
				if (Cursor == IndexStart || Cursor != RowPointer.Len()) return false;
				if (ParsedStrings.Num() <= RowIndex) ParsedStrings.SetNum(RowIndex + 1);
				if (bSpan) {
					auto& Value = ParsedStrings[RowIndex].SpanValues[Location.ArrayIndex];
					if (!Value.bPresent) Value = {Scalar.Lexeme, true, Scalar.Line, Scalar.Column, Scalar.ByteStart, Scalar.ByteEnd};
				} else {
					auto& Value = ParsedStrings[RowIndex].Values[Field];
					if (!Value.bPresent) Value = {Scalar.Text, Scalar.Lexeme, true, Scalar.Line, Scalar.Column, Scalar.ByteStart, Scalar.ByteEnd};
				}
				if (ActiveReadState) {
					++ActiveReadState->PublishedInventoryScalarNodesSkipped;
					if (bSpan) ++ActiveReadState->PublishedInventorySpanNodesSkipped;
					else ++ActiveReadState->PublishedInventoryBorrowedKeys;
					ActiveReadState->PublishedInventoryBorrowedTokenTexts += Scalar.bBorrowedText;
					ActiveReadState->PublishedInventoryBorrowedTokenLexemes += Scalar.bBorrowedLexeme;
					}
				return true;
			};
			double RowsSeconds = 0, CopySeconds = 0, RetireSeconds = 0;
			const auto ConsumeInventory = [&](const FHjsonNode& InventoryRoot, FWorkspaceError& RowError) {
				TArray<FInventoryRow> Rows;
				double PartStarted = FPlatformTime::Seconds();
				if (!ParseInventory(InventoryRoot, Rows, RowError, &ParsedStrings)) return false;
				RowsSeconds += FPlatformTime::Seconds() - PartStarted;
				PartStarted = FPlatformTime::Seconds();
				for (int32 Index = 0; Index < Rows.Num(); ++Index) {
					const auto& Row = Rows[Index]; const auto& Span = Row.SourceNode->GetProvenance();
					if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > SourceBytes)
						return Fail(RowError, TEXT("semantic_workspace_inventory_source_invalid"),
							TEXT("The parsed inventory row has no valid source byte range."), Path);
					FResolvedWorkspaceInventory::FRow Entry{Row.Syntax};
					Entry.InventoryOrdinal = Index; Entry.InventorySpan = {Span.GetByteStart(), Span.GetByteEnd()};
					Entry.EnvelopeSpan = Row.Syntax->StoredEnvelopeSpan;
					OutRows.Add(MoveTemp(Entry));
					}
				CopySeconds += FPlatformTime::Seconds() - PartStarted;
				if (ActiveReadState) ActiveReadState->PublishedInventoryRowsRead += Rows.Num();
				PartStarted = FPlatformTime::Seconds();
				Rows.Empty();
				RetireSeconds += FPlatformTime::Seconds() - PartStarted;
				return true;
			};
			double Started = FPlatformTime::Seconds();
			if (!TryConsumeHjsonScalarProjection(Text, Path, Resources, SelectScalar, ConsumeInventory, Error)) return false;
			if (ActiveReadState) {
				ActiveReadState->PublishedInventorySyntaxSeconds += FPlatformTime::Seconds() - Started - RowsSeconds - CopySeconds - RetireSeconds;
				ActiveReadState->PublishedInventoryRowsSeconds += RowsSeconds;
				ActiveReadState->PublishedInventoryCopySeconds += CopySeconds;
				ActiveReadState->PublishedInventoryRetireSeconds += RetireSeconds;
				}
			Started = FPlatformTime::Seconds();
			ParsedStrings.Empty();
			if (ActiveReadState) ActiveReadState->PublishedInventoryRetireSeconds += FPlatformTime::Seconds() - Started;
			return true;
		}

		bool ReadPublishedInventoryDocument(const FString& Root, const FString& Path, const FString& ExpectedHash,
			FUeAgentResourceTracker& Resources, TArray<FResolvedWorkspaceInventory::FRow>& OutRows, FWorkspaceError& Error)
		{
			FString Text; int64 SourceBytes = 0;
			if (!ReadPublishedInventoryInput(Root, Path, ExpectedHash, Resources, Text, SourceBytes, Error)
				|| !ParsePublishedInventoryInput(Text, Path, SourceBytes, Resources, OutRows, Error)) return false;
			const double Started = FPlatformTime::Seconds(); Text.Empty();
			if (ActiveReadState) ActiveReadState->PublishedInventoryRetireSeconds += FPlatformTime::Seconds() - Started;
			return true;
		}

		bool ReadGeneratedInventory(const FString& Root,
			FUeAgentResourceTracker& Resources, FParsedSource& Parsed, TArray<FInventoryRow>& Rows,
			FWorkspaceError& OutError)
		{
			const auto& Candidate = *Parsed.GeneratedSource;
			if (!Candidate.Model || !Candidate.Inventory)
				return FailWorkspaceDataflowHandoff(OutError, TEXT("source.document_inventory"),
					TEXT("publication model binding / document inventory"), Root,
					TEXT("incomplete directory handoff"), TEXT("same-publication row and root bindings"), TEXT("ReadPublishedDirectory"));
			const auto& Inventory = *Candidate.Inventory;
			TSet<FString> Expanded;
			Inventory.VisitExpandedDocuments([&](const FString& Path) { Expanded.Add(Path); });
			TArray<FString> Documents;
			if (ActiveReadState->bEstablishFileObservation)
				Inventory.VisitDocuments([&](const FString& Path) { Documents.Add(Path); });
			else {
				TSet<FString> Selected;
				for (const auto& Path : ActiveReadState->SelectedFilePaths) {
					if (!IsPublicSemanticFileCandidate(Path)) continue;
					const bool bUnpublished = !ActiveReadState->PublishedFileHashes || !ActiveReadState->PublishedFileHashes->Contains(Path);
					if (Candidate.FileHashes.Contains(Path) || bUnpublished) Selected.Add(Path);
				}
				// A formatted content_ref file belongs to the row that emitted it.
				// Its path is a Source input, but that row lives in the owner's inventory.
				TSet<FString> Owners, Fragments;
				if (!Selected.IsEmpty()) for (const FString& Owner : Expanded) {
					if (!Inventory.VisitDocument(Owner, [&](const FAcceptedWorkspaceInventoryRow& Row) {
						for (const FString& Content : Row.ContentFiles) if (Selected.Contains(Content)) {
							Fragments.Add(Content);
							Owners.Add(Row.DocumentPath);
						}
						return true;
					})) {
						if (!Candidate.Model->CheckValueReadResult(OutError)) return false;
						return FailWorkspaceDataflowHandoff(OutError, TEXT("source.document_inventory"), Owner, Root / Owner,
							TEXT("expanded document row bindings unavailable"), TEXT("content_ref owner selection"), TEXT("publication inventory"));
					}
				}
				for (const FString& Content : Fragments) Selected.Remove(Content);
				Selected.Append(Owners);
				Documents = Selected.Array();
				for (const auto& Path : Documents) if (Inventory.GetInventoryPath(Path).IsEmpty())
					return Fail(OutError, TEXT("workspace_new_document_binding_missing"), TEXT("The selected new/renamed document has no stable unit/member binding. No directory rebuild was selected."), Path);
			}
			Documents.Sort();
			if (ActiveReadState->bEstablishFileObservation) {
				// The old empty row visitor only caused shard readiness; a zero
				// document match also returned true. Preserve first-document machine
				// error order without manufacturing any row for this consumer.
				const double PreparationStarted = FPlatformTime::Seconds();
				FString FailedDocument;
				if (!Inventory.PrepareDocumentsMetadata(Documents, FailedDocument)) {
					if (!Candidate.Model->CheckValueReadResult(OutError)) return false;
					return FailWorkspaceDataflowHandoff(OutError, TEXT("source.document_inventory"), FailedDocument,
						Root / FailedDocument, TEXT("accepted metadata preparation did not complete"),
						TEXT("original physical inventory readiness"), TEXT("publication inventory producer"));
				}
				UE_LOG(LogTemp, Display, TEXT("UAI source timing phase=source_inventory_preparation documents=%d row_visitors=0 first_error_order=original_document_order total_s=%.6f"),
					Documents.Num(), FPlatformTime::Seconds() - PreparationStarted);
			}
			TMap<FString, TSharedPtr<const FString>> EnvelopeNames;
			int32 SharedSyntaxRows = 0;
			int32 UnchangedDocuments = 0;
			const double DocumentLoopStarted = FPlatformTime::Seconds();
			const double ProofBeginBefore = ActiveReadState->FileProofBeginSeconds, BodyReadBefore = ActiveReadState->FileBodyReadSeconds;
			const double ByteHashBefore = ActiveReadState->FileByteHashSeconds, TextEncodingBefore = ActiveReadState->FileTextEncodingSeconds;
			const double ProofEndBefore = ActiveReadState->FileProofEndSeconds, AdmissionBefore = ActiveReadState->FileAdmissionSeconds;
			for (const FString& Path : Documents) {
				if (ActiveReadState->bEstablishFileObservation && !Expanded.Contains(Path)
					&& !Candidate.PendingAuthorDocuments.Contains(Path)) {
					// Establish the actual document version before projecting its rows.
					// The immutable publication inventory remains the original naming,
					// hash and baseline owner; no per-row execution wrapper is needed
					// for a file with neither byte changes nor pending author intent.
					const auto* Expected = Candidate.FileHashes.Find(Path);
					if (Expected) {
						FString Text, Hash; int64 Characters = 0;
						const FString FullPath = FPaths::Combine(Root, Path);
						if (IsReparsePoint(FullPath)) return Fail(OutError, TEXT("workspace_reparse_point_rejected"),
							TEXT("The managed document or an ancestor is a reparse point."), Path);
						if (!ObserveSourceFile(FullPath, false, Text, Hash, Characters, nullptr, nullptr, nullptr, &OutError)) {
							if (OutError.HasErrors()) return false;
							return Fail(OutError, TEXT("workspace_source_read_failed"), TEXT("The managed document could not be observed while establishing its input version."), Path);
						}
						if (Hash == *Expected) {
							ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
							Parsed.DocumentHashes.Add(Path, Hash);
							++UnchangedDocuments;
							continue;
						}
						}
					}
				const int32 First = Rows.Num();
				const FString InventoryPath = Inventory.GetInventoryPath(Path);
				const auto InventorySourceName = MakeShared<const FString>(InventoryPath);
				const bool bGeneratedLayout = Inventory.GetGeneratedLayout() != nullptr;
				const bool bProduced = Inventory.VisitDocument(Path, [&](const FAcceptedWorkspaceInventoryRow& Input) {
					TSharedPtr<const FInventoryRowSyntax> Storage = Input.SourceSyntax;
					if (Storage) ++SharedSyntaxRows;
					else {
					FInventoryRowSyntax Syntax;
					Syntax.UnitIdentity = Input.SharedUnitIdentity;
					if (!Syntax.UnitIdentity) Syntax.UnitIdentity = MakeShared<const FString, ESPMode::ThreadSafe>(Input.UnitId);
					Syntax.UnitKind = Input.UnitKind;
					if (Input.bPersistedEnvelopeSpan) Syntax.StoredEnvelopeSpan = Input.EnvelopeSpan;
					if (bGeneratedLayout) {
						Syntax.GeneratedOwner = Candidate.Bindings;
						Syntax.GeneratedPublicRef = &Input.PublicRef;
						Syntax.GeneratedLogicalPath = &Input.LogicalPath;
						Syntax.GeneratedDocumentPath = &Input.DocumentPath;
						Syntax.GeneratedUnitPointer = &Input.UnitPointer;
						if (!Input.ContentFiles.IsEmpty()) Syntax.GeneratedContentFiles = &Input.ContentFiles;
					} else {
						// Only this requested document's K rows are materialized. A
						// synchronous visitor is not an immutable string owner.
						Syntax.PublicRef = Input.PublicRef; Syntax.LogicalPath = Input.LogicalPath;
						Syntax.DocumentPath = Input.DocumentPath; Syntax.UnitPointer = Input.UnitPointer;
						Syntax.ContentFiles = Input.ContentFiles;
					}
					Syntax.ContentHash.Set(Input.CanonicalContentHash());
					Syntax.EditHash.Set(Input.ReadEditHash());
					auto& Envelope = EnvelopeNames.FindOrAdd(Input.EnvelopePath);
					if (!Envelope) Envelope = MakeShared<const FString>(Input.EnvelopePath);
					Syntax.EnvelopeRef = Envelope;
					Storage = MakeShared<const FInventoryRowSyntax>(MoveTemp(Syntax));
					}
					FInventoryRow Row(Storage.ToSharedRef(), nullptr, nullptr);
					Row.SourceOrigin = FSourceProvenance(InventorySourceName, FString(), 0, 0, Input.InventorySpan.Begin, Input.InventorySpan.End, FString());
					Row.GeneratedInventoryOrdinal = Input.InventoryOrdinal;
					Row.GeneratedEnvelopeSpan = Input.EnvelopeSpan;
					Row.bGeneratedProjection = true;
					Rows.Add(MoveTemp(Row));
					return true;
				});
				if (!bProduced) {
					Rows.RemoveAt(First, Rows.Num() - First, EAllowShrinking::No);
					if (!Candidate.Model->CheckValueReadResult(OutError)) return false;
					return FailWorkspaceDataflowHandoff(OutError, TEXT("source.document_inventory"),
						TEXT("row projection for ") + Path, Root / Path,
						TEXT("accepted inventory visitor returned no rows"), TEXT("document-local inventory rows"),
						TEXT("publication directory VisitDocument"));
				}
				if (!ActiveReadState->bEstablishFileObservation && First == Rows.Num())
					return Fail(OutError, TEXT("workspace_new_document_binding_missing"), TEXT("The selected document has no stable unit/member binding."), Path);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI source timing phase=source_public_document_observations documents=%d unchanged=%d materialized_rows=%d proof_begin_s=%.6f body_read_s=%.6f byte_hash_s=%.6f text_encoding_s=%.6f proof_end_s=%.6f admission_s=%.6f loop_s=%.6f"),
				Documents.Num(), UnchangedDocuments, Rows.Num(), ActiveReadState->FileProofBeginSeconds - ProofBeginBefore,
				ActiveReadState->FileBodyReadSeconds - BodyReadBefore, ActiveReadState->FileByteHashSeconds - ByteHashBefore,
				ActiveReadState->FileTextEncodingSeconds - TextEncodingBefore, ActiveReadState->FileProofEndSeconds - ProofEndBefore,
				ActiveReadState->FileAdmissionSeconds - AdmissionBefore, FPlatformTime::Seconds() - DocumentLoopStarted);
			Rows.Sort([](const FInventoryRow& A, const FInventoryRow& B) {
				const FString& APath = A.Syntax->GetEnvelopeRef();
				const FString& BPath = B.Syntax->GetEnvelopeRef();
				if (APath != BPath) return APath < BPath;
				const int64 AStart = A.SourceOrigin.IsSet() ? A.SourceOrigin.GetValue().GetByteStart() : A.SourceNode->GetProvenance().GetByteStart();
				const int64 BStart = B.SourceOrigin.IsSet() ? B.SourceOrigin.GetValue().GetByteStart() : B.SourceNode->GetProvenance().GetByteStart();
				return AStart < BStart;
			});
			for (const auto& File : Candidate.FileHashes) if (File.Key.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) {
				Parsed.InventoryHashes.Add(File.Key, File.Value);
			}
			Parsed.ReferenceMapHash = Candidate.ReferenceMapHash;
			Parsed.bSparseGeneratedInventory = true;
			Parsed.SparseManagedPrefix = Parsed.ManagedPaths.Num();
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generated_inventory_view logical_rows=%d materialized_rows=%d legacy_shards=%d documents=%d unchanged_documents_without_row_wrappers=%d"),
				Inventory.Num(), Rows.Num(), 0, Documents.Num(), UnchangedDocuments);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_inventory_storage shared_syntax_rows=%d constructed_rows=%d digest_text_roundtrips=0_for_shared"),
				SharedSyntaxRows, Rows.Num() - SharedSyntaxRows);
			return true;
		}

		bool ParseCapturedEnvelopeDirectory(const FString& Text, const FString& Path,
			FSemanticWorkspaceReadState::FCapturedDocument& Read, FUeAgentResourceTracker& Resources, FWorkspaceError& Error,
			bool bCaptureLineColumns)
		{
			FHjsonNodePtr Directory;
			if (!TryParseHjsonSelectedNodes(Text, Path, Resources, Directory, Error,
				[](const FString& Pointer, const FString&) {
					// Explicit complete-input establishment needs original row
					// boundaries, not an additional declaration/value tree.
					return Pointer.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1) == INDEX_NONE;
				})) return false;
			if (Directory->GetKind() != EHjsonNodeKind::Object) return FailNode(Error,
				TEXT("semantic_workspace_envelope_ref_invalid"), TEXT("A baseline document must contain envelopes keyed by unit identity."), *Directory);
			if (Read.Envelopes.IsEmpty()) {
				int64 KeyBytes = 0;
				for (const auto& Entry : Directory->GetEntries()) {
					const auto& Origin = Entry.Value->GetProvenance();
					Read.Envelopes.Add(Entry.Key, {Origin.GetByteStart(), Origin.GetByteEnd(),
						bCaptureLineColumns ? Origin.GetLine() : 0, bCaptureLineColumns ? Origin.GetColumn() : 0});
					KeyBytes += Entry.Key.GetAllocatedSize();
				}
				const int64 Bytes = Read.Envelopes.GetAllocatedSize() + KeyBytes;
				if (!Charge(Resources, {{EUeAgentResourceDimension::EstimatedMemoryBytes, Bytes}},
					TEXT("semantic_workspace:captured_span_directory"), Error)) return false;
				ActiveReadState->CapturedSpanRows += Read.Envelopes.Num();
				ActiveReadState->CapturedSpanBytes += Bytes;
			}
			return true;
		}
		bool ReadCapturedEnvelopeBytes(const FString& Root, const FString& Path, const FString& ExpectedByteHash,
			FUeAgentResourceTracker& Resources, FSemanticWorkspaceReadState::FCapturedDocument& Read,
			FString& Text, FWorkspaceError& Error)
		{
			const FString FullPath = Root / Path;
			if (IsReparsePoint(FullPath)) return Fail(Error, TEXT("workspace_reparse_point_rejected"),
				TEXT("The captured document or an ancestor is a reparse point."), Path);
			FString Hash, ByteHash;
			if (ActiveReadState->bEstablishFileObservation) {
				// Explicit complete-input establishment also accepts supported old
				// physical encodings. Its parser addresses their decoded UTF-8.
				int64 Characters = 0;
				if (!ObserveSourceFile(FullPath, true, Text, Hash, Characters, &Read.Utf8, &ByteHash))
					return Fail(Error, TEXT("workspace_source_read_failed"), TEXT("The captured baseline document could not be read."), Path);
			} else {
				// The current publication's spans were produced beside these exact
				// canonical UTF-8 bytes. Do not manufacture a whole FString and
				// convert it back before reading one row.
				auto Proof = TryBeginWorkspaceFileObservation(FullPath);
				FWorkspaceReadPathScope Scope(Root);
				if (!Scope.IsAvailable() || !Scope.Observe(FullPath, [&](void* Handle, int64, int64) {
					return FWorkspaceReadPathScope::ReadBytes(Handle, Read.Utf8);
				}) || !Scope.IsCurrent()) return Fail(Error, TEXT("workspace_source_read_failed"),
					TEXT("The captured baseline's current ordinary file could not be read."), Path);
				Hash = ByteHash = Sha256(Read.Utf8);
				if (ByteHash == ExpectedByteHash) {
					Read.ByteProof = Proof;
					RememberObservedSemanticSourceFile(Root, FullPath, Read.Utf8,
						ByteHash, MoveTemp(Proof), MeasureCanonicalWorkspaceTextEncoding(Read.Utf8, ByteHash));
				}
			}
			if (ByteHash != ExpectedByteHash) return Fail(Error, TEXT("workspace_source_changed"),
				TEXT("The required captured file differs from its published generation."), Path);
			if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, Error)) return false;
			// Scanner offsets address canonical UTF-8 even for supported physical
			// encodings. Only a real declaration/value consumer requests these bytes.
			if (Hash != ByteHash) {
				const FTCHARToUTF8 Utf8(*Text);
				Read.Utf8.Reset(Utf8.Length()); Read.Utf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			}
			const int64 Bytes = Read.Utf8.GetAllocatedSize();
			if (!Charge(Resources, {{EUeAgentResourceDimension::EstimatedMemoryBytes, Bytes}},
				TEXT("semantic_workspace:captured_span_bytes"), Error)) return false;
			++ActiveReadState->CapturedByteReads;
			ActiveReadState->CapturedLiveBytes += Bytes;
			ActiveReadState->CapturedPeakBytes = FMath::Max(ActiveReadState->CapturedPeakBytes, ActiveReadState->CapturedLiveBytes);
			Read.ByteHash = ByteHash;
			ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
			// A later native/type consumer can read a baseline after the initial
			// source acceptance. Enrol that actual input, not every published
			// file, in the same source checkpoint.
			return !ActiveReadState->SourceProof || ActiveReadState->SourceProof->TryIncludeObservedInput(FullPath, Hash, Error, Read.ByteProof);
		}
		bool ReadCapturedDocumentBytes(const FString& Root, const FString& Path, const FString& ExpectedByteHash,
			FUeAgentResourceTracker& Resources, FSemanticWorkspaceReadState::FCapturedDocument*& Out, FString& Text, FWorkspaceError& Error)
		{
			if (!ActiveReadState) return Fail(Error, TEXT("workspace_read_scope_unavailable"), TEXT("A captured row requires its formal synchronous Source scope."), Path);
			Out = ActiveReadState->CapturedDocuments.Find(Root / Path);
			if (!Out) Out = &ActiveReadState->CapturedDocuments.Add(Root / Path);
			if (!Out->ByteHash.IsEmpty() && Out->ByteHash != ExpectedByteHash)
				return Fail(Error, TEXT("workspace_source_changed"), TEXT("The captured row was requested from different publications."), Path);
			return !Out->Utf8.IsEmpty() || ReadCapturedEnvelopeBytes(Root, Path, ExpectedByteHash, Resources, *Out, Text, Error);
		}
		bool ReadCapturedEnvelopeDirectory(const FString& Root, const FString& Path, const FString& ExpectedByteHash,
			FUeAgentResourceTracker& Resources, FSemanticWorkspaceReadState::FCapturedDocument*& Out, FWorkspaceError& Error,
			const FString& RequiredId = FString())
		{
			Out = nullptr; FString Text; FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
			if (!ReadCapturedDocumentBytes(Root, Path, ExpectedByteHash, Resources, Read, Text, Error)) return false;
			if (ActiveReadState->bEstablishFileObservation) {
				// Explicit complete-input establishment is a distinct caller, not a
				// failed sparse row request selecting a different parser.
				if (Read->Envelopes.IsEmpty()) {
					if (Text.IsEmpty()) { const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Read->Utf8.GetData()), Read->Utf8.Num()); Text = FString(Decoded.Length(), Decoded.Get()); }
					if (!ParseCapturedEnvelopeDirectory(Text, Path, *Read, Resources, Error)) return false;
				}
				Out = Read; return true;
			}
			if (RequiredId.IsEmpty() || !ActiveReadState->PublishedEnvelopeSpan)
				return Fail(Error, TEXT("workspace_publication_row_span_missing"), TEXT("A local baseline read requires a stable unit and the accepted formatter's row-span producer. No whole-file parser was selected."), Path);
			if (!Read->Envelopes.Contains(RequiredId)) {
				FAcceptedWorkspaceByteSpan Span;
				if (!ActiveReadState->PublishedEnvelopeSpan(RequiredId, Span, Error)) return false;
				if (!Span.IsValid()) return Fail(Error, TEXT("workspace_publication_row_span_missing"), TEXT("The same-generation formatter did not supply this unit's row span. No alternate baseline parser was selected."), RequiredId);
				Read->Envelopes.Add(RequiredId, {Span.Begin, Span.End, 0, 0}); ++ActiveReadState->CapturedSpanRows;
			}
			Out = Read; return true;
		}

		class FPublishedWorkspaceDirectory final : public FAcceptedWorkspaceInventory, public FResolvedWorkspaceUnitBindings
		{
		public:
			FString Root;
			FHjsonDocumentPtr Manifest;
			// Ordered references into this same immutable manifest, not another
			// path/string authority or a persistent inventory index.
			TArray<const FHjsonNode*> InventoryEntries;
			FWorkspacePublication Publication;
			TMap<FString, FString> PublishedHashes;
			mutable FResolvedWorkspaceInventory Loaded;
			mutable TSet<FString> LoadedShards;
			mutable TMap<FString, FIntPoint> ShardRanges;
			struct FDeclarationSlot {
				FSemanticUnit::FDeclarationView View;
				int32 RowIndex;
			};
			mutable TMap<FString, FDeclarationSlot> Declarations;
			TSharedPtr<const FPublishedSemanticDirectory> Organization;
			mutable FWorkspaceError ReadError;
			mutable TSharedPtr<FDefinitionProjectionHints> Hints;
			mutable bool bHintsRead = false;

			bool ValidateShardPath(const FString& Path) const
			{
				if (!ActiveReadState) return Fail(ReadError, TEXT("workspace_read_scope_unavailable"),
					TEXT("A published directory requires its synchronous accepted read scope."), Path);
				const auto* Expected = PublishedHashes.Find(Path);
				if (!Expected) return Fail(ReadError, TEXT("workspace_captured_file_unavailable"),
					TEXT("The required inventory shard has no accepted publication binding."), Path);
				int32 FirstEntry = 0, LastEntry = InventoryEntries.Num();
				while (FirstEntry < LastEntry) {
					const int32 Middle = FirstEntry + (LastEntry - FirstEntry) / 2;
					if (InventoryEntries[Middle]->GetText().Compare(Path, ESearchCase::CaseSensitive) < 0) FirstEntry = Middle + 1;
					else LastEntry = Middle;
					}
				const bool bListed = InventoryEntries.IsValidIndex(FirstEntry)
					&& InventoryEntries[FirstEntry]->GetText().Equals(Path, ESearchCase::CaseSensitive);
				if (!bListed) return Fail(ReadError, TEXT("workspace_captured_file_unavailable"),
					TEXT("The required inventory shard is absent from the accepted manifest."), Path);
				return true;
			}
			bool AdoptShard(const FString& Path, TArray<FResolvedWorkspaceInventory::FRow>&& Rows) const
			{
				const int32 First = Loaded.Rows.Num(); Loaded.Rows.Append(MoveTemp(Rows));
				// The same publication may already own exact baseline ranges. Pass
				// them beside the original inventory rows; full output must not lose
				// this input and parse the same immutable baseline again to recover it.
				const auto Session = WorkspaceSourceObservations.FindRef(Root);
				const FString Envelope = Path.Replace(TEXT("/inventory/"), TEXT("/baselines/"));
				const auto* Version = Session ? Session->Files.Find(Envelope) : nullptr;
				if (Session && Session->Generation == Publication.Generation && Version
					&& Version->Hash == PublishedHashes.FindRef(Envelope) && Version->FragmentSpans && Version->FragmentIdentities
					&& Version->FragmentSpans->Num() == Version->FragmentIdentities->Num()) {
					for (int32 Index = First; Index < Loaded.Rows.Num(); ++Index) {
						auto& Row = Loaded.Rows[Index]; const int32 Ordinal = Row.InventoryOrdinal;
						if (Row.Syntax->StoredEnvelopeSpan.IsValid()) continue;
						if (!Version->FragmentSpans->IsValidIndex(Ordinal)
							|| *(*Version->FragmentIdentities)[Ordinal] != Row.Syntax->GetUnitId())
							return Fail(ReadError, TEXT("workspace_publication_row_binding_invalid"),
								TEXT("The same-byte published baseline ordinal does not match its original inventory identity."), Row.Syntax->GetUnitId());
						const auto Span = (*Version->FragmentSpans)[Ordinal]; Row.EnvelopeSpan = {Span.X, Span.Y};
					}
					}
				LoadedShards.Add(Path); ShardRanges.Add(Path, {First, Loaded.Rows.Num() - First}); return true;
			}
			bool ReadShard(const FString& Path) const
			{
				if (LoadedShards.Contains(Path)) return true;
				if (!ValidateShardPath(Path)) return false;
				auto Resources = MakeSemanticWorkspaceResourceTracker(); TArray<FResolvedWorkspaceInventory::FRow> Rows;
				if (!ReadPublishedInventoryDocument(Root, Path, PublishedHashes.FindChecked(Path), Resources, Rows, ReadError)) return false;
				return AdoptShard(Path, MoveTemp(Rows));
			}

			const FResolvedWorkspaceInventory::FRow* FindRow(const FString& Id, const TCHAR* Consumer = TEXT("row")) const
			{
				// An absent optional reference is not an unknown workspace identity.
				if (Id.IsEmpty()) return nullptr;
				if (Organization) {
					if (!Organization->FindDeclaration(Id)) return nullptr;
					FString Envelope; int32 Ordinal = INDEX_NONE;
					if (!FindPublishedRowAddress(Id, Envelope, Ordinal)) return nullptr;
					const FString Shard = Envelope.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
					if (!ReadShard(Shard)) return nullptr;
					const auto Range = ShardRanges.FindChecked(Shard); const int32 Index = Range.X + Ordinal;
					if (Ordinal < 0 || Ordinal >= Range.Y || !Loaded.Rows.IsValidIndex(Index) || Loaded.Rows[Index].Syntax->GetUnitId() != Id) {
						Fail(ReadError, TEXT("workspace_publication_row_binding_invalid"), TEXT("The original formatter's current ordinal does not match the actual inventory row."), Id); return nullptr;
					}
					if (ActiveReadState) ++ActiveReadState->PublicationRowComparisons;
					return &Loaded.Rows[Index];
				}
				if (const auto* Declared = Declarations.Find(Id)) {
					if (!Loaded.Rows.IsValidIndex(Declared->RowIndex) || Loaded.Rows[Declared->RowIndex].Syntax->GetUnitId() != Id) {
						Fail(ReadError, TEXT("workspace_declaration_row_binding_invalid"), TEXT("The declaration's original physical row no longer belongs to this directory. No alternative identity search was selected."), Id); return nullptr;
					}
					return &Loaded.Rows[Declared->RowIndex];
				}
				for (const auto& Row : Loaded.Rows) { if (ActiveReadState) ++ActiveReadState->PublicationRowComparisons; if (Row.Syntax->GetUnitId() == Id) return &Row; }
				// Stable placement already addresses standalone roots/items. Reuse
				// that real producer's naming rule against this publication's file
				// namespace, then verify the complete UnitId in the original shard.
				// Packed fields must be supplied by their addressed document. An
				// unknown id is not permission to scan every unrelated shard.
				const FString Suffix = GetStableProjectionIdentitySuffix(Id);
				for (const auto& File : PublishedHashes) {
					if (!IsPublicSemanticFileCandidate(File.Key)
						|| (!File.Key.EndsWith(Suffix + TEXT(".hjson"), ESearchCase::CaseSensitive)
							&& !(File.Key.Contains(Suffix + TEXT("/"), ESearchCase::CaseSensitive)
								&& File.Key.EndsWith(TEXT("/state.hjson"), ESearchCase::CaseSensitive)))) continue;
					const FString Shard = GetInventoryPath(File.Key);
					if (!PublishedHashes.Contains(Shard) || LoadedShards.Contains(Shard)) continue;
					const int32 First = Loaded.Rows.Num();
					if (!ReadShard(Shard)) return nullptr;
					for (int32 Index = First; Index < Loaded.Rows.Num(); ++Index)
						if (Loaded.Rows[Index].Syntax->GetUnitId() == Id) return &Loaded.Rows[Index];
				}
				FailWorkspaceDataflowHandoff(ReadError, FString(TEXT("publication.row_lookup.")) + Consumer,
					TEXT("addressed row for ") + Id, Root / ManifestPath, TEXT("not in addressed/loaded shard"),
					TEXT("document/member identity and physical bucket"), TEXT("Source/actual member or reference producer"));
				return nullptr;
			}
			bool PrepareDocumentMetadata(const FString& Path) const override { return ReadShard(GetInventoryPath(Path)); }
			bool PrepareDocumentsMetadata(const TArray<FString>& Documents, FString& FailedDocument) const override
			{
				struct FParseWork {
					FString Path, Text; const FString* Document = nullptr; int64 Bytes = 0;
					TArray<FResolvedWorkspaceInventory::FRow> Rows; FWorkspaceError Error;
					TUniquePtr<FSemanticWorkspaceReadState> Counters;
					double WorkSeconds = 0; bool bInputReady = false, bParsed = false;
				};
				TArray<FParseWork> Batch;
				const int32 Width = FMath::Max(1, FTaskGraphInterface::Get().GetNumWorkerThreads() + 1);
				auto* MainState = ActiveReadState;
				if (MainState) MainState->bPublishedInventoryParallel = true;
				int32 Batches = 0, Jobs = 0, MaxBatch = 0; int64 PeakInputCharacters = 0;
				double WallSeconds = 0, WorkSeconds = 0;
				const auto Flush = [&]() {
					if (Batch.IsEmpty()) return true;
					++Batches; MaxBatch = FMath::Max(MaxBatch, Batch.Num()); Jobs += Batch.Num();
					int64 Characters = 0; for (const auto& Work : Batch) Characters += Work.Text.Len();
					PeakInputCharacters = FMath::Max(PeakInputCharacters, Characters);
					const double Started = FPlatformTime::Seconds();
					ParallelFor(Batch.Num(), [&](int32 Index) {
						auto& Work = Batch[Index]; if (!Work.bInputReady) return;
						// The original grammar and row validator consume private input only.
						// No filesystem/observation/directory/registry state is used here.
						Work.Counters = MakeUnique<FSemanticWorkspaceReadState>();
						auto* PreviousState = ActiveReadState; ActiveReadState = Work.Counters.Get();
						ON_SCOPE_EXIT { ActiveReadState = PreviousState; };
						auto Resources = MakeSemanticWorkspaceResourceTracker();
						const double WorkStarted = FPlatformTime::Seconds();
						Work.bParsed = ParsePublishedInventoryInput(Work.Text, Work.Path, Work.Bytes, Resources, Work.Rows, Work.Error);
						Work.WorkSeconds = FPlatformTime::Seconds() - WorkStarted;
					});
					WallSeconds += FPlatformTime::Seconds() - Started;
					for (auto& Work : Batch) {
						WorkSeconds += Work.WorkSeconds;
						if (!Work.bInputReady || !Work.bParsed) {
							FailedDocument = *Work.Document; ReadError = MoveTemp(Work.Error); return false;
						}
						if (MainState) {
							const auto& Count = *Work.Counters;
							MainState->PublishedInventoryRowsRead += Count.PublishedInventoryRowsRead;
							MainState->PublishedInventoryScalarNodesSkipped += Count.PublishedInventoryScalarNodesSkipped;
							MainState->PublishedInventoryBorrowedTokenTexts += Count.PublishedInventoryBorrowedTokenTexts;
							MainState->PublishedInventoryBorrowedTokenLexemes += Count.PublishedInventoryBorrowedTokenLexemes;
							MainState->PublishedInventoryBorrowedKeys += Count.PublishedInventoryBorrowedKeys;
							MainState->PublishedInventorySpanNodesSkipped += Count.PublishedInventorySpanNodesSkipped;
							MainState->InventoryFieldNodesVisited += Count.InventoryFieldNodesVisited;
							MainState->InventoryCompactDigestViews += Count.InventoryCompactDigestViews;
							MainState->InventoryValidatedPathReuses += Count.InventoryValidatedPathReuses;
							MainState->PublishedInventorySyntaxSeconds += Count.PublishedInventorySyntaxSeconds;
							MainState->PublishedInventoryRowsSeconds += Count.PublishedInventoryRowsSeconds;
							MainState->PublishedInventoryCopySeconds += Count.PublishedInventoryCopySeconds;
							MainState->PublishedInventoryRetireSeconds += Count.PublishedInventoryRetireSeconds;
						}
						if (!AdoptShard(Work.Path, MoveTemp(Work.Rows))) { FailedDocument = *Work.Document; return false; }
						const double RetireStarted = FPlatformTime::Seconds(); Work.Text.Empty(); Work.Counters.Reset();
						if (MainState) MainState->PublishedInventoryRetireSeconds += FPlatformTime::Seconds() - RetireStarted;
					}
					Batch.Reset(); return true;
				};
				for (const auto& Document : Documents) {
					FString Path = GetInventoryPath(Document);
					if (LoadedShards.Contains(Path) || Batch.ContainsByPredicate([&](const auto& Work) { return Work.Path == Path; })) continue;
					auto& Work = Batch.AddDefaulted_GetRef(); Work.Path = MoveTemp(Path); Work.Document = &Document;
					if (ValidateShardPath(Work.Path)) {
						auto Resources = MakeSemanticWorkspaceResourceTracker();
						Work.bInputReady = ReadPublishedInventoryInput(Root, Work.Path, PublishedHashes.FindChecked(Work.Path), Resources,
							Work.Text, Work.Bytes, Work.Error);
					} else { Work.Error = MoveTemp(ReadError); ReadError.Reset(); }
					if ((!Work.bInputReady || Batch.Num() == Width) && !Flush()) return false;
				}
				if (!Flush()) return false;
				UE_LOG(LogTemp, Display, TEXT("UAI source timing phase=inventory_parse_tasks batches=%d jobs=%d max_batch_files=%d peak_batch_input_characters=%lld task_wall_s=%.6f worker_elapsed_sum_s=%.6f io_and_admission=original_document_order row_algorithm=original_parser"),
					Batches, Jobs, MaxBatch, PeakInputCharacters, WallSeconds, WorkSeconds);
				return true;
			}

			int32 Num() const override { return Publication.PublishedUnitCount.GetValue(); }
			TOptional<int32> GetPublishedUnitCount() const override { return Publication.PublishedUnitCount; }
			TOptional<FString> FindPublicRef(const FString& Ref) const override {
				return Organization ? Organization->FindUnitByPublicRef(Ref) : FAcceptedWorkspaceInventory::FindPublicRef(Ref);
			}
			TOptional<bool> GetScopedMachineBuckets() const override { return Publication.ScopedMachineBuckets; }
			FString GetInventoryPath(const FString& Document) const override {
				if (!Publication.ScopedMachineBuckets.IsSet()) return Loaded.GetInventoryPath(Document);
				return GetPublishedMachineEnvelopePath(Document, Publication.ScopedMachineBuckets.GetValue())
					.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
			}
			void VisitDocuments(TFunctionRef<void(const FString&)> Visitor) const override {
				if (!Publication.ScopedMachineBuckets.IsSet()) { Loaded.VisitDocuments(Visitor); return; }
				for (const auto& File : PublishedHashes)
					if (IsPublicSemanticFileCandidate(File.Key)
						&& PublishedHashes.Contains(GetInventoryPath(File.Key))) Visitor(File.Key);
			}
			bool VisitDocument(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
				const FString Shard = GetInventoryPath(Path);
				if (!ReadShard(Shard)) return false;
				// The same owner just produced this exact shard range. Rows loaded
				// for unrelated earlier documents are not inputs to this selection.
				// A coarse bucket may contain multiple documents: keep the exact
				// document predicate inside that real physical range.
				const auto Range = ShardRanges.FindChecked(Shard);
				if (ActiveReadState) ++ActiveReadState->PublishedInventoryDocumentQueries;
				return Loaded.VisitRange(Range.X, Range.Y, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (ActiveReadState) ++ActiveReadState->PublishedInventoryDocumentSlots;
					if (Row.DocumentPath != Path) return true;
					if (ActiveReadState) ++ActiveReadState->PublishedInventoryDocumentMatches;
					return Visitor(Row);
				});
			}
			bool VisitUnit(const FString& Id, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
				const auto* Row = FindRow(Id); if (!Row) return false;
				return Loaded.VisitRange(static_cast<int32>(Row - Loaded.Rows.GetData()), 1, Visitor);
			}
			bool VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor,
				bool bRequireEnvelopeSpan = true) const override {
				const FString Shard = Path.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
				if (!ReadShard(Shard)) return false;
				const auto Range = ShardRanges.FindChecked(Shard);
				for (int32 Index = Range.X; bRequireEnvelopeSpan && Index < Range.X + Range.Y; ++Index) {
					const auto Row = Loaded.Rows[Index]; FAcceptedWorkspaceByteSpan Span;
					if (!ReadEnvelopeSpanAtOrdinal(Row.Syntax->GetUnitId(), Path, Row.InventoryOrdinal, Span)) return false;
					Loaded.Rows[Index].EnvelopeSpan = Span;
				}
				return Loaded.VisitRange(Range.X, Range.Y, Visitor);
			}
			bool Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
				const auto* Files = Manifest->GetRoot()->Get(TEXT("unit_inventory_files"));
				for (const auto& File : Files->GetItems()) {
					if (!ReadShard(File->GetText())) return false;
					const auto Range = ShardRanges.FindChecked(File->GetText());
					if (!Loaded.VisitRange(Range.X, Range.Y, Visitor)) return false;
					if (ActiveReadState) if (auto* Captured = ActiveReadState->CapturedDocuments.Find(Root /
						File->GetText().Replace(TEXT("/inventory/"), TEXT("/baselines/")))) {
							const int64 Bytes = Captured->Utf8.GetAllocatedSize();
						ActiveReadState->CapturedLiveBytes -= Bytes; ActiveReadState->CapturedRetiredBytes += Bytes;
						Captured->Utf8.Empty();
					}
				}
				return true;
			}
			void VisitHashes(TFunctionRef<void(const FString&, const FString&)> Visitor) const override {
				Visit([&](const FAcceptedWorkspaceInventoryRow& Row) { Visitor(Row.UnitId, Row.CanonicalContentHash()); return true; });
			}
			void VisitPaths(TFunctionRef<void(const FString&, const FString&, const FString&, const TArray<FString>&)> Visitor) const override {
				Visit([&](const FAcceptedWorkspaceInventoryRow& Row) { Visitor(Row.UnitId, Row.DocumentPath, Row.EnvelopePath, Row.ContentFiles); return true; });
			}
			bool HasContentFileDirectory() const override { return Publication.ExpandedAuthorDocuments.IsSet(); }
			void VisitExpandedDocuments(TFunctionRef<void(const FString&)> Visitor) const override {
				if (Publication.ExpandedAuthorDocuments.IsSet()) for (const auto& Path : Publication.ExpandedAuthorDocuments.GetValue()) Visitor(Path);
			}
			void VisitContentFiles(TFunctionRef<void(const FString&)> Visitor) const override {
				VisitExpandedDocuments([&](const FString& Path) { VisitDocument(Path, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					for (const auto& File : Row.ContentFiles) Visitor(File); return true;
				}); });
			}
			TOptional<FSemanticUnit::FDeclarationView> ReadPublishedDeclaration(const FString& Id, ESemanticUnitKind Kind,
				TSharedRef<const FString, ESPMode::ThreadSafe> Identity, const FString& Path, const FString& Pointer, int32 Ordinal) const
			{
				if (Organization) return Organization->FindDeclaration(Id);
				if (const auto* Existing = Declarations.Find(Id)) return Existing->View;
				const auto* Hash = PublishedHashes.Find(Path);
				if (!Hash) { Fail(ReadError, TEXT("workspace_captured_file_unavailable"), TEXT("The required declaration has no published file binding."), Path); return {}; }
				auto Resources = MakeSemanticWorkspaceResourceTracker();
				FSemanticWorkspaceReadState::FCapturedDocument* Document = nullptr;
				TOptional<FSourceProvenance> Origin;
				if (ActiveReadState && ActiveReadState->bEstablishFileObservation) {
					if (!ReadCapturedEnvelopeDirectory(Root, Path, *Hash, Resources, Document, ReadError, Id)) return {};
					Origin = Document->FindOrigin(Path, Id);
				} else {
					FString Text; FAcceptedWorkspaceByteSpan Span;
					if (!ReadEnvelopeSpanAtOrdinal(Id, Path, Ordinal, Span)
						|| !ReadCapturedDocumentBytes(Root, Path, *Hash, Resources, Document, Text, ReadError)) return {};
					Origin.Emplace(Path, TEXT("/") + EscapeJsonPointerSegment(Id), 0, 0, Span.Begin, Span.End, Id);
				}
				if (!Origin) { Fail(ReadError, TEXT("workspace_publication_row_span_missing"), TEXT("The addressed declaration has no original row span."), Id); return {}; }
				auto IncludePredicate = [](const FString& Pointer, const FString&) {
					const FString Values(TEXT("/captured_values/")); const int32 Begin = Pointer.Find(Values, ESearchCase::CaseSensitive);
					if (Begin != INDEX_NONE) return Pointer.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Begin + Values.Len()) == INDEX_NONE;
					return !Pointer.Contains(TEXT("/captured_type/"), ESearchCase::CaseSensitive);
				};
				TFunctionRef<bool(const FString&, const FString&)> Include(IncludePredicate);
				TOptional<FSemanticUnit::FDeclarationView> Declaration;
				if (!TryConsumeGeneratedHjsonSpan(Document->Utf8, Origin.GetValue(), Resources,
					[&](const FHjsonNode& Envelope, FWorkspaceError& Failure) {
						const auto* Values = Envelope.Get(TEXT("captured_values"));
						if (!Values || Values->GetKind() != EHjsonNodeKind::Object) return Fail(Failure,
							TEXT("semantic_workspace_captured_values_missing"), TEXT("The required declaration has no captured field shape."), Path);
						TOptional<FSemanticUnit> Unit; TOptional<bool> Changed; TSet<FString> Roots; FHjsonNodePtr Sources;
						const TMap<FString, FString> NoPublicRemapping;
						if (!ParseSemanticUnit(Envelope, &Envelope, Identity, LexToString(Kind),
							Pointer, NoPublicRemapping, Resources, Unit, Changed, Roots, Sources, Failure, true) || !Unit) return false;
						TArray<FString> Keys; for (const auto& Field : Values->GetEntries()) Keys.Add(Field.Key);
						const auto* Type = Values->Get(TEXT("type"));
						Declaration = Unit->GetDeclarationForCapturedShape(Keys, Type && Type->GetKind() == EHjsonNodeKind::Object);
						return true;
					}, ReadError, &Include)) return {};
				// FDeclarationView contains identities/type/shape, not source
				// coordinates or syntax nodes. The temporary row tree dies here.
				const auto* Range = ShardRanges.Find(Path.Replace(TEXT("/baselines/"), TEXT("/inventory/")));
				const int32 RowIndex = Range && Ordinal >= 0 && Ordinal < Range->Y ? Range->X + Ordinal : INDEX_NONE;
				if (!Loaded.Rows.IsValidIndex(RowIndex) || Loaded.Rows[RowIndex].Syntax->GetUnitId() != Id) {
					Fail(ReadError, TEXT("workspace_declaration_row_binding_invalid"), TEXT("The inventory producer did not hand off the declaration's original physical row/ordinal. No alternative search was selected."), Id); return {};
				}
				Declarations.Add(Id, FDeclarationSlot{Declaration.GetValue(), RowIndex});
				if (ActiveReadState) ++ActiveReadState->CapturedDeclarationRows;
				return Declaration;
			}
			TOptional<FSemanticUnit::FDeclarationView> FindPublishedDeclaration(const FString& Id) const override {
				if (Organization) return Organization->FindDeclaration(Id);
				if (const auto* Existing = Declarations.Find(Id)) return Existing->View;
				const auto* Found = FindRow(Id, TEXT("declaration")); if (!Found) return {};
				const auto Row = *Found; const auto Syntax = Row.Syntax;
				return ReadPublishedDeclaration(Id, Syntax->UnitKind, Syntax->UnitIdentity.ToSharedRef(), Syntax->GetEnvelopeRef(), Syntax->GetUnitPointer(), Row.InventoryOrdinal);
			}
			bool VisitPublishedDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&, int32)> Visitor) const override {
				if (Organization) {
					const double Started = FPlatformTime::Seconds(); int32 Visited = 0;
					const bool Result = Organization->VisitDeclarations([&](const auto& Declaration, int32 Position) { ++Visited; return Visitor(Declaration, Position); });
					UE_LOG(LogTemp, Display, TEXT("UAI source timing phase=published_declarations visited=%d metadata=original_compiler_and_actual_delta row_ast=0 inventory_rows_parsed=0 total_s=%.6f"), Visited, FPlatformTime::Seconds() - Started);
					return Result;
				}
				int32 Index = 0;
				return Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (!Row.SharedUnitIdentity) return Fail(ReadError, TEXT("workspace_declaration_identity_missing"),
						TEXT("The actual inventory producer did not hand off this row's immutable identity."), Row.UnitId);
					const auto Declaration = ReadPublishedDeclaration(Row.UnitId, Row.UnitKind, Row.SharedUnitIdentity.ToSharedRef(), Row.EnvelopePath, Row.UnitPointer, Row.InventoryOrdinal);
					return Declaration.IsSet() && Visitor(Declaration.GetValue(), Index++);
				});
			}

			bool VisitPublishedDefinitionDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&)> Visitor) const override {
				if (Organization) {
					for (const auto& Binding : Organization->DefinitionHints.Definitions) {
						const auto Declaration = Organization->FindDeclaration(Binding.Key);
						if (!Declaration) return Fail(ReadError, TEXT("workspace_publication_definition_missing"), TEXT("The published definition binding has no same-generation declaration."), Binding.Key);
						if (!Visitor(Declaration.GetValue())) return false;
					}
					return true;
				}
				for (const auto& File : PublishedHashes) if (File.Key.EndsWith(TEXT("/definitions.hjson"), ESearchCase::CaseSensitive)) {
					if (!VisitDocument(File.Key, [&](const FAcceptedWorkspaceInventoryRow& Row) {
						if (Row.UnitKind != ESemanticUnitKind::CollectionState) return true;
						if (!Row.SharedUnitIdentity) return Fail(ReadError, TEXT("workspace_declaration_identity_missing"), TEXT("The definition row has no original immutable identity."), Row.UnitId);
						const auto Declaration = ReadPublishedDeclaration(Row.UnitId, Row.UnitKind, Row.SharedUnitIdentity.ToSharedRef(), Row.EnvelopePath, Row.UnitPointer, Row.InventoryOrdinal);
						return Declaration && (Declaration->GetRole() != TEXT("property_definition") || Visitor(Declaration.GetValue()));
					})) return false;
				}
				return true;
			}
			bool VisitPublishedTargetIdentity(const FString& Target,
				TFunctionRef<void(const FSemanticUnit::FDeclarationView&)> Visitor) const override {
				TArray<FSemanticUnit::FDeclarationView> Matches;
				for (const auto& Pair : Declarations) if (Pair.Value.View.GetTargetIdentityKey() == Target) Matches.Add(Pair.Value.View);
				for (const auto& Declaration : Matches) Visitor(Declaration);
				if (!Matches.IsEmpty()) return true; // Stable producer identities were bound by current machine bytes.
				// Actual effects on another field of the selected native owner do
				// not require metadata for every unrelated owner. These rows came
				// from the same real selected machine shards.
				TArray<TSharedRef<const FInventoryRowSyntax>> Selected;
				for (const auto& Row : Loaded.Rows) Selected.Add(Row.Syntax);
				for (const auto& Row : Selected) {
					const auto Declaration = FindPublishedDeclaration(Row->GetUnitId());
					if (Declaration && Declaration->GetTargetIdentityKey() == Target) Matches.Add(Declaration.GetValue());
				}
				for (const auto& Declaration : Matches) Visitor(Declaration);
				if (!Matches.IsEmpty()) return true;
				return FailWorkspaceDataflowHandoff(ReadError, TEXT("publication.target_identity"),
					TEXT("addressed declaration for ") + Target, Root / ManifestPath,
					TEXT("not in selected declaration rows"), TEXT("current native owner to published row binding"),
					TEXT("scoped current observation / Source publication binding"));
			}
			bool ReadEnvelopeSpanAtOrdinal(const FString& Id, const FString& Path, int32 Ordinal, FAcceptedWorkspaceByteSpan& Out) const {
				const auto Session = WorkspaceSourceObservations.FindRef(Root);
				const auto* Version = Session ? Session->Files.Find(Path) : nullptr;
				if (!Session || Session->Generation != Publication.Generation || !Version
					|| Version->Hash != PublishedHashes.FindRef(Path) || !Version->FragmentSpans
					|| !Version->FragmentSpans->IsValidIndex(Ordinal))
					return Fail(ReadError, TEXT("workspace_publication_row_span_missing"),
						FString::Printf(TEXT("Stage=source.machine_row_span; unit=%s; bucket=%s; ordinal=%d; available_spans=%d; same_generation=%s; same_bytes=%s. Producer=published formatter or explicit complete-input version establishment. No bucket parser or alternate inventory was selected."),
							*Id, *Path, Ordinal, Version && Version->FragmentSpans ? Version->FragmentSpans->Num() : 0,
							Session && Session->Generation == Publication.Generation ? TEXT("true") : TEXT("false"),
							Version && Version->Hash == PublishedHashes.FindRef(Path) ? TEXT("true") : TEXT("false")), Id);
				const auto Span = (*Version->FragmentSpans)[Ordinal];
				Out = {Span.X, Span.Y};
				return Out.IsValid();
			}
			bool FindPublishedRowAddress(const FString& Id, FString& Path, int32& Ordinal) const {
				const auto Document = Organization ? Organization->FindDocument(Id) : TOptional<FString>();
				if (!Document) return Fail(ReadError, TEXT("workspace_publication_row_location_missing"), TEXT("The same-publication directory has no physical document for this unit."), Id);
				Path = GetPublishedMachineEnvelopePath(Document.GetValue(), Organization->bScopedBuckets);
					const auto Session = WorkspaceSourceObservations.FindRef(Root);
					const auto* Version = Session ? Session->Files.Find(Path) : nullptr;
					if (!Session || Session->Generation != Publication.Generation || !Version || Version->Hash != PublishedHashes.FindRef(Path)
						|| !Version->FragmentIdentities || !Version->FragmentSpans || Version->FragmentIdentities->Num() != Version->FragmentSpans->Num())
						return Fail(ReadError, TEXT("workspace_publication_row_identity_missing"), TEXT("The current machine-file version has no same-byte row identities/ranges. No inventory parser was selected."), Path);
					for (int32 Index = 0; Index < Version->FragmentIdentities->Num(); ++Index) if (*(*Version->FragmentIdentities)[Index] == Id) { Ordinal = Index; return true; }
					return Fail(ReadError, TEXT("workspace_publication_row_identity_missing"), TEXT("The addressed unit is absent from its published physical row sequence."), Id);
			}
			bool FindEnvelopeSpan(const FString& Id, FAcceptedWorkspaceByteSpan& Out) const {
				if (Organization) { FString Path; int32 Ordinal = INDEX_NONE;
					return FindPublishedRowAddress(Id, Path, Ordinal) && ReadEnvelopeSpanAtOrdinal(Id, Path, Ordinal, Out); }
				const auto* Row = FindRow(Id); if (!Row) return false;
				return ReadEnvelopeSpanAtOrdinal(Id, Row->Syntax->GetEnvelopeRef(), Row->InventoryOrdinal, Out);
			}

			TSharedPtr<const FDefinitionProjectionHints> GetProjectionHints() const override {
				return Organization ? TSharedPtr<const FDefinitionProjectionHints>(Organization, &Organization->DefinitionHints) : Hints;
			}
			TSharedPtr<const FDefinitionProjectionBinding> FindPublishedDefinitionBinding(const FString& Id) const override {
				if (Organization) {
					const auto* Binding = Organization->DefinitionHints.Definitions.Find(Id);
					return Binding ? TSharedPtr<const FDefinitionProjectionBinding>(Organization, Binding) : nullptr;
					}
				// A property formatter asks for this one definition. Its catalog
				// shard is a real declaration source, not every property's metadata.
				for (const auto& File : PublishedHashes) if (File.Key.EndsWith(TEXT("/definitions.hjson"), ESearchCase::CaseSensitive))
					if (!ReadShard(GetInventoryPath(File.Key))) return nullptr;
				TSharedPtr<const FDefinitionProjectionBinding> Result;
				if (!VisitUnit(Id, [&](const FAcceptedWorkspaceInventoryRow& Row) {
					if (Row.UnitKind != ESemanticUnitKind::CollectionState) return true;
					const auto* Unit = FindBaseline(Id); if (!Unit) return false;
					FDefinitionProjectionBinding Binding;
					if (TryReadPersistedDefinitionProjectionBinding(*Unit, Row.UnitPointer, Row.PublicRef, Binding))
						Result = MakeShared<const FDefinitionProjectionBinding>(MoveTemp(Binding));
					return true;
				})) return nullptr;
				return Result;
			}
			TSharedPtr<const FDefinitionProjectionHints> ReadPublishedDefinitionBindings() const override {
				if (Organization) return GetProjectionHints();
				if (bHintsRead) return Hints;
				bHintsRead = true; Hints = MakeShared<FDefinitionProjectionHints>();
				int64 Version = 0;
				if (!RequiredString(*Manifest->GetRoot(), TEXT("contract_id"), Hints->ContractId, ReadError)
					|| !RequiredInteger(*Manifest->GetRoot(), TEXT("contract_version"), 1, Version, ReadError)
					|| !RequiredString(*Manifest->GetRoot(), TEXT("profile_id"), Hints->ProfileId, ReadError)
					|| !ReadStringArray(*Manifest->GetRoot(), TEXT("target_unit_ids"), true, Hints->TargetUnitIds, ReadError)) return nullptr;
				Hints->ContractVersion = static_cast<int32>(Version);
				for (const auto& File : PublishedHashes) if (File.Key.EndsWith(TEXT("/definitions.hjson"), ESearchCase::CaseSensitive)) {
					if (!VisitDocument(File.Key, [&](const FAcceptedWorkspaceInventoryRow& Row) {
						if (!Row.SharedUnitIdentity) return Fail(ReadError, TEXT("workspace_declaration_identity_missing"), TEXT("The definition row has no original immutable identity."), Row.UnitId);
						if (!ReadPublishedDeclaration(Row.UnitId, Row.UnitKind, Row.SharedUnitIdentity.ToSharedRef(), Row.EnvelopePath, Row.UnitPointer, Row.InventoryOrdinal)) return false;
						const auto* Unit = FindBaseline(Row.UnitId); if (!Unit) return false;
						FDefinitionProjectionBinding Binding;
						if (TryReadPersistedDefinitionProjectionBinding(*Unit, Row.UnitPointer, Row.PublicRef, Binding)) Hints->Definitions.Add(Row.UnitId, MoveTemp(Binding));
						return true;
					})) return nullptr;
				}
				return Hints;
			}
			const FSourceProvenance* FindSource(const FString&) const override { return nullptr; }
			const FWorkspaceError* GetReadFailure() const override { return ReadError.HasErrors() ? &ReadError : FindScopedCapturedSemanticReadFailure(*this); }
			const FAcceptedWorkspaceInventory* GetGeneratedInventory() const override { return this; }
			FString ReadBaselineHash(const FString& Id) const override {
				const auto* Row = FindRow(Id);
				if (!Row) return {};
				if (Row->Syntax->ContentHash.bCompact) return Row->Syntax->ContentHash.ToString();
				FailWorkspaceDataflowHandoff(ReadError, TEXT("publication.row_digest"),
					TEXT("canonical content_hash for ") + Id, Root / Row->Syntax->GetEnvelopeRef(),
					TEXT("row digest absent or not canonical"), TEXT("published same-row SHA-256 content digest"),
					TEXT("formatter machine row writer"));
				return {};
			}
			const FSemanticUnit* FindBaseline(const FString& Id) const override {
				const auto Declaration = FindPublishedDeclaration(Id);
				if (!Declaration) return nullptr;
				FString Path;
				if (Organization) {
					const auto Document = Organization->FindDocument(Id); if (!Document) return nullptr;
					Path = GetPublishedMachineEnvelopePath(Document.GetValue(), Organization->bScopedBuckets);
				} else {
					const auto* Row = FindRow(Id); if (!Row) return nullptr; Path = Row->Syntax->GetEnvelopeRef();
				}
				const FString Hash = PublishedHashes.FindRef(Path);
				return ReadScopedCapturedSemanticUnit(*this, Id, [&](FUeAgentResourceTracker& Resources,
					TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) {
					return TryReadPublishedCapturedSemanticUnit(Root, Path, Hash, Declaration.GetValue(), Resources, Out, Error);
				}, ReadError);
			}
			bool TryBaselineContainsAnyString(const FString& Id, TFunctionRef<bool(const FString&)> Matches,
				TOptional<bool>& Out, FWorkspaceError& Error) const override {
				Out.Reset();
				const auto Declaration = FindPublishedDeclaration(Id);
				if (!Declaration) { if (ReadError.HasErrors()) { Error = ReadError; return false; } return true; }
				FString Path;
				if (Organization) {
					const auto Document = Organization->FindDocument(Id); if (!Document) return true;
					Path = GetPublishedMachineEnvelopePath(Document.GetValue(), Organization->bScopedBuckets);
				} else {
					const auto* Row = FindRow(Id); if (!Row) { Error = ReadError; return !Error.HasErrors(); }
					Path = Row->Syntax->GetEnvelopeRef();
				}
				bool Match = false; auto Resources = MakeSemanticWorkspaceResourceTracker();
				if (!TryPublishedCapturedContainsAnyString(Root, Path, PublishedHashes.FindRef(Path), Id, Matches, Resources, Match, Error)) return false;
				Out = Match; return true;
			}
			bool VisitBaselineMemberNames(const FString& Id, const FString& RootField, FKeyedMemberNameVisitor Visitor,
				FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const override {
				FString Path;
				if (Organization) {
					const auto Document = Organization->FindDocument(Id); if (!Document) return true;
					Path = GetPublishedMachineEnvelopePath(Document.GetValue(), Organization->bScopedBuckets);
				} else {
					const auto* Row = FindRow(Id, TEXT("keyed_member_names")); if (!Row) return !ReadError.HasErrors(); Path = Row->Syntax->GetEnvelopeRef();
				}
				const FString Hash = PublishedHashes.FindRef(Path);
				return ReadScopedCapturedMemberNames(*this, Id, RootField, Resources, Visitor, [&](FHjsonNodePtr& Out, FWorkspaceError& Failure) {
					return TryReadPublishedCapturedMemberNames(Root, Path, Hash, Id, RootField, Resources, Out, Failure);
				}, Error);
			}
			bool TryReadBaselineMembers(const FString& Id, const FString& RootField, const TSet<FString>& Keys,
				FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const override {
				Out.Reset(); const auto Declaration = FindPublishedDeclaration(Id);
				if (!Declaration) { if (ReadError.HasErrors()) { Error = ReadError; return false; } return true; }
				FString Path;
				if (Organization) {
					const auto Document = Organization->FindDocument(Id); if (!Document) return true;
					Path = GetPublishedMachineEnvelopePath(Document.GetValue(), Organization->bScopedBuckets);
				} else {
					const auto* Row = FindRow(Id); if (!Row) { if (ReadError.HasErrors()) Error = ReadError; return !ReadError.HasErrors(); } Path = Row->Syntax->GetEnvelopeRef();
				}
				const FString Hash = PublishedHashes.FindRef(Path);
				return TryReadPublishedCapturedSemanticMembers(Root, Path, Hash, Declaration.GetValue(), RootField, Keys, Resources, Out, Error);
			}
			FHjsonDocumentPtr FindSourceDocument(const FString&) const override { return nullptr; }
			void CopySources(TMap<FString, FSourceProvenance>&) const override {}
			void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const override {
				Visit([&](const FAcceptedWorkspaceInventoryRow& Row) { if (const auto* Unit = FindBaseline(Row.UnitId)) Out.Add(Row.UnitId, *Unit); return !ReadError.HasErrors(); });
			}
		};

		TSharedPtr<const FGeneratedSourceCandidate> ReadPublishedDirectory(const FString& Root, FParsedSource& Parsed,
			FWorkspaceError& Error)
		{
			FWorkspacePublication Publication;
			if (!TryReadWorkspacePublication(*Parsed.Manifest, ManifestPath, Publication, Error)) return {};
			const auto Missing = [&](const FString& Fields) -> TSharedPtr<const FGeneratedSourceCandidate> {
				const auto* Schema = Parsed.Manifest->Get(TEXT("workspace_schema"));
				FailWorkspaceDataflowHandoff(Error, TEXT("source.publication_directory"), Fields, Root / ManifestPath,
					Schema ? Schema->GetText() : TEXT("workspace_schema absent"),
					TEXT("same committed /4 publication containing ") + Fields,
					TEXT("composition publication header / machine row writer"));
				return {};
			};
			if (!Publication.bDirectDataflow) return Missing(TEXT("workspace_schema=/4"));
			TArray<FString> MissingFields;
			if (!Publication.ScopedMachineBuckets.IsSet()) MissingFields.Add(TEXT("machine_bucket_organization"));
			if (!Publication.PublishedUnitCount.IsSet()) MissingFields.Add(TEXT("published_unit_count"));
			if (!Publication.ExpandedAuthorDocuments.IsSet()) MissingFields.Add(TEXT("expanded_author_documents"));
			if (!MissingFields.IsEmpty()) return Missing(FString::Join(MissingFields, TEXT(",")));
			auto Directory = MakeShared<FPublishedWorkspaceDirectory>();
			Directory->Root = Root; Directory->Manifest = Parsed.ManifestDocument;
			Directory->Publication = Publication;
			if (const auto* Files = Parsed.Manifest->Get(TEXT("unit_inventory_files"))) {
				for (const auto& File : Files->GetItems()) Directory->InventoryEntries.Add(&File.Get());
				Directory->InventoryEntries.Sort([](const FHjsonNode& A, const FHjsonNode& B) {
					return A.GetText().Compare(B.GetText(), ESearchCase::CaseSensitive) < 0;
					});
				}
			const FString OwnerPath = Root / TEXT(".uai-export-owner.json");
			FString OwnerText, OwnerHash; int64 OwnerCharacters = 0;
			if (IsReparsePoint(OwnerPath) || !ObserveSourceFile(OwnerPath, true, OwnerText, OwnerHash, OwnerCharacters))
				return Missing(TEXT("readable publication owner bytes"));
			const auto* OwnerObservation = ObservedSourceFiles.Find(OwnerPath);
			const auto OwnerProof = OwnerObservation ? OwnerObservation->Proof : FWorkspaceFileProofPtr();
			FString State, Owner; ArtifactPublisher::FArtifactPublishError Failure;
			if (!ArtifactPublisher::TryReadObservedWorkspaceConsumerState(Root, FString(), OwnerText,
				State, Owner, Failure, &Directory->PublishedHashes)) {
				Error.SetFailure(MoveTemp(Failure)); return {};
			}
			ActiveReadState->PublisherOwnerId = Owner;
			for (const auto& Path : ActiveReadState->SelectedFilePaths) if (Path.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive)) {
				const auto* Expected = Directory->PublishedHashes.Find(Path);
				FString Text, Hash, Bytes; int64 Characters = 0;
				if (!Expected || IsReparsePoint(Root / Path)
					|| !ObserveSourceFile(Root / Path, false, Text, Hash, Characters, nullptr, &Bytes) || Bytes != *Expected)
					return Missing(TEXT("selected machine input differs from its publication: ") + Path);
				ActiveReadState->HashOnlyDocuments.Add(Root / Path, Hash);
			}
			auto Candidate = MakeShared<FGeneratedSourceCandidate>(); Candidate->Root = Root; Candidate->Publication = Publication;
			TArray<FString> MachinePaths;
			for (const auto& File : Directory->PublishedHashes) if (File.Key.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive)) {
				// This is the committed owner's expected byte table. Actual bytes
				// are verified only by the selected row/declaration consumer.
				Candidate->FileHashes.Add(File.Key, File.Value);
				if (File.Key.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) MachinePaths.Add(File.Key);
			}
			// Exact accepted table bytes, not a full semantic fingerprint or an
			// invented cache token, bind this request's reference lookup origin.
			MachinePaths.Sort(); FWorkspaceTextHash References;
			for (const auto& Path : MachinePaths) { References.Append(Path); References.Append(TEXT("|")); References.Append(Candidate->FileHashes.FindChecked(Path)); }
			Candidate->ReferenceMapHash = References.Finish();
			TSet<FString> Pending, Expanded;
			const bool bPendingKnown = TryReadPublishedPendingAuthorDocuments(State, Publication, Pending);
			if (!bPendingKnown) return Missing(TEXT("consumer_state.pending_author_documents for this publication_generation"));
			Candidate->PendingAuthorDocuments = Pending;
			Directory->VisitExpandedDocuments([&](const FString& Path) { Expanded.Add(Path); });
			const auto Session = WorkspaceSourceObservations.FindRef(Root);
			for (const auto& File : Directory->PublishedHashes) if (IsPublicSemanticFileCandidate(File.Key)) {
				const auto* Version = Session ? Session->Files.Find(File.Key) : nullptr;
				// A cold hash is only a byte comparison candidate. The original
				// inventory reader still determines which files actually own rows.
				if (ActiveReadState->bEstablishFileObservation || (Version && Version->bSemanticSourceDocument))
					Candidate->FileHashes.Add(File.Key, File.Value);
			}
			if (!ActiveReadState->bEstablishFileObservation) {
				if (!Session || Session->Generation != Publication.Generation) return Missing(TEXT("Windows observation publication_generation"));
				if (!Session->PublishedDirectory || Session->PublishedDirectory->UnitCount != Publication.PublishedUnitCount.GetValue()
					|| Session->PublishedDirectory->bScopedBuckets != Publication.ScopedMachineBuckets.GetValue()) return Missing(TEXT("same-generation publication declarations/placements/names"));
				Directory->Organization = Session->PublishedDirectory;
				ActiveReadState->SelectedFilePaths.Append(Pending);
			}
			ActiveReadState->PublishedEnvelopeSpan = [Directory](const FString& Id, FAcceptedWorkspaceByteSpan& Span, FWorkspaceError& Failure) {
				if (Directory->FindEnvelopeSpan(Id, Span)) return true;
				Failure = Directory->ReadError; return false;
			};
			// The stream/root is a real native/committed consumer. Its original
			// state document supplies that binding; no complete target index is built.
			for (const auto& File : Directory->PublishedHashes)
				if (File.Key.EndsWith(TEXT("/definitions.hjson"), ESearchCase::CaseSensitive)
					&& !Directory->ReadShard(Directory->GetInventoryPath(File.Key))) { Error = Directory->ReadError; return {}; }
			for (const auto& File : Directory->PublishedHashes)
				if (File.Key.StartsWith(TEXT("targets/"), ESearchCase::CaseSensitive)
					&& File.Key.EndsWith(TEXT("/state.hjson"), ESearchCase::CaseSensitive)
					&& !File.Key.Contains(TEXT("/items/"), ESearchCase::CaseSensitive))
					if (!Directory->VisitDocument(File.Key, [&](const FAcceptedWorkspaceInventoryRow& Row) {
						if (!Row.SharedUnitIdentity) return Fail(Directory->ReadError, TEXT("workspace_declaration_identity_missing"), TEXT("The target row has no original immutable identity."), Row.UnitId);
						return Directory->ReadPublishedDeclaration(Row.UnitId, Row.UnitKind, Row.SharedUnitIdentity.ToSharedRef(), Row.EnvelopePath, Row.UnitPointer, Row.InventoryOrdinal).IsSet();
					})) {
						if (Directory->ReadError.HasErrors()) { Error = Directory->ReadError; return {}; }
						return Missing(TEXT("published target declaration in ") + File.Key);
					}
			if (!OwnerProof || !OwnerProof->IsCurrent()) {
				FString Text, Hash; int64 Characters = 0;
				if (!ObserveSourceFile(OwnerPath, true, Text, Hash, Characters) || Hash != OwnerHash)
					return Fail(Error, TEXT("workspace_source_changed"), TEXT("The publication ownership changed during directory lookup."), OwnerPath), TSharedPtr<const FGeneratedSourceCandidate>();
			}
			FString Contract, Profile, Revision; int64 Version = 0; TArray<FString> Targets;
			if (!RequiredString(*Parsed.Manifest, TEXT("contract_id"), Contract, Error)
				|| !RequiredInteger(*Parsed.Manifest, TEXT("contract_version"), 1, Version, Error)
				|| !RequiredString(*Parsed.Manifest, TEXT("profile_id"), Profile, Error)
				|| !RequiredString(*Parsed.Manifest, TEXT("base_revision"), Revision, Error)
				|| !ReadStringArray(*Parsed.Manifest, TEXT("target_unit_ids"), true, Targets, Error)) return {};
			TArray<FSemanticUnit> RootUnits;
			for (const auto& Id : Targets) {
				const auto* Unit = Directory->FindBaseline(Id);
				if (!Unit) {
					if (Directory->ReadError.HasErrors()) { Error = Directory->ReadError; return {}; }
					return Missing(TEXT("published root captured row: ") + Id);
				}
				RootUnits.Add(*Unit);
			}
			const auto RootView = FAuthoringModel::MakePropertySourceView(MoveTemp(Contract), static_cast<int32>(Version),
				MoveTemp(Profile), MoveTemp(Revision), MoveTemp(RootUnits), MoveTemp(Targets), Candidate->ReferenceMapHash);
			Candidate->Model = FAuthoringModel::MakePublicationDirectory(*RootView, Directory);
			Candidate->Bindings = Directory; Candidate->Inventory = Directory;
			ActiveReadState->PublishedSourceHash = [Directory](const FString& Path) { return Directory->PublishedHashes.Find(Path); };
			ActiveReadState->PublishedFileHashes = &Directory->PublishedHashes;
			Candidate->FindEnvelopeSpan = [Directory](const FString& Id, FAcceptedWorkspaceByteSpan& Span) { return Directory->FindEnvelopeSpan(Id, Span); };
			return Candidate;
		}

		bool ReadDirectorySource(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FParsedSource& OutParsed,
			TArray<FInventoryRow>& OutInventory,
			FWorkspaceError& OutError,
			const TOptional<ESemanticWorkspaceMode> ExpectedMode = {},
			const bool bAllowManagedUnitEdits = false)
		{
			const double DirectoryStarted = FPlatformTime::Seconds();
			const bool bLocalEditedState = bAllowManagedUnitEdits && ExpectedMode.IsSet()
				&& ExpectedMode.GetValue() == ESemanticWorkspaceMode::State;
			FString Root =
				FPaths::ConvertRelativePathToFull(Source.GetPath());
			FPaths::NormalizeDirectoryName(Root);
			FScopedHjsonSyntaxGroups SyntaxGroups(Root);
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_source_directory_missing"), TEXT("The semantic workspace directory does not exist."),
					Root);
			}
			if (IsReparsePoint(Root))
			{
				return Fail(
					OutError,
					TEXT(
						"workspace_reparse_point_rejected"), TEXT("The semantic workspace directory is a reparse point."),
					Root);
			}
			if (ActiveReadState && !ActiveReadState->bEstablishFileObservation && bLocalEditedState
				&& !SelectWorkspaceChangedFiles(Root, OutError)) return false;
			const FString FullManifest =
				FPaths::Combine(Root, ManifestPath);
			if (!IFileManager::Get().FileExists(*FullManifest))
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_manifest_missing"), TEXT("The directory has no semantic workspace metadata; export the workspace again."),
					ManifestPath);
			}
			TSharedPtr<const FManifestIndexProof> ManifestIndex;
			if (!bLocalEditedState && ExpectedMode.IsSet() && ActiveReadState && PreviousManifestIndex && PreviousResolvedRows
				&& PreviousManifestIndex->Root == Root && PreviousResolvedRows->Root == Root
				&& PreviousResolvedRows->Mode == ExpectedMode.GetValue()
				&& PreviousResolvedRows->bManagedEdits == bAllowManagedUnitEdits
				&& PreviousResolvedRows->ReferenceMapHash == PreviousManifestIndex->ReferenceMapHash
				&& !IsReparsePoint(FullManifest))
			{
				TArray<uint8> ActualBytes;
				auto Observation = TryBeginWorkspaceFileObservation(FullManifest);
				if (FFileHelper::LoadFileToArray(ActualBytes, *FullManifest)
					&& Sha256(ActualBytes) == PreviousManifestIndex->ContentHash)
				{
					if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:manifest"), OutError)) return false;
					FUeAgentResourceChargeError ResourceError;
					if (!Resources.TryChargeMany(PreviousManifestIndex->ParseCharges, TEXT("semantic_workspace:validated_manifest"), ResourceError))
						return FailResource(OutError, ResourceError);
					ManifestIndex = PreviousManifestIndex;
					OutParsed.ManifestDocument = ManifestIndex->Header;
					auto& Cached = ActiveReadState->Documents.FindOrAdd(FullManifest);
					Cached.SourceUnit = ManifestPath;
					FString ManifestText;
					FFileHelper::BufferToString(ManifestText, ActualBytes.GetData(), ActualBytes.Num());
					Cached.ContentHash = Sha256Text(ManifestText);
					Cached.SourceCharacters = ManifestText.Len();
					if (Observation && !Observation->IsCurrent()) Observation.Reset();
					StoreObservedSourceFile(FullManifest, {Cached.ContentHash, Cached.SourceCharacters, MoveTemp(Observation),
						Cached.ContentHash == ManifestIndex->ContentHash ? FString() : ManifestIndex->ContentHash, true});
					Cached.Parsed = OutParsed.ManifestDocument;
					OutInventory = ManifestIndex->Inventory;
				}
			}
			if (!ManifestIndex && (!ReadDocument(
					FullManifest,
					ManifestPath,
					Resources,
					OutParsed.ManifestDocument,
					OutError)
				|| !OutParsed.ManifestDocument.IsValid()))
			{
				return false;
			}
			OutParsed.Manifest =
				&OutParsed.ManifestDocument->GetRoot().Get();
			FWorkspaceError Diagnostics;
			if (OutParsed.Manifest->GetKind() != EHjsonNodeKind::Object)
				return Fail(OutError, TEXT("semantic_workspace_inventory_invalid"), TEXT("The workspace metadata root must be an object."), ManifestPath);
			const bool bInventoryDocuments = OutParsed.Manifest->Get(TEXT("unit_inventory_files")) != nullptr;
			if (bLocalEditedState) {
				if (!bInventoryDocuments || !ActiveReadState)
					return FailWorkspaceDataflowHandoff(OutError, TEXT("source.edited_directory"),
						!bInventoryDocuments ? TEXT("unit_inventory_files") : TEXT("request read scope"), Root / ManifestPath,
						TEXT("edited directory/state input"), TEXT("published /4 directory and active source scope"),
						TEXT("workspace publication / source loader"));
				OutParsed.GeneratedSource = ReadPublishedDirectory(Root, OutParsed, OutError);
				if (!OutParsed.GeneratedSource) {
					if (OutError.HasErrors()) return false;
					return FailWorkspaceDataflowHandoff(OutError, TEXT("source.edited_directory"),
						TEXT("publication directory"), Root / ManifestPath, TEXT("no directory result"),
						TEXT("same-generation local row directory"), TEXT("ReadPublishedDirectory"));
				}
			}
			if (!ManifestIndex && !(bInventoryDocuments
				? (OutParsed.GeneratedSource && OutParsed.GeneratedSource->Inventory
					? ReadGeneratedInventory(Root, Resources, OutParsed, OutInventory, OutError)
					: ReadInventoryDocuments(Root, *OutParsed.Manifest, Resources, OutParsed, OutInventory, OutError))
				: ParseInventory(*OutParsed.Manifest, OutInventory, OutError)))
			{
				if (bLocalEditedState) return false;
				Diagnostics.Append(OutError);
				Diagnostics.MarkUnchecked(TEXT("inventory_units"), FString(), TEXT("invalid_inventory_rows"));
			}
			const bool bReusePriorRows = ExpectedMode.IsSet() && ActiveReadState
				&& PreviousResolvedRows && PreviousResolvedRows->Root == Root
				&& PreviousResolvedRows->Mode == ExpectedMode.GetValue()
				&& PreviousResolvedRows->bManagedEdits == bAllowManagedUnitEdits;
			if (bReusePriorRows) OutParsed.PriorRowsOwner = PreviousResolvedRows;
			if (OutParsed.ReferenceMapHash.IsEmpty()) OutParsed.ReferenceMapHash = HashReferenceMap(OutInventory);
			const bool bSameReferenceMap = bReusePriorRows
				&& PreviousResolvedRows->ReferenceMapHash == OutParsed.ReferenceMapHash;
			if (bReusePriorRows && PreviousResolvedRows->GroupIndex
				&& MatchesResolvedGroupIndex(*PreviousResolvedRows->GroupIndex, OutParsed.InventoryRanges))
				OutParsed.GroupIndex = PreviousResolvedRows->GroupIndex;
			if (!OutParsed.bSparseGeneratedInventory)
				for (const auto& Row : OutInventory) OutParsed.PublicRefs.Add(Row.Syntax->GetPublicRef(), Row.Syntax->GetUnitId());
			TMap<const FReferenceLookupProof*, bool> CheckedReferences;
			int32 InputChangedRows = 0, DependencyChangedRows = 0, ReboundRows = 0, SourceFallbackRows = 0;
			const double InventoryReadFinished = FPlatformTime::Seconds();
			OutParsed.GeneratedRows.Init(false, OutInventory.Num());
			const auto* InputMode = OutParsed.Manifest->Get(TEXT("import_mode"));
			const auto* InputStructure = OutParsed.Manifest->Get(TEXT("structural_edits"));
			if (ActiveReadState && InputMode && InputMode->GetKind() == EHjsonNodeKind::String
				&& InputMode->GetText() == TEXT("state") && InputStructure
				&& InputStructure->GetKind() == EHjsonNodeKind::Array && InputStructure->GetItems().IsEmpty()
				&& OutParsed.Manifest->Get(TEXT("unit_inventory_files"))) {
				if (OutParsed.bSparseGeneratedInventory) OutParsed.bCanonicalInputFingerprint = true;
				else {
					ArtifactPublisher::FArtifactPublishError OptionalError;
					const FString OwnerPath = Root / TEXT(".uai-export-owner.json");
					FString OwnerText, OwnerHash, ConsumerState, PublisherOwner; int64 OwnerCharacters = 0;
					const bool bObservedOwner = !IsReparsePoint(OwnerPath)
						&& ObserveSourceFile(OwnerPath, false, OwnerText, OwnerHash, OwnerCharacters);
					const auto* Observation = bObservedOwner ? ObservedSourceFiles.Find(OwnerPath) : nullptr;
					OutParsed.FingerprintOwnerProof = Observation ? Observation->Proof : FWorkspaceFileProofPtr();
					// Same ownership reader as publication-only preparation. An empty
					// requested owner is not permission to read another publisher's table.
					OutParsed.bCanonicalInputFingerprint = OutParsed.FingerprintOwnerProof && OutParsed.FingerprintOwnerProof->IsCurrent()
						&& ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), ConsumerState, PublisherOwner, OptionalError)
						&& ArtifactPublisher::TryReadPublishedWorkspaceFileHashes(Root, PublisherOwner,
							OutParsed.FingerprintPublishedHashes, OptionalError)
						&& OutParsed.FingerprintOwnerProof->IsCurrent();
				}
			}
			if (OutParsed.bCanonicalInputFingerprint && !OutParsed.bSparseGeneratedInventory) {
				OutParsed.SparseManagedPrefix = OutParsed.ManagedPaths.Num();
				OutParsed.SparseManagedSlices.SetNum(OutInventory.Num());
			}

			TSet<FString> ManagedDocumentPaths;
			TSet<FString> DocumentPointers;
			TMap<FString, FHjsonDocumentPtr> Documents;
			TMap<FString, FHjsonDocumentPtr> ContentDocuments;
			TMap<FString, bool> DocumentExistence;
			TSet<FString> FailedDocuments;
			TSet<FString> UniqueSourcePaths;
			TArray<FString> SourcePaths;
			TSet<FString> GeneratedDocuments, ExpandedDocuments;
			for (const auto& Row : OutInventory) if (!Row.Syntax->GetContentFiles().IsEmpty()) ExpandedDocuments.Add(Row.GetDocumentPath());
			if (OutParsed.GroupIndex) SourcePaths = OutParsed.GroupIndex->Documents;
			else {
				for (const FInventoryRow& Row : OutInventory) UniqueSourcePaths.Add(Row.GetDocumentPath());
				SourcePaths = UniqueSourcePaths.Array(); SourcePaths.Sort();
			}
			for (const FString& Path : SourcePaths)
			{
				FString SafePath;
				FWorkspaceError Error;
				FHjsonDocumentPtr Document;
				if (!SafeRelativePath(Path, SafePath))
				{
					Fail(Error, TEXT("workspace_path_invalid"), TEXT("A managed document path must be relative and remain inside the workspace."),
						FString(), TEXT("/"), MakeShared<FJsonValueString>(Path));
					Diagnostics.Append(Error);
					continue;
				}
				if (ActiveReadState->bEstablishFileObservation && OutParsed.GeneratedSource && !ExpandedDocuments.Contains(SafePath)
					&& !OutParsed.GeneratedSource->PendingAuthorDocuments.Contains(SafePath)) {
					const auto* Expected = OutParsed.GeneratedSource->FileHashes.Find(SafePath);
					FString Text, Hash; int64 Characters = 0;
					const FString FullPath = FPaths::Combine(Root, SafePath);
					if (Expected && !IsReparsePoint(FullPath) && ObserveSourceFile(FullPath, false, Text, Hash, Characters) && Hash == *Expected) {
						GeneratedDocuments.Add(SafePath);
						ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
						OutParsed.DocumentHashes.Add(SafePath, Hash);
						continue;
					}
				}
				if (!ReadDocument(FPaths::Combine(Root, SafePath), SafePath, Resources, Document, Error))
				{
					Diagnostics.Append(Error);
					FailedDocuments.Add(SafePath);
					continue;
				}
				Documents.Add(SafePath, Document);
				ContentDocuments.Add(SafePath, Document);
				OutParsed.UnitDocuments.Add(Document);
			}

			OutParsed.UnitDocuments.Reserve(
				OutInventory.Num());
			if (OutParsed.bCanonicalInputFingerprint && !OutParsed.bSparseGeneratedInventory) {
				// Published ownership is only a candidate. Bind it to this reader's
				// actual metadata observations before reconstructing generated input order.
				const auto& Published = OutParsed.FingerprintPublishedHashes;
				const auto* HeaderHash = Published.Find(ManifestPath);
				const auto* Header = ActiveReadState->Documents.Find(FullManifest);
				OutParsed.bCanonicalInputFingerprint = HeaderHash && Header && *HeaderHash == Header->ContentHash;
				for (const auto& Pair : OutParsed.InventoryHashes) {
					const auto* Expected = Published.Find(Pair.Key);
					OutParsed.bCanonicalInputFingerprint &= Expected && *Expected == Pair.Value;
				}
				if (OutParsed.bCanonicalInputFingerprint) for (const auto& Pair : Documents) {
					const auto* Expected = Published.Find(Pair.Key);
					const auto* Actual = ActiveReadState->Documents.Find(FPaths::Combine(Root, Pair.Key));
					if (ExpandedDocuments.Contains(Pair.Key) || !Expected || !Actual || *Expected != Actual->ContentHash)
						OutParsed.FingerprintRequiredDocuments.Add(Pair.Key);
				}
			}
			const double DocumentsReadFinished = FPlatformTime::Seconds();
			double BaselineParseSeconds = 0, BaselineReadSeconds = 0;
			double RowPathsSeconds = 0, RowExistsSeconds = 0, RowPointerSeconds = 0, RowProofSeconds = 0, RowExactEditSeconds = 0;
			OutParsed.UnitRows.Reserve(OutInventory.Num());
			auto& CurrentDocumentHashes = OutParsed.DocumentHashes;
			TMap<FString, FString> BaselineSourceTexts;
			TSet<FString> ProducedBaselineSources;
			int32 BaselineRowDocuments = 0, BaselineSpanMisses = 0;
			const auto FindProducedBaselineSpan = [&](const FInventoryRow& Row, FAcceptedWorkspaceByteSpan& Span) {
				if (!OutParsed.GeneratedSource || !OutParsed.GeneratedSource->Bindings) return false;
				const auto& Candidate = *OutParsed.GeneratedSource;
				const auto* Expected = Candidate.FileHashes.Find(Row.Syntax->GetEnvelopeRef());
				const auto* Observed = OutParsed.BaselineProofs.Find(Row.Syntax->GetEnvelopeRef());
				if (!Expected || !Observed || *Expected != Observed->ContentHash) return false;
				Span = Row.GeneratedEnvelopeSpan;
				if (!Span.IsValid() && Candidate.FindEnvelopeSpan) Candidate.FindEnvelopeSpan(Row.Syntax->GetUnitId(), Span);
				return Span.IsValid();
			};
			auto LoadBaselineRow = [&](FInventoryRow& Row) -> bool {
				if (Row.Envelope) return true;
				const FString& Path = Row.Syntax->GetEnvelopeRef();
				FAcceptedWorkspaceByteSpan Span;
				if (FindProducedBaselineSpan(Row, Span)) {
					// The cold metadata reader already owns this exact file's canonical
					// bytes and scanner spans. Consume the row without another read,
					// full envelope AST, or prefix scan per requested unit.
					if (const auto* Existing = ActiveReadState ? ActiveReadState->CapturedDocuments.Find(Root / Path) : nullptr) {
						const FString ExpectedBytes = Existing->ByteHash;
						FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
						if (!ReadCapturedEnvelopeDirectory(Root, Path, ExpectedBytes, Resources, Read, OutError, Row.Syntax->GetUnitId())) return false;
						FHjsonDocumentPtr Document;
						const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Row.Syntax->GetUnitId()),
							0, 0, Span.Begin, Span.End, Row.Syntax->GetUnitId());
						if (!TryParseGeneratedHjsonSpan(Read->Utf8, Origin, Resources, Document, OutError)) return false;
						Row.Envelope = &Document->GetRoot().Get(); OutParsed.UnitDocuments.Add(Document);
						ProducedBaselineSources.Add(Path); ++BaselineRowDocuments;
						return true;
					}
					FString* Text = BaselineSourceTexts.Find(Path);
					if (!Text) {
						FString Source, Hash; int64 Characters = 0;
						const FString FullPath = FPaths::Combine(Root, Path);
						if (IsReparsePoint(FullPath) || !ObserveSourceFile(FullPath, true, Source, Hash, Characters))
							return Fail(OutError, TEXT("workspace_source_read_failed"), TEXT("The captured baseline document could not be read."), Path);
						if (Hash != OutParsed.BaselineProofs.FindChecked(Path).ContentHash)
							return Fail(OutError, TEXT("workspace_source_changed"), TEXT("The captured baseline document changed while this workspace was being resolved."), Path);
						if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, OutError)) return false;
						if (ActiveReadState) ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
						Text = &BaselineSourceTexts.Add(Path, MoveTemp(Source));
					}
					const double Started = FPlatformTime::Seconds();
					FHjsonDocumentPtr Document;
					const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(Row.Syntax->GetUnitId()),
						0, 0, Span.Begin, Span.End, Row.Syntax->GetUnitId());
					if (!TryParseHjsonDocumentSpan(*Text, Origin, Resources, Document, OutError)) return false;
					if (Document->GetRoot()->GetKind() != EHjsonNodeKind::Object)
						return FailNode(OutError, TEXT("semantic_workspace_envelope_ref_invalid"),
							TEXT("The addressed baseline row must be an object. No whole-file reader was selected."), *Document->GetRoot(),
							MakeShared<FJsonValueString>(TEXT("object")), false);
						Row.Envelope = &Document->GetRoot().Get();
						OutParsed.UnitDocuments.Add(Document);
						ProducedBaselineSources.Add(Path); ++BaselineRowDocuments;
						BaselineParseSeconds += FPlatformTime::Seconds() - Started;
						return true;
					}
				++BaselineSpanMisses;
				return FailWorkspaceDataflowHandoff(OutError, TEXT("source.baseline_row"),
					TEXT("same-generation baseline envelope span"), Path,
					TEXT("missing or unmatched row span for ") + Row.Syntax->GetUnitId(),
					TEXT("accepted file bytes and original row address"), TEXT("machine row writer / Source publication directory"));
			};
			int32 ReusedManifestRows = 0;
			TMap<FString, FString> GroupBaselineHashes;
			if (OutParsed.GroupIndex && PreviousResolvedRows->Groups.Num() == OutParsed.InventoryRanges.Num()) {
				TBitArray<> Invalid(false, OutParsed.InventoryRanges.Num());
				const auto Invalidate = [&](const TArray<int32>& Groups) { for (const int32 Group : Groups) Invalid[Group] = true; };
				for (int32 I = 0; I < OutParsed.InventoryRanges.Num(); ++I) {
					const auto& Group = PreviousResolvedRows->Groups[I];
					Invalid[I] = !Group || OutParsed.InventoryRanges[I].Count <= 0
						|| Group->Rows.Num() != OutParsed.InventoryRanges[I].Count || Group->Nodes.Num() != Group->Rows.Num()
						|| Group->InventoryHash != OutParsed.InventoryRanges[I].ContentHash;
				}
				for (const auto& Pair : OutParsed.GroupIndex->DocumentGroups) {
					const auto* Document = Documents.Find(Pair.Key);
					const auto* Current = ActiveReadState->Documents.Find(FPaths::Combine(Root, Pair.Key));
					if (Current) CurrentDocumentHashes.Add(Pair.Key, Current->ContentHash);
					const auto* Hash = PreviousResolvedRows->DocumentHashes.Find(Pair.Key);
					const auto* Weak = PreviousResolvedRows->GroupDocuments.Find(Pair.Key);
					if (!Document || !Document->IsValid() || !Current || !Hash || *Hash != Current->ContentHash || !Weak
						|| Weak->Pin() != *Document) Invalidate(Pair.Value);
				}
				for (const auto& Pair : OutParsed.GroupIndex->BaselineGroups) {
					const FString Path = FPaths::Combine(Root, Pair.Key);
					FString Text, Hash; int64 Characters = 0;
					const auto* Prior = PreviousResolvedRows->Baselines.Find(Pair.Key);
					// This observation does not consume file/parse charges. The original
					// first-use order below still owns those charges and any diagnostic.
					if (IsReparsePoint(Path) || !ObserveSourceFile(Path, false, Text, Hash, Characters)) {
						Invalidate(Pair.Value); continue;
					}
					GroupBaselineHashes.Add(Pair.Key, Hash);
					if (!Prior || Prior->ContentHash != Hash) Invalidate(Pair.Value);
				}
				for (int32 I = 0; I < Invalid.Num(); ++I) if (!Invalid[I]) {
					const auto& Group = PreviousResolvedRows->Groups[I];
					bool bSourcesAvailable = true;
					int32 CurrentRow = OutParsed.InventoryRanges[I].First;
					for (const auto& Row : Group->Rows) {
						// Expanded input must keep its real per-row path consumption,
						// just as the generated reader does. A whole-shard cache does
						// not own that ordered source material.
						const bool bExpandedInput = OutParsed.bCanonicalInputFingerprint
							&& (ExpandedDocuments.Contains(OutInventory[CurrentRow].GetDocumentPath())
								|| OutParsed.FingerprintRequiredDocuments.Contains(OutInventory[CurrentRow].GetDocumentPath()));
						++CurrentRow;
						if (bExpandedInput || (Row->SourceDocument && !Row->SourceDocument->IsAvailable())) {
							bSourcesAvailable = false; break;
						}
					}
					if (!bSourcesAvailable) continue;
					if (ReferenceLookupSetMatches(Group->Lookups, OutParsed.PublicRefs, bSameReferenceMap, CheckedReferences))
						OutParsed.ReusedGroups.Add(OutParsed.InventoryRanges[I].First, Group);
				}
			}
			int32 GroupReadRows = 0, GeneratedEnvelopeRows = 0, GeneratedEnvelopeReaderRows = 0;
			for (int32 InventoryIndex = 0; InventoryIndex < OutInventory.Num(); ++InventoryIndex)
			{
				const int32 FingerprintStart = OutParsed.FingerprintRowPaths.Num();
				const int32 ManagedRow = InventoryIndex;
				ON_SCOPE_EXIT {
					if (OutParsed.bCanonicalInputFingerprint && !OutParsed.bSparseGeneratedInventory)
						OutParsed.SparseManagedSlices[ManagedRow] = FIntPoint(FingerprintStart, OutParsed.FingerprintRowPaths.Num() - FingerprintStart);
				};
				FInventoryRow& Row = OutInventory[InventoryIndex];
				const bool bFingerprintRow = OutParsed.bCanonicalInputFingerprint && !OutParsed.bSparseGeneratedInventory
					&& OutParsed.FingerprintRequiredDocuments.Contains(Row.GetDocumentPath());
				auto AddManagedRowPath = [&](const FString& Path) {
					if (bFingerprintRow && !Path.IsEmpty()) OutParsed.FingerprintRowPaths.Add(Path);
					if (!Path.IsEmpty() && !ManagedDocumentPaths.Contains(Path)) {
						ManagedDocumentPaths.Add(Path); OutParsed.ManagedPaths.Add(Path);
					}
				};
				// ReadGeneratedInventory already transferred this publication row's
				// exact span and ordinal. Re-querying by identity here discarded that
				// address and searched the loaded directory once per row, even before
				// an unchanged document's continue below. Only a real baseline-value
				// consumer may request a missing span in FindProducedBaselineSpan.
				if (OutParsed.GeneratedSource && Row.GeneratedEnvelopeSpan.IsValid()) {
					++GeneratedEnvelopeRows;
					if (!Row.bGeneratedProjection) ++GeneratedEnvelopeReaderRows;
					}
				if (OutParsed.GeneratedSource && GeneratedDocuments.Contains(Row.GetDocumentPath())) {
					for (const FString& Path : {Row.GetDocumentPath(), Row.Syntax->GetEnvelopeRef()})
						if (!Path.IsEmpty() && !ManagedDocumentPaths.Contains(Path)) { ManagedDocumentPaths.Add(Path); OutParsed.ManagedPaths.Add(Path); }
					OutParsed.GeneratedRows[InventoryIndex] = true;
					OutParsed.UnitRows.Add(nullptr); OutParsed.UnitEnvelopes.Add(nullptr);
					auto& Proof = OutParsed.RowProofs.AddDefaulted_GetRef(); Proof.EnvelopeSpan = Row.GeneratedEnvelopeSpan;
					OutParsed.ReusableRows.Add(nullptr);
					++ReusedManifestRows;
					continue;
				}
				if (const auto* Group = OutParsed.ReusedGroups.Find(InventoryIndex)) {
					const auto& Position = OutParsed.GroupIndex->FindPositionChecked(Row.Syntax->GetUnitId());
					for (const FString& Path : OutParsed.GroupIndex->FirstManagedPaths[Position.X]) {
						if (const auto* Hash = GroupBaselineHashes.Find(Path)) {
							if (!OutParsed.BaselineProofs.Contains(Path)) {
								if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}}, TEXT("semantic_workspace:") + Path, OutError)) return false;
								FBaselineParseProof Proof; Proof.ContentHash = *Hash;
								OutParsed.BaselineProofs.Add(Path, MoveTemp(Proof));
								ActiveReadState->HashOnlyDocuments.Add(FPaths::Combine(Root, Path), *Hash);
							}
						}
						if (!ManagedDocumentPaths.Contains(Path)) { ManagedDocumentPaths.Add(Path); OutParsed.ManagedPaths.Add(Path); }
					}
					const int32 Count = (*Group)->Rows.Num();
					OutParsed.UnitRows.Append((*Group)->Nodes);
					OutParsed.UnitEnvelopes.AddZeroed(Count);
					OutParsed.RowProofs.AddDefaulted(Count);
					OutParsed.ReusableRows.AddDefaulted(Count);
					ReusedManifestRows += Count; GroupReadRows += Count;
					InventoryIndex += Count - 1;
					continue;
				}
				OutError.Reset();
				auto ReadRow = [&]() -> bool
				{
					const double PathsStarted = FPlatformTime::Seconds();
					double ThisBaselineRead = 0;
					FString Relative;
					if (!SafeRelativePath(
							Row.GetDocumentPath(),
							Relative))
					{
						if (!EnsureInventorySource(Row, OutError)) return false;
						const auto* DocumentPath = Row.SourceNode->Get(TEXT("document_path"));
						const FString Field = DocumentPath && !DocumentPath->GetText().TrimStartAndEnd().IsEmpty() ? TEXT("document_path") : TEXT("logical_path");
						return FailField(OutError, TEXT("workspace_path_invalid"),
							TEXT("The inventory document path must be relative and remain inside the workspace."), *Row.SourceNode, Field,
							MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
					}
					if (Row.GetDocumentPath() != Relative) Row.DocumentPathOverride = Relative;
					if (FailedDocuments.Contains(Relative)) return false;
					if (bFingerprintRow && !Row.Syntax->GetEnvelopeRef().IsEmpty())
						OutParsed.FingerprintRowPaths.Add(Row.Syntax->GetEnvelopeRef());
					if (!Row.Syntax->GetEnvelopeRef().IsEmpty() && !OutParsed.BaselineProofs.Contains(Row.Syntax->GetEnvelopeRef()))
					{
						const double Started = FPlatformTime::Seconds();
						const FString Path = FPaths::Combine(Root, Row.Syntax->GetEnvelopeRef());
						if (IsReparsePoint(Path)) return Fail(OutError, TEXT("workspace_reparse_point_rejected"),
							TEXT("The baseline document or an ancestor is a reparse point."), Row.Syntax->GetEnvelopeRef());
						if (!Charge(Resources, {{EUeAgentResourceDimension::Files, 1}},
							TEXT("semantic_workspace:") + Row.Syntax->GetEnvelopeRef(), OutError)) return false;
						FString Text, Hash; int64 SourceCharacters = 0;
						if (!ObserveSourceFile(Path, false, Text, Hash, SourceCharacters)) return Fail(OutError,
							TEXT("workspace_source_read_failed"), TEXT("The captured baseline document could not be read."), Row.Syntax->GetEnvelopeRef());
						FBaselineParseProof Proof; Proof.ContentHash = Hash;
						if (ActiveReadState) ActiveReadState->HashOnlyDocuments.Add(Path, Proof.ContentHash);
						OutParsed.BaselineProofs.Add(Row.Syntax->GetEnvelopeRef(), MoveTemp(Proof));
						// A generated row in this same shard can already have declared
						// its managed path without requiring a parsed baseline. File
						// membership and the baseline read/parse proof are independent.
						if (!ManagedDocumentPaths.Contains(Row.Syntax->GetEnvelopeRef())) {
							ManagedDocumentPaths.Add(Row.Syntax->GetEnvelopeRef());
							OutParsed.ManagedPaths.Add(Row.Syntax->GetEnvelopeRef());
						}
						ThisBaselineRead = FPlatformTime::Seconds() - Started; BaselineReadSeconds += ThisBaselineRead;
					}
					for (int32 Index = 0; Index < Row.Syntax->GetContentFiles().Num(); ++Index)
					{
						const FString& ContentPath = Row.Syntax->GetContentFiles()[Index];
						FString SafePath;
						if (!SafeRelativePath(ContentPath, SafePath))
						{
							if (!EnsureInventorySource(Row, OutError)) return false;
							return FailNode(OutError, TEXT("workspace_path_invalid"),
								TEXT("The content file path must be relative and remain inside the workspace."),
								Row.SourceNode->Get(TEXT("content_files"))->GetItems()[Index].Get(),
								MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
						}
						AddManagedRowPath(SafePath);
					}
					if (!Row.GetLogicalPath().IsEmpty())
					{
						FString Logical;
						if (!SafeRelativePath(Row.GetLogicalPath(), Logical))
						{
							if (!EnsureInventorySource(Row, OutError)) return false;
							return FailField(OutError, TEXT("workspace_path_invalid"),
								TEXT("The inventory logical path must be relative and remain inside the workspace."), *Row.SourceNode, TEXT("logical_path"),
								MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
						}
						if (Row.GetLogicalPath() != Logical) Row.LogicalPathOverride = Logical;
					}
					RowPathsSeconds += FPlatformTime::Seconds() - PathsStarted - ThisBaselineRead;
					if (!OutParsed.GroupIndex && DocumentPointers.Contains(Row.GetDocumentPath() + TEXT("|") + Row.Syntax->GetUnitPointer()))
					{
						if (!EnsureInventorySource(Row, OutError)) return false;
						auto Diagnostic = HjsonNodeFailure(TEXT("semantic_workspace_unit_pointer_duplicate"),
							TEXT("More than one inventory unit points to the same document node."), TEXT("workspace_resolve"), *Row.SourceNode, nullptr, false);
						Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
						Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Row.GetDocumentPath() + TEXT("|") + Row.Syntax->GetUnitPointer());
						return FailAtNode(OutError, MoveTemp(Diagnostic), *Row.SourceNode);
					}
					if (!OutParsed.GroupIndex) DocumentPointers.Add(Row.GetDocumentPath() + TEXT("|") + Row.Syntax->GetUnitPointer());
					const FString FullPath =
						FPaths::Combine(Root, Relative);
					const double ExistsStarted = FPlatformTime::Seconds();
					const bool* Exists = DocumentExistence.Find(FullPath);
					if (!Exists) Exists = &DocumentExistence.Add(FullPath, IFileManager::Get().FileExists(*FullPath));
					const bool bDocumentExists = *Exists;
					RowExistsSeconds += FPlatformTime::Seconds() - ExistsStarted;
					if (!bDocumentExists)
					{
						if (!EnsureInventorySource(Row, OutError)) return false;
						const auto* DocumentPath = Row.SourceNode->Get(TEXT("document_path"));
						const auto* PathNode = DocumentPath && !DocumentPath->GetText().TrimStartAndEnd().IsEmpty()
							? DocumentPath : Row.SourceNode->Get(TEXT("logical_path"));
						return FailNode(OutError, TEXT("semantic_workspace_unit_missing"),
							TEXT("A document declared by the workspace inventory does not exist."), *PathNode,
							MakeShared<FJsonValueString>(TEXT("existing workspace document")));
					}
					FHjsonDocumentPtr* ExistingDocument = Documents.Find(Row.GetDocumentPath());
					if (ExistingDocument == nullptr)
					{
						FHjsonDocumentPtr Document;
						if (!ReadDocument(
								FullPath,
								Row.GetDocumentPath(),
								Resources,
								Document,
								OutError)
							|| !Document.IsValid())
						{
							return false;
						}
						Documents.Add(Row.GetDocumentPath(), Document);
						OutParsed.UnitDocuments.Add(Document);
						ExistingDocument = Documents.Find(Row.GetDocumentPath());
					}
					const double PointerStarted = FPlatformTime::Seconds();
					const FHjsonNode* Unit = (*ExistingDocument)->Find(Row.Syntax->GetUnitPointer());
					RowPointerSeconds += FPlatformTime::Seconds() - PointerStarted;
					if (Unit == nullptr
						|| Unit->GetKind()
							!= EHjsonNodeKind::Object)
					{
						if (Unit) return FailNode(OutError, TEXT("semantic_workspace_unit_document_invalid"),
							TEXT("The inventory unit pointer does not identify an object in its document."), *Unit,
							MakeShared<FJsonValueString>(TEXT("object")));
						auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_document_invalid"),
							TEXT("The inventory unit pointer does not identify an object in its document."), TEXT("workspace_resolve"));
						Diagnostic.ActualValuePresent = false;
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
						Diagnostic.Source.JsonPath = Row.Syntax->GetUnitPointer();
						OutError.SetFailure(MoveTemp(Diagnostic), Row.GetDocumentPath(), Row.Syntax->GetUnitPointer());
						OutError.Locate(*(*ExistingDocument)->GetRoot());
						return false;
					}
					// The original directory/span consumer also proves this file's
					// captured syntax. Carry it before an exact row can exit; otherwise
					// the final baseline pass would reconstruct the complete file.
					FAcceptedWorkspaceByteSpan ProducedSpan;
					if (FindProducedBaselineSpan(Row, ProducedSpan)) ProducedBaselineSources.Add(Row.Syntax->GetEnvelopeRef());
					// This is the original exact edit-node proof, before constructing a
					// captured envelope solely for a later optional row-reuse proof.
					// Real content_ref expansion still belongs to the original path.
					if (bAllowManagedUnitEdits && OutParsed.GeneratedSource
						&& Row.Syntax->GetContentFiles().IsEmpty()
						&& OutParsed.ReferenceMapHash == OutParsed.GeneratedSource->ReferenceMapHash
						&& !Row.Syntax->EditHash.IsEmpty()) {
						const double ExactStarted = FPlatformTime::Seconds();
						TArray<uint8> EditBytes; bool bHasContentReference = false;
						if (!TrySerializeEditNode(*Unit, Resources, OutError, EditBytes, &bHasContentReference)) return false;
						if (!bHasContentReference && !EditBytes.IsEmpty()) {
							FString Digest = Sha256(EditBytes);
							if (Row.Syntax->EditHash.Matches(Digest)) {
								if (const auto* Read = ActiveReadState->Documents.Find(FullPath))
									CurrentDocumentHashes.Add(Row.GetDocumentPath(), Read->ContentHash);
								OutParsed.GeneratedRows[InventoryIndex] = true;
								OutParsed.UnitRows.Add(nullptr); OutParsed.UnitEnvelopes.Add(nullptr);
								auto& Proof = OutParsed.RowProofs.AddDefaulted_GetRef(); Proof.EnvelopeSpan = Row.GeneratedEnvelopeSpan;
								OutParsed.ReusableRows.Add(nullptr); AddManagedRowPath(Row.GetDocumentPath());
								++ReusedManifestRows; ++OutParsed.EarlyExactEditRows;
								RowExactEditSeconds += FPlatformTime::Seconds() - ExactStarted;
								return true;
							}
							OutParsed.EarlyEditDigests.Add(InventoryIndex, MoveTemp(Digest));
							}
						RowExactEditSeconds += FPlatformTime::Seconds() - ExactStarted;
						}
					const double ProofStarted = FPlatformTime::Seconds();
					const auto Previous = bReusePriorRows ? PreviousResolvedRows->FindRow(Row.Syntax->GetUnitId()) : nullptr;
					const auto* Current = ActiveReadState ? ActiveReadState->Documents.Find(FullPath) : nullptr;
					FReadRowProof ReadProof;
					FString* DocumentHash = CurrentDocumentHashes.Find(Row.GetDocumentPath());
					if (!DocumentHash && Current)
						DocumentHash = &CurrentDocumentHashes.Add(Row.GetDocumentPath(), Current->ContentHash);
					const auto* PreviousDocumentHash = bReusePriorRows
						? PreviousResolvedRows->DocumentHashes.Find(Row.GetDocumentPath()) : nullptr;
					const bool bSameDocument = DocumentHash && PreviousDocumentHash && *DocumentHash == *PreviousDocumentHash;
					const auto InputDigest = [&](const FHjsonNode& Node, const FString& CurrentHash) {
						FHjsonInputDigest Cached;
						FRowInputDigest Result;
						const auto& Provenance = Node.GetProvenance();
						bool bKnownDigest = FindHjsonInputDigest(Provenance.GetSourceUnit(), CurrentHash, Provenance.GetJsonPointer(), Cached);
						if (!bKnownDigest) {
							const auto* Old = PreviousDocumentReads.Find(FullPath);
							const auto* OldNode = Old && Old->Parsed && Old->SourceUnit == Provenance.GetSourceUnit()
								? Old->Parsed->Find(Provenance.GetJsonPointer()) : nullptr;
							if (OldNode && OldNode->GetValueStorageIdentity() == Node.GetValueStorageIdentity()
								&& FindHjsonInputDigest(Old->SourceUnit, Old->ContentHash, Provenance.GetJsonPointer(), Cached)) {
								bKnownDigest = true;
								RememberHjsonInputDigest(Provenance.GetSourceUnit(), CurrentHash, Provenance.GetJsonPointer(), Cached);
								if (ActiveReadState) ++ActiveReadState->ReusedSyntaxRowDigests;
							}
						}
						if (bKnownDigest)
							FMemory::Memcpy(Result.Bytes, Cached.GetData(), sizeof(Result.Bytes));
						else {
							Result = HashRowInputNode(Node);
							FMemory::Memcpy(Cached.GetData(), Result.Bytes, sizeof(Result.Bytes));
							RememberHjsonInputDigest(Provenance.GetSourceUnit(), CurrentHash, Provenance.GetJsonPointer(), Cached);
						}
						return Result;
					};
					ReadProof.AuthoredStorage = Unit->GetValueStorageIdentity();
					ReadProof.AuthoredDocument = *ExistingDocument;
					ReadProof.Authored = Previous && Previous->AuthoredDocument.IsValid()
						&& Previous->AuthoredStorage == ReadProof.AuthoredStorage
						? Previous->AuthoredHash : InputDigest(*Unit, DocumentHash ? *DocumentHash : FString());
					const auto* PreviousBaseline = bReusePriorRows ? PreviousResolvedRows->Baselines.Find(Row.Syntax->GetEnvelopeRef()) : nullptr;
					const bool bSameBaseline = Previous && PreviousBaseline
						&& Previous->GetEnvelopeRef() == Row.Syntax->GetEnvelopeRef()
						&& PreviousBaseline->ContentHash == OutParsed.BaselineProofs.FindChecked(Row.Syntax->GetEnvelopeRef()).ContentHash;
					if (!Row.Syntax->GetEnvelopeRef().IsEmpty())
					{
						if (bSameBaseline) {
							ReadProof.Envelope = Previous->EnvelopeHash;
							ReadProof.EnvelopeSpan = Previous->EnvelopeSpan;
						}
						else {
							if (!LoadBaselineRow(Row)) return false;
							if (!Row.Envelope || Row.Envelope->GetKind() != EHjsonNodeKind::Object) {
								if (!EnsureInventorySource(Row, OutError)) return false;
								return FailField(OutError, TEXT("semantic_workspace_envelope_missing"),
									TEXT("The referenced baseline document has no object envelope for this unit identity."),
									*Row.SourceNode, TEXT("envelope_ref"));
							}
							ReadProof.Envelope = InputDigest(*Row.Envelope,
								OutParsed.BaselineProofs.FindChecked(Row.Syntax->GetEnvelopeRef()).ContentHash);
							ReadProof.EnvelopeSpan = {Row.Envelope->GetProvenance().GetByteStart(), Row.Envelope->GetProvenance().GetByteEnd()};
						}
					}
					else if (Row.Envelope) ReadProof.Envelope = HashRowInputNode(*Row.Envelope);
					if (Row.SourceRowHash.IsEmpty() && Row.SourceOrigin.IsSet() && Row.SourceUtf8) {
						const auto& Span = Row.SourceOrigin.GetValue();
						if (Span.GetByteStart() >= 0 && Span.GetByteEnd() > Span.GetByteStart() && Span.GetByteEnd() <= Row.SourceUtf8->Num())
							Row.SourceRowHash.SetFromBytes(Row.SourceUtf8->GetData() + Span.GetByteStart(), Span.GetByteEnd() - Span.GetByteStart());
					}
					ReadProof.bValid = Current && Row.Syntax->GetContentFiles().IsEmpty() && !Row.SourceRowHash.IsEmpty();
					ReadProof.Input = HashRowInputs(Row.SourceRowHash, ReadProof.Authored, ReadProof.Envelope);
					bool bReuseResolvedRow = ReadProof.bValid && Previous && Previous->Unit.IsSet()
						&& Previous->InputHash == ReadProof.Input
						&& (!Previous->Source.IsSet() || Previous->Source->GetSourceUnit().Equals(
							Unit->GetProvenance().GetSourceUnit(), ESearchCase::CaseSensitive));
					if (Previous && !bReuseResolvedRow) ++InputChangedRows;
					if (bReuseResolvedRow && !ReferenceLookupsMatch(*Previous, OutParsed.PublicRefs, bSameReferenceMap, CheckedReferences)) {
						bReuseResolvedRow = false; ++DependencyChangedRows;
					}
					if (bReuseResolvedRow) {
						if (bSameDocument) {
							ReadProof.ReboundSource = Previous->SourceDocument ? Previous->SourceDocument->Pin() : nullptr;
							bReuseResolvedRow = !Previous->SourceDocument || ReadProof.ReboundSource.IsValid();
						}
						else {
							bReuseResolvedRow = TryRebindRowSource(*Previous, *Unit, ReadProof.ReboundSource);
							if (bReuseResolvedRow) ++ReboundRows; else ++SourceFallbackRows;
						}
					}
					if (!bReuseResolvedRow && ManifestIndex
						&& !MaterializeInventoryRow(Row, *ManifestIndex, Resources, OutError)) return false;
					if (bReuseResolvedRow) ++ReusedManifestRows;
					if (!bReuseResolvedRow && !Row.Syntax->GetEnvelopeRef().IsEmpty() && !Row.Envelope) {
						if (!LoadBaselineRow(Row)) return false;
						if (!Row.Envelope || Row.Envelope->GetKind() != EHjsonNodeKind::Object)
						{
							if (!EnsureInventorySource(Row, OutError)) return false;
							return FailField(OutError, TEXT("semantic_workspace_envelope_missing"),
								TEXT("The referenced baseline document has no object envelope for this unit identity."),
								*Row.SourceNode, TEXT("envelope_ref"));
						}
					}
					RowProofSeconds += FPlatformTime::Seconds() - ProofStarted;
					if (!bReuseResolvedRow)
					{
						TSet<FString> Expanding;
						TFunction<bool(const FHjsonNodeRef&, FHjsonNodePtr&)> Expand;
						Expand = [&](const FHjsonNodeRef& Node, FHjsonNodePtr& Out) -> bool
						{
							if (Node->GetKind() == EHjsonNodeKind::Object)
							{
								const FHjsonNode* Reference = Node->Get(TEXT("content_ref"));
								if (Reference && Node->GetEntries().Num() == 1
									&& Reference->GetKind() == EHjsonNodeKind::String)
								{
									OutParsed.ContentReferenceUnits.Add(Row.Syntax->GetUnitId());
									FString Path;
									if (!SafeRelativePath(Reference->GetText(), Path))
										return FailNode(OutError, TEXT("workspace_path_invalid"),
											TEXT("The content reference must be relative and remain inside the workspace."), *Reference,
											MakeShared<FJsonValueString>(TEXT("relative workspace document path")), false);
									AddManagedRowPath(Path);
									if (Expanding.Contains(Path))
										return FailNode(OutError, TEXT("semantic_workspace_content_reference_cycle"),
											TEXT("This content reference forms a cycle."), *Reference, MakeShared<FJsonValueString>(TEXT("acyclic document reference")));
									if (FailedDocuments.Contains(Path)) return false;
									auto* ContentDocument = ContentDocuments.Find(Path);
									if (!ContentDocument)
									{
										FHjsonDocumentPtr ParsedContent;
										if (!ReadDocument(FPaths::Combine(Root, Path), Path, Resources, ParsedContent, OutError))
										{
											FailedDocuments.Add(Path);
											return false;
										}
										ContentDocuments.Add(Path, ParsedContent);
										OutParsed.UnitDocuments.Add(ParsedContent);
										ContentDocument = ContentDocuments.Find(Path);
									}
									const auto& ContentRoot = (*ContentDocument)->GetRoot();
									const auto* Content = ContentRoot->GetEntries().FindByPredicate([](const auto& Field) {
										return Field.Key == TEXT("value"); });
									if (ContentRoot->GetKind() != EHjsonNodeKind::Object)
										return FailNode(OutError, TEXT("semantic_workspace_content_value_missing"),
											TEXT("The referenced content document must be an object containing value."), *ContentRoot,
											MakeShared<FJsonValueString>(TEXT("object")));
									if (!Content) return FailField(OutError, TEXT("semantic_workspace_content_value_missing"),
										TEXT("The referenced content document requires a value field."), *ContentRoot, TEXT("value"));
									Expanding.Add(Path);
									const bool bExpanded = Expand(Content->Value, Out);
									Expanding.Remove(Path);
									return bExpanded;
								}
								bool bChildrenValid = true;
								bool bChildrenChanged = false;
								TArray<TPair<FString, FHjsonNodeRef>> Entries;
								for (int32 EntryIndex = 0; EntryIndex < Node->GetEntries().Num(); ++EntryIndex)
								{
									const auto& Field = Node->GetEntries()[EntryIndex];
									FHjsonNodePtr Value;
									if (!Expand(Field.Value, Value))
								{
									Diagnostics.Append(OutError);
									OutError.Reset();
									bChildrenValid = false;
									continue;
								}
									if (Value.Get() != &Field.Value.Get())
									{
										if (!bChildrenChanged) Entries = Node->GetEntries();
										bChildrenChanged = true;
										Entries[EntryIndex].Value = Value.ToSharedRef();
									}
								}
								if (!bChildrenValid) return false;
								if (bChildrenChanged)
									Out = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Node->GetProvenance(),
										FString(), FString(), false, TArray<FHjsonNodeRef>{}, MoveTemp(Entries));
								else Out = Node;
							}
							else if (Node->GetKind() == EHjsonNodeKind::Array)
							{
								bool bChildrenValid = true;
								bool bChildrenChanged = false;
								TArray<FHjsonNodeRef> Items;
								for (int32 ItemIndex = 0; ItemIndex < Node->GetItems().Num(); ++ItemIndex)
								{
									const auto& Item = Node->GetItems()[ItemIndex];
									FHjsonNodePtr Value;
									if (!Expand(Item, Value))
								{
									Diagnostics.Append(OutError);
									OutError.Reset();
									bChildrenValid = false;
									continue;
								}
									if (Value.Get() != &Item.Get())
									{
										if (!bChildrenChanged) Items = Node->GetItems();
										bChildrenChanged = true;
										Items[ItemIndex] = Value.ToSharedRef();
									}
								}
								if (!bChildrenValid) return false;
								if (bChildrenChanged)
									Out = MakeShared<FHjsonNode>(EHjsonNodeKind::Array, Node->GetProvenance(),
										FString(), FString(), false, MoveTemp(Items), TArray<TPair<FString, FHjsonNodeRef>>{});
								else Out = Node;
							}
							else Out = Node; // Preserve scalar lexemes, including exact integers.
							return true;
					};
					FHjsonNodePtr Expanded;
					const FHjsonNodeRef Original = MakeShared<FHjsonNode>(*Unit);
					if (!Expand(Original, Expanded)) return false;
					OutParsed.AssembledRows.Add(Expanded.ToSharedRef());
					Unit = Expanded.Get();
				}
				OutParsed.UnitRows.Add(Unit);
				OutParsed.UnitEnvelopes.Add(Row.Envelope);
				ReadProof.bValid &= !OutParsed.ContentReferenceUnits.Contains(Row.Syntax->GetUnitId());
				OutParsed.RowProofs.Add(MoveTemp(ReadProof));
				OutParsed.ReusableRows.Add(bReuseResolvedRow ? Previous : nullptr);
				AddManagedRowPath(Row.GetDocumentPath());

				return true;
				};
				if (!ReadRow())
				{
					Diagnostics.Append(OutError);
					Diagnostics.MarkUnchecked(TEXT("unit_semantics"), Row.GetDocumentPath(), TEXT("unit_or_dependency_invalid"));
				}
			}
			int32 ReusedBaselineSyntax = 0;
			const double RowsReadFinished = FPlatformTime::Seconds();
			if (!Diagnostics.HasErrors()) for (auto& Baseline : OutParsed.BaselineProofs)
			{
				if (ProducedBaselineSources.Contains(Baseline.Key)) continue;
				const auto* Prior = bReusePriorRows && PreviousResolvedRows
					? PreviousResolvedRows->Baselines.Find(Baseline.Key) : nullptr;
				if (Prior && Prior->bExact && Prior->ContentHash == Baseline.Value.ContentHash)
				{
					FUeAgentResourceChargeError ResourceError;
					if (!Resources.TryChargeMany(Prior->Charges, TEXT("semantic_workspace:validated_baseline"), ResourceError))
						return FailResource(OutError, ResourceError);
					Baseline.Value = *Prior; ++ReusedBaselineSyntax;
				}
				// No row-value consumer requested this baseline. Its actual byte
				// observation remains a dependency; a whole syntax tree would only
				// manufacture optional parse-accounting proof.
			}
			if (!OutParsed.BaselineProofs.IsEmpty())
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=baseline_documents files=%d parsed_files=%d row_documents=%d span_misses=%d reused_syntax=%d"),
					OutParsed.BaselineProofs.Num(), 0, BaselineRowDocuments, BaselineSpanMisses, ReusedBaselineSyntax);
			OutParsed.SparseManagedTail = OutParsed.ManagedPaths.Num();
			for (const TPair<FString, FHjsonDocumentPtr>& DocumentPair : Documents)
			{
				OutError.Reset();
				auto ReadInterface = [&]() -> bool
				{
					const FHjsonNode* InterfaceRef =
						DocumentPair.Value->Find(TEXT("/interface_ref"));
					if (InterfaceRef == nullptr)
					{
						return true;
					}
					if (InterfaceRef->GetKind() != EHjsonNodeKind::String)
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface_ref field must be a string."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					const FString InterfaceFileName = InterfaceRef->GetText();
					if (InterfaceFileName.IsEmpty()
						|| FPaths::GetCleanFilename(InterfaceFileName)
							!= InterfaceFileName
						|| !InterfaceFileName.EndsWith(
							TEXT(".interfaces.hjson"),
							ESearchCase::IgnoreCase))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface reference must be a filename ending in .interfaces.hjson, without directory components."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					FString InterfacePath;
					if (!SafeRelativePath(
							FPaths::Combine(
								FPaths::GetPath(DocumentPair.Key),
								InterfaceFileName),
							InterfacePath))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_ref_invalid"), TEXT("The interface reference cannot be resolved inside the workspace."),
							DocumentPair.Key,
							TEXT("/interface_ref"));
					}
					if (!IFileManager::Get().FileExists(
							*FPaths::Combine(Root, InterfacePath)))
					{
						return Fail(
							OutError,
							TEXT("semantic_workspace_interface_missing"), TEXT("The referenced interface document does not exist."),
							InterfacePath);
					}
					if (OutParsed.bCanonicalInputFingerprint && !OutParsed.bSparseGeneratedInventory
						&& OutParsed.FingerprintRequiredDocuments.Contains(DocumentPair.Key))
						OutParsed.FingerprintInterfacePaths.Add(InterfacePath);
					if (!ManagedDocumentPaths.Contains(InterfacePath))
					{
						ManagedDocumentPaths.Add(InterfacePath);
						OutParsed.ManagedPaths.Add(InterfacePath);
					}
					return true;
				};
				if (!ReadInterface()) Diagnostics.Append(OutError);
			}
			if (OutParsed.bSparseGeneratedInventory) {
				auto SparsePaths = MoveTemp(OutParsed.ManagedPaths);
				const auto InventoryOwner = OutParsed.GeneratedSource->Inventory;
				TSet<FString> Added;
				const auto Add = [&](const FString& Path) {
					if (!Path.IsEmpty() && !Added.Contains(Path)) { Added.Add(Path); OutParsed.ManagedPaths.Add(Path); }
				};
				for (int32 I = 0; I < OutParsed.SparseManagedPrefix; ++I) Add(SparsePaths[I]);
				InventoryOwner->VisitDocuments([&](const FString& Path) { Add(Path); });
				InventoryOwner->VisitContentFiles([&](const FString& Path) { Add(Path); });
				for (const auto& File : OutParsed.GeneratedSource->FileHashes)
					if (File.Key.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive)) Add(File.Key);
				// The only consumer is this path union. The row loop already wrote
				// actual paths here; unit-indexed slices and an all-unit override map
				// merely regrouped the same strings before discarding their keys.
				for (int32 I = OutParsed.SparseManagedPrefix; I < OutParsed.SparseManagedTail; ++I) Add(SparsePaths[I]);
				for (int32 I = OutParsed.SparseManagedTail; I < SparsePaths.Num(); ++I) Add(SparsePaths[I]);
			}
			if (Diagnostics.HasErrors())
			{
				Diagnostics.MarkUnchecked(TEXT("workspace_semantics"), FString(), TEXT("invalid_units"));
				OutError = MoveTemp(Diagnostics);
				return false;
			}
			// A generated model already supplies the unchanged units. Only parsed
			// overrides enter the sparse row cache; a complete legacy group index
			// and all-unit slot array have no consumer on this path.
			if (!OutParsed.GeneratedSource && !OutParsed.GroupIndex) OutParsed.GroupIndex = MakeResolvedGroupIndex(OutParsed, OutInventory);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_row_groups reused_groups=%d group_rows=%d total_rows=%d index_bytes=%lld"),
				OutParsed.ReusedGroups.Num(), GroupReadRows, OutInventory.Num(), OutParsed.GroupIndex ? OutParsed.GroupIndex->ChargeBytes : 0);
			if (bReusePriorRows)
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=manifest_index reused_rows=%d parsed_rows=%d input_changed=%d dependency_changed=%d source_rebound=%d source_fallback=%d reference_map_same=%s"),
					ReusedManifestRows, OutInventory.Num() - ReusedManifestRows, InputChangedRows, DependencyChangedRows,
					ReboundRows, SourceFallbackRows, bSameReferenceMap ? TEXT("true") : TEXT("false"));
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=directory_source rows=%d documents=%d reused_public_syntax=%d inventory_s=%.6f documents_s=%.6f rows_s=%.6f row_paths_s=%.6f row_exists_s=%.6f row_pointer_s=%.6f row_exact_edit_s=%.6f row_proof_s=%.6f baseline_read_s=%.6f baseline_parse_s=%.6f total_s=%.6f"),
				OutInventory.Num(), Documents.Num(), ActiveReadState ? ActiveReadState->ReusedPublicSyntax : 0, InventoryReadFinished - DirectoryStarted,
				DocumentsReadFinished - InventoryReadFinished, RowsReadFinished - DocumentsReadFinished,
				RowPathsSeconds, RowExistsSeconds, RowPointerSeconds, RowExactEditSeconds, RowProofSeconds, BaselineReadSeconds, BaselineParseSeconds,
				FPlatformTime::Seconds() - DirectoryStarted);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generated_envelope_handoff rows=%d reader_metadata_rows=%d retained_copy=false"),
				GeneratedEnvelopeRows, GeneratedEnvelopeReaderRows);
			if (ActiveReadState) UE_LOG(LogTemp, Display,
				TEXT("UAI import timing phase=source_syntax_update documents=%d whole_documents=%d missing_old_text=%d parsed_bytes=%lld reused_subtrees=%lld reused_row_digests=%lld"),
				ActiveReadState->UpdatedSyntaxDocuments, ActiveReadState->WholeSyntaxDocuments, ActiveReadState->MissingSyntaxText,
				ActiveReadState->UpdatedSyntaxBytes, ActiveReadState->ReusedSyntaxSubtrees, ActiveReadState->ReusedSyntaxRowDigests);
			return true;
		}

		bool ReadEmbeddedSource(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FParsedSource& OutParsed,
			TArray<FInventoryRow>& OutInventory,
			FWorkspaceError& OutError)
		{
			FString Text;
			FString SourceUnit = EmbeddedPath;
			if (Source.GetKind()
				== EWorkspaceSourceKind::Inline)
			{
				Text = Source.GetText();
				if (!Source.GetSourceUnit().IsEmpty())
				{
					SourceUnit = Source.GetSourceUnit();
				}
				if (!Charge(
						Resources,
						{{EUeAgentResourceDimension::Files, 1}},
						TEXT("semantic_workspace:") + SourceUnit,
						OutError))
				{
					return false;
				}
				if (!TryParseHjsonDocument(
						Text,
						SourceUnit,
						Resources,
						OutParsed.Envelope,
						OutError))
				{
					return false;
				}
			}
			else
			{
				const FString Path =
					FPaths::ConvertRelativePathToFull(
						Source.GetPath());
				if (!IFileManager::Get().FileExists(*Path))
				{
					return Fail(
						OutError,
						TEXT(
							"workspace_source_file_missing"), TEXT("The embedded semantic workspace file does not exist."),
						Path);
				}
				if (!ReadDocument(
						Path,
						SourceUnit,
						Resources,
						OutParsed.Envelope,
						OutError))
				{
					return false;
				}
			}
			if (!OutParsed.Envelope.IsValid())
			{
				return false;
			}
			const FHjsonNode& Root =
				OutParsed.Envelope->GetRoot().Get();
			OutParsed.Manifest =
				FindEntry(Root, TEXT("manifest"));
			const FHjsonNode* Units =
				FindEntry(Root, TEXT("units"));
			if (OutParsed.Manifest == nullptr
				|| OutParsed.Manifest->GetKind()
					!= EHjsonNodeKind::Object
				|| Units == nullptr
				|| Units->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"semantic_workspace_envelope_invalid"), TEXT("The embedded workspace requires a manifest object and a units array."),
					SourceUnit);
			}
			if (!ParseInventory(
					*OutParsed.Manifest,
					OutInventory,
					OutError))
			{
				return false;
			}
			if (OutInventory.Num() != Units->GetItems().Num())
			{
				return Fail(
					OutError,
					OutInventory.Num()
							> Units->GetItems().Num()
						? TEXT(
							"semantic_workspace_unit_missing")
						: TEXT(
							"semantic_workspace_unit_extra"), TEXT("The embedded unit count differs from the workspace inventory count."),
					SourceUnit, Units->GetProvenance().GetJsonPointer(),
					MakeShared<FJsonValueNumber>(Units->GetItems().Num()), MakeShared<FJsonValueNumber>(OutInventory.Num()));
			}
			for (int32 Index = 0;
				Index < OutInventory.Num();
				++Index)
			{
				const FString ExpectedPath =
					FString::Printf(
						TEXT("#/units/%d"),
						Index);
				if (OutInventory[Index].GetLogicalPath()
						!= ExpectedPath
					|| Units->GetItems()[Index]->GetKind()
						!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_embedded_inventory_invalid"), TEXT("An embedded unit must be an object at the array position declared by its inventory logical path."),
						SourceUnit);
				}
				OutParsed.UnitRows.Add(
					&Units->GetItems()[Index].Get());
				OutParsed.UnitEnvelopes.Add(nullptr);
				OutParsed.ManagedPaths.Add(ExpectedPath);
			}
			return true;
		}

		bool ParseRevision(
			const FHjsonNode& Manifest,
			FUeAgentStateRevision& OutRevision,
			FWorkspaceError& OutError)
		{
			const FHjsonNode* Revision =
				Manifest.Get(TEXT("base_state_revision"));
			if (Revision == nullptr
				|| Revision->GetKind()
					!= EHjsonNodeKind::Object)
			{
				return FailField(OutError, TEXT("semantic_workspace_manifest_field_invalid"),
					TEXT("Workspace metadata requires a base_state_revision object."), Manifest, TEXT("base_state_revision"),
					MakeShared<FJsonValueString>(TEXT("object")), false);
			}
			FString SourceId;
			FString Generation;
			int64 Sequence = 0;
			if (!RequiredString(
					*Revision,
					TEXT("source_id"),
					SourceId,
					OutError)
				|| !RequiredString(
					*Revision,
					TEXT("generation"),
					Generation,
					OutError)
				|| !RequiredInteger(
					*Revision,
					TEXT("sequence"),
					0,
					Sequence,
					OutError))
			{
				return false;
			}
			OutRevision = FUeAgentStateRevision(
				MoveTemp(SourceId),
				MoveTemp(Generation),
				Sequence);
			return OutRevision.IsValid()
				|| FailTypedValidation(OutError, Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_manifest_field_invalid"),
					OutRevision.GetValidationDiagnostic()), *Revision);
		}

		bool ParseStructuralEdits(
			const FHjsonNode& Manifest,
			const FAuthoringModel& Model,
			TArray<FStructuralEdit>& OutEdits,
			FWorkspaceError& OutError)
		{
			OutEdits.Reset();
			const FHjsonNode* Edits =
				Manifest.Get(TEXT("structural_edits"));
			if (Edits == nullptr
				|| Edits->GetKind()
					!= EHjsonNodeKind::Array)
			{
				return FailField(OutError, TEXT("semantic_workspace_structural_edits_invalid"),
					TEXT("Workspace metadata requires a structural_edits array."), Manifest, TEXT("structural_edits"),
					MakeShared<FJsonValueString>(TEXT("array")), false);
			}
			FWorkspaceError Diagnostics;
			TSet<FString> EditIds;
			for (const FHjsonNodeRef& Row : Edits->GetItems())
			{
				OutError.Reset();
				auto ParseEdit = [&]() -> bool
				{
				if (Row->GetKind()
					!= EHjsonNodeKind::Object)
				{
					return FailNode(OutError, TEXT("semantic_workspace_structural_edit_invalid"),
						TEXT("Each structural edit must be an object."), Row.Get(), MakeShared<FJsonValueString>(TEXT("object")), false);
				}
				FString EditId;
				FString KindText;
				FString OwnerUnitId;
				FString SubjectUnitId;
				FString ReplacementUnitId;
				FString RelationId;
				TArray<FString> OrderedUnitIds;
				if (!RequiredString(
						Row.Get(),
						TEXT("edit_id"),
						EditId,
						OutError)
					|| !RequiredString(
						Row.Get(),
						TEXT("kind"),
						KindText,
						OutError)
					|| !RequiredString(
						Row.Get(),
						TEXT("owner_unit_id"),
						OwnerUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("subject_unit_id"),
						SubjectUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("replacement_unit_id"),
						ReplacementUnitId,
						OutError)
					|| !OptionalString(
						Row.Get(),
						TEXT("relation_id"),
						RelationId,
						OutError)
					|| !ReadStringArray(
						Row.Get(),
						TEXT("ordered_unit_ids"),
						false,
						OrderedUnitIds,
						OutError))
				{
					return false;
				}
				if (EditIds.Contains(EditId))
				{
					return FailField(OutError, TEXT("semantic_workspace_structural_edit_duplicate"),
						TEXT("This structural edit identity occurs more than once."), Row.Get(), TEXT("edit_id"),
						MakeShared<FJsonValueString>(TEXT("unique edit identity")), false);
				}
				EStructuralEditKind Kind;
				if (!ParseStructuralEditKind(KindText, Kind))
				{
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (uint8 Index = 0; Index <= static_cast<uint8>(EStructuralEditKind::SetOrder); ++Index)
						Allowed.Add(MakeShared<FJsonValueString>(LexToString(static_cast<EStructuralEditKind>(Index))));
					return FailField(OutError, TEXT("semantic_workspace_structural_edit_invalid"),
						TEXT("The structural edit kind is unsupported."), Row.Get(), TEXT("kind"), MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
				}
				FStructuralEdit Edit(
					EditId,
					Kind,
					OwnerUnitId,
					SubjectUnitId,
					ReplacementUnitId,
					RelationId,
					MoveTemp(OrderedUnitIds), Row->GetProvenance());
				if (!Edit.IsValid())
				{
					return FailTypedValidation(OutError, Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_structural_edit_invalid"),
						Edit.GetValidationDiagnostic()), Row.Get());
				}
				if (Model.FindUnit(OwnerUnitId) == nullptr)
				{
					return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
						TEXT("The structural edit owner is absent from the workspace."), Row.Get(), TEXT("owner_unit_id"),
						MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
				}
				for (const auto& Reference : {TPair<FString, FString>(TEXT("subject_unit_id"), SubjectUnitId),
					TPair<FString, FString>(TEXT("replacement_unit_id"), ReplacementUnitId)})
				{
					if (!Reference.Value.IsEmpty()
						&& Model.FindUnit(Reference.Value) == nullptr)
					{
						return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
							FString::Printf(TEXT("Structural edit field '%s' refers to a unit absent from the workspace."), *Reference.Key),
							Row.Get(), Reference.Key, MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
					}
				}
				for (int32 Index = 0; Index < Edit.GetOrderedUnitIds().Num(); ++Index)
				{
					const FString& Reference = Edit.GetOrderedUnitIds()[Index];
					if (Model.FindUnit(Reference) == nullptr)
					{
						const auto* Entries = Row->Get(TEXT("ordered_unit_ids"));
						const auto* Entry = Entries && Entries->GetItems().IsValidIndex(Index) ? &Entries->GetItems()[Index].Get() : nullptr;
						if (Entry) return FailNode(OutError, TEXT("semantic_workspace_reference_unresolved"),
							TEXT("This ordered unit referenced by the structural edit is absent from the workspace."), *Entry,
							MakeShared<FJsonValueString>(TEXT("existing workspace unit")), false);
						return FailField(OutError, TEXT("semantic_workspace_reference_unresolved"),
							TEXT("An ordered unit referenced by the structural edit is absent from the workspace."), Row.Get(), TEXT("ordered_unit_ids"), nullptr, false);
					}
				}
				EditIds.Add(EditId);
				OutEdits.Add(MoveTemp(Edit));
				return true;
				};
				if (!ParseEdit())
				{
					OutError.Locate(Row->GetProvenance());
					Diagnostics.Append(OutError);
					Diagnostics.MarkUnchecked(TEXT("structural_edit"), Row->GetProvenance().GetSourceUnit(), TEXT("edit_invalid"));
				}
			}
			OutError = MoveTemp(Diagnostics);
			return !OutError.HasErrors();
		}
	}

	TSharedPtr<const TArray<uint8>> GetAcceptedGeneratedInventoryBytes(const FString& WorkspaceRoot, const FString& RelativePath)
	{
		const auto Bindings = GetAcceptedGeneratedWorkspaceBindings(WorkspaceRoot);
		if (!Bindings || !InventoryProofRoot.Equals(ActiveReadState->ResolvedRoot, ESearchCase::IgnoreCase)) return nullptr;
		const auto Proof = InventoryProofs.FindRef(RelativePath);
		const auto* AcceptedHash = FindAcceptedSemanticSourceHash(WorkspaceRoot, RelativePath);
		if (!Proof || !AcceptedHash || !Proof->ContentHash.Equals(*AcceptedHash, ESearchCase::CaseSensitive) || Proof->Rows.IsEmpty()) return nullptr;
		const auto& First = Proof->Rows[0];
		return First.bGeneratedProjection && First.Syntax->GeneratedOwner == Bindings ? First.SourceUtf8 : nullptr;
	}

	bool TryReadAcceptedSemanticSourceBytes(const FString& WorkspaceRoot, const FString& RelativePath,
		TArray<uint8>& OutBytes, FString& OutHash, FWorkspaceError& Error)
	{
		OutBytes.Reset(); OutHash.Reset();
		const FString* AcceptedHash = FindAcceptedSemanticSourceHash(WorkspaceRoot, RelativePath);
		if (!AcceptedHash) return false;
		const FString Path = FPaths::Combine(ActiveReadState->ResolvedRoot, RelativePath);
		FWorkspaceReadPathScope Scope(ActiveReadState->ResolvedRoot);
		if (!Scope.IsAvailable()) return Fail(Error, TEXT("workspace_protected_read_unavailable"),
			TEXT("The formal protected file reader is unavailable for this path. No alternate file reader was selected."), Path);
		const bool bRead = Scope.Observe(Path, [&](void* Handle, int64, int64) { return FWorkspaceReadPathScope::ReadBytes(Handle, OutBytes); });
		if (!bRead || (Scope.IsAvailable() && !Scope.IsCurrent())) return Fail(Error,
			TEXT("workspace_source_version_changed"), TEXT("An accepted source file is no longer available at its current logical path."), Path);
		OutHash = Sha256(OutBytes);
		if (OutHash.Equals(*AcceptedHash, ESearchCase::CaseSensitive))
			return !ActiveReadState->SourceProof || ActiveReadState->SourceProof->TryIncludeObservedInput(Path, *AcceptedHash, Error);
		// Accepted text may have a BOM/alternate encoding. Such input remains
		// legal, but is not already the canonical UTF-8 output being reused.
		FString Text; FFileHelper::BufferToString(Text, OutBytes.GetData(), OutBytes.Num());
		if (!Sha256Text(Text).Equals(*AcceptedHash, ESearchCase::CaseSensitive)) return Fail(Error,
			TEXT("workspace_source_version_changed"), TEXT("Workspace source changed after resolution; the current operation cannot consume or replace it."), Path);
		OutBytes.Reset(); OutHash.Reset(); return false;
	}

	bool TryReadPublishedWorkspaceBytes(const FString& WorkspaceRoot, const FString& RelativePath,
		const FString& PublishedByteHash, TArray<uint8>& OutBytes, FWorkspaceError& Error)
	{
		OutBytes.Reset();
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot); FPaths::NormalizeDirectoryName(Root);
		const FString Path = FPaths::Combine(Root, RelativePath);
		FWorkspaceReadPathScope Scope(Root);
		if (!Scope.IsAvailable()) return Fail(Error, TEXT("workspace_protected_read_unavailable"),
			TEXT("The formal protected file reader is unavailable for this path. No alternate file reader was selected."), Path);
		const bool bRead = Scope.Observe(Path, [&](void* Handle, int64, int64) { return FWorkspaceReadPathScope::ReadBytes(Handle, OutBytes); });
		if (!bRead || (Scope.IsAvailable() && !Scope.IsCurrent())) return Fail(Error,
			TEXT("workspace_published_path_changed"), TEXT("A required owned publication file is no longer available at its current logical path."), RelativePath);
		if (Sha256(OutBytes) != PublishedByteHash) return Fail(Error, TEXT("workspace_published_bytes_changed"),
			TEXT("A required generated file differs from the matching publication's owned bytes. Its contents were retained."), RelativePath);
		if (ActiveReadState && ActiveReadState->ResolvedRoot == Root && ActiveReadState->SourceProof
			&& !ActiveReadState->SourceProof->TryIncludeObservedInput(Path, PublishedByteHash, Error)) return false;
		return true;
	}

	TSharedRef<const FWorkspaceTextEncodingProof> MeasureCanonicalWorkspaceTextEncoding(const TArray<uint8>& CanonicalUtf8, const FString& ByteHash)
	{
		// UE's counting converter uses SourceLen * 2 internally. Preserve the
		// existing generic observation for lengths outside that converter range.
		if (CanonicalUtf8.Num() > MAX_int32 / 2) return ObserveWorkspaceTextEncoding(CanonicalUtf8, ByteHash);
		const TSharedRef<FWorkspaceTextEncodingProof> Result = MakeShareable(new FWorkspaceTextEncodingProof());
		Result->ByteHash = ByteHash; Result->ByteCount = CanonicalUtf8.Num();
		Result->Characters = CanonicalUtf8.IsEmpty() ? 0 : FUTF8ToTCHAR_Convert::ConvertedLength(
			reinterpret_cast<const ANSICHAR*>(CanonicalUtf8.GetData()), CanonicalUtf8.Num());
		return Result;
	}

	TSharedRef<const FWorkspaceTextEncodingProof> ObserveWorkspaceTextEncoding(const TArray<uint8>& ActualBytes, const FString& ActualBytesHash)
	{
		const TSharedRef<FWorkspaceTextEncodingProof> Result = MakeShareable(new FWorkspaceTextEncodingProof());
		Result->ByteHash = ActualBytesHash; Result->ByteCount = ActualBytes.Num();
		FString Text;
		FFileHelper::BufferToString(Text, ActualBytes.GetData(), ActualBytes.Num());
		const FTCHARToUTF8 Utf8(*Text);
		const bool bSameBytes = Utf8.Length() == ActualBytes.Num()
			&& (ActualBytes.IsEmpty() || FMemory::Memcmp(Utf8.Get(), ActualBytes.GetData(), ActualBytes.Num()) == 0);
		if (!bSameBytes) Result->DifferentTextHash = Sha256Text(Text);
		Result->Characters = Text.Len();
		return Result;
	}

	bool RememberObservedSemanticSourceFile(const FString& WorkspaceRoot, const FString& AbsolutePath,
		const TArray<uint8>& ActualBytes, const FString& ActualBytesHash, FWorkspaceFileProofPtr Proof,
	TSharedPtr<const FWorkspaceTextEncodingProof> Encoding, bool bPublishedWrite)
	{
		if (!Proof || !AbsolutePath.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase) || !Proof->IsCurrent()) return false;
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		FString Path = FPaths::ConvertRelativePathToFull(AbsolutePath);
		FPaths::NormalizeFilename(Path);
		if (!Path.StartsWith(Root / TEXT(""), ESearchCase::CaseSensitive)) return false;
		if (ObservedSourceRoot != Root) { ObservedSourceFiles.Reset(); ObservedSourceRoot = Root; }
		if (ObservedSourceFiles.Num() >= 16384 && !ObservedSourceFiles.Contains(Path)) return false;
		// Current bytes/hash and lease remain independent observations. Only the
		// deterministic decoded hash/length can come from equal prior bytes.
		if (!Encoding || !Encoding->Matches(ActualBytesHash, ActualBytes.Num()))
			Encoding = ObserveWorkspaceTextEncoding(ActualBytes, ActualBytesHash);
		if (!Proof->IsCurrent()) return false;
	StoreObservedSourceFile(Path, {Encoding->GetTextHash(), Encoding->GetCharacters(), MoveTemp(Proof),
		Encoding->GetTextHash() == ActualBytesHash ? FString() : ActualBytesHash, true}, bPublishedWrite);
		return true;
	}

	void RetireGeneratedSemanticWorkspace(const FString& WorkspaceRoot)
	{
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot); FPaths::NormalizeDirectoryName(Root);
		if (GeneratedSourceCandidate && GeneratedSourceCandidate->Root == Root) GeneratedSourceCandidate.Reset();
	}
	void RememberGeneratedSemanticWorkspace(const FString& WorkspaceRoot, FAuthoringModelRef Model,
		TSharedRef<const FResolvedWorkspaceUnitBindings> Bindings, const FExportWorkspacePlan& Plan,
		TFunction<bool(const FString&, FAcceptedWorkspaceByteSpan&)> FindEnvelopeSpan,
		TFunctionRef<void(TFunctionRef<void(const FAcceptedWorkspaceInventoryRow&)>)> VisitRows,
		const FWorkspacePublication& Publication)
	{
		const double Started = FPlatformTime::Seconds();
		auto Candidate = MakeShared<FGeneratedSourceCandidate>();
		Candidate->Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Candidate->Root);
		Candidate->Model = Model; Candidate->Bindings = Bindings;
		Candidate->Publication = Publication;
		Candidate->FindEnvelopeSpan = MoveTemp(FindEnvelopeSpan);
		if (const auto* Inventory = Bindings->GetGeneratedInventory(); Inventory
			&& Inventory->GetGeneratedLayout() && &Inventory->GetGeneratedLayout()->GetModel().Get() == &Model.Get())
			Candidate->Inventory = TSharedPtr<const FAcceptedWorkspaceInventory>(Bindings, Inventory);
		TMap<FString, const FExportFilePlan*> InventoryFiles;
		TMap<FString, TSharedPtr<FInventoryDocumentProof>> Shards;
		TMap<FString, int32> ExpectedShardRows;
		TMap<FString, TSharedPtr<const FString>> EnvelopePaths;
		TMap<FString, TSharedRef<const FString>> InventorySourceNames;
		TSet<FWorkspacePlacementStorage::FKey> ContentInputs;
		if (Candidate->Inventory) Candidate->Inventory->VisitContentFiles([&](const FString& Path) { ContentInputs.Add({&Path}); });
		for (const auto& File : Plan.GetFiles()) {
			// Keep observations for the actual source producer's documents and
			// content, rather than every generated guide. A later explicit new
			// content_ref is still observed normally by its real ReadDocument.
			if (Candidate->Inventory && !File.GetRelativePath().StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive)
				&& !Candidate->Inventory->GetGeneratedLayout()->GetPhysicalFiles().Contains(File.GetRelativePath())
				&& !ContentInputs.Contains(FWorkspacePlacementStorage::FKey{&File.GetRelativePath()})) continue;
			Candidate->FileHashes.Add(File.GetRelativePath(), File.GetContentHash());
			if (File.GetRelativePath().StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) {
				InventoryFiles.Add(File.GetRelativePath(), &File);
				if (!Candidate->Inventory) {
					InventorySourceNames.Add(File.GetRelativePath(), MakeShared<const FString>(File.GetRelativePath()));
					auto Shard = MakeShared<FInventoryDocumentProof>(); Shard->ContentHash = File.GetContentHash();
					Shards.Add(File.GetRelativePath(), MoveTemp(Shard));
				}
			}
		}
		if (Candidate->Inventory && ObservedSourceRoot == Candidate->Root) {
			const FString Prefix = Candidate->Root + TEXT("/");
			int32 RetiredNonInputs = 0;
			for (auto It = ObservedSourceFiles.CreateIterator(); It; ++It) {
				if (It.Key().StartsWith(Prefix, ESearchCase::CaseSensitive)
					&& !Candidate->FileHashes.Contains(It.Key().Mid(Prefix.Len()))) { It.RemoveCurrent(); ++RetiredNonInputs; }
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generated_source_observation_retention kept=%d retired_noninputs=%d producer=actual_layout_and_content"),
				ObservedSourceFiles.Num(), RetiredNonInputs);
		}
		// Old inline metadata continues through its reader; no parallel old/new
		// import is run. This handoff consumes the split writer's actual rows.
		if (InventoryFiles.IsEmpty()) { GeneratedSourceCandidate.Reset(); return; }
		// A publication replaces semantic/captured state, not the exact pairing
		// of an observed source text and its syntax. Keeping this existing bounded
		// last-read pairing lets the next edit use Myers even if the formatter
		// normalized that file. It is never the consumed-author baseline.
		// Retire unrelated roots/removed files instead of retaining generations.
		const FString SyntaxPrefix = Candidate->Root + TEXT("/");
		for (auto It = PreviousDocumentReads.CreateIterator(); It; ++It) {
			if (!It.Key().StartsWith(SyntaxPrefix, ESearchCase::CaseSensitive)
				|| !Candidate->FileHashes.Contains(It.Key().Mid(SyntaxPrefix.Len()))) It.RemoveCurrent();
		}
		TArray<FString> NamespacePaths; InventoryFiles.GenerateKeyArray(NamespacePaths); NamespacePaths.Sort();
		SHA256_CTX NamespaceHash; SHA256_Init(&NamespaceHash);
		AddDerivedKeyText(NamespaceHash, TEXT("generated_inventory_namespace/1"));
		for (const FString& Path : NamespacePaths) {
			AddDerivedKeyText(NamespaceHash, Path);
			AddDerivedKeyText(NamespaceHash, InventoryFiles.FindChecked(Path)->GetContentHash());
		}
		uint8 NamespaceDigest[SHA256_DIGEST_LENGTH]; SHA256_Final(NamespaceDigest, &NamespaceHash);
		for (uint8 Byte : NamespaceDigest) Candidate->ReferenceMapHash += FString::Printf(TEXT("%02x"), Byte);
		if (Candidate->Inventory) {
			InventoryProofs.Reset(); InventoryProofRoot = Candidate->Root;
			PreviousInventoryIdentities.Reset(); PreviousManifestIndex.Reset();
			PreviousResolvedRows.Reset();
			GeneratedSourceCandidate = MoveTemp(Candidate);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generated_source_handoff admitted=true rows=%d expected_rows=%d shards=0 incomplete_shards=0 retained_bytes=0 generated_ast=false materialized_rows=0 projection_fields=producer_view total_s=%.6f"),
				Model->GetUnitCount(), Model->GetUnitCount(), FPlatformTime::Seconds() - Started);
			return;
		}
		int32 Rows = 0;
		VisitRows([&](const FAcceptedWorkspaceInventoryRow& Generated) {
			const FString EditHash = Generated.ReadEditHash();
			const FString Path = Generated.EnvelopePath.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
			auto* Shard = Shards.Find(Path);
			if (!Shard) return;
			++ExpectedShardRows.FindOrAdd(Path);
			if (!Generated.InventorySpan.IsValid() || !Generated.EnvelopeSpan.IsValid()
				|| Generated.UnitPointer.IsEmpty() || EditHash.IsEmpty()) return;
			const auto* File = InventoryFiles.FindChecked(Path);
			const auto Bytes = File->GetCanonicalUtf8Identity().Pin();
			if (!Bytes || Generated.InventorySpan.End > Bytes->Num()) return;
			FInventoryRowSyntax Syntax;
			Syntax.UnitIdentity = Generated.SharedUnitIdentity ? Generated.SharedUnitIdentity
				: MakeShared<const FString, ESPMode::ThreadSafe>(Generated.UnitId);
			Syntax.UnitKind = Generated.UnitKind; Syntax.GeneratedOwner = Bindings;
			Syntax.GeneratedPublicRef = &Generated.PublicRef;
			Syntax.GeneratedLogicalPath = &Generated.LogicalPath;
			Syntax.GeneratedDocumentPath = &Generated.DocumentPath;
			Syntax.GeneratedUnitPointer = &Generated.UnitPointer;
			if (!Generated.ContentFiles.IsEmpty()) Syntax.GeneratedContentFiles = &Generated.ContentFiles;
			Syntax.ContentHash.Set(Generated.CanonicalContentHash()); Syntax.EditHash.Set(EditHash);
			auto& EnvelopePath = EnvelopePaths.FindOrAdd(Generated.EnvelopePath);
			if (!EnvelopePath) EnvelopePath = MakeShared<const FString>(Generated.EnvelopePath);
			Syntax.EnvelopeRef = EnvelopePath;
			FInventoryRow Row(MoveTemp(Syntax), nullptr, nullptr);
			Row.GeneratedInventoryOrdinal = ExpectedShardRows.FindChecked(Path) - 1;
			Row.SourceOrigin = FSourceProvenance(InventorySourceNames.FindChecked(Path), FString(),
				0, 0, Generated.InventorySpan.Begin, Generated.InventorySpan.End, FString());
			Row.SourceUtf8 = Bytes; Row.GeneratedEnvelopeSpan = Generated.EnvelopeSpan;
			Row.bGeneratedProjection = true;
			(*Shard)->Rows.Add(MoveTemp(Row)); ++Rows;
		});
		InventoryProofs.Reset(); InventoryProofRoot = Candidate->Root;
		PreviousInventoryIdentities.Reset(); PreviousManifestIndex.Reset();
		int64 RetainedBytes = 0; int32 AdmittedShards = 0, IncompleteShards = 0;
		for (auto& Pair : Shards) {
			auto& Shard = *Pair.Value;
			// Typed shard reuse needs every row, but the complete generated model
			// and current-file candidate do not depend on this optional row cache.
			// Missing writer metadata/span falls back only this shard's reader.
			if (Shard.Rows.Num() != ExpectedShardRows.FindRef(Pair.Key)) { ++IncompleteShards; continue; }
			// This same producer already supplied the complete inventory bytes.
			// Equality of that stronger digest proves both ordered tuple views;
			// unlike a tuple digest it may miss reuse on unrelated row changes.
			// Legacy parsed shards retain their original narrower tuple proofs.
			Shard.MembershipHash = Shard.ContentHash; Shard.IdentityHash = Shard.ContentHash;
			Shard.RetainedBytes = sizeof(FInventoryDocumentProof) + 128 + Pair.Key.GetAllocatedSize()
				+ Shard.ContentHash.GetAllocatedSize() + Shard.MembershipHash.GetAllocatedSize() + Shard.IdentityHash.GetAllocatedSize()
				+ InventoryFiles.FindChecked(Pair.Key)->GetCanonicalUtf8().GetAllocatedSize() + RetainedInventoryRowsBytes(Shard.Rows);
			if (Shard.RetainedBytes <= 512LL * 1024 * 1024 - RetainedBytes) {
				RetainedBytes += Shard.RetainedBytes; InventoryProofs.Add(Pair.Key, MoveTemp(Pair.Value)); ++AdmittedShards;
			}
		}
		// Replace the prior resolved generation, rather than retaining a second
		// all-row graph just to consume these already generated typed units.
		PreviousResolvedRows.Reset();
		GeneratedSourceCandidate = MoveTemp(Candidate);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generated_source_handoff admitted=true rows=%d expected_rows=%d shards=%d incomplete_shards=%d retained_bytes=%lld generated_ast=false projection_fields=borrowed total_s=%.6f"),
			Rows, Model->GetUnitCount(), AdmittedShards, IncompleteShards, RetainedBytes, FPlatformTime::Seconds() - Started);
	}

	void RememberParsedSemanticManifest(const FString& WorkspaceRoot, const FString& ContentHash,
		const FHjsonDocument& Document, const TArray<uint8>& Utf8,
		const TMap<EUeAgentResourceDimension, int64>& ParseCharges)
	{
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (Document.GetSourceUnit().StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive)) {
			FWorkspaceError Error;
			MakeInventoryDocumentProof(Root, Document.GetSourceUnit(), ContentHash, Document, Utf8, ParseCharges, Error);
			return;
		}
		if (!PreviousResolvedRows || PreviousResolvedRows->Root != Root
			|| Utf8.Num() > 512LL * 1024 * 1024) return;
		const auto* Schema = Document.GetRoot()->Get(TEXT("workspace_schema"));
		const auto* Rows = Document.GetRoot()->Get(TEXT("unit_inventory"));
		if (!Schema || !IsSupportedSemanticWorkspaceSchema(Schema->GetText()) || !Rows
			|| Rows->GetKind() != EHjsonNodeKind::Array) return;
		auto Proof = MakeShared<FManifestIndexProof>();
		FWorkspaceError Error;
		if (!ParseInventory(*Document.GetRoot(), Proof->Inventory, Error)) return;
		Proof->ReferenceMapHash = HashReferenceMap(Proof->Inventory);
		if (Proof->ReferenceMapHash != PreviousResolvedRows->ReferenceMapHash) return;
		Proof->Root = MoveTemp(Root);
		Proof->ContentHash = ContentHash;
		Proof->Utf8 = Utf8;
		Proof->ParseCharges = ParseCharges;
		TArray<TPair<FString, FHjsonNodeRef>> HeaderFields;
		for (const auto& Field : Document.GetRoot()->GetEntries())
			if (Field.Key != TEXT("unit_inventory")) HeaderFields.Add(Field);
		Proof->Header = MakeShared<FHjsonDocument>(Document.GetSourceUnit(), MakeShared<FHjsonNode>(
			EHjsonNodeKind::Object, Document.GetRoot()->GetProvenance(), FString(), FString(), false,
			TArray<FHjsonNodeRef>{}, MoveTemp(HeaderFields)));
		for (auto& Row : Proof->Inventory)
		{
			const auto& Span = Row.SourceNode->GetProvenance();
			if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) return;
			// Keep only the small index/diagnostic fields. Large captured envelopes
			// remain exact UTF-8 spans and are parsed lazily for changed rows.
			TArray<TPair<FString, FHjsonNodeRef>> Fields;
			for (const auto& Field : Row.SourceNode->GetEntries())
				if (Field.Key != TEXT("envelope")) Fields.Add(Field);
			Row.RetainedSourceNode = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Span,
				FString(), FString(), false, TArray<FHjsonNodeRef>{}, MoveTemp(Fields));
			Row.SourceNode = Row.RetainedSourceNode.Get();
			Row.Envelope = nullptr;
		}
		const int64 IndexBytes = sizeof(FManifestIndexProof) + Proof->Root.GetAllocatedSize()
			+ Proof->ContentHash.GetAllocatedSize() + Proof->ReferenceMapHash.GetAllocatedSize()
			+ Proof->Utf8.GetAllocatedSize() + Proof->ParseCharges.GetAllocatedSize() + RetainedInventoryRowsBytes(Proof->Inventory)
			+ sizeof(FHjsonDocument) + Document.GetSourceUnit().GetAllocatedSize()
			+ RetainedHjsonTreesBytes({&Proof->Header->GetRoot().Get()});
		// Legacy inline metadata is one physical cache entry. Its byte budget may
		// reject that entry, but never clears independently proven split shards.
		if (IndexBytes <= 512ll * 1024 * 1024) PreviousManifestIndex = MoveTemp(Proof);
	}


	static bool TryCaptureCurrentReadSourceProof(const FString& Root, const TArray<FString>& ManagedPaths,
		TSharedPtr<const FResolvedWorkspaceSourceProof>& OutProof, FWorkspaceError& OutError);

	bool TryRestoreCapturedSemanticUnit(const FHjsonNode& Envelope, const FSemanticUnit::FDeclarationView& Declaration,
		FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError)
	{
		OutUnit.Reset();
		const auto* Captured = Envelope.Get(TEXT("captured_values"));
		if (!Captured || Captured->GetKind() != EHjsonNodeKind::Object)
			return FailCapturedField(OutError, TEXT("semantic_workspace_captured_values_missing"),
				TEXT("The required captured unit has no values object."), Envelope, TEXT("captured_values"), MakeShared<FJsonValueString>(TEXT("object")));
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		for (const auto& Field : Captured->GetEntries()) {
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalValue(*Field.Value, Resources, Value, OutError)) return false;
			Values.Add(Field.Key, Value.ToSharedRef());
		}
		OutUnit = FSemanticUnit::FromDeclaration(Declaration, MoveTemp(Values));
		if (!OutUnit->IsValid()) {
			auto Diagnostic = OutUnit->GetValidationDiagnostic(); OutUnit.Reset();
			return FailAtNode(OutError, MoveTemp(Diagnostic), Envelope);
		}
		return true;
	}

	bool TryRestoreCapturedSemanticUnit(const FUeAgentCanonicalValue& Envelope,
		const FSemanticUnit::FDeclarationView& Declaration, TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError)
	{
		OutUnit.Reset();
		const auto* Captured = Envelope.GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("captured_values"); });
		if (!Captured || Captured->Value->GetKind() != EUeAgentValueKind::Object)
			return Fail(OutError, TEXT("semantic_workspace_captured_values_missing"),
				TEXT("The required captured unit has no values object."), Declaration.GetUnitId());
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		for (const auto& Field : Captured->Value->GetObjectItems()) Values.Add(Field.Key, Field.Value);
		OutUnit = FSemanticUnit::FromDeclaration(Declaration, MoveTemp(Values));
		if (!OutUnit->IsValid()) { OutError.SetFailure(OutUnit->GetValidationDiagnostic()); OutUnit.Reset(); return false; }
		return true;
	}
	const FSemanticUnit* ReadScopedCapturedSemanticUnit(const FResolvedWorkspaceUnitBindings& Owner, const FString& UnitId,
		TFunctionRef<bool(FUeAgentResourceTracker&, TOptional<FSemanticUnit>&, FWorkspaceError&)> Read, FWorkspaceError& OutError)
	{
		OutError.Reset();
		if (!ActiveReadState) {
			OutError.SetSourceFailure(TEXT("workspace_read_scope_unavailable"),
				TEXT("Captured values require their synchronous workspace read scope."), UnitId);
			return nullptr;
		}
		auto& Entry = ActiveReadState->CapturedRows.FindOrAdd(&Owner);
		if (!Entry) Entry = MakeUnique<FSemanticWorkspaceReadState::FCapturedRows>();
		auto& Rows = *Entry;
		if (const auto* Existing = Rows.Index.Find(UnitId)) return &Rows.Values[*Existing].GetValue();
		if (Rows.Error.HasErrors()) { OutError = Rows.Error; return nullptr; }
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		TOptional<FSemanticUnit> Unit;
		if (!Read(Resources, Unit, Rows.Error) || !Unit) { OutError = Rows.Error; return nullptr; }
		++ActiveReadState->CapturedValueRowsRead;
		const int32 Index = Rows.Values.Emplace(MoveTemp(Unit.GetValue()));
		Rows.Index.Add(UnitId, Index);
		return &Rows.Values[Index].GetValue();
	}
	const FWorkspaceError* FindScopedCapturedSemanticReadFailure(const FResolvedWorkspaceUnitBindings& Owner)
	{
		const auto* Rows = ActiveReadState ? ActiveReadState->CapturedRows.Find(&Owner) : nullptr;
		return Rows && *Rows && (*Rows)->Error.HasErrors() ? &(*Rows)->Error : nullptr;
	}
	void RecordScopedCapturedSemanticReadFailure(const FResolvedWorkspaceUnitBindings& Owner, const FWorkspaceError& Error)
	{
		if (!ActiveReadState || !Error.HasErrors()) return;
		auto& Entry = ActiveReadState->CapturedRows.FindOrAdd(&Owner);
		if (!Entry) Entry = MakeUnique<FSemanticWorkspaceReadState::FCapturedRows>();
		if (!Entry->Error.HasErrors()) Entry->Error = Error;
	}
	bool TryReadCapturedMemberNameNodes(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		const FString& RootField, FUeAgentResourceTracker& Resources, FHjsonNodePtr& Out, FWorkspaceError& Error)
	{
		Out.Reset();
		const FString Collection = Span.GetJsonPointer() + TEXT("/captured_values/") + EscapeJsonPointerSegment(RootField);
		const FString Prefix = Collection + TEXT("/");
		auto IncludePredicate = [&](const FString& Pointer, const FString&) {
			if (Pointer == Collection || Collection.StartsWith(Pointer + TEXT("/"), ESearchCase::CaseSensitive)) return true;
			if (!Pointer.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
			FString Tail = Pointer.Mid(Prefix.Len()); int32 Slash;
			if (!Tail.FindChar(TEXT('/'), Slash)) return true;
			Tail.RightChopInline(Slash + 1);
			return Tail == TEXT("unit_id") || Tail == TEXT("actor_guid") || Tail == TEXT("managed_id") || Tail == TEXT("component_name");
		};
		TFunctionRef<bool(const FString&, const FString&)> Include(IncludePredicate);
		FHjsonDocumentPtr Parsed;
		if (!TryParseGeneratedHjsonSpan(Utf8, Span, Resources, Parsed, Error, &Include)) return false;
		const auto* Values = Parsed->GetRoot()->Get(TEXT("captured_values"));
		if (Values) for (const auto& Field : Values->GetEntries()) if (Field.Key == RootField) { Out = Field.Value; break; }
		return true;
	}
	bool ReadScopedCapturedMemberNames(const FResolvedWorkspaceUnitBindings& Owner, const FString& Id, const FString& RootField,
		FUeAgentResourceTracker& Resources, FKeyedMemberNameVisitor Visitor,
		TFunctionRef<bool(FHjsonNodePtr&, FWorkspaceError&)> Read, FWorkspaceError& Error)
	{
		if (!ActiveReadState) return Fail(Error, TEXT("workspace_read_scope_unavailable"),
			TEXT("Captured names require their synchronous workspace read scope."), Id);
		auto& Entry = ActiveReadState->CapturedRows.FindOrAdd(&Owner);
		if (!Entry) Entry = MakeUnique<FSemanticWorkspaceReadState::FCapturedRows>();
		auto& Rows = *Entry;
		if (Rows.Error.HasErrors()) { Error = Rows.Error; return false; }
		const FString Key = FString::Printf(TEXT("%d:"), Id.Len()) + Id + RootField;
		FHjsonNodePtr Names;
		if (const auto* Existing = Rows.Names.Find(Key)) Names = *Existing;
		else {
			if (!Read(Names, Error)) { Rows.Error = Error; return false; }
			FUeAgentResourceChargeError Optional;
			if (Resources.TryChargeMany({{EUeAgentResourceDimension::EstimatedMemoryBytes, 64 + static_cast<int64>(Key.GetAllocatedSize())}},
				TEXT("semantic_workspace:captured_name_directory"), Optional)) Rows.Names.Add(Key, Names);
		}
		int32 Count = 0;
		const FString Empty;
		if (Names && Names->GetKind() == EHjsonNodeKind::Object) for (const auto& Member : Names->GetEntries()) {
			if (Member.Value->GetKind() != EHjsonNodeKind::Object) continue;
			const auto Field = [&](const TCHAR* Name) -> const FString& {
				const auto* Value = Member.Value->Get(Name);
				return Value && Value->GetKind() == EHjsonNodeKind::String ? Value->GetText() : Empty;
			};
			Visitor(Member.Key, Field(TEXT("unit_id")), Field(TEXT("actor_guid")), Field(TEXT("managed_id")), Field(TEXT("component_name")));
			++Count;
		}
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=keyed_baseline_names root=%s members=%d typed_value_rows=0"), *RootField, Count);
		return true;
	}
	bool TryReadPublishedCapturedMemberNames(const FString& Root, const FString& Path, const FString& Hash,
		const FString& Id, const FString& RootField, FUeAgentResourceTracker& Resources, FHjsonNodePtr& Out, FWorkspaceError& Error)
	{
		FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
		if (!ReadCapturedEnvelopeDirectory(Root, Path, Hash, Resources, Read, Error, Id)) return false;
		const auto Origin = Read->FindOrigin(Path, Id);
		if (!Origin) return Fail(Error, TEXT("semantic_workspace_captured_values_missing"),
			TEXT("The required captured row is absent from its published file."), Path);
		return TryReadCapturedMemberNameNodes(Read->Utf8, Origin.GetValue(), RootField, Resources, Out, Error);
	}
	bool TryRestoreCapturedSemanticMembers(const FUeAgentCanonicalValue& Envelope,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
	{
		const auto* Captured = Envelope.GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("captured_values"); });
		if (!Captured || Captured->Value->GetKind() != EUeAgentValueKind::Object)
			return Fail(Error, TEXT("semantic_workspace_captured_values_missing"),
				TEXT("Stage=captured.keyed_members; missing=captured_values object; producer=captured keyed envelope. No whole-unit decoder was selected."), Declaration.GetUnitId(), TEXT("/captured_values"));
		auto Values = Captured->Value->GetObjectItems();
		for (auto& Field : Values) if (Field.Key == RootField && Field.Value->GetKind() == EUeAgentValueKind::Object) {
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Members;
			for (const auto& Member : Field.Value->GetObjectItems()) if (Keys.Contains(Member.Key)) Members.Add(Member);
			FUeAgentCanonicalValuePtr Selected; FUeAgentStateSemanticError Failure;
			if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Members), Resources, Selected, Failure)) {
				Error.SetFailure(MoveTemp(Failure)); return false;
			}
			Field.Value = Selected.ToSharedRef();
		}
		FUeAgentCanonicalValuePtr SelectedValues; FUeAgentStateSemanticError Failure;
		if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Values), Resources, SelectedValues, Failure)) {
			Error.SetFailure(MoveTemp(Failure)); return false;
		}
		// The existing captured codec consumes only this values object. Keep
		// declaration/type/presence authority with that codec and its declaration.
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		Fields.Emplace(TEXT("captured_values"), SelectedValues.ToSharedRef());
		FUeAgentCanonicalValuePtr SelectedEnvelope;
		if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, SelectedEnvelope, Failure)) {
			Error.SetFailure(MoveTemp(Failure)); return false;
		}
		const bool bRead = TryRestoreCapturedSemanticUnit(*SelectedEnvelope, Declaration, Out, Error);
		if (bRead) {
			if (ActiveReadState) ++ActiveReadState->CapturedValueRowsRead;
			const auto* Members = Out.IsSet() ? Out->FindValue(RootField) : nullptr;
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=keyed_baseline_members root=%s requested=%d decoded_members=%d complete_typed_collection=false backing=canonical"),
				*RootField, Keys.Num(), Members && (*Members)->GetKind() == EUeAgentValueKind::Object ? (*Members)->GetObjectItems().Num() : 0);
		}
		return bRead;
	}
	bool TryReadCapturedSemanticMembers(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
	{
		const FString Prefix = Span.GetJsonPointer() + TEXT("/captured_values/") + EscapeJsonPointerSegment(RootField) + TEXT("/");
		TSet<FString> EscapedKeys; for (const auto& Key : Keys) EscapedKeys.Add(EscapeJsonPointerSegment(Key));
		auto IncludePredicate = [&](const FString& Pointer, const FString&) {
			if (!Pointer.StartsWith(Prefix, ESearchCase::CaseSensitive)) return true;
			FString Key = Pointer.Mid(Prefix.Len()); int32 Separator;
			if (Key.FindChar(TEXT('/'), Separator)) Key.LeftInline(Separator);
			return EscapedKeys.Contains(Key);
		};
		TFunctionRef<bool(const FString&, const FString&)> Include(IncludePredicate);
		FHjsonDocumentPtr Selected;
		if (!TryParseGeneratedHjsonSpan(Utf8, Span, Resources, Selected, Error, &Include)
			|| !TryRestoreCapturedSemanticUnit(*Selected->GetRoot(), Declaration, Resources, Out, Error)) return false;
		if (ActiveReadState) ++ActiveReadState->CapturedValueRowsRead;
		const auto* Members = Out.IsSet() ? Out->FindValue(RootField) : nullptr;
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=keyed_baseline_members root=%s requested=%d decoded_members=%d complete_typed_collection=false backing=span"),
			*RootField, Keys.Num(), Members && (*Members)->GetKind() == EUeAgentValueKind::Object ? (*Members)->GetObjectItems().Num() : 0);
		return true;
	}
	bool TryReadPublishedCapturedSemanticMembers(const FString& Root, const FString& Path, const FString& Hash,
		const FSemanticUnit::FDeclarationView& Declaration, const FString& RootField, const TSet<FString>& Keys,
		FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
	{
		FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
		if (!ReadCapturedEnvelopeDirectory(Root, Path, Hash, Resources, Read, Error, Declaration.GetUnitId())) return false;
		const auto Origin = Read->FindOrigin(Path, Declaration.GetUnitId());
		if (!Origin) return Fail(Error, TEXT("semantic_workspace_captured_values_missing"),
			TEXT("The required captured row is absent from its published file."), Path);
		return TryReadCapturedSemanticMembers(Read->Utf8, Origin.GetValue(), Declaration, RootField, Keys, Resources, Out, Error);
	}
	bool TryCapturedCanonicalContainsAnyString(const FUeAgentCanonicalValue& Envelope,
		TFunctionRef<bool(const FString&)> Matches, bool& OutMatch, FWorkspaceError& Error)
	{
		OutMatch = false;
		const auto* Captured = Envelope.GetKind() == EUeAgentValueKind::Object ? Envelope.GetObjectItems().FindByPredicate(
			[](const auto& Field) { return Field.Key == TEXT("captured_values"); }) : nullptr;
		if (!Captured || Captured->Value->GetKind() != EUeAgentValueKind::Object)
			return Fail(Error, TEXT("semantic_workspace_captured_values_missing"), TEXT("The captured reference query has no captured_values object."), FString());
		OutMatch = Captured->Value->ContainsStringMatching(Matches);
		return true;
	}

	static bool CapturedNodeContainsAnyString(const FHjsonNode& Envelope, TFunctionRef<bool(const FString&)> Matches,
		bool& OutMatch, FWorkspaceError& Error)
	{
		OutMatch = false;
		const auto* Values = Envelope.Get(TEXT("captured_values"));
		if (!Values || Values->GetKind() != EHjsonNodeKind::Object)
			return Fail(Error, TEXT("semantic_workspace_captured_values_missing"),
				TEXT("The addressed reference query has no captured_values object."), Envelope.GetProvenance().GetSourceUnit());
		TArray<const FHjsonNode*> Pending{Values};
		while (!Pending.IsEmpty()) {
			const auto* Node = Pending.Pop(EAllowShrinking::No);
			if (Node->GetKind() == EHjsonNodeKind::String && Matches(Node->GetText())) { OutMatch = true; break; }
			if (Node->GetKind() == EHjsonNodeKind::Array) for (const auto& Item : Node->GetItems()) Pending.Add(&Item.Get());
			else if (Node->GetKind() == EHjsonNodeKind::Object) for (const auto& Item : Node->GetEntries()) Pending.Add(&Item.Value.Get());
		}
		return true;
	}

	bool TryCapturedSpanContainsAnyString(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		TFunctionRef<bool(const FString&)> Matches, FUeAgentResourceTracker& Resources, bool& OutMatch, FWorkspaceError& Error)
	{
		const FString ValuesPointer = Span.GetJsonPointer() + TEXT("/captured_values");
		auto IncludePredicate = [&](const FString& Pointer, const FString&) {
			return Pointer == ValuesPointer || Pointer.StartsWith(ValuesPointer + TEXT("/"), ESearchCase::CaseSensitive)
				|| ValuesPointer.StartsWith(Pointer + TEXT("/"), ESearchCase::CaseSensitive);
		};
		TFunctionRef<bool(const FString&, const FString&)> Include(IncludePredicate);
		return TryConsumeGeneratedHjsonSpan(Utf8, Span, Resources, [&](const FHjsonNode& Envelope, FWorkspaceError& Failure) {
			return CapturedNodeContainsAnyString(Envelope, Matches, OutMatch, Failure);
		}, Error, &Include);
	}

	bool TryReadCapturedReferenceScope(TFunctionRef<bool()> Consumer, FWorkspaceError& Error)
	{
		if (!ActiveReadState) return Consumer(); // In-memory binding consumers own no disk observations.
		auto& State = *ActiveReadState;
		const bool bOuter = State.CapturedReferenceBatchDepth++ == 0;
		ON_SCOPE_EXIT { --State.CapturedReferenceBatchDepth; };
		if (!Consumer()) return false;
		if (!bOuter || !State.CapturedReferencePaths) return true;
		const double Started = FPlatformTime::Seconds();
		const bool bCurrent = State.CapturedReferencePaths->IsCurrent();
		State.CapturedReferenceRootSeconds += FPlatformTime::Seconds() - Started;
		return bCurrent || Fail(Error, TEXT("workspace_source_version_changed"),
			TEXT("The workspace root binding changed during the addressed reference selection."), State.CapturedReferenceRoot);
	}

	bool TryPublishedCapturedContainsAnyString(const FString& Root, const FString& Path, const FString& Hash,
		const FString& UnitId, TFunctionRef<bool(const FString&)> Matches, FUeAgentResourceTracker& Resources,
		bool& OutMatch, FWorkspaceError& Error)
	{
		OutMatch = false;
		if (!ActiveReadState || !ActiveReadState->PublishedEnvelopeSpan) return Fail(Error,
			TEXT("workspace_publication_row_span_missing"), TEXT("The reference query requires its accepted row-span producer."), Path);
		FAcceptedWorkspaceByteSpan Range;
		if (!ActiveReadState->PublishedEnvelopeSpan(UnitId, Range, Error)) return false;
		if (!Range.IsValid()) return Fail(Error, TEXT("workspace_publication_row_span_missing"), TEXT("The reference query has no valid addressed row."), UnitId);
		const FSourceProvenance Origin(Path, TEXT("/") + EscapeJsonPointerSegment(UnitId), 0, 0, Range.Begin, Range.End, UnitId);
		const FString FullPath = Root / Path;
		const auto* Observed = ObservedSourceFiles.Find(FullPath);
		// A current, actually observed whole-file digest already proves the row
		// backing. This is the same optional observation used by checkpoints;
		// no hash/bytes/index is manufactured from a declaration or filename.
		auto Proof = Observed && Observed->bByteHashObserved && Observed->ContentHash == Hash
			&& Observed->GetByteHash() == Hash ? Observed->Proof : FWorkspaceFileProofPtr();
		if (const auto* Read = ActiveReadState->CapturedDocuments.Find(FullPath); Read && Read->ByteHash == Hash && Read->ByteProof)
			Proof = Read->ByteProof; // This request's actual file read survives optional global memo admission.
		if (Proof) {
			TArray<uint8> Fragment;
			auto& State = *ActiveReadState;
			if (!State.CapturedReferencePaths) {
				const double Started = FPlatformTime::Seconds();
				State.CapturedReferencePaths = MakeShared<FWorkspaceReadPathScope>(Root);
				State.CapturedReferenceRoot = Root;
				State.CapturedReferenceRootSeconds += FPlatformTime::Seconds() - Started;
			}
			if (State.CapturedReferenceRoot != Root) return Fail(Error, TEXT("workspace_source_version_changed"),
				TEXT("One reference selection attempted to use two workspace roots."), Root);
			auto& Scope = *State.CapturedReferencePaths;
			bool bEligible = false;
			double LeaseSeconds = 0, IoSeconds = 0;
			const double ObserveStarted = FPlatformTime::Seconds();
			const bool bObserved = Scope.IsAvailable() && Scope.ObserveHandle(FullPath, [&](void* Handle) {
				double Started = FPlatformTime::Seconds();
				bEligible = Proof->IsCurrentHandle(Handle);
				LeaseSeconds += FPlatformTime::Seconds() - Started;
				if (!bEligible) return true; // No row read began; this file needs its real digest observation below.
				Started = FPlatformTime::Seconds();
				const bool bRead = FWorkspaceReadPathScope::ReadRange(Handle, Range.Begin, Range.End, Fragment);
				IoSeconds += FPlatformTime::Seconds() - Started;
				Started = FPlatformTime::Seconds();
				const bool bSame = Proof->IsCurrentHandle(Handle);
				LeaseSeconds += FPlatformTime::Seconds() - Started;
				return bRead && bSame;
			});
			State.CapturedReferenceObserveSeconds += FPlatformTime::Seconds() - ObserveStarted - LeaseSeconds - IoSeconds;
			State.CapturedReferenceProofSeconds += LeaseSeconds; State.CapturedReferenceIoSeconds += IoSeconds;
			bool bRootCurrent = true;
			if (bObserved && State.CapturedReferenceBatchDepth == 0) {
				const double Started = FPlatformTime::Seconds(); bRootCurrent = Scope.IsCurrent();
				State.CapturedReferenceRootSeconds += FPlatformTime::Seconds() - Started;
			}
			if (!bObserved || !bRootCurrent) return Fail(Error, TEXT("workspace_source_version_changed"),
				TEXT("The addressed reference row changed during its protected read. No larger reader was selected."), Path);
			if (bEligible) {
			ActiveReadState->HashOnlyDocuments.Add(FullPath, Hash);
			if (ActiveReadState->SourceProof && !ActiveReadState->SourceProof->TryIncludeObservedInput(FullPath, Hash, Error, Proof)) return false;
			++ActiveReadState->CapturedStringQueries; ++ActiveReadState->CapturedRangeReads;
			ActiveReadState->CapturedRangeReadBytes += Fragment.Num();
			const FString ValuesPointer = Origin.GetJsonPointer() + TEXT("/captured_values");
			auto IncludePredicate = [&](const FString& Pointer, const FString&) {
				return Pointer == ValuesPointer || Pointer.StartsWith(ValuesPointer + TEXT("/"), ESearchCase::CaseSensitive)
					|| ValuesPointer.StartsWith(Pointer + TEXT("/"), ESearchCase::CaseSensitive);
			};
			TFunctionRef<bool(const FString&, const FString&)> Include(IncludePredicate);
			double StringSeconds = 0;
			FHjsonFragmentConsumeTimes Times;
			const double SyntaxStarted = FPlatformTime::Seconds();
			const bool bConsumed = TryConsumeGeneratedHjsonFragment(Fragment, Origin, Resources, [&](const FHjsonNode& Envelope, FWorkspaceError& Failure) {
				const double Started = FPlatformTime::Seconds();
				const bool bMatch = CapturedNodeContainsAnyString(Envelope, Matches, OutMatch, Failure);
				StringSeconds += FPlatformTime::Seconds() - Started; return bMatch;
			}, Error, &Include, &Times);
			State.CapturedReferenceDecodeSeconds += Times.Decode; State.CapturedReferenceParseSeconds += Times.Parse;
			State.CapturedReferenceRetireSeconds += Times.Retire;
			State.CapturedReferenceSyntaxSeconds += FPlatformTime::Seconds() - SyntaxStarted - StringSeconds;
			State.CapturedReferenceStringSeconds += StringSeconds;
			if (bConsumed) return true;
			// Error location is the only prefix consumer. Do not retry parsing or
			// construct a typed value; preserve the real failure and absolute bytes.
			TArray<uint8> Prefix;
			if (Scope.ObserveHandle(FullPath, [&](void* Handle) {
				return Proof->IsCurrentHandle(Handle) && FWorkspaceReadPathScope::ReadRange(Handle, 0, Range.End, Prefix)
					&& Proof->IsCurrentHandle(Handle);
			})) RelocateGeneratedHjsonFragmentError(Prefix, Origin, Error);
			return false;
			}
		}
		// Without a current byte observation, the existing file-digest contract
		// needs one real observation of this required file. Only this row is
		// parsed; a row/parser failure never retries a larger syntax algorithm.
		FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
		const double WholeStarted = FPlatformTime::Seconds();
		if (!ReadCapturedEnvelopeDirectory(Root, Path, Hash, Resources, Read, Error, UnitId)) return false;
		ActiveReadState->CapturedReferenceWholeSeconds += FPlatformTime::Seconds() - WholeStarted;
		const auto Located = Read->FindOrigin(Path, UnitId);
		if (!Located) return Fail(Error, TEXT("semantic_workspace_captured_values_missing"), TEXT("The addressed reference row is absent."), Path);
		++ActiveReadState->CapturedStringQueries;
		const bool bRead = TryCapturedSpanContainsAnyString(Read->Utf8, Located.GetValue(), Matches, Resources, OutMatch, Error);
		if (Read->ByteProof && Read->ByteProof->IsCurrent()) {
			const int64 Bytes = Read->Utf8.GetAllocatedSize(); Read->Utf8.Empty();
			ActiveReadState->CapturedLiveBytes -= Bytes; ActiveReadState->CapturedRetiredBytes += Bytes;
		}
		return bRead;
	}

	bool TryReadPublishedCapturedSemanticUnit(const FString& Root, const FString& Path, const FString& ExpectedHash,
		const FSemanticUnit::FDeclarationView& Declaration, FUeAgentResourceTracker& Resources,
		TOptional<FSemanticUnit>& OutUnit, FWorkspaceError& OutError)
	{
		FSemanticWorkspaceReadState::FCapturedDocument* Read = nullptr;
		if (!ReadCapturedEnvelopeDirectory(Root, Path, ExpectedHash, Resources, Read, OutError, Declaration.GetUnitId())) return false;
		const auto Origin = Read->FindOrigin(Path, Declaration.GetUnitId());
		if (!Origin) return Fail(OutError, TEXT("semantic_workspace_captured_values_missing"),
			TEXT("The required captured row is absent from its published file."), Path);
		FHjsonDocumentPtr Document;
		if (!TryParseGeneratedHjsonSpan(Read->Utf8, Origin.GetValue(), Resources, Document, OutError)) return false;
		return TryRestoreCapturedSemanticUnit(*Document->GetRoot(), Declaration, Resources, OutUnit, OutError);
	}

	bool TryReadAcceptedSemanticSourceDocument(const FString& Root, const FString& Path,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError)
	{
		OutDocument = FindAcceptedSemanticSourceDocument(Root, Path);
		if (OutDocument) return true;
		const auto* Accepted = FindAcceptedSemanticSourceHash(Root, Path);
		if (!Accepted) return Fail(OutError, TEXT("workspace_source_not_accepted"),
			TEXT("The required document has no accepted source-byte observation."), Path);
		const FString Expected = *Accepted;
		FString Hash;
		if (!ReadDocument(FPaths::Combine(Root, Path), Path, Resources, OutDocument, OutError, &Hash)) return false;
		if (Hash != Expected) return Fail(OutError, TEXT("workspace_source_changed"),
			TEXT("The required source document changed after acceptance."), Path);
		return true;
	}

	static bool TryResolveSemanticWorkspaceInternal(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		const bool bAllowManagedUnitEdits,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError,
		TSharedPtr<const FResolvedWorkspaceSourceProof>* OutUnchangedImportProof = nullptr)
	{
		const double ResolveStarted = FPlatformTime::Seconds();
		const auto SyntaxCacheBefore = GetHjsonSyntaxCacheStats();
		const int32 FileProofsBefore = ActiveReadState ? ActiveReadState->ReusedFileObservations : 0;
		const int32 FileReadsBefore = ActiveReadState ? ActiveReadState->ActualFileReads : 0;
		OutWorkspace.Reset();
		OutError.Reset();
		if (OutUnchangedImportProof) OutUnchangedImportProof->Reset();
		FWorkspacePublicationReadFence PublicationFence;
		if (Source.GetKind() == EWorkspaceSourceKind::Directory && !PublicationFence.Begin(Source.GetPath(), OutError)) return false;
		FString ResolutionRoot;
		if (ActiveReadState && Source.GetKind() == EWorkspaceSourceKind::Directory)
		{
			ResolutionRoot = FPaths::ConvertRelativePathToFull(Source.GetPath());
			FPaths::NormalizeDirectoryName(ResolutionRoot);
			if (ObservedSourceRoot != ResolutionRoot) { ObservedSourceFiles.Reset(); ObservedSourceRoot = ResolutionRoot; }
			auto& Cached = *ActiveReadState;
			bool bReusable = Cached.ResolvedWorkspace.IsValid()
				&& Cached.ResolvedRoot.Equals(ResolutionRoot, ESearchCase::CaseSensitive)
				&& Cached.ResolvedMode == ExpectedMode
				&& Cached.bResolvedManagedEdits == bAllowManagedUnitEdits
				&& Cached.ResolvedDocuments.Contains(FPaths::Combine(ResolutionRoot, ManifestPath))
				&& IFileManager::Get().DirectoryExists(*ResolutionRoot)
				&& !IsReparsePoint(ResolutionRoot);
			if (bReusable)
			{
				for (const auto& Document : Cached.ResolvedDocuments)
				{
					const auto* Parsed = Cached.Documents.Find(Document.Key);
					FString CurrentText, CurrentHash; int64 CurrentCharacters = 0;
					if (!Parsed || Parsed->Parsed != Document.Value || IsReparsePoint(Document.Key)
						|| !ObserveSourceFile(Document.Key, false, CurrentText, CurrentHash, CurrentCharacters)
						|| Parsed->ContentHash != CurrentHash)
					{
						bReusable = false;
						break;
					}
				}
			}
			if (bReusable)
			{
				for (const auto& Document : Cached.ResolvedHashOnlyDocuments)
				{
					FString Text, Hash; int64 Characters = 0;
					if (IsReparsePoint(Document.Key) || !ObserveSourceFile(Document.Key, false, Text, Hash, Characters)
						|| Hash != Document.Value) { bReusable = false; break; }
				}
			}
			if (bReusable)
			{
				// Interface references are checked for existence by ReadDirectorySource
				// without parsing their contents. Preserve that input as well.
				for (const FString& Path : Cached.ResolvedWorkspace->GetManagedUnitPaths())
				{
					if (!IFileManager::Get().FileExists(*FPaths::Combine(ResolutionRoot, Path)))
					{
						bReusable = false;
						break;
					}
				}
			}
			if (bReusable)
			{
				if (!Charge(Resources, {{EUeAgentResourceDimension::Files,
					Cached.ResolvedDocuments.Num() + Cached.ResolvedHashOnlyDocuments.Num()}},
					TEXT("semantic_workspace:validated_resolution"), OutError)) return false;
				OutWorkspace = Cached.ResolvedWorkspace;
				OutError = Cached.ResolvedDiagnostics;
				if (!PublicationFence.End(OutError)) { OutWorkspace.Reset(); return false; }
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=semantic_resolve cached=true files=%d total_s=%.6f"),
					Cached.ResolvedDocuments.Num(), FPlatformTime::Seconds() - ResolveStarted);
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_file_observation reused=%d actual_reads=%d direct_hash_only_files_scope=%d direct_hash_only_bytes_scope=%lld"),
					Cached.ReusedFileObservations - FileProofsBefore, Cached.ActualFileReads - FileReadsBefore, Cached.DirectHashOnlyFiles, Cached.DirectHashOnlyBytes);
				return true;
			}
		}
		FParsedSource Parsed;
		TArray<FInventoryRow> Inventory;
		const bool bRead =
			Source.GetKind()
					== EWorkspaceSourceKind::Directory
				? ReadDirectorySource(
					Source,
					Resources,
					Parsed,
					Inventory,
					OutError, ExpectedMode, bAllowManagedUnitEdits)
				: ReadEmbeddedSource(
					Source,
					Resources,
					Parsed,
					Inventory,
					OutError);
		if (!bRead || Parsed.Manifest == nullptr)
		{
			return false;
		}
		const double SourceReadFinished = FPlatformTime::Seconds();

		FString Schema;
		FString ModeText;
		FString ContractId;
		FString ProfileId;
		FString BaseRevision;
		FString ExpectedFingerprint;
		int64 ContractVersion = 0;
		int64 LayoutVersion = 0;
		if (!RequiredString(
				*Parsed.Manifest,
				TEXT("workspace_schema"),
				Schema,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("import_mode"),
				ModeText,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("contract_id"),
				ContractId,
				OutError)
			|| !RequiredInteger(
				*Parsed.Manifest,
				TEXT("contract_version"),
				1,
				ContractVersion,
				OutError)
			|| !RequiredInteger(
				*Parsed.Manifest,
				TEXT("layout_version"),
				1,
				LayoutVersion,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("profile_id"),
				ProfileId,
				OutError)
			|| !RequiredString(
				*Parsed.Manifest,
				TEXT("base_revision"),
				BaseRevision,
				OutError)
			|| (Schema != GetDirectWorkspaceSchemaId() && !RequiredString(
				*Parsed.Manifest, TEXT("semantic_fingerprint"), ExpectedFingerprint, OutError)))
		{
			return false;
		}
		if (!IsSupportedSemanticWorkspaceSchema(Schema))
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_schema_unsupported"), TEXT("The semantic workspace schema is unsupported."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("workspace_schema"))->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueString>(Schema), MakeShared<FJsonValueString>(GetSemanticWorkspaceSchemaId()));
		}
		ESemanticWorkspaceMode Mode;
		if (!ParseMode(ModeText, Mode))
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_unsupported"), TEXT("The semantic workspace import mode is unsupported."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("import_mode"))->GetProvenance().GetJsonPointer(), MakeShared<FJsonValueString>(ModeText),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("state")),
					MakeShared<FJsonValueString>(TEXT("seed")), MakeShared<FJsonValueString>(TEXT("edit_bundle"))}));
		}
		if (Mode != ExpectedMode)
		{
			return Fail(
				OutError,
				TEXT(
					"semantic_workspace_mode_mismatch"), TEXT("The workspace import mode differs from the mode required by this operation."),
				Parsed.Manifest->GetProvenance()
					.GetSourceUnit(),
				Parsed.Manifest->Get(TEXT("import_mode"))->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueString>(ModeText), MakeShared<FJsonValueString>(LexToString(ExpectedMode)));
		}
		if (Inventory.Num() != Parsed.UnitRows.Num())
		{
			return Fail(
				OutError,
				Inventory.Num() > Parsed.UnitRows.Num()
					? TEXT(
						"semantic_workspace_unit_missing")
					: TEXT(
						"semantic_workspace_unit_extra"), TEXT("The resolved document count differs from the workspace inventory count."),
				Parsed.Manifest->GetProvenance().GetSourceUnit(), Parsed.Manifest->GetProvenance().GetJsonPointer(),
				MakeShared<FJsonValueNumber>(Parsed.UnitRows.Num()), MakeShared<FJsonValueNumber>(Inventory.Num()));
		}

		TArray<FSemanticUnit> Units;
		Units.Reserve(Parsed.GeneratedSource ? 8 : Parsed.UnitRows.Num());
		TArray<FString> EditedUnitIds;
		TMap<FString, TSet<FString>> ResolvedKeyedOperationRoots;
		TMap<FString, FSourceProvenance> UnitSources;
		TMap<FString, FHjsonDocumentRef> UnitSourceDocuments;
		TMap<FString, FSemanticUnit> BaselineUnits;
		TSet<FString> SeenUnitIds;
		TMap<FString, FString> PublicRefToUnitId = MoveTemp(Parsed.PublicRefs);
		if (!Parsed.bSparseGeneratedInventory && PublicRefToUnitId.IsEmpty()) for (const FInventoryRow& Row : Inventory)
			PublicRefToUnitId.Add(Row.Syntax->GetPublicRef(), Row.Syntax->GetUnitId());
		FGeneratedReferenceLookup GeneratedReferenceLookup(Parsed.bSparseGeneratedInventory
			? Parsed.GeneratedSource->Inventory.Get() : static_cast<const FAcceptedWorkspaceInventory*>(nullptr));
		const auto* GeneratedReferences = Parsed.bSparseGeneratedInventory ? &GeneratedReferenceLookup : nullptr;
		TSharedPtr<FResolvedRowsProof> NextRows;
		FReferenceProofPool ReferencePool;
		int32 ReusedSemanticRows = 0, SharedRetainedRows = 0;
		int32 RetainedSemanticRows = 0;
		int64 RetainedSemanticBytes = 0;
		FResolvedRowCost MeasuredRowCosts;
		TSet<const FString*> RetainedSourceNames;
		int64 RetainedSourceNameBytes = 0;
		TSet<const FReferenceLookupProof*> RetainedLookups;
		int64 RetainedLookupBytes = 0;
		TSet<const FString*> RetainedUnitStrings;
		int64 RetainedUnitStringBytes = 0;
		TSet<const void*> RetainedUnitContainers;
		int64 RetainedUnitContainerBytes = 0;
		TSet<const FUeAgentCanonicalValue*> RetainedCanonicalNodes;
		int64 RetainedCanonicalBytes = 0;
		TSet<const FResolvedResourceCharges*> RetainedCharges;
		int64 RetainedChargeBytes = 0;
		TSet<const FResolvedRowShape*> RetainedSourceShapes;
		int64 RetainedSourceShapeBytes = 0;
		// Shared backing is admitted with the same complete consumer group as
		// its row edges. A rejected group must not consume the next generation's
		// budget (nor leave borrowed identities for backing it no longer owns).
		TArray<TPair<const FString*, int64>> GroupSourceNames;
		TArray<TPair<const FReferenceLookupProof*, int64>> GroupLookups;
		TArray<TPair<const FString*, int64>> GroupUnitStrings;
		TArray<TPair<const void*, int64>> GroupUnitContainers;
		TArray<TPair<const FUeAgentCanonicalValue*, int64>> GroupCanonicalNodes;
		TArray<TPair<const FResolvedResourceCharges*, int64>> GroupCharges;
		TArray<TPair<const FResolvedRowShape*, int64>> GroupSourceShapes;
		// This is admission scratch, not another per-row retained pointer index.
		// A retained immutable node implies that its complete subtree was charged
		// in this candidate generation. Group rollback removes only new identities.
		TArray<const FUeAgentCanonicalValue*> PendingCanonicalBacking;
		int64 ReusedContainerMeasurements = 0, GroupLookupEdgesNotRevisited = 0;
		auto AdmitLookup = [&](const FReferenceLookupProofPtr& Lookup) {
			if (!Lookup || RetainedLookups.Contains(Lookup.Get())) return;
			RetainedLookups.Add(Lookup.Get());
			// The current ReferencePool owns a second map key only until this
			// resolver returns. Retained rows/groups own this proof and its one
			// Key/Result allocation; their pointer arrays are charged separately.
			const int64 Bytes = sizeof(FReferenceLookupProof) + 32 + Lookup->Key.GetAllocatedSize()
				+ (Lookup->Result.IsSet() ? Lookup->Result->GetAllocatedSize() : 0);
			RetainedLookupBytes += Bytes; RetainedSemanticBytes += Bytes;
			GroupLookups.Emplace(Lookup.Get(), Bytes);
		};
		auto AdmitSharedBacking = [&](const FResolvedRowProof& Row, bool bLookupsAdmitted = false) {
			if (Row.SourceDocument) {
				const auto& Shape = Row.SourceDocument->GetShape();
				if (!RetainedSourceShapes.Contains(&Shape)) {
					RetainedSourceShapes.Add(&Shape);
					const int64 Bytes = Shape.GetAllocatedSize();
					RetainedSourceShapeBytes += Bytes; RetainedSemanticBytes += Bytes;
					GroupSourceShapes.Emplace(&Shape, Bytes);
				}
			}
			if (Row.Charges && !RetainedCharges.Contains(Row.Charges.Get())) {
				RetainedCharges.Add(Row.Charges.Get());
				const int64 Bytes = sizeof(FResolvedResourceCharges) + 32 + Row.Charges->GetAllocatedSize();
				RetainedChargeBytes += Bytes; RetainedSemanticBytes += Bytes;
				GroupCharges.Emplace(Row.Charges.Get(), Bytes);
			}
			auto AdmitSourceName = [&](const FString* Name) {
				if (RetainedSourceNames.Contains(Name)) return;
				RetainedSourceNames.Add(Name);
				const int64 Bytes = sizeof(FString) + 32 + Name->GetAllocatedSize();
				RetainedSourceNameBytes += Bytes; RetainedSemanticBytes += Bytes;
				GroupSourceNames.Emplace(Name, Bytes);
			};
			if (Row.Source.IsSet()) AdmitSourceName(&Row.Source->GetSourceUnit());
			if (Row.SourceDocument) AdmitSourceName(&Row.SourceDocument->GetSharedSourceName());
			if (!bLookupsAdmitted) for (const auto& Lookup : Row.ReferenceLookups) AdmitLookup(Lookup);
			auto AdmitString = [&](const FString& Value) {
				// This exact backing is already fully charged to the accepted
				// index retained by this candidate. Rebuilt/rejected/different
				// indices cannot exempt another row's still-live string allocation.
				if (NextRows && NextRows->GroupIndex && NextRows->GroupIndex->OwnsIdentity(Value)) return;
				if (RetainedUnitStrings.Contains(&Value)) return;
				RetainedUnitStrings.Add(&Value);
				const int64 Bytes = sizeof(FString) + 32 + Value.GetAllocatedSize();
				RetainedUnitStringBytes += Bytes; RetainedSemanticBytes += Bytes;
				GroupUnitStrings.Emplace(&Value, Bytes);
			};
			if (Row.EnvelopeRef) AdmitString(*Row.EnvelopeRef);
			if (Row.Unit.IsSet()) Row.Unit->VisitSharedStorageStrings(AdmitString);
			if (Row.Baseline.IsSet() && (!Row.Unit.IsSet() || !Row.Unit->SharesStorageWith(Row.Baseline.GetValue())))
				Row.Baseline->VisitSharedStorageStrings(AdmitString);
			auto AdmitContainer = [&](const void* Identity, int64 Bytes) {
				if (RetainedUnitContainers.Contains(Identity)) return;
				RetainedUnitContainers.Add(Identity);
				RetainedUnitContainerBytes += Bytes; RetainedSemanticBytes += Bytes;
				GroupUnitContainers.Emplace(Identity, Bytes);
			};
			auto NeedsContainerMeasurement = [&](const void* Identity) {
				if (!RetainedUnitContainers.Contains(Identity)) return true;
				++ReusedContainerMeasurements;
				return false;
			};
			if (Row.Unit.IsSet()) Row.Unit->VisitSharedStorageContainers(NeedsContainerMeasurement, AdmitContainer);
			if (Row.Baseline.IsSet() && (!Row.Unit.IsSet() || !Row.Unit->SharesStorageWith(Row.Baseline.GetValue())))
				Row.Baseline->VisitSharedStorageContainers(NeedsContainerMeasurement, AdmitContainer);
			PendingCanonicalBacking.Reset();
			auto AddCanonicalRoots = [&](const FSemanticUnit& Unit) {
				Unit.VisitValues([&](const FString&, const FUeAgentCanonicalValueRef& Value) { PendingCanonicalBacking.Add(&Value.Get()); return true; });
			};
			if (Row.Unit.IsSet()) AddCanonicalRoots(Row.Unit.GetValue());
			if (Row.Baseline.IsSet() && (!Row.Unit.IsSet() || !Row.Unit->SharesStorageWith(Row.Baseline.GetValue())))
				AddCanonicalRoots(Row.Baseline.GetValue());
			while (!PendingCanonicalBacking.IsEmpty()) {
				const auto* Value = PendingCanonicalBacking.Pop(EAllowShrinking::No);
				if (RetainedCanonicalNodes.Contains(Value)) continue;
				RetainedCanonicalNodes.Add(Value);
				const int64 Bytes = RetainedCanonicalNodeBytes(*Value);
				RetainedCanonicalBytes += Bytes; RetainedSemanticBytes += Bytes;
				GroupCanonicalNodes.Emplace(Value, Bytes);
				for (const auto& Child : Value->GetArrayItems()) PendingCanonicalBacking.Add(&Child.Get());
				for (const auto& Child : Value->GetObjectItems()) PendingCanonicalBacking.Add(&Child.Value.Get());
			}
		};
		int32 BudgetRejectedRows = 0;
		TSet<FString> AdmittedProofSourceUnits;
		TMap<FString, FCompletedDocumentRead> NextDocumentReads;
		const int64 SyntaxBudget = FMath::Max<int64>(0, SourceVersionRetentionBudget - PublishedComparisonBytes);
		int64 RetainedSyntaxBytes = 0;
		int64 CandidateSyntaxBytes = 0;
		int32 RejectedSyntaxDocuments = 0;
		int32 RetainedSyntaxGroups = 0;
		const double SyntaxAdmissionStarted = FPlatformTime::Seconds();
		if (ActiveReadState && !ResolutionRoot.IsEmpty())
		{
			// The previous character cap bounded input text, not the retained AST.
			// Measure each immutable parsed tree once, preserve whole-document
			// provenance, and locally decline only optional next-call syntax proof.
			// A generated/group hit intentionally did not ask for syntax. That is
			// not evidence that its last exact text/tree pairing became invalid.
			// Admit it through the same capacity owner, without re-reading it or
			// requiring its old hash to equal the current publication's hash.
			struct FSyntaxCandidate { const FString* Path; const FSemanticWorkspaceReadState::FDocument* Document; int64 Bytes; bool bCurrentSyntax; };
			TArray<FSyntaxCandidate> SyntaxCandidates;
			TMap<FString, int32> CandidateBySource;
			auto AddSyntaxCandidate = [&](const FString& Path, FSemanticWorkspaceReadState::FDocument& Document) {
				if (!Path.StartsWith(ResolutionRoot + TEXT("/"), ESearchCase::CaseSensitive)
					|| Document.SourceUnit.StartsWith(TEXT(".uai/"), ESearchCase::CaseSensitive)
					|| Parsed.BaselineProofs.Contains(Document.SourceUnit) || !Document.Parsed
					|| !Document.bExactParseCharges) return;
				if (Document.RetainedSyntaxBytes <= 0) {
					TSet<const FString*> SourceNames;
					TArray<const FHjsonNode*> Roots{&Document.Parsed->GetRoot().Get()};
					if (Document.DiffBaseDocument) Roots.Add(&Document.DiffBaseDocument->GetRoot().Get());
					int64 Bytes = sizeof(FHjsonDocument) + 32 + Document.Parsed->GetOwnedSourceUnitAllocatedSize()
						+ RetainedHjsonTreesBytes(Roots, &SourceNames);
					for (const FString* Name : SourceNames) Bytes += sizeof(FString) + 32 + Name->GetAllocatedSize();
					Document.RetainedSyntaxBytes = Bytes;
				}
				const int64 Bytes = Document.RetainedSyntaxBytes + Path.GetAllocatedSize()
					+ Document.SourceUnit.GetAllocatedSize() + Document.ContentHash.GetAllocatedSize()
					+ Document.ParseCharges.GetAllocatedSize()
					+ Document.DiffBaseHash.GetAllocatedSize() + Document.Changes.GetAllocatedSize() + KeyedDocumentMetadataBytes(Document)
					+ (Document.Text ? sizeof(FString) + 32 + Document.Text->GetAllocatedSize() : 0);
				const auto* ActualHash = Parsed.DocumentHashes.Find(Document.SourceUnit);
				const bool bCurrentSyntax = ActualHash && *ActualHash == Document.ContentHash;
				if (bCurrentSyntax) CandidateBySource.Add(Document.SourceUnit, SyntaxCandidates.Num());
				SyntaxCandidates.Add({&Path, &Document, Bytes, bCurrentSyntax});
				CandidateSyntaxBytes += Bytes;
			};
			for (auto& Document : ActiveReadState->Documents) AddSyntaxCandidate(Document.Key, Document.Value);
			for (auto& Previous : PreviousDocumentReads) {
				if (!ActiveReadState->Documents.Contains(Previous.Key)
					&& ActiveReadState->HashOnlyDocuments.Contains(Previous.Key)
					&& Parsed.DocumentHashes.Contains(Previous.Value.SourceUnit))
					AddSyntaxCandidate(Previous.Key, Previous.Value);
			}
			TBitArray<> AdmittedSyntax(false, SyntaxCandidates.Num());
			auto AdmitSyntax = [&](TConstArrayView<int32> Candidates) {
				int64 Extra = 0;
				for (const int32 Candidate : Candidates) if (!AdmittedSyntax[Candidate]) {
					const int64 Bytes = SyntaxCandidates[Candidate].Bytes;
					if (Bytes > SyntaxBudget - RetainedSyntaxBytes - static_cast<int64>(NextDocumentReads.GetAllocatedSize()) - Extra) return false;
					Extra += Bytes;
				}
				TArray<int32> Added;
				for (const int32 Candidate : Candidates) if (!AdmittedSyntax[Candidate]) {
					const auto& Item = SyntaxCandidates[Candidate]; const auto& Document = *Item.Document;
					NextDocumentReads.Add(*Item.Path, Document);
					Added.Add(Candidate);
				}
				if (RetainedSyntaxBytes + Extra + static_cast<int64>(NextDocumentReads.GetAllocatedSize()) > SyntaxBudget) {
					for (const int32 Candidate : Added) NextDocumentReads.Remove(*SyntaxCandidates[Candidate].Path);
					NextDocumentReads.Shrink(); return false;
				}
				RetainedSyntaxBytes += Extra;
				for (const int32 Candidate : Added) {
					AdmittedSyntax[Candidate] = true;
					if (SyntaxCandidates[Candidate].bCurrentSyntax)
						AdmittedProofSourceUnits.Add(SyntaxCandidates[Candidate].Document->SourceUnit);
				}
				return true;
			};
			// Syntax and typed rows share this existing consumer boundary, but not
			// their proofs. Admit the complete document dependency set of a shard
			// before optional leftovers, so local AST pressure cannot scatter a
			// missing document through every otherwise reusable semantic group.
			if (Parsed.GroupIndex) {
				TArray<TArray<int32>> GroupDocuments; GroupDocuments.SetNum(Parsed.GroupIndex->Shards.Num());
				TBitArray<> Complete(true, GroupDocuments.Num());
				for (const auto& Pair : Parsed.GroupIndex->DocumentGroups) {
					const int32* Candidate = CandidateBySource.Find(Pair.Key);
					for (const int32 Group : Pair.Value) {
						if (Candidate) GroupDocuments[Group].Add(*Candidate); else Complete[Group] = false;
					}
				}
				for (int32 Group = 0; Group < GroupDocuments.Num(); ++Group)
					if (Complete[Group] && !GroupDocuments[Group].IsEmpty() && AdmitSyntax(GroupDocuments[Group])) ++RetainedSyntaxGroups;
			}
			for (int32 Candidate = 0; Candidate < SyntaxCandidates.Num(); ++Candidate)
				if (!AdmittedSyntax[Candidate]) AdmitSyntax(MakeArrayView(&Candidate, 1));
			RejectedSyntaxDocuments = SyntaxCandidates.Num() - NextDocumentReads.Num();
			const auto* Manifest = ActiveReadState->Documents.Find(FPaths::Combine(ResolutionRoot, ManifestPath));
			if (Manifest)
			{
				NextRows = MakeShared<FResolvedRowsProof>();
				NextRows->Root = ResolutionRoot;
				NextRows->Mode = ExpectedMode;
				NextRows->bManagedEdits = bAllowManagedUnitEdits;
				NextRows->ReferenceMapHash = Parsed.ReferenceMapHash.IsEmpty()
					? HashReferenceMap(Inventory) : Parsed.ReferenceMapHash;
				NextRows->Baselines = Parsed.BaselineProofs;
				NextRows->DocumentHashes = Parsed.DocumentHashes;
				NextRows->GroupIndex = Parsed.GroupIndex;
				NextRows->InitializeSparseRows();
				NextRows->GroupChargeBytes = Parsed.GroupIndex ? Parsed.GroupIndex->ChargeBytes : 0;
				if (Parsed.GroupIndex) NextRows->Groups.SetNum(Parsed.GroupIndex->Shards.Num());
			}
		}
		const double SyntaxAdmissionSeconds = FPlatformTime::Seconds() - SyntaxAdmissionStarted;
		FWorkspaceError UnitDiagnostics;
		TBitArray<> ConsumedGroups(false, Parsed.GroupIndex ? Parsed.GroupIndex->Shards.Num() : 0);
		int32 ReusedSemanticGroups = 0, ReusedGroupRows = 0, ExactGeneratedEditRows = Parsed.EarlyExactEditRows;
		double GroupAvailabilitySeconds = 0, GroupBackingSeconds = 0;
		int32 AdmissionShard = 0;
		double UnitParseSeconds = 0, RowHashSeconds = 0, EditHashSeconds = 0, BaselineHashSeconds = 0;
		SHA256_CTX DerivedScopeHash; SHA256_Init(&DerivedScopeHash);
		const int64 DerivedContext[] = {DerivedRowVersion, sizeof(TCHAR), ContractVersion, LayoutVersion,
			static_cast<int64>(ExpectedMode), bAllowManagedUnitEdits ? 1 : 0};
		SHA256_Update(&DerivedScopeHash, DerivedContext, sizeof(DerivedContext));
		AddDerivedKeyText(DerivedScopeHash, ResolutionRoot); AddDerivedKeyText(DerivedScopeHash, ContractId);
		AddDerivedKeyText(DerivedScopeHash, ProfileId); AddDerivedKeyText(DerivedScopeHash, Schema);
		FRowInputDigest DerivedScope; SHA256_Final(DerivedScope.Bytes, &DerivedScopeHash);
		FDerivedRowMaterials DerivedMaterials(FDerivedRowMaterial::Text(DerivedScope.Bytes));
		for (int32 Index = 0;
			Index < Parsed.UnitRows.Num();
			++Index)
		{
			OutError.Reset();
			if (Parsed.GeneratedSource && Parsed.GeneratedRows.IsValidIndex(Index) && Parsed.GeneratedRows[Index]) {
				// Exact current metadata/public bytes bind this already generated unit.
				// Keep its immutable backing in the producer model; ordinary fields
				// only need the changed rows, not a second complete Units array.
				++ReusedSemanticRows;
				continue;
			}
			FString ObservedEditDigest;
			if (bAllowManagedUnitEdits && Parsed.GeneratedSource && Parsed.GeneratedRows.IsValidIndex(Index)
				&& Parsed.UnitRows[Index] && Parsed.ReferenceMapHash == Parsed.GeneratedSource->ReferenceMapHash
				&& !Inventory[Index].Syntax->EditHash.IsEmpty()) {
				// Same representation as the existing edit_hash producer/consumer:
				// the entire projected edit node, including its readonly type fields.
				// Exact equality needs no typed value restoration. A different digest
				// still goes through the original schema-aware semantic comparison;
				// in particular an unapplied author value is never declared applied.
				if (const auto* Early = Parsed.EarlyEditDigests.Find(Index)) ObservedEditDigest = *Early;
				else {
					TArray<uint8> EditBytes;
					if (!TrySerializeEditNode(*Parsed.UnitRows[Index], Resources, OutError, EditBytes)) return false;
					if (!EditBytes.IsEmpty()) ObservedEditDigest = Sha256(EditBytes);
				}
				if (!ObservedEditDigest.IsEmpty() && Inventory[Index].Syntax->EditHash.Matches(ObservedEditDigest)) {
					Parsed.GeneratedRows[Index] = true;
					++ReusedSemanticRows; ++ExactGeneratedEditRows;
					continue;
				}
			}
			ON_SCOPE_EXIT {
				if (!NextRows) return;
				int32 First = Index, Count = 1, GroupNumber = INDEX_NONE;
				if (Parsed.GroupIndex) {
					while (Parsed.InventoryRanges.IsValidIndex(AdmissionShard) && Parsed.InventoryRanges[AdmissionShard].Count == 0) ++AdmissionShard;
					if (Parsed.InventoryRanges.IsValidIndex(AdmissionShard)) {
						const auto& Shard = Parsed.InventoryRanges[AdmissionShard];
						if (Index != Shard.First + Shard.Count - 1) return;
						GroupNumber = AdmissionShard++;
						First = Shard.First; Count = Shard.Count;
					}
				}
				if (RetainedSemanticBytes + NextRows->SparseAllocatedBytes() <= ResolvedRowsBudget) {
					GroupSourceNames.Reset(); GroupLookups.Reset(); GroupUnitStrings.Reset(); GroupUnitContainers.Reset(); GroupCanonicalNodes.Reset(); GroupCharges.Reset(); GroupSourceShapes.Reset();
					return;
				}
				// Admit complete inventory consumer groups. A budget miss must not
				// scatter holes through every group/compile block. Current resolved
				// units remain complete; only this group's optional next-call proof
				// is retired, with its current provenance restored to the result.
				const bool bSharedGroup = GroupNumber != INDEX_NONE && NextRows->Groups[GroupNumber].IsValid();
				for (int32 Offset = 0; Offset < Count; ++Offset) {
					const int32 RowIndex = First + Offset;
					const FString& Id = Inventory[RowIndex].Syntax->GetUnitId();
					const auto Proof = NextRows->FindRow(Id);
					if (!Proof) continue;
					if (Proof->Source.IsSet()) UnitSources.Add(Id, Proof->Source.GetValue());
					if (Proof->SourceDocument) if (const auto Document = Proof->SourceDocument->Pin()) UnitSourceDocuments.Add(Id, Document.ToSharedRef());
					if (Proof->Baseline.IsSet()) BaselineUnits.Add(Id, Proof->Baseline.GetValue());
					if (bSharedGroup || (Parsed.ReusableRows.IsValidIndex(RowIndex) && Parsed.ReusableRows[RowIndex] == Proof)) --SharedRetainedRows;
					RetainedSemanticBytes -= Proof->RetainedBytes;
					--RetainedSemanticRows; ++BudgetRejectedRows;
					NextRows->RemoveSparseRow(Id);
				}
				if (bSharedGroup) NextRows->Groups[GroupNumber].Reset();
				// A rejected sparse group must not leave unused set capacity charged
				// against later groups. Indexed slots stay allocated until final seal,
				// including the full-group-to-sparse fallback, and are always charged.
				NextRows->Rows.Shrink();
				for (const auto& Pair : GroupSourceNames) {
					RetainedSourceNames.Remove(Pair.Key);
					RetainedSourceNameBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupLookups) {
					RetainedLookups.Remove(Pair.Key);
					RetainedLookupBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupUnitStrings) {
					RetainedUnitStrings.Remove(Pair.Key);
					RetainedUnitStringBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupCanonicalNodes) {
					RetainedCanonicalNodes.Remove(Pair.Key);
					RetainedCanonicalBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupCharges) {
					RetainedCharges.Remove(Pair.Key);
					RetainedChargeBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupUnitContainers) {
					RetainedUnitContainers.Remove(Pair.Key);
					RetainedUnitContainerBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				for (const auto& Pair : GroupSourceShapes) {
					RetainedSourceShapes.Remove(Pair.Key);
					RetainedSourceShapeBytes -= Pair.Value; RetainedSemanticBytes -= Pair.Value;
				}
				GroupSourceNames.Reset(); GroupLookups.Reset(); GroupUnitStrings.Reset(); GroupUnitContainers.Reset(); GroupCanonicalNodes.Reset(); GroupCharges.Reset(); GroupSourceShapes.Reset();
			};
			if (const auto* Group = Parsed.ReusedGroups.Find(Index)) {
				const int32 GroupNumber = Parsed.GroupIndex->FindPositionChecked(Inventory[Index].Syntax->GetUnitId()).X;
				FUeAgentResourceChargeError ResourceError;
				const double AvailabilityStarted = FPlatformTime::Seconds();
				bool bCanRetainGroup = NextRows.IsValid();
				if (bCanRetainGroup) for (const auto& Row : (*Group)->Rows)
					if (Row->SourceDocument && (!Row->SourceDocument->IsAvailable()
						|| !AdmittedProofSourceUnits.Contains(Row->SourceDocument->GetSourceUnit()))) { bCanRetainGroup = false; break; }
				GroupAvailabilitySeconds += FPlatformTime::Seconds() - AvailabilityStarted;
				if (bCanRetainGroup && ReferencePool.TryAdoptGroup(**Group, false)
					&& Resources.TryChargeMany((*Group)->Charges, TEXT("semantic_workspace:validated_row"), ResourceError)) {
					ReferencePool.TryAdoptGroup(**Group);
					Units.Append((*Group)->Units); EditedUnitIds.Append((*Group)->EditedIds);
					for (const auto& Pair : (*Group)->KeyedRoots) ResolvedKeyedOperationRoots.Add(Pair.Key, *Pair.Value);
					NextRows->Groups[GroupNumber] = *Group; ConsumedGroups[GroupNumber] = true;
					const int32 Count = (*Group)->Rows.Num();
					RetainedSemanticRows += Count;
					const double BackingStarted = FPlatformTime::Seconds();
					// This complete set was built from these immutable rows and its
					// current bindings were just validated by TryAdoptGroup.
					for (const auto& Lookup : (*Group)->Lookups) AdmitLookup(Lookup);
					GroupLookupEdgesNotRevisited += (*Group)->ReferenceEdges - (*Group)->Lookups.Num();
					for (const auto& Row : (*Group)->Rows) { RetainedSemanticBytes += Row->RetainedBytes; AdmitSharedBacking(*Row, true); }
					GroupBackingSeconds += FPlatformTime::Seconds() - BackingStarted;
					ReusedSemanticRows += Count; SharedRetainedRows += Count;
					ReusedGroupRows += Count; ++ReusedSemanticGroups;
					Index += Count - 1;
					continue;
				}
				// Budget/pool conflicts use exactly the existing per-row path.
				// TryChargeMany rejects atomically, so no failed aggregate was consumed.
				for (int32 Offset = 0; Offset < (*Group)->Rows.Num(); ++Offset) {
					const auto& Prior = (*Group)->Rows[Offset];
					auto& Proof = Parsed.RowProofs[Index + Offset];
					Proof.Input = Prior->InputHash; Proof.Authored = Prior->AuthoredHash; Proof.Envelope = Prior->EnvelopeHash;
					Proof.EnvelopeSpan = Prior->EnvelopeSpan;
					Proof.ReboundSource = Prior->SourceDocument ? Prior->SourceDocument->Pin() : nullptr; Proof.bValid = true;
					Parsed.ReusableRows[Index + Offset] = Prior;
				}
			}
			const auto& ProofInventory = Inventory[Index];
			const auto* ReadProof = Parsed.RowProofs.IsValidIndex(Index) ? &Parsed.RowProofs[Index] : nullptr;
			const auto Prior = Parsed.ReusableRows.IsValidIndex(Index) ? Parsed.ReusableRows[Index] : nullptr;
			if (Prior && ReadProof && ReadProof->bValid && Prior->Unit.IsSet())
			{
				FUeAgentResourceChargeError ResourceError;
				if (!Resources.TryChargeMany(*Prior->Charges, TEXT("semantic_workspace:validated_row"), ResourceError))
					return FailResource(OutError, ResourceError);
				Units.Add(Prior->Unit.GetValue());
				SeenUnitIds.Add(ProofInventory.Syntax->GetUnitId());
				if (Prior->KeyedRoots) ResolvedKeyedOperationRoots.Add(ProofInventory.Syntax->GetUnitId(), *Prior->KeyedRoots);
				if (Prior->bEdited) EditedUnitIds.Add(ProofInventory.Syntax->GetUnitId());
				OutError.Reset();
				if (NextRows
					&& (!ReadProof->ReboundSource || AdmittedProofSourceUnits.Contains(ReadProof->ReboundSource->GetSourceUnit()))) {
					if (ReadProof->ReboundSource == (Prior->SourceDocument ? Prior->SourceDocument->Pin() : nullptr)
						&& ReadProof->ReboundSource
						&& Prior->Source.IsSet()
						&& SameRowProvenance(Prior->Source.GetValue(), Parsed.UnitRows[Index]->GetProvenance())
						&& Prior->EnvelopeSpan.Equals(ReadProof->EnvelopeSpan)
						&& ReferencePool.TryAdopt(*Prior)) {
						NextRows->AddSparseRow(Prior);
						++RetainedSemanticRows;
						RetainedSemanticBytes += Prior->RetainedBytes;
						AdmitSharedBacking(*Prior);
						++SharedRetainedRows;
						++ReusedSemanticRows;
						continue;
					}
					FResolvedRowProof Retained = *Prior;
					Retained.AuthoredStorage = ReadProof->AuthoredStorage;
					Retained.AuthoredDocument = ReadProof->AuthoredDocument;
					Retained.EnvelopeSpan = ReadProof->EnvelopeSpan;
					Retained.LookupKeyOverride = ProofInventory.Syntax->GetUnitId().Equals(Retained.Unit->GetUnitId(), ESearchCase::CaseSensitive)
						? FString() : ProofInventory.Syntax->GetUnitId();
					Retained.Source = Parsed.UnitRows[Index]->GetProvenance();
					Retained.SourceDocument.Reset();
					if (ReadProof->ReboundSource)
						Retained.SourceDocument = MakeShared<FResolvedRowSource>(ReadProof->ReboundSource.ToSharedRef());
					if (Retained.bCompleteReferenceLookups) {
						FRowReferenceRecorder Recorder{&PublicRefToUnitId, &ReferencePool, GeneratedReferences};
						for (const auto& Lookup : Retained.ReferenceLookups)
							Recorder.Observe(Lookup->Key, Lookup->Result.IsSet() ? &Lookup->Result.GetValue() : nullptr);
						Retained.ReferenceLookups = MoveTemp(Recorder.Lookups);
						Retained.bCompleteReferenceLookups = Recorder.bComplete;
					}
					Retained.RetainedBytes = RetainedResolvedRowBytes(Retained, MeasuredRowCosts);
					if (Retained.RetainedBytes <= ResolvedRowsBudget) {
						RetainedSemanticBytes += Retained.RetainedBytes;
						AdmitSharedBacking(Retained);
						NextRows->AddSparseRow(MakeShared<const FResolvedRowProof>(MoveTemp(Retained)));
						++RetainedSemanticRows;
					}
				}
				if (Prior->Baseline.IsSet()) BaselineUnits.Add(ProofInventory.Syntax->GetUnitId(), Prior->Baseline.GetValue());
				UnitSources.Add(ProofInventory.Syntax->GetUnitId(), Parsed.UnitRows[Index]->GetProvenance());
				if (ReadProof->ReboundSource) UnitSourceDocuments.Add(ProofInventory.Syntax->GetUnitId(), ReadProof->ReboundSource.ToSharedRef());
				++ReusedSemanticRows;
				continue;
			}
			const auto UsageBefore = Resources.Snapshot();
			const int32 EditedBefore = EditedUnitIds.Num();
			FRowReferenceRecorder ReferenceRecorder{&PublicRefToUnitId, &ReferencePool, GeneratedReferences};
			auto ParseUnit = [&]() -> bool
			{
				const FHjsonNode* UnitEnvelope =
					Parsed.UnitEnvelopes.IsValidIndex(Index)
						? Parsed.UnitEnvelopes[Index]
						: nullptr;
				if (bAllowManagedUnitEdits && UnitEnvelope == nullptr)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_editable_envelope_"
							"reexport_required"), TEXT("The editable unit has no captured envelope; export the workspace again."),
						Inventory[Index].Syntax->GetUnitId(),
						TEXT("/unit_inventory/envelope"));
				}
				TOptional<FSemanticUnit> Unit;
				TOptional<bool> ManagedSemanticChanged;
				TSet<FString> UnitResolvedKeyedOperationRoots;
				FHjsonNodePtr UnitValueSources;
				const double UnitParseStarted = FPlatformTime::Seconds();
				auto UnitIdentity = Inventory[Index].Syntax->UnitIdentity.ToSharedRef();
				if (Parsed.GroupIndex) UnitIdentity = Parsed.GroupIndex->BindIdentity(MoveTemp(UnitIdentity));
				if (!ParseSemanticUnit(
						*Parsed.UnitRows[Index],
						UnitEnvelope,
						MoveTemp(UnitIdentity),
						LexToString(Inventory[Index].Syntax->UnitKind),
						Inventory[Index].Syntax->GetUnitPointer(),
						PublicRefToUnitId,
						Resources,
						Unit,
						ManagedSemanticChanged,
						UnitResolvedKeyedOperationRoots,
						UnitValueSources,
						OutError, false, Parsed.GeneratedSource ? Parsed.GeneratedSource->Model.Get() : nullptr)
					|| !Unit.IsSet())
				{
					return false;
				}
				UnitParseSeconds += FPlatformTime::Seconds() - UnitParseStarted;
				const FInventoryRow& InventoryRow =
					Inventory[Index];
				if (Source.GetKind() == EWorkspaceSourceKind::Directory)
				{
					const FHjsonNode& Row = *Parsed.UnitRows[Index];
					UnitSources.Add(Unit->GetUnitId(), Row.GetProvenance());
					const FHjsonNode* Values = Row.Get(TEXT("values"));
					// A shallow root copy retains the parser's original child nodes and
					// provenance, including expanded public file references. No reparse.
					const FHjsonNodeRef SourceRoot = UnitValueSources.IsValid()
						? UnitValueSources.ToSharedRef() : MakeShared<FHjsonNode>(Values ? *Values : Row);
					UnitSourceDocuments.Add(Unit->GetUnitId(), SourceRoot->GetProvenance().GetSourceUnit().Equals(
						Row.GetProvenance().GetSourceUnit(), ESearchCase::CaseSensitive)
						? MakeShared<FHjsonDocument>(SourceRoot)
						: MakeShared<FHjsonDocument>(Row.GetProvenance().GetSourceUnit(), SourceRoot));
				}
				if (!UnitResolvedKeyedOperationRoots.IsEmpty())
					ResolvedKeyedOperationRoots.Add(Unit->GetUnitId(), MoveTemp(UnitResolvedKeyedOperationRoots));
				if (bAllowManagedUnitEdits && InventoryRow.Syntax->EditHash.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("semantic_workspace_edit_hash_reexport_required"), TEXT("The editable unit has no baseline edit digest; export the workspace again."),
						InventoryRow.Syntax->GetUnitId(),
						TEXT("/unit_inventory/edit_hash"));
				}
				const auto* GroupPosition = Parsed.GroupIndex ? Parsed.GroupIndex->FindPosition(Unit->GetUnitId()) : nullptr;
				bool bDuplicatesGeneratedUnit = false;
				if (Parsed.GeneratedSource && Unit->GetUnitId() != InventoryRow.Syntax->GetUnitId()) {
					// Preserve the existing duplicate-versus-extra diagnostic order for
					// malformed input without copying every unchanged identity into a
					// second set on successful edits. Only this error path needs a scan.
					for (int32 Before = 0; Before < Index; ++Before)
						if (Parsed.GeneratedRows[Before] && Inventory[Before].Syntax->GetUnitId() == Unit->GetUnitId()) {
							bDuplicatesGeneratedUnit = true; break;
						}
				}
				if (SeenUnitIds.Contains(Unit->GetUnitId()) || bDuplicatesGeneratedUnit || (GroupPosition && ConsumedGroups[GroupPosition->X]))
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_duplicate"),
						TEXT("More than one resolved document declares the same semantic unit."), TEXT("workspace_resolve"));
					Diagnostic.PropertyPath = TEXT("unit_id");
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Unit->GetUnitId());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				if (Unit->GetUnitId() != InventoryRow.Syntax->GetUnitId())
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_extra"),
						TEXT("The resolved semantic unit identity differs from its inventory entry."), TEXT("workspace_resolve"));
					Diagnostic.PropertyPath = TEXT("unit_id");
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Unit->GetUnitId());
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryRow.Syntax->GetUnitId());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				if (Unit->GetUnitKind()
					!= InventoryRow.Syntax->UnitKind)
				{
					return Fail(
						OutError,
						TEXT(
							"semantic_workspace_unit_kind_mismatch"), TEXT("The resolved semantic unit kind differs from its inventory entry."),
						Parsed.UnitRows[Index]->GetProvenance().GetSourceUnit(),
						Parsed.UnitRows[Index]->GetProvenance().GetJsonPointer(),
						MakeShared<FJsonValueString>(LexToString(Unit->GetUnitKind())), MakeShared<FJsonValueString>(LexToString(InventoryRow.Syntax->UnitKind)));
				}
				const bool bDerivedEligible = ReadProof && ReadProof->bValid && ReferenceRecorder.bComplete;
				// v3's physical inventory shard is stable across insertions and
				// deletions. The exact row key still binds input, provenance and all
				// reference lookups; neither row ordinal nor old membership is proof.
				const FString DerivedGroup = GroupPosition
					? Parsed.GroupIndex->Shards[GroupPosition->X].Path
					: TEXT("legacy_block:") + FString::FromInt(Index / 4096);
				FDerivedRowMaterial Derived;
				const FDerivedRowMaterial* PriorDerived = nullptr;
				const int32 MaterialLookupCount = ReferenceRecorder.Lookups.Num();
				if (bDerivedEligible) {
					const double KeyStarted = FPlatformTime::Seconds();
					Derived.Key = DerivedRowKey(*ReadProof, InventoryRow, *Parsed.UnitRows[Index], UnitEnvelope, ReferenceRecorder);
					DerivedMaterials.KeySeconds += FPlatformTime::Seconds() - KeyStarted;
					PriorDerived = DerivedMaterials.Find(DerivedGroup, Index, Derived.Key);
					if (PriorDerived) Derived = *PriorDerived;
				}
				const double RowHashStarted = FPlatformTime::Seconds();
				FString ActualContentHash;
				if (PriorDerived) { ActualContentHash = FDerivedRowMaterial::Text(Derived.Content); ++DerivedMaterials.RowHashHits; }
				else if (!TryHashSemanticUnitRow(Unit.GetValue(), ActualContentHash, OutError, InventoryRow.GetDocumentPath())) return false;
				if (FInventoryDigest::Decode(ActualContentHash, Derived.Content)) Derived.Flags |= 1;
				RowHashSeconds += FPlatformTime::Seconds() - RowHashStarted;
				const bool bContentHashChanged = !InventoryRow.Syntax->ContentHash.Matches(ActualContentHash);
				const FSemanticUnit* ProducedBaseline = nullptr;
				bool bDeferredKeyedBaseline = false;
				if (bContentHashChanged && Parsed.GeneratedSource && Parsed.GeneratedSource->Bindings) {
					const auto* Expected = Parsed.GeneratedSource->FileHashes.Find(InventoryRow.Syntax->GetEnvelopeRef());
					const auto* Observed = Parsed.BaselineProofs.Find(InventoryRow.Syntax->GetEnvelopeRef());
					if (Expected && Observed && *Expected == Observed->ContentHash) {
						const auto* Roots = ResolvedKeyedOperationRoots.Find(Unit->GetUnitId());
						const auto Declaration = Parsed.GeneratedSource->Model->FindDeclaration(Unit->GetUnitId());
						bDeferredKeyedBaseline = bAllowManagedUnitEdits && ManagedSemanticChanged.IsSet() && Roots && !Roots->IsEmpty()
							&& Declaration.IsSet() && Unit->MatchesCompiledDeclaration(Declaration->GetDeclarationProof());
						if (bDeferredKeyedBaseline) for (const auto& Root : *Roots)
							if (Root != TEXT("actors") && Root != TEXT("components")) { bDeferredKeyedBaseline = false; break; }
						// Original published bytes and declaration are already bound.
						// The keyed before consumer requests its K members below; do not
						// eagerly decode/hash the complete collection just to drop it.
						if (!bDeferredKeyedBaseline) ProducedBaseline = Parsed.GeneratedSource->Bindings->FindBaseline(Unit->GetUnitId());
					}
				}
				// Reuse the writer's captured machine values, not the edited visible row.
				// A hash match binds the complete unit (including identity/definition refs).
				// Older envelopes may lack these values; unchanged rows remain usable.
				if (!bContentHashChanged) BaselineUnits.Add(Unit->GetUnitId(), Unit.GetValue());
				else if (ProducedBaseline && Unit->MatchesCompiledDeclaration(ProducedBaseline->GetDeclarationProof()))
					BaselineUnits.Add(Unit->GetUnitId(), *ProducedBaseline);
				else if (const FHjsonNode* Captured = UnitEnvelope ? UnitEnvelope->Get(TEXT("captured_values")) : nullptr;
					!bDeferredKeyedBaseline && Captured && Captured->GetKind() == EHjsonNodeKind::Object)
				{
					const double BaselineHashStarted = FPlatformTime::Seconds();
					FWorkspaceError BaselineError;
					TOptional<FSemanticUnit> Restored;
					if (!TryRestoreCapturedSemanticUnit(*UnitEnvelope, Unit->GetDeclaration(), Resources, Restored, BaselineError))
					{ OutError = MoveTemp(BaselineError); return false; }
					{
						auto Baseline = MoveTemp(Restored.GetValue());
						FString BaselineHash;
						if (PriorDerived && (Derived.Flags & 2)) { BaselineHash = FDerivedRowMaterial::Text(Derived.Baseline); ++DerivedMaterials.BaselineHashHits; }
						else if (!TryHashSemanticUnitRow(Baseline, BaselineHash, BaselineError, InventoryRow.GetDocumentPath()))
						{ OutError = MoveTemp(BaselineError); return false; }
						if (FInventoryDigest::Decode(BaselineHash, Derived.Baseline)) Derived.Flags |= 2;
						if (InventoryRow.Syntax->ContentHash.Matches(BaselineHash))
							BaselineUnits.Add(Unit->GetUnitId(), MoveTemp(Baseline));
					}
					BaselineHashSeconds += FPlatformTime::Seconds() - BaselineHashStarted;
				}
				if (!bAllowManagedUnitEdits && bContentHashChanged)
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_hash_mismatch"),
						TEXT("The semantic unit content differs from its recorded digest in a read-only workspace load."), TEXT("workspace_resolve"));
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ActualContentHash);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(InventoryRow.Syntax->ContentHash.ToString());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.UnitRows[Index]);
				}
				// The original row above is the file/inventory integrity proof. The
				// accepted baseline below serves semantic rebase, not raw-byte identity.
				if (auto* Baseline = BaselineUnits.Find(Unit->GetUnitId());
					Baseline && !TryNormalizeAuthoringUnit(*Baseline, Resources, OutError)) return false;
				const double EditHashStarted = FPlatformTime::Seconds();
				TArray<uint8> EditBytes;
				FString ActualEditHash;
				bool bEditMaterialReused = false;
				const bool bNeedsEditDigest = bAllowManagedUnitEdits && !ManagedSemanticChanged.IsSet();
				if (bNeedsEditDigest && !ObservedEditDigest.IsEmpty()) {
					bEditMaterialReused = true;
					ActualEditHash = ObservedEditDigest;
					if (FInventoryDigest::Decode(ActualEditHash, Derived.Edit)) Derived.Flags |= 4;
				}
				else if (bNeedsEditDigest && PriorDerived && (Derived.Flags & 4)) {
					bEditMaterialReused = true;
					ActualEditHash = FDerivedRowMaterial::Text(Derived.Edit); ++DerivedMaterials.EditHashHits;
				}
				if (bNeedsEditDigest && !bEditMaterialReused
					&& !TrySerializeEditNode(
						*Parsed.UnitRows[Index], Resources, OutError, EditBytes))
				{
					if (OutError.GetCode().IsEmpty())
					{
						return Fail(OutError,
							TEXT("semantic_workspace_edit_hash_failed"), TEXT("The edited semantic unit could not be serialized for change detection."), InventoryRow.Syntax->GetUnitId());
					}
					return false;
				}
				if (bNeedsEditDigest && !bEditMaterialReused && EditBytes.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("semantic_workspace_edit_hash_failed"), TEXT("Serializing the edited semantic unit produced no bytes for change detection."),
						InventoryRow.Syntax->GetUnitId());
				}
				if (bNeedsEditDigest && !bEditMaterialReused) {
					ActualEditHash = Sha256(EditBytes);
					if (FInventoryDigest::Decode(ActualEditHash, Derived.Edit)) Derived.Flags |= 4;
				}
				if (bAllowManagedUnitEdits && (ManagedSemanticChanged.IsSet()
					? ManagedSemanticChanged.GetValue() : !InventoryRow.Syntax->EditHash.Matches(ActualEditHash)))
				{
					// Raw edit bytes select the changed row; admitted typed semantics
					// decide authored intent. Keyed-operation owners retain their own
					// result because an operation projection is not a captured state.
					if (!ManagedSemanticChanged.IsSet())
					{
						if (const auto* Baseline = BaselineUnits.Find(Unit->GetUnitId())) {
							auto Intent = Unit.GetValue();
							bool bEquivalent = false;
							if (!TryNormalizeAuthoringUnit(Intent, Resources, OutError)
								|| !TryAreSemanticUnitsEquivalent(Intent, *Baseline, Resources, bEquivalent, OutError)) return false;
							ManagedSemanticChanged = !bEquivalent;
						}
					}
					if (!ManagedSemanticChanged.IsSet()
						|| ManagedSemanticChanged.GetValue())
					{
						EditedUnitIds.Add(Unit->GetUnitId());
					}
				}
				EditHashSeconds += FPlatformTime::Seconds() - EditHashStarted;
				if (bDerivedEligible && (Derived.Flags & 1) && ReferenceRecorder.bComplete
					&& MaterialLookupCount == ReferenceRecorder.Lookups.Num()
					&& (!PriorDerived || PriorDerived->Flags != Derived.Flags)) DerivedMaterials.Remember(DerivedGroup, Index, Derived);
				SeenUnitIds.Add(Unit->GetUnitId());
				Units.Add(MoveTemp(Unit.GetValue()));
				return true;
			};
			bool bParsed = false;
			{
				TGuardValue<FRowReferenceRecorder*> Guard(ActiveRowReferenceRecorder, &ReferenceRecorder);
				bParsed = ParseUnit();
			}
			if (!bParsed)
			{
				OutError.Locate(Parsed.UnitRows[Index]->GetProvenance());
				UnitDiagnostics.Append(OutError);
				UnitDiagnostics.MarkUnchecked(TEXT("unit_semantics"), Inventory[Index].GetDocumentPath(), TEXT("unit_structure_invalid"));
			}
			else if (NextRows && ReadProof && ReadProof->bValid)
			{
				FResolvedRowProof Proof;
				Proof.AuthoredStorage = ReadProof->AuthoredStorage;
				Proof.AuthoredDocument = ReadProof->AuthoredDocument;
				Proof.InputHash = ReadProof->Input;
				Proof.AuthoredHash = ReadProof->Authored;
				Proof.EnvelopeHash = ReadProof->Envelope;
				Proof.EnvelopeSpan = ReadProof->EnvelopeSpan;
				Proof.EnvelopeRef = ProofInventory.Syntax->EnvelopeRef;
				Proof.ReferenceLookups = MoveTemp(ReferenceRecorder.Lookups);
				Proof.bCompleteReferenceLookups = ReferenceRecorder.bComplete;
				Proof.Unit = Units.Last(); // Before graph-layout derived values below.
				Proof.LookupKeyOverride = ProofInventory.Syntax->GetUnitId().Equals(Proof.Unit->GetUnitId(), ESearchCase::CaseSensitive)
					? FString() : ProofInventory.Syntax->GetUnitId();
				Proof.bEdited = EditedUnitIds.Num() != EditedBefore;
				if (const auto* Value = BaselineUnits.Find(ProofInventory.Syntax->GetUnitId())) Proof.Baseline = *Value;
				if (const auto* Value = UnitSources.Find(ProofInventory.Syntax->GetUnitId())) Proof.Source = *Value;
				if (const auto* Value = UnitSourceDocuments.Find(ProofInventory.Syntax->GetUnitId())) Proof.SourceDocument = MakeShared<FResolvedRowSource>(*Value);
				if (const auto* Value = ResolvedKeyedOperationRoots.Find(ProofInventory.Syntax->GetUnitId()); Value && !Value->IsEmpty())
					Proof.KeyedRoots = MakeShared<const TSet<FString>>(*Value);
				const auto UsageAfter = Resources.Snapshot();
				bool bExact = true;
				FResolvedResourceCharges RowCharges;
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D)
				{
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 Before = UsageBefore.Consumed(Dimension), After = UsageAfter.Consumed(Dimension);
					if (After == MAX_int64 || After < Before) { bExact = false; break; }
					if (After > Before) RowCharges.Add(Dimension, After - Before);
				}
				Proof.Charges = SealResolvedCharges(MoveTemp(RowCharges));
				// SourceDocument is a per-unit shallow wrapper, not the whole-file
				// parser document. Valid row proofs already exclude content_files
				// and expanded content_ref dependencies. Admit their actual source
				// backing by provenance; CompletedDocuments retains that same file.
				if (bExact && !OutError.HasErrors() && (!Proof.SourceDocument || AdmittedProofSourceUnits.Contains(Proof.SourceDocument->GetSourceUnit()))) {
					Proof.RetainedBytes = RetainedResolvedRowBytes(Proof, MeasuredRowCosts);
					if (Proof.RetainedBytes <= ResolvedRowsBudget) {
						RetainedSemanticBytes += Proof.RetainedBytes;
						AdmitSharedBacking(Proof);
						NextRows->AddSparseRow(MakeShared<const FResolvedRowProof>(MoveTemp(Proof)));
						++RetainedSemanticRows;
					}
				}
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_edit_row_handoff exact_edit_rows=%d typed_restoration=not_needed remaining_rows=original_semantic_reader"), ExactGeneratedEditRows);
		if (UnitDiagnostics.HasErrors())
		{
			UnitDiagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_units"));
			OutError = MoveTemp(UnitDiagnostics);
			return false;
		}
		// Value edits do not invalidate the compiler's declaration/index proof.
		// This includes keyed native structure fields: their actual membership
		// is interpreted by the domain, not by recreating the semantic graph.
		bool bGeneratedIndexedSource = Parsed.GeneratedSource && ActiveReadState
			&& Source.GetKind() == EWorkspaceSourceKind::Directory && bAllowManagedUnitEdits
			&& Mode == ESemanticWorkspaceMode::State;
		if (bGeneratedIndexedSource) {
			for (auto& Unit : Units) {
				const auto Before = Parsed.GeneratedSource->Model->FindDeclaration(Unit.GetUnitId());
				if (!Before || !Unit.MatchesCompiledDeclaration(Before->GetDeclarationProof())) {
					bGeneratedIndexedSource = false; break;
				}
			}
		}
		if (bGeneratedIndexedSource) {
			// The original graph layout projection needs this graph's item ids
			// and layout rows, not every value in the publication. Declaration
			// membership is independent of the optional decoded row storage.
			TSet<FString> Graphs, Present;
			for (const auto& Unit : Units) {
				Present.Add(Unit.GetUnitId());
				if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit.HasValue(TEXT("nodes")) && Unit.HasValue(TEXT("edges")))
					Graphs.Add(Unit.GetUnitId());
			}
			int32 RelatedRows = 0;
			if (!Graphs.IsEmpty()) {
				const auto Append = [&](const FString& Id) {
					if (Id.IsEmpty() || Present.Contains(Id)) return true;
					const auto* Value = Parsed.GeneratedSource->Model->FindUnit(Id);
					if (!Value) {
						if (!Parsed.GeneratedSource->Model->CheckValueReadResult(OutError)) return false;
						return Fail(OutError, TEXT("semantic_workspace_graph_layout_missing"),
							TEXT("A related graph item or layout row is missing from its captured publication."), Id);
					}
					Units.Add(*Value); Present.Add(Id); ++RelatedRows; return true;
				};
				for (const auto& Declaration : Parsed.GeneratedSource->Model->GetDeclarations())
					if (Declaration.GetUnitKind() == ESemanticUnitKind::ItemState
						&& Graphs.Contains(Declaration.GetOwnerRef()) && !Declaration.GetLayoutRef().IsEmpty())
						if (!Append(Declaration.GetUnitId()) || !Append(Declaration.GetLayoutRef())) return false;
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_graph_dependencies graphs=%d related_value_rows=%d complete_units_materialized=false"), Graphs.Num(), RelatedRows);
		}

		// Graph layout is edited through the ordinary node pos. Hidden layout units
		// are derived from that same resolved value, never an independent edit source.
		if (bAllowManagedUnitEdits)
		{
			FWorkspaceError LayoutDiagnostics;
			TSet<FString> AmbiguousGraphs;
			TMap<FString, int32> UnitIndices;
			// The layout consumer has no work without an actual graph. Build its
			// full index only after observing nodes/edges, not for every Level row.
			auto EnsureLayoutIndices = [&]() {
				if (!UnitIndices.IsEmpty()) return;
				UnitIndices.Reserve(Units.Num());
				for (int32 Index = 0; Index < Units.Num(); ++Index) UnitIndices.Add(Units[Index].GetUnitId(), Index);
			};
			auto CollectLayoutError = [&](const FSemanticUnit& Unit)
			{
				if (const auto* Source = UnitSources.Find(Unit.GetUnitId())) OutError.Locate(*Source);
				LayoutDiagnostics.Append(OutError);
				OutError.Reset();
			};
			TMap<FString, TMap<FString, FUeAgentCanonicalValueRef>> NodesByGraph;
			for (int32 Index = 0; Index < Units.Num(); ++Index)
			{
				const FSemanticUnit& Unit = Units[Index];
				if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState) continue;
				const auto* Nodes = Unit.FindValue(TEXT("nodes"));
				if (!Nodes || (*Nodes)->GetKind() != EUeAgentValueKind::Object || !Unit.HasValue(TEXT("edges"))) continue;
				EnsureLayoutIndices();
				auto& ById = NodesByGraph.FindOrAdd(Unit.GetUnitId());
				for (const auto& Node : (*Nodes)->GetObjectItems())
				{
					if (Node.Value->GetKind() != EUeAgentValueKind::Object) continue;
					for (const auto& Field : Node.Value->GetObjectItems())
						if (Field.Key == TEXT("id") && Field.Value->GetKind() == EUeAgentValueKind::String)
						{
							if (ById.Contains(Field.Value->GetText()))
							{
								Fail(OutError, TEXT("semantic_workspace_graph_layout_identity_ambiguous"), TEXT("More than one graph node declares the same layout identity."), Unit.GetUnitId());
								CollectLayoutError(Unit);
								AmbiguousGraphs.Add(Unit.GetUnitId());
								continue;
							}
							ById.Add(Field.Value->GetText(), Node.Value);
						}
				}
			}
			if (!NodesByGraph.IsEmpty()) for (const FSemanticUnit& Node : Units)
			{
				if (Node.GetUnitKind() != ESemanticUnitKind::ItemState || Node.GetLayoutRef().IsEmpty()) continue;
				if (AmbiguousGraphs.Contains(Node.GetOwnerRef()))
				{
					LayoutDiagnostics.MarkUnchecked(TEXT("graph_layout"), Node.GetSourceProvenance(), TEXT("ambiguous_node_identity"));
					continue;
				}
				const auto* ById = NodesByGraph.Find(Node.GetOwnerRef());
				const auto* Identity = Node.FindValue(TEXT("id"));
				if (!ById || !Identity || (*Identity)->GetKind() != EUeAgentValueKind::String) continue;
				const auto* Resolved = ById->Find((*Identity)->GetText());
				if (!Resolved) continue;
				TMap<FString, FUeAgentCanonicalValueRef> Fields;
				for (const auto& Field : (*Resolved)->GetObjectItems()) Fields.Add(Field);
				const auto* Operation = Fields.Find(TEXT("operation"));
				if (Operation && (*Operation)->GetKind() == EUeAgentValueKind::String && (*Operation)->GetText() == TEXT("remove")) continue;
				if (!TryNormalizeGraphAuthoringNodePosition(Fields, Node.GetUnitId(), Resources, OutError))
				{
					CollectLayoutError(Node);
					continue;
				}
				const auto* Position = Fields.Find(TEXT("pos"));
				if (!Position) continue;
				const int32* LayoutIndex = UnitIndices.Find(Node.GetLayoutRef());
				if (!LayoutIndex)
				{
					Fail(OutError, TEXT("semantic_workspace_graph_layout_missing"), TEXT("The graph node's referenced layout unit is missing."), Node.GetUnitId());
					CollectLayoutError(Node);
					continue;
				}
				const FSemanticUnit& Layout = Units[*LayoutIndex];
				if (Layout.GetUnitKind() != ESemanticUnitKind::LayoutState || Layout.GetOwnerRef() != Node.GetUnitId())
				{
					Fail(OutError, TEXT("semantic_workspace_graph_layout_owner_mismatch"), TEXT("The graph node's layout has the wrong unit kind or belongs to a different owner."), Node.GetUnitId());
					CollectLayoutError(Node);
					continue;
				}
				TMap<FString, FUeAgentCanonicalValueRef> Values;
				for (const auto& Field : (*Position)->GetObjectItems()) Values.Add(Field);
				Units[*LayoutIndex] = FSemanticUnit(Layout.GetUnitId(), Layout.GetUnitKind(), Layout.GetOwnerRef(),
					Layout.GetSemanticTypeRef(), Layout.GetRole(), Layout.GetFieldSetRef(), MoveTemp(Values),
					Layout.GetItemRefs(), Layout.GetRelationRefs(), Layout.GetOrderingRef(), Layout.GetLayoutRef(),
					Layout.GetDependencyRefs(), Layout.GetProfileVisibility(), Layout.GetSourceProvenance(),
					Layout.GetTargetIdentityKey(), Layout.GetCategoryPath());
			}
			if (LayoutDiagnostics.HasErrors())
			{
				LayoutDiagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_graph_layout"));
				OutError = MoveTemp(LayoutDiagnostics);
				return false;
			}
		}

		TArray<FString> TargetUnitIds;
		if (!ReadStringArray(
				*Parsed.Manifest,
				TEXT("target_unit_ids"),
				true,
				TargetUnitIds,
				OutError))
		{
			return false;
		}
		double GroupSealSeconds = 0;
		const auto SealResolvedSourceCandidates = [&] {
			const double Started = FPlatformTime::Seconds();
			if (NextRows) {
				CompleteResolvedRowGroups(*NextRows, Parsed, Inventory);
				NextRows->ShrinkSparseRows();
			}
			GroupSealSeconds = FPlatformTime::Seconds() - Started;
		};
		const auto RetainResolvedSourceCandidates = [&] {
			if (NextRows) {
				// Admission preceded semantic resolution. Carry its newly produced
				// candidates only with that exact admitted text/tree version, under
				// the same budget; a miss drops these optional facts, not the input.
				int32 KeyedDocuments = 0;
				for (auto& Pair : NextDocumentReads) {
					auto& Stored = Pair.Value;
					const auto* Current = ActiveReadState ? ActiveReadState->Documents.Find(Pair.Key) : nullptr;
					if (Current && Current->ContentHash == Stored.ContentHash && Current->Parsed == Stored.Parsed) {
						const int64 OldBytes = KeyedDocumentMetadataBytes(Stored), NewBytes = KeyedDocumentMetadataBytes(*Current);
						if (NewBytes - OldBytes <= SyntaxBudget - RetainedSyntaxBytes - static_cast<int64>(NextDocumentReads.GetAllocatedSize())) {
							Stored.KeyedPending = Current->KeyedPending;
							Stored.KeyedReferences = Current->KeyedReferences;
							RetainedSyntaxBytes += NewBytes - OldBytes;
						} else {
							Stored.KeyedPending.Empty(); Stored.KeyedReferences.Empty();
							RetainedSyntaxBytes -= OldBytes;
						}
					}
					KeyedDocuments += !Stored.KeyedPending.IsEmpty();
				}
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_keyed_candidates documents=%d existing_syntax_budget=true"), KeyedDocuments);
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_syntax_retention documents=%d retained_bytes=%lld candidate_bytes=%lld budget_bytes=%lld rejected_documents=%d complete_groups=%d total_s=%.6f"),
					NextDocumentReads.Num(), RetainedSyntaxBytes + NextDocumentReads.GetAllocatedSize(), CandidateSyntaxBytes,
					512LL * 1024 * 1024, RejectedSyntaxDocuments, RetainedSyntaxGroups, SyntaxAdmissionSeconds);
				PreviousDocumentReads = MoveTemp(NextDocumentReads);
				PreviousResolvedRows = MoveTemp(NextRows);
			} else { PreviousResolvedRows.Reset(); PreviousDocumentReads.Reset(); }
		};
		// An import with no authored intent has no execution model to prepare.
		// The current rows have already passed source, envelope, lookup and typed
		// intent validation against their captured baselines. This conclusion does
		// not depend on retaining an index graph or optional row cache entries.
		// This is not an export or a declaration that the whole native world is
		// unchanged. The caller must still close the current namespace/bytes fence.
		const FHjsonNode* StructuralIntent = Parsed.Manifest->Get(TEXT("structural_edits"));
		if (OutUnchangedImportProof && ActiveReadState
			&& Source.GetKind() == EWorkspaceSourceKind::Directory
			&& bAllowManagedUnitEdits && Mode == ESemanticWorkspaceMode::State
			&& EditedUnitIds.IsEmpty() && StructuralIntent
			&& StructuralIntent->GetKind() == EHjsonNodeKind::Array && StructuralIntent->GetItems().IsEmpty()) {
			FUeAgentStateRevision Revision(TEXT("invalid"), TEXT("invalid"), -1);
			if (!ParseRevision(*Parsed.Manifest, Revision, OutError)
				|| !TryCaptureCurrentReadSourceProof(ResolutionRoot, Parsed.ManagedPaths, *OutUnchangedImportProof, OutError)
				|| !PublicationFence.End(OutError)) return false;
			if (const auto Session = WorkspaceSourceObservations.FindRef(ResolutionRoot);
				Session && ActiveReadState->SelectedNotificationThrough) {
				FileChanges::FError Native;
				if (!AcknowledgeSourceNotifications(*Session, ActiveReadState->SelectedNotificationThrough, Native))
					return FailFileObservation(OutError, ResolutionRoot, Native);
			}
			// Retain the already validated typed rows and exact input proofs under
			// their existing budgets. These are source candidates, not a compiled
			// model, accepted native state, or a compilation reuse proof.
			SealResolvedSourceCandidates();
			RetainResolvedSourceCandidates();
			DerivedMaterials.Finish();
			OutError.SetFailure(Contract::FDiagnostic::Failure(TEXT("hjson_import_no_explicit_changes"),
				TEXT("The workspace contains no explicit property or structural change; omitted and unchanged values preserve the current state."), TEXT("hjson_import")));
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_empty_intent units=%d reused_rows=%d typed_edits=0 model_compile=false retained_rows=%d rejected_rows=%d group_seal_s=%.6f total_s=%.6f"),
				Inventory.Num(), ReusedSemanticRows, RetainedSemanticRows, BudgetRejectedRows, GroupSealSeconds, FPlatformTime::Seconds() - ResolveStarted);
			return false;
		}
		FAuthoringModelPtr Model;
		const double CompileStarted = FPlatformTime::Seconds();
		bool bPropertySourceView = bGeneratedIndexedSource || (ActiveReadState && Source.GetKind() == EWorkspaceSourceKind::Directory
			&& bAllowManagedUnitEdits && Mode == ESemanticWorkspaceMode::State && StructuralIntent
			&& StructuralIntent->GetKind() == EHjsonNodeKind::Array && StructuralIntent->GetItems().IsEmpty());
		TMap<FString, int32> EditedRows;
		if (bPropertySourceView) {
			TSet<FString> Edits(EditedUnitIds);
			for (int32 I = 0; I < Units.Num(); ++I) if (Edits.Contains(Units[I].GetUnitId())) EditedRows.Add(Units[I].GetUnitId(), I);
			for (const auto& Pair : EditedRows) {
				const auto& Unit = Units[Pair.Value];
				const auto* Before = BaselineUnits.Find(Pair.Key);
				bool bSameDeclaration = false;
				if (bGeneratedIndexedSource) continue;
				bool bNativeKeyedValues = Unit.GetUnitKind() == ESemanticUnitKind::StructureState
					&& Unit.GetLayoutRef().IsEmpty()
					&& !(Unit.HasValue(TEXT("nodes")) && Unit.HasValue(TEXT("edges")));
				bool bHasKeyedField = false;
				if (bNativeKeyedValues) for (const auto& Field : Unit.GetValues()) {
					FKeyedItemProjectionContract Contract;
					bHasKeyedField |= Field.Value->GetKind() == EUeAgentValueKind::Object
						&& TryGetKeyedItemProjectionContract(Field.Key, Contract) && !Contract.bIdentityFromMapKey;
				}
				bNativeKeyedValues &= bHasKeyedField;
				// This value lowers to the original native collection owner. A cold
				// declaration-preserving edit must not acquire a complete graph
				// requirement merely because the optional generated model is absent.
				if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet && !bNativeKeyedValues) { bPropertySourceView = false; break; }
				const auto CapturedDeclaration = Parsed.GeneratedSource
					? Parsed.GeneratedSource->Model->FindDeclaration(Unit.GetUnitId()) : TOptional<FSemanticUnit::FDeclarationView>();
				if (CapturedDeclaration.IsSet()) bSameDeclaration = Unit.MatchesCompiledDeclaration(CapturedDeclaration->GetDeclarationProof());
				else if (Before) {
					if (!TryAreSemanticUnitsEquivalent(Unit.WithValues(Before->GetValues()), *Before, Resources, bSameDeclaration, OutError)) return false;
				}
				if (!bSameDeclaration) { bPropertySourceView = false; break; }
			}
		}
		if (!bPropertySourceView && Source.GetKind() == EWorkspaceSourceKind::Directory
			&& bAllowManagedUnitEdits && Mode == ESemanticWorkspaceMode::State)
			return FailWorkspaceDataflowHandoff(OutError, TEXT("source.typed_intent_view"),
				TEXT("matching declaration / supported local structural intent"), ResolutionRoot,
				Schema, TEXT("addressed declaration-preserving rows and domain operation records"),
				TEXT("Source parser and domain keyed/graph intent producer"));
		FString SourceObservationStamp;
		if (ActiveReadState && (bPropertySourceView || Parsed.bCanonicalInputFingerprint)) {
			// One actual byte-observation formula, independent of whether values
			// were restored through generated rows or the full parser/compiler.
			TMap<FString, FString> Hashes = ActiveReadState->HashOnlyDocuments;
			for (const auto& Pair : ActiveReadState->Documents) Hashes.Add(Pair.Key, Pair.Value.ContentHash);
			TArray<FString> Paths; Hashes.GetKeys(Paths); Paths.Sort();
			FWorkspaceTextHash Stamp;
			for (const auto& Path : Paths) { Stamp.Append(Path); Stamp.Append(TEXT("|")); Stamp.Append(Hashes.FindChecked(Path)); }
			SourceObservationStamp = Stamp.Finish();
		}
		if (bPropertySourceView) {
			for (auto& Unit : Units) if (!TryNormalizeAuthoringUnit(Unit, Resources, OutError)) return false;
			if (Parsed.GeneratedSource) {
				TMap<FString, FSemanticUnit> Replacements;
				// Include the original graph owner's derived layout rows as well as
				// the explicit edits. They stay in this synchronous Source view;
				// EditedUnitIds still identifies the actual author intent.
				for (const auto& Unit : Units) Replacements.Add(Unit.GetUnitId(), Unit);
				Model = FAuthoringModel::MakePropertySourceView(Parsed.GeneratedSource->Model.ToSharedRef(), MoveTemp(BaseRevision), MoveTemp(Replacements), SourceObservationStamp);
			} else Model = FAuthoringModel::MakePropertySourceView(MoveTemp(ContractId), static_cast<int32>(ContractVersion),
				MoveTemp(ProfileId), MoveTemp(BaseRevision), MoveTemp(Units), MoveTemp(TargetUnitIds), SourceObservationStamp);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=property_source_view units=%d edited=%d parsed_rows=%d generated_rows=%d full_compile=false graph_fingerprint=false"),
				Model->GetUnitCount(), EditedRows.Num(), Parsed.UnitRows.Num() - ReusedSemanticRows, ReusedSemanticRows);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_intent_view generated_declarations_reused=%s edited_rows=%d complete_units_materialized=false"),
				bGeneratedIndexedSource ? TEXT("true") : TEXT("false"), EditedRows.Num());
		}
		else if (!TryCompileAuthoringSourceModel(
				MoveTemp(ContractId),
				static_cast<int32>(ContractVersion),
				MoveTemp(ProfileId),
				MoveTemp(BaseRevision),
				MoveTemp(Units),
				MoveTemp(TargetUnitIds),
				Resources,
				Model,
				OutError, nullptr, nullptr,
				Parsed.PriorRowsOwner ? Parsed.PriorRowsOwner->CompilerProof.Get() : nullptr,
				bAllowManagedUnitEdits || Schema == GetDirectWorkspaceSchemaId() ? nullptr : &ExpectedFingerprint,
				Schema != GetDirectWorkspaceSchemaId())
			|| !Model.IsValid())
		{
			if (OutError.GetCode() == TEXT("semantic_workspace_fingerprint_mismatch"))
				return Fail(OutError, TEXT("semantic_workspace_fingerprint_mismatch"),
					TEXT("The semantic workspace content differs from its recorded fingerprint."),
					Parsed.Manifest->GetProvenance().GetSourceUnit(), TEXT("/semantic_fingerprint"));
			if (NextRows && SharedRetainedRows > 0) {
				TMap<FString, FSourceProvenance> AllSources;
				FResolvedRowBindings(NextRows.ToSharedRef(), false).CopySources(AllSources);
				for (const auto& Pair : UnitSources) AllSources.Add(Pair.Key, Pair.Value);
				OutError.LocateUnits(AllSources);
			} else OutError.LocateUnits(UnitSources);
			if (OutError.GetCode() == TEXT("unresolved_semantic_unit_reference")
				|| OutError.GetCode() == TEXT("missing_target_unit"))
				OutError.Recode(TEXT("semantic_workspace_reference_unresolved"), TEXT("The workspace reference does not resolve to a semantic unit."));
			return false;
		}
		const double CompileSeconds = FPlatformTime::Seconds() - CompileStarted;

		FUeAgentStateRevision BaseStateRevision(
			TEXT("invalid"),
			TEXT("invalid"),
			-1);
		if (!ParseRevision(
				*Parsed.Manifest,
				BaseStateRevision,
				OutError))
		{
			return false;
		}
		TArray<FStructuralEdit> StructuralEdits;
		if (!ParseStructuralEdits(
				*Parsed.Manifest,
				*Model,
				StructuralEdits,
				OutError))
		{
			return false;
		}
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		TArray<FString> ClosureUnitIds;
		if (!OptionalString(
				*Parsed.Manifest,
				TEXT("selection_fingerprint"),
				SelectionFingerprint,
				OutError)
			|| !OptionalString(
				*Parsed.Manifest,
				TEXT("closure_fingerprint"),
				ClosureFingerprint,
				OutError)
			|| !ReadStringArray(
				*Parsed.Manifest,
				TEXT("closure_unit_ids"),
				false,
				ClosureUnitIds,
				OutError))
		{
			return false;
		}
		if (Mode == ESemanticWorkspaceMode::EditBundle)
		{
			TSet<FString> ClosureSet(ClosureUnitIds);
			if (SelectionFingerprint.IsEmpty() || ClosureFingerprint.IsEmpty())
			{
				const FString Field = SelectionFingerprint.IsEmpty() ? TEXT("selection_fingerprint") : TEXT("closure_fingerprint");
				return FailField(OutError, TEXT("semantic_workspace_closure_incomplete"),
					FString::Printf(TEXT("Edit bundle metadata requires a non-empty %s."), *Field), *Parsed.Manifest, Field,
					MakeShared<FJsonValueString>(TEXT("non-empty fingerprint")), false);
			}
			if (ClosureSet.Num() != Model->GetUnitCount())
			{
				auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_closure_incomplete"),
					TEXT("The edit bundle closure count differs from the model unit count."), TEXT("workspace_resolve"),
					*Parsed.Manifest, TEXT("closure_unit_ids"), nullptr, false);
				Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ClosureSet.Num());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Model->GetUnitCount());
				return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.Manifest);
			}
			for (const auto& Unit : Model->GetDeclarations())
			{
				if (!ClosureSet.Contains(Unit.GetUnitId()))
				{
					auto Diagnostic = HjsonFieldFailure(TEXT("semantic_workspace_closure_incomplete"),
						TEXT("The edit bundle closure omits a semantic unit required by the model."), TEXT("workspace_resolve"),
						*Parsed.Manifest, TEXT("closure_unit_ids"), nullptr, false);
					Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Unit.GetUnitId());
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Parsed.Manifest);
				}
			}
		}
		const double WorkspaceMaterialStarted = FPlatformTime::Seconds();
		FWorkspaceTextHash WorkspaceMaterial, CanonicalMaterial;
		const auto* InputSchema = Parsed.Manifest->Get(TEXT("workspace_schema"));
		const bool bByteCheckpointInput = Source.GetKind() == EWorkspaceSourceKind::Directory && Mode == ESemanticWorkspaceMode::State
			&& InputSchema && InputSchema->GetText() == GetDirectWorkspaceSchemaId();
		if (bByteCheckpointInput && !ActiveReadState) return Fail(OutError, TEXT("workspace_read_scope_unavailable"),
			TEXT("A direct workspace input requires its accepted Source read scope. No legacy fingerprint builder was selected."), ResolutionRoot);
		if (!bByteCheckpointInput) {
			WorkspaceMaterial.Append(LexToString(Mode)); WorkspaceMaterial.Append(TEXT("|"));
			WorkspaceMaterial.Append(Model->GetSemanticFingerprint());
		}
		const bool bCanonicalInput = !bByteCheckpointInput && Parsed.bCanonicalInputFingerprint && !SourceObservationStamp.IsEmpty()
			&& (Parsed.bSparseGeneratedInventory || (Parsed.FingerprintOwnerProof && Parsed.FingerprintOwnerProof->IsCurrent()));
		TSet<FString> CanonicalBaselines;
		if (bCanonicalInput) {
			CanonicalMaterial.Append(LexToString(Mode)); CanonicalMaterial.Append(TEXT("|source:"));
			CanonicalMaterial.Append(SourceObservationStamp);
			TSet<FString> Added;
			auto AddPath = [&](const FString& Path) {
				if (!Path.IsEmpty() && !Added.Contains(Path)) {
					Added.Add(Path); CanonicalMaterial.Append(TEXT("|")); CanonicalMaterial.Append(Path);
				}
			};
			if (Parsed.bSparseGeneratedInventory) {
				for (const auto& Path : Parsed.ManagedPaths) AddPath(Path);
				for (const auto& Pair : Parsed.BaselineProofs) CanonicalBaselines.Add(Pair.Key);
			} else {
				for (int32 I = 0; I < Parsed.SparseManagedPrefix; ++I) AddPath(Parsed.ManagedPaths[I]);
				for (int32 I = 0; I < Inventory.Num(); ++I) {
					const auto& Row = Inventory[I];
					if (Parsed.FingerprintRequiredDocuments.Contains(Row.GetDocumentPath())) {
						const auto Slice = Parsed.SparseManagedSlices[I];
						for (int32 J = 0; J < Slice.Y; ++J) AddPath(Parsed.FingerprintRowPaths[Slice.X + J]);
						if (!Row.Syntax->GetEnvelopeRef().IsEmpty()) CanonicalBaselines.Add(Row.Syntax->GetEnvelopeRef());
					} else { AddPath(Row.GetDocumentPath()); AddPath(Row.Syntax->GetEnvelopeRef()); }
				}
				for (const auto& Path : Parsed.FingerprintInterfacePaths) AddPath(Path);
			}
		}
		if (!bByteCheckpointInput) {
		for (const FString& Path : Parsed.ManagedPaths)
		{
			WorkspaceMaterial.Append(TEXT("|")); WorkspaceMaterial.Append(Path);
		}
		TArray<FString> BaselinePaths;
		Parsed.BaselineProofs.GetKeys(BaselinePaths);
		BaselinePaths.Sort();
		for (const FString& Path : BaselinePaths) {
			WorkspaceMaterial.Append(TEXT("|baseline:")); WorkspaceMaterial.Append(Path); WorkspaceMaterial.Append(TEXT("|"));
			WorkspaceMaterial.Append(Parsed.BaselineProofs.FindChecked(Path).ContentHash);
			if (bCanonicalInput && CanonicalBaselines.Contains(Path)) {
				CanonicalMaterial.Append(TEXT("|baseline:")); CanonicalMaterial.Append(Path); CanonicalMaterial.Append(TEXT("|"));
				CanonicalMaterial.Append(Parsed.BaselineProofs.FindChecked(Path).ContentHash);
			}
		}
		TArray<FString> InventoryPaths;
		Parsed.InventoryHashes.GetKeys(InventoryPaths); InventoryPaths.Sort();
		for (const FString& Path : InventoryPaths) {
			WorkspaceMaterial.Append(TEXT("|inventory:")); WorkspaceMaterial.Append(Path); WorkspaceMaterial.Append(TEXT("|"));
			WorkspaceMaterial.Append(Parsed.InventoryHashes.FindChecked(Path));
			if (bCanonicalInput) {
				CanonicalMaterial.Append(TEXT("|inventory:")); CanonicalMaterial.Append(Path); CanonicalMaterial.Append(TEXT("|"));
				CanonicalMaterial.Append(Parsed.InventoryHashes.FindChecked(Path));
			}
		}
		}
		auto BaselineHashView = MakeShared<FCompactBaselineHashes>();
		if (!Parsed.bSparseGeneratedInventory && !bByteCheckpointInput) BaselineHashView->Rows.Reserve(Inventory.Num());
		TSharedPtr<const FResolvedWorkspaceBaselineHashes> AcceptedBaselineHashes = BaselineHashView;
		if (Parsed.bSparseGeneratedInventory || (bByteCheckpointInput && Parsed.GeneratedSource && Parsed.GeneratedSource->Inventory))
			AcceptedBaselineHashes = MakeShared<FGeneratedBaselineHashes>(Parsed.GeneratedSource->Inventory.ToSharedRef());
		// Naming facts are a real output consumer, not a side effect of retaining
		// every source execution row. The original publication owner supplies its
		// definition-only binding reader once (or its already owned generation).
		const auto GeneratedProjectionHints = Parsed.bSparseGeneratedInventory
			? Parsed.GeneratedSource->Bindings->ReadPublishedDefinitionBindings() : nullptr;
		if (Parsed.bSparseGeneratedInventory && !GeneratedProjectionHints) {
			if (!Parsed.GeneratedSource->Model->CheckValueReadResult(OutError)) return false;
			return FailWorkspaceDataflowHandoff(OutError, TEXT("source.definition_names"),
				TEXT("definition projection bindings for this publication"), ResolutionRoot,
				TEXT("missing"), TEXT("same-generation definition names/aliases and their exact content binding"),
				TEXT("publication ReadPublishedDefinitionBindings"));
			}
		auto ProjectionHints = MakeShared<FDefinitionProjectionHints>();
		ProjectionHints->ContractId = Model->GetContractId();
		ProjectionHints->ContractVersion = Model->GetContractVersion();
		ProjectionHints->ProfileId = Model->GetProfileId();
		ProjectionHints->TargetUnitIds = Model->GetTargetUnitIds();
		int64 HintPayloadBytes = sizeof(FDefinitionProjectionHints) + 32
			+ ProjectionHints->ContractId.GetAllocatedSize() + ProjectionHints->ProfileId.GetAllocatedSize()
			+ ProjectionHints->TargetUnitIds.GetAllocatedSize();
		for (const auto& Target : ProjectionHints->TargetUnitIds) HintPayloadBytes += Target.GetAllocatedSize();
		constexpr int64 HintBudget = 4LL * 1024 * 1024;
		bool bAdmitHints = !GeneratedProjectionHints && ActiveReadState && !ResolutionRoot.IsEmpty() && HintPayloadBytes <= HintBudget;
		if (Parsed.bSparseGeneratedInventory && !bByteCheckpointInput) Parsed.GeneratedSource->Inventory->VisitHashes([&](const FString& Id, const FString& Hash) {
			WorkspaceMaterial.Append(TEXT("|")); WorkspaceMaterial.Append(Id);
			WorkspaceMaterial.Append(TEXT("|")); WorkspaceMaterial.Append(Hash);
			if (bCanonicalInput) {
				CanonicalMaterial.Append(TEXT("|")); CanonicalMaterial.Append(Id);
				CanonicalMaterial.Append(TEXT("|")); CanonicalMaterial.Append(Hash);
			}
		});
		bool bAcceptedInventoryRemapped = false;
		for (const FInventoryRow& Row : Inventory)
		{
			bAcceptedInventoryRemapped |= !Row.DocumentPathOverride.IsEmpty() || !Row.LogicalPathOverride.IsEmpty();
			if (!Parsed.bSparseGeneratedInventory && !bByteCheckpointInput) {
				WorkspaceMaterial.Append(TEXT("|")); WorkspaceMaterial.Append(Row.Syntax->GetUnitId());
				WorkspaceMaterial.Append(TEXT("|")); WorkspaceMaterial.Append(Row.Syntax->ContentHash);
				if (bCanonicalInput) {
					CanonicalMaterial.Append(TEXT("|")); CanonicalMaterial.Append(Row.Syntax->GetUnitId());
					CanonicalMaterial.Append(TEXT("|")); CanonicalMaterial.Append(Row.Syntax->ContentHash);
				}
				BaselineHashView->Rows.Emplace(Row.Syntax->UnitIdentity, Row.Syntax->ContentHash);
			}
			if (bAdmitHints && Row.Syntax->UnitKind == ESemanticUnitKind::CollectionState) {
				const auto* Unit = Model->FindUnit(Row.Syntax->GetUnitId());
				FDefinitionProjectionBinding Binding;
				if (Unit && TryReadPersistedDefinitionProjectionBinding(*Unit, Row.Syntax->GetUnitPointer(),
					Row.Syntax->GetPublicRef(), Binding)) {
					const int64 ItemBytes = Row.Syntax->GetUnitId().GetAllocatedSize() + Binding.ContentKey.GetAllocatedSize()
						+ Binding.DefinitionName.GetAllocatedSize() + Binding.ReadableAlias.GetAllocatedSize() + Binding.PublicRef.GetAllocatedSize();
					ProjectionHints->Definitions.Add(Row.Syntax->GetUnitId(), MoveTemp(Binding));
					if (HintPayloadBytes + ItemBytes + ProjectionHints->Definitions.GetAllocatedSize() <= HintBudget)
						HintPayloadBytes += ItemBytes;
					else {
						ProjectionHints->Definitions.Remove(Row.Syntax->GetUnitId());
						ProjectionHints->Definitions.Shrink(); bAdmitHints = false;
					}
				}
			}
		}
		const FString LegacyWorkspaceFingerprint = bByteCheckpointInput ? FString() : WorkspaceMaterial.Finish();
		FString WorkspaceFingerprint;
		if (bByteCheckpointInput) {
			TSet<FString> Seen; const FString Prefix = ResolutionRoot + TEXT("/");
			for (const auto& Input : ActiveReadState->Documents) if (Input.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) Seen.Add(Input.Key.Mid(Prefix.Len()));
			for (const auto& Input : ActiveReadState->HashOnlyDocuments) if (Input.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) Seen.Add(Input.Key.Mid(Prefix.Len()));
			TArray<FString> Paths = Seen.Array(); Paths.Sort(); FWorkspaceTextHash Bytes;
			Bytes.Append(TEXT("uai.accepted_source_bytes/2|")); Bytes.Append(LexToString(Mode));
			if (Parsed.GeneratedSource) { Bytes.Append(TEXT("|publication=")); Bytes.Append(Parsed.GeneratedSource->Publication.Generation); }
			for (const auto& Path : Paths) {
				FString Text, Hash, ByteHash; int64 Characters = 0;
				const FString FullPath = ResolutionRoot / Path;
				const auto* Observed = ObservedSourceFiles.Find(FullPath);
				const bool bReadInScope = ActiveReadState->Documents.Contains(FullPath) || ActiveReadState->HashOnlyDocuments.Contains(FullPath)
					|| (Parsed.GeneratedSource && Path == TEXT(".uai-export-owner.json"));
				if (bReadInScope && Observed && Observed->bByteHashObserved) ByteHash = Observed->GetByteHash();
				else if (IsReparsePoint(FullPath) || !ObserveSourceFile(FullPath, false, Text, Hash, Characters, nullptr, &ByteHash))
					return Fail(OutError, TEXT("workspace_source_read_failed"), TEXT("An accepted source byte checkpoint could not be observed."), Path);
				// This describes bytes already accepted at source read time. The
				// existing later current-byte/namespace fence remains independent;
				// computing a label must not perform that fence a second time.
				Bytes.Append(TEXT("|")); Bytes.Append(FString::FromInt(Path.Len())); Bytes.Append(TEXT(":")); Bytes.Append(Path);
				Bytes.Append(TEXT("|")); Bytes.Append(ByteHash);
			}
			WorkspaceFingerprint = TEXT("source-bytes/2:") + Bytes.Finish();
		} else WorkspaceFingerprint = bCanonicalInput ? CanonicalMaterial.Finish() : LegacyWorkspaceFingerprint;
		const double WorkspaceMaterialSeconds = FPlatformTime::Seconds() - WorkspaceMaterialStarted;
		TOptional<TArray<FString>> ExplicitEditedUnitIds;
		if (bAllowManagedUnitEdits
			&& Source.GetKind() == EWorkspaceSourceKind::Directory)
		{
			ExplicitEditedUnitIds = MoveTemp(EditedUnitIds);
		}
		SealResolvedSourceCandidates();
		const double CompilerProofStarted = FPlatformTime::Seconds();
		int64 CompilerProofBytes = 0;
		if (Model->IsCompiled() && NextRows && RetainedSemanticRows == Model->GetUnitCount() && BudgetRejectedRows == 0) {
			// The retained rows already own every declaration referenced by this
			// weak compiler token. It retains neither their values nor the index
			// graph, and its extra allocation participates in the same row budget.
			const auto Proof = Model->CreateCompilationReuseProof();
			const int64 Bytes = Proof->GetAllocatedSize();
			if (RetainedSemanticBytes + NextRows->SparseAllocatedBytes() + Bytes <= ResolvedRowsBudget) {
				NextRows->CompilerProof = Proof;
				RetainedSemanticBytes += Bytes; CompilerProofBytes = Bytes;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_compiler_proof retained=%s bytes=%lld total_s=%.6f"),
			CompilerProofBytes ? TEXT("true") : TEXT("false"), CompilerProofBytes, FPlatformTime::Seconds() - CompilerProofStarted);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_row_admission retained_rows=%d retained_bytes=%lld budget_bytes=%lld rejected_rows=%d sparse_bytes=%lld"),
			RetainedSemanticRows, RetainedSemanticBytes + (NextRows ? NextRows->SparseAllocatedBytes() : 0), ResolvedRowsBudget, BudgetRejectedRows,
			NextRows ? NextRows->SparseAllocatedBytes() : 0);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_row_cost measured_rows=%lld source_bytes=%lld unit_storage_bytes=%lld canonical_bytes=%lld proof_bytes=%lld source_name_bytes=%lld lookup_bytes=%lld lookup_objects=%d unit_string_bytes=%lld unit_strings=%d"),
			MeasuredRowCosts.Rows, MeasuredRowCosts.SourceBytes, MeasuredRowCosts.UnitBytes, MeasuredRowCosts.CanonicalBytes, MeasuredRowCosts.ProofBytes, RetainedSourceNameBytes, RetainedLookupBytes, RetainedLookups.Num(), RetainedUnitStringBytes, RetainedUnitStrings.Num());
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_canonical_backing unique_nodes=%d retained_bytes=%lld"),
			RetainedCanonicalNodes.Num(), RetainedCanonicalBytes);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_resource_backing unique_deltas=%d retained_bytes=%lld"),
			RetainedCharges.Num(), RetainedChargeBytes);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_unit_containers unique_containers=%d retained_bytes=%lld"),
			RetainedUnitContainers.Num(), RetainedUnitContainerBytes);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_source_shapes unique_shapes=%d retained_bytes=%lld"),
			RetainedSourceShapes.Num(), RetainedSourceShapeBytes);
		TSharedPtr<const FResolvedWorkspaceUnitBindings> CurrentBindings;
		const double BindingsStarted = FPlatformTime::Seconds();
		if (NextRows && SharedRetainedRows > 0) {
			const auto ResolvedBindings = MakeShared<FResolvedRowBindings>(NextRows.ToSharedRef());
			if (!ResolvedBindings->IsComplete()) return Fail(OutError, TEXT("workspace_source_changed"),
				TEXT("The resolved source backing retired before the current workspace could retain it."), ResolutionRoot);
			CurrentBindings = ResolvedBindings;
		}
		if (Parsed.GeneratedSource) CurrentBindings = MakeShared<FLayeredSourceBindings>(CurrentBindings, Parsed.GeneratedSource->Bindings);
		const double BindingsSeconds = FPlatformTime::Seconds() - BindingsStarted;
		OutWorkspace =
			MakeShared<FResolvedSemanticWorkspace>(
				Source.GetKind(),
				Mode,
				Model.ToSharedRef(),
				MoveTemp(BaseStateRevision),
				MoveTemp(StructuralEdits),
				MoveTemp(Parsed.ManagedPaths),
				MoveTemp(SelectionFingerprint),
				MoveTemp(ClosureFingerprint),
				MoveTemp(ClosureUnitIds),
				MoveTemp(WorkspaceFingerprint),
				MoveTemp(ExplicitEditedUnitIds), MoveTemp(ResolvedKeyedOperationRoots), MoveTemp(UnitSources), TMap<FString, FString>(),
				MoveTemp(UnitSourceDocuments), MoveTemp(BaselineUnits),
				Parsed.Manifest->Get(TEXT("structural_edits"))->GetProvenance(),
				CurrentBindings, AcceptedBaselineHashes, MoveTemp(ExpectedFingerprint));
		if (!OutWorkspace->IsValid())
		{
			auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("semantic_workspace_invalid"), OutWorkspace->GetValidationDiagnostic());
			OutWorkspace.Reset();
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}
		if (!PublicationFence.End(OutError)) { OutWorkspace.Reset(); return false; }
		DerivedMaterials.Finish();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=derived_row_material queries=%lld hits=%lld misses=%lld block_hits=%lld block_misses=%lld block_rejected=%lld loaded_rows=%lld loaded_bytes=%lld sealed_rows=%lld rejected_rows=%lld peak_bytes=%lld key_s=%.6f load_validate_s=%.6f seal_s=%.6f store_s=%.6f edit_charge_s=%.6f stored_blocks=%d stored_bytes=%lld evicted_syntax=%d evicted_derived=%d evicted_bytes=%lld observed_disk_bytes=%lld disk_observe_s=%.6f disk_write_s=%.6f"),
			DerivedMaterials.Queries, DerivedMaterials.Hits, DerivedMaterials.Misses, DerivedMaterials.BlockHits,
			DerivedMaterials.BlockMisses, DerivedMaterials.BlockRejected, DerivedMaterials.LoadedRows, DerivedMaterials.LoadedBytes,
			DerivedMaterials.SealedRows, DerivedMaterials.RejectedRows, DerivedMaterials.PeakBytes, DerivedMaterials.KeySeconds,
			DerivedMaterials.LoadSeconds, DerivedMaterials.SealSeconds, DerivedMaterials.StoreSeconds, DerivedMaterials.ChargeSeconds,
			DerivedMaterials.StoreStats.Stored, DerivedMaterials.StoreStats.StoredBytes, DerivedMaterials.StoreStats.EvictedSyntax,
			DerivedMaterials.StoreStats.EvictedDerived, DerivedMaterials.StoreStats.EvictedBytes,
			DerivedMaterials.StoreStats.ObservedDiskBytes, DerivedMaterials.StoreStats.ObserveSeconds, DerivedMaterials.StoreStats.WriteSeconds);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=derived_material_consumption row_hash_hits=%lld baseline_hash_hits=%lld edit_hash_hits=%lld lookup_s=%.6f store_scratch_bytes=%lld relocated_hits=%lld grouping=inventory_path_exact_row_key"),
			DerivedMaterials.RowHashHits, DerivedMaterials.BaselineHashHits, DerivedMaterials.EditHashHits,
			DerivedMaterials.LookupSeconds, DerivedMaterials.StoreStats.ScratchBytes, DerivedMaterials.RelocatedHits);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=workspace_material rows=%d compact_hash_bytes=%lld shared_identity_handles=%d canonical_input=%s total_s=%.6f"),
			Inventory.Num(), BaselineHashView->GetAllocatedSize(), BaselineHashView->Rows.Num(), bCanonicalInput ? TEXT("true") : TEXT("false"), WorkspaceMaterialSeconds);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_group_consumption groups=%d rows=%d current_availability_s=%.6f backing_admission_s=%.6f group_seal_s=%.6f source_bindings_s=%.6f container_sizes_reused=%lld group_lookup_edges_not_revisited=%lld"),
			ReusedSemanticGroups, ReusedGroupRows, GroupAvailabilitySeconds, GroupBackingSeconds, GroupSealSeconds, BindingsSeconds,
			ReusedContainerMeasurements, GroupLookupEdgesNotRevisited);
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_binding_view shared_rows=%d unit_rows=%d"),
			SharedRetainedRows, Inventory.Num());
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=resolved_row_groups reused_groups=%d group_rows=%d sparse_rows=%d proof_bytes=%lld"),
			ReusedSemanticGroups, ReusedGroupRows, NextRows ? NextRows->SparseRowCount() : 0, NextRows ? NextRows->GroupChargeBytes : 0);
		if (ActiveReadState && !ResolutionRoot.IsEmpty())
		{
			auto& Cached = *ActiveReadState;
			Cached.ResolvedRoot = ResolutionRoot;
			FWorkspacePublication Publication;
			if (!TryReadWorkspacePublication(*Parsed.Manifest, ResolutionRoot / ManifestPath, Publication, OutError)) return false;
			Cached.ResolvedPublication = MoveTemp(Publication);
			Cached.ResolvedMode = ExpectedMode;
			Cached.bResolvedManagedEdits = bAllowManagedUnitEdits;
			Cached.ResolvedWorkspace = OutWorkspace;
			Cached.BaselineSource.Reset(); Cached.CommittedBaseline.Reset(); Cached.BaselineCharges.Reset();
			const bool bSameDirectPublication = Cached.ResolvedPublication->bDirectDataflow
				&& Parsed.GeneratedSource && Parsed.GeneratedSource->Publication.bDirectDataflow
				&& Parsed.GeneratedSource->Publication.Generation.Equals(Cached.ResolvedPublication->Generation, ESearchCase::CaseSensitive);
			if (Parsed.GeneratedSource && (bSameDirectPublication || (!Cached.ResolvedPublication->bDirectDataflow
				&& Parsed.GeneratedSource->Model->GetSemanticFingerprint().Equals(
					OutWorkspace->GetPublishedSemanticFingerprint(), ESearchCase::CaseSensitive)))) {
				Cached.BaselineSource = OutWorkspace;
				Cached.CommittedBaseline = Parsed.GeneratedSource->Model;
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=published_baseline_handoff units=%d rebuilt=false consumer=committed_export"),
					Cached.CommittedBaseline->GetUnitCount());
			}
			Cached.ResolvedDiagnostics = OutError;
			Cached.ResolvedDocuments.Reset();
			Cached.ResolvedHashOnlyDocuments.Reset();
			Cached.SourceProof.Reset();
			Cached.bSourceAccepted = false;
			Cached.bPrepareHandoff = false;
			Cached.ResolvedProjectionHints.Reset();
			Cached.ResolvedInventory.Reset();
			Cached.GeneratedBindings = Parsed.GeneratedSource ? Parsed.GeneratedSource->Bindings : nullptr;
			Cached.GeneratedDocument = {};
			if (Parsed.GeneratedSource) Cached.GeneratedDocument = [Generated = Parsed.GeneratedSource](const FString& Path, const FString& Hash) {
				const auto* Expected = Generated->FileHashes.Find(Path);
				return Expected && *Expected == Hash ? Generated->Bindings->FindGeneratedDocument(Path) : FUeAgentCanonicalValuePtr();
			};
			// Request-local handles share the exact immutable inventory backing
			// already parsed/validated above. No second tree, native object or
			// cross-process proof is retained. Remapped legacy rows keep fallback.
			if (GeneratedProjectionHints) Cached.ResolvedProjectionHints = GeneratedProjectionHints;
			else if (!ProjectionHints->Definitions.IsEmpty()) Cached.ResolvedProjectionHints = ProjectionHints;
			if (Parsed.GeneratedSource && (Parsed.bSparseGeneratedInventory || !bAcceptedInventoryRemapped)) {
				// Complete source resolution does not invalidate the original
				// publication's metadata/bucket capability. Keep that same owner,
				// rather than copying all rows into a less capable generic view.
				Cached.ResolvedInventory = Parsed.GeneratedSource->Inventory;
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=accepted_inventory_handoff rows=%d producer=original_source copied_rows=0 bucket_dispatch=original_producer array_reordered=false"), Inventory.Num());
				}
			else if (int64(Inventory.Num()) * sizeof(FResolvedWorkspaceInventory::FRow) <= 64LL * 1024 * 1024) {
				TSharedPtr<FResolvedWorkspaceInventory> ExportInventory = MakeShared<FResolvedWorkspaceInventory>();
				ExportInventory->ProjectionHints = Cached.ResolvedProjectionHints;
				ExportInventory->Rows.Reserve(Inventory.Num());
				for (int32 Index = 0; Index < Inventory.Num(); ++Index) {
					const auto& Row = Inventory[Index];
					if (!Row.DocumentPathOverride.IsEmpty() || !Row.LogicalPathOverride.IsEmpty()) { ExportInventory.Reset(); break; }
					FResolvedWorkspaceInventory::FRow Entry{Row.Syntax};
					Entry.InventoryOrdinal = Row.GeneratedInventoryOrdinal;
					Entry.bGeneratedProjection = Row.bGeneratedProjection;
					const auto* Origin = Row.SourceOrigin.IsSet() ? &Row.SourceOrigin.GetValue()
						: Row.SourceNode ? &Row.SourceNode->GetProvenance() : nullptr;
					if (Origin && !Row.Syntax->GetEnvelopeRef().IsEmpty()
						&& Origin->GetSourceUnit().Equals(Row.Syntax->GetEnvelopeRef().Replace(TEXT("/baselines/"), TEXT("/inventory/")), ESearchCase::CaseSensitive))
						Entry.InventorySpan = {Origin->GetByteStart(), Origin->GetByteEnd()};
					if (Parsed.RowProofs.IsValidIndex(Index)) Entry.EnvelopeSpan = Parsed.RowProofs[Index].EnvelopeSpan;
					if (!Entry.EnvelopeSpan.IsValid()) Entry.EnvelopeSpan = Row.GeneratedEnvelopeSpan;
					if (!Entry.EnvelopeSpan.IsValid() && NextRows) {
						const auto Proof = NextRows->FindRow(Row.Syntax->GetUnitId());
						if (Proof) Entry.EnvelopeSpan = Proof->EnvelopeSpan;
					}
					ExportInventory->Rows.Add(MoveTemp(Entry));
				}
				if (ExportInventory) {
					ExportInventory->CompletePhysicalOrganization();
					FUeAgentResourceChargeError ChargeError;
					if (Resources.TryChargeMany({{EUeAgentResourceDimension::EstimatedMemoryBytes,
						int64(sizeof(FResolvedWorkspaceInventory)) + 32 + int64(ExportInventory->Rows.GetAllocatedSize())}},
						TEXT("semantic_workspace:accepted_inventory_view"), ChargeError))
						Cached.ResolvedInventory = MoveTemp(ExportInventory);
				}
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_definition_projection candidates=%d retained_bytes=%lld admitted_all=%s accepted=false publication_definitions=%d producer=%s"),
				ProjectionHints->Definitions.Num(), HintPayloadBytes + ProjectionHints->Definitions.GetAllocatedSize(), bAdmitHints ? TEXT("true") : TEXT("false"),
				GeneratedProjectionHints ? GeneratedProjectionHints->Definitions.Num() : 0,
				GeneratedProjectionHints ? TEXT("original_publication_owner") : TEXT("source_rows"));
			const FString Prefix = ResolutionRoot + TEXT("/");
			for (const auto& Document : Cached.Documents)
			{
				if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
					Cached.ResolvedDocuments.Add(Document.Key, Document.Value.Parsed);
			}
			for (const auto& Document : Cached.HashOnlyDocuments)
				if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)
					&& !Cached.ResolvedDocuments.Contains(Document.Key))
					Cached.ResolvedHashOnlyDocuments.Add(Document.Key, Document.Value);
		}
		RetainResolvedSourceCandidates();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=semantic_resolve cached=false units=%d reused_rows=%d shared_row_proofs=%d source_read_s=%.6f semantic_s=%.6f total_s=%.6f unit_parse_s=%.6f row_hash_s=%.6f edit_hash_s=%.6f baseline_hash_s=%.6f compile_s=%.6f"),
			OutWorkspace->GetModel()->GetUnitCount(), ReusedSemanticRows, SharedRetainedRows, SourceReadFinished - ResolveStarted,
			FPlatformTime::Seconds() - SourceReadFinished, FPlatformTime::Seconds() - ResolveStarted,
			UnitParseSeconds, RowHashSeconds, EditHashSeconds, BaselineHashSeconds, CompileSeconds);
		if (ActiveReadState) UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_file_observation reused=%d actual_reads=%d direct_hash_only_files_scope=%d direct_hash_only_bytes_scope=%lld"),
			ActiveReadState->ReusedFileObservations - FileProofsBefore, ActiveReadState->ActualFileReads - FileReadsBefore, ActiveReadState->DirectHashOnlyFiles, ActiveReadState->DirectHashOnlyBytes);
		const auto SyntaxCacheAfter = GetHjsonSyntaxCacheStats();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=persistent_syntax hits=%lld misses=%lld stored=%lld rejected=%lld loaded_bytes=%lld load_s=%.6f store_s=%.6f"),
			SyntaxCacheAfter.Hits - SyntaxCacheBefore.Hits, SyntaxCacheAfter.Misses - SyntaxCacheBefore.Misses,
			SyntaxCacheAfter.Stored - SyntaxCacheBefore.Stored, SyntaxCacheAfter.Rejected - SyntaxCacheBefore.Rejected,
			SyntaxCacheAfter.LoadedBytes - SyntaxCacheBefore.LoadedBytes,
			SyntaxCacheAfter.LoadSeconds - SyntaxCacheBefore.LoadSeconds, SyntaxCacheAfter.StoreSeconds - SyntaxCacheBefore.StoreSeconds);
		return true;
	}

	void ResetSemanticWorkspaceReuse()
	{
		ResolvedChargeCandidates.Reset();
		ResolvedShapeCandidates.Reset();
		PreviousResolvedRows.Reset();
		PreviousDocumentReads.Reset();
		GeneratedSourceCandidate.Reset();
		PreviousManifestIndex.Reset();
		InventoryProofs.Reset();
		PreviousInventoryIdentities.Reset();
		InventoryProofRoot.Reset();
		ObservedSourceFiles.Reset(); ObservedSourceRoot.Reset(); WorkspaceSourceObservations.Reset();
		ResetWorkspaceFileProofs();
	}

	struct FResolvedWorkspaceSourceProof::FState
	{
		FString Root;
		struct FFile { FString Hash; FWorkspaceFileProofPtr Proof; };
		mutable TMap<FString, FFile> Files;
		mutable TSharedPtr<FWorkspaceReadPathScope> CapturedReferencePaths;
	};
	FResolvedWorkspaceSourceProof::FResolvedWorkspaceSourceProof(TSharedRef<const FState> InState)
		: State(MoveTemp(InState)) {}
	bool FResolvedWorkspaceSourceProof::TryIncludeObservedInput(const FString& Path, const FString& Hash, FWorkspaceError& Error,
		FWorkspaceFileProofPtr ActualProof) const
	{
		if (auto* Existing = State->Files.Find(Path)) {
			if (Existing->Hash != Hash) return Fail(Error, TEXT("workspace_source_version_changed"), TEXT("One request attempted to consume two versions of the same input."), Path);
			// The actual reader checked this same leaf before/after IO. Do not
			// reopen it here; the real source checkpoint checks its later version.
			if (ActualProof) Existing->Proof = MoveTemp(ActualProof);
			if (ActiveReadState && ActiveReadState->CapturedReferenceRoot == State->Root)
				State->CapturedReferencePaths = ActiveReadState->CapturedReferencePaths;
			return true;
		}
		FState::FFile Input{Hash, MoveTemp(ActualProof)};
		if (!Input.Proof) if (const auto* Actual = ObservedSourceFiles.Find(Path); Actual && Actual->ContentHash == Hash) Input.Proof = Actual->Proof;
		if (ActiveReadState && ActiveReadState->CapturedReferenceRoot == State->Root)
			State->CapturedReferencePaths = ActiveReadState->CapturedReferencePaths;
		State->Files.Add(Path, MoveTemp(Input)); return true;
	}
	bool FResolvedWorkspaceSourceProof::HasObservedContent(const FString& Path, const FString& TextHash) const
	{
		const auto* File = State->Files.Find(Path);
		return File && File->Hash.Equals(TextHash, ESearchCase::CaseSensitive);
	}
	bool FResolvedWorkspaceSourceProof::HasCurrentByteObservation(const FString& Path, const FString& ByteHash) const
	{
		const auto* File = State->Files.Find(Path);
		const auto* Observed = ObservedSourceFiles.Find(Path);
		return File && Observed && Observed->bByteHashObserved && File->Proof
			&& File->Proof == Observed->Proof && File->Hash == Observed->ContentHash
			&& Observed->GetByteHash() == ByteHash && File->Proof->IsCurrent();
	}
	bool HasAcceptedSemanticSourceByteObservation(const FString& Root, const FString& RelativePath, const FString& ByteHash)
	{
		const auto* Accepted = FindAcceptedSemanticSourceHash(Root, RelativePath);
		if (!Accepted || !ActiveReadState->SourceProof) return false;
		const FString Path = FPaths::Combine(ActiveReadState->ResolvedRoot, RelativePath);
		return ActiveReadState->SourceProof->HasObservedContent(Path, *Accepted)
			&& ActiveReadState->SourceProof->HasCurrentByteObservation(Path, ByteHash);
	}
	bool TryReadAcceptedSemanticSourceByteHash(const FString& Root, const FString& RelativePath,
		FString& OutHash, FWorkspaceError& Error)
	{
		OutHash.Reset();
		const auto* Accepted = FindAcceptedSemanticSourceHash(Root, RelativePath);
		if (!Accepted) return Fail(Error, TEXT("workspace_source_changed"), TEXT("The retained source has no accepted observation."), RelativePath);
		const FString Path = FPaths::Combine(ActiveReadState->ResolvedRoot, RelativePath);
		const auto* Observed = ObservedSourceFiles.Find(Path);
		if (ActiveReadState->SourceProof && Observed && Observed->bByteHashObserved
			&& ActiveReadState->SourceProof->HasObservedContent(Path, *Accepted)
			&& ActiveReadState->SourceProof->HasCurrentByteObservation(Path, Observed->GetByteHash())) {
			OutHash = Observed->GetByteHash(); return true;
		}
		// A missing optional lease is not a publication qualification. Read only
		// this required file through the original observer and retain its real
		// physical encoding; the publisher still closes the source checkpoint.
		FString Text, Hash; int64 Characters = 0;
		if (!ObserveSourceFile(Path, false, Text, Hash, Characters, nullptr, &OutHash) || Hash != *Accepted)
			return Fail(Error, TEXT("workspace_source_changed"), TEXT("The retained source changed after it was accepted."), RelativePath);
		return true;
	}
	bool FResolvedWorkspaceSourceProof::TryUpdateRetainedFileHashes(TMap<FString, FString>& Retained,
		FWorkspaceError& Error) const
	{
		const FString Prefix = State->Root + TEXT("/");
		int32 Updated = 0;
		for (const auto& Input : State->Files) {
			if (!Input.Key.StartsWith(Prefix, ESearchCase::CaseSensitive))
				return Fail(Error, TEXT("workspace_source_input_scope_mismatch"),
					FString::Printf(TEXT("Stage=publication.retained_source_bytes; input=%s; expected_workspace=%s. A foreign input cannot qualify this destination's retained files."),
					*Input.Key, *State->Root), Input.Key);
			const FString Relative = Input.Key.Mid(Prefix.Len());
			auto* Hash = Retained.Find(Relative);
			if (!Hash) continue;
			FString CurrentByteHash;
			if (!TryReadAcceptedSemanticSourceByteHash(State->Root, Relative, CurrentByteHash, Error)) return false;
			*Hash = MoveTemp(CurrentByteHash); ++Updated;
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=retained_source_bytes observed_inputs=%d retained_updates=%d unrelated_retained_observations=0"),
			State->Files.Num(), Updated);
		return true;
	}
	bool TryUpdateAcceptedRetainedFileHashes(const FString& Root, TMap<FString, FString>& Retained, FWorkspaceError& Error)
	{
		TSharedPtr<const FResolvedWorkspaceSourceProof> Proof;
		return TryGetResolvedWorkspaceSourceProof(Root, Proof, Error) && Proof->TryUpdateRetainedFileHashes(Retained, Error);
	}
	static void RecordObservedSourceProofFile(FResolvedWorkspaceSourceProof::FState& ProofState,
		const FString& Path, const FString& ContentHash)
	{
		FResolvedWorkspaceSourceProof::FState::FFile File{ContentHash, nullptr};
		if (const auto* Observation = ObservedSourceFiles.Find(Path);
			Observation && Observation->ContentHash == ContentHash && Observation->Proof) File.Proof = Observation->Proof;
		ProofState.Files.Add(Path, MoveTemp(File));
	}
	static bool TryCaptureCurrentReadSourceProof(const FString& Root, const TArray<FString>& ManagedPaths,
		TSharedPtr<const FResolvedWorkspaceSourceProof>& OutProof, FWorkspaceError& OutError)
	{
		if (!ActiveReadState) return false;
		const FString Prefix = Root + TEXT("/");
		auto State = MakeShared<FResolvedWorkspaceSourceProof::FState>();
		State->Root = Root;
		// The source maps are populated without erasing entries. Preserve their
		// encounter/override order directly in the final proof owner; no second
		// temporary map of the same owned paths and hashes has a reader.
		for (const auto& Document : ActiveReadState->HashOnlyDocuments)
			if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) RecordObservedSourceProofFile(*State, Document.Key, Document.Value);
		for (const auto& Document : ActiveReadState->Documents)
			if (Document.Key.StartsWith(Prefix, ESearchCase::CaseSensitive)) RecordObservedSourceProofFile(*State, Document.Key, Document.Value.ContentHash);
		OutProof = MakeShared<FResolvedWorkspaceSourceProof>(State);
		return true;
	}
	bool FResolvedWorkspaceSourceProof::ValidateFile(const FString& Path, void* CurrentFile, FWorkspaceError& Error) const
	{
		auto* File = State->Files.Find(Path);
		if (!File) return true;
		if (File->Proof && (CurrentFile ? File->Proof->IsCurrentHandle(CurrentFile) : File->Proof->IsCurrent())) return true;
		// This consumer is already reading the actual file. Transfer that same
		// byte/hash observation to Source rather than discarding it and making
		// the next checkpoint or row reader observe the same contents again.
		auto Observation = TryBeginWorkspaceFileObservation(Path);
		TArray<uint8> Bytes;
		const bool bRead = CurrentFile ? FWorkspaceReadPathScope::ReadBytes(CurrentFile, Bytes)
			: !IsReparsePoint(Path) && FFileHelper::LoadFileToArray(Bytes, *Path);
		const FString ByteHash = bRead ? Sha256(Bytes) : FString();
		TSharedPtr<const FWorkspaceTextEncodingProof> Encoding;
		if (bRead) Encoding = ByteHash == File->Hash
			? MeasureCanonicalWorkspaceTextEncoding(Bytes, ByteHash)
			: ObserveWorkspaceTextEncoding(Bytes, ByteHash);
		const bool bSame = Encoding && Encoding->GetTextHash() == File->Hash;
		if (bSame && Observation && (CurrentFile ? Observation->IsCurrentHandle(CurrentFile) : Observation->IsCurrent())) {
			File->Proof = Observation;
			RememberObservedSemanticSourceFile(State->Root, Path, Bytes, ByteHash, MoveTemp(Observation), MoveTemp(Encoding));
		}
		return bSame
			|| Fail(Error, TEXT("workspace_source_version_changed"),
				FString::Printf(TEXT("Stage=source.content_checkpoint; path=%s; body_read=%s; proof_present=%s; current_guard=%s. Current text does not match the exact resolved input; it was not consumed or replaced."),
					*Path, bRead ? TEXT("true") : TEXT("false"), File->Proof ? TEXT("true") : TEXT("false"), CurrentFile ? TEXT("true") : TEXT("false")), Path);
	}
	bool FResolvedWorkspaceSourceProof::ValidateRemaining(const TArray<FString>& ObservedPaths,
		const FWorkspaceReadPathScope* PathScope, FWorkspaceError& Error) const
	{
		if (State->CapturedReferencePaths && !State->CapturedReferencePaths->IsCurrent())
			return Fail(Error, TEXT("workspace_source_version_changed"),
				TEXT("The original addressed-reference workspace root no longer names the accepted directory."), State->Root);
		// The real namespace enumeration already verified every observed HJSON
		// file using its current handle. Only other actual inputs remain here.
		for (const auto& File : State->Files) {
			if (Algo::BinarySearch(ObservedPaths, File.Key) != INDEX_NONE) continue;
			if (PathScope) {
				if (!PathScope->Observe(File.Key, [&](void* Handle, int64, int64) { return ValidateFile(File.Key, Handle, Error); })) {
					if (Error.HasErrors()) return false; // Preserve the real byte/consumer error, rather than claiming deletion.
					return Fail(Error, TEXT("workspace_source_version_changed"), FString::Printf(
						TEXT("Stage=source.content_checkpoint; path=%s. Current logical file observation failed; the OS stage/error is recorded in the source path diagnostic. No deletion or successful byte comparison was inferred."), *File.Key), File.Key);
				}
			}
			else if (!ValidateFile(File.Key, nullptr, Error)) return false;
		}
		return true;
	}
	bool FResolvedWorkspaceSourceProof::ValidateAll(FWorkspaceError& Error) const
	{
		FWorkspaceReadPathScope Scope(State->Root);
		if (Scope.IsRejected()) return Fail(Error, TEXT("workspace_reparse_point_rejected"),
			TEXT("The workspace root is no longer an ordinary directory."), State->Root);
		const bool bValid = ValidateRemaining({}, Scope.IsAvailable() ? &Scope : nullptr, Error)
			&& (!Scope.IsAvailable() || Scope.IsCurrent()
				|| Fail(Error, TEXT("workspace_source_version_changed"), TEXT("The workspace root binding changed during source validation."), State->Root));
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=source_content_checkpoint files=%d current_handles_reused=%d current_handles_opened=%d success=%s"),
			State->Files.Num(), Scope.GetReusedCurrentHandles(), Scope.GetOpenedCurrentHandles(), bValid ? TEXT("true") : TEXT("false"));
		return bValid;
	}

	bool TryGetResolvedWorkspaceSourceProof(const FString& WorkspaceRoot,
		TSharedPtr<const FResolvedWorkspaceSourceProof>& OutProof, FWorkspaceError& OutError)
	{
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (!ActiveReadState || ActiveReadState->ResolvedRoot != Root
			|| !ActiveReadState->ResolvedWorkspace)
			return Fail(OutError, TEXT("workspace_source_proof_missing"),
				TEXT("Workspace publication requires a successful resolution of its current source."), Root);
		if (ActiveReadState->SourceProof) { OutProof = ActiveReadState->SourceProof; return true; }
		if (!ActiveReadState->ResolvedWorkspace)
			return Fail(OutError, TEXT("workspace_source_proof_missing"), TEXT("Publication inputs have no current source proof."), Root);
		auto ProofState = MakeShared<FResolvedWorkspaceSourceProof::FState>();
		ProofState->Root = Root;
		if (ActiveReadState->CapturedReferenceRoot == Root) ProofState->CapturedReferencePaths = ActiveReadState->CapturedReferencePaths;
		for (const auto& Document : ActiveReadState->ResolvedHashOnlyDocuments)
			RecordObservedSourceProofFile(*ProofState, Document.Key, Document.Value);
		for (const auto& Document : ActiveReadState->ResolvedDocuments) {
			const auto* Read = ActiveReadState->Documents.Find(Document.Key);
			if (!Read || Read->Parsed != Document.Value)
				return Fail(OutError, TEXT("workspace_source_proof_missing"), TEXT("A resolved source document is unavailable."), Document.Key);
			RecordObservedSourceProofFile(*ProofState, Document.Key, Read->ContentHash);
		}
		ActiveReadState->SourceProof = MakeShared<FResolvedWorkspaceSourceProof>(ProofState);
		OutProof = ActiveReadState->SourceProof;
		return true;
	}
	bool TryMakeSemanticWorkspaceSourceCheckpoint(const FString& WorkspaceRoot,
		FHjsonOperationCheckpoint& OutCheckpoint, FWorkspaceError& OutError)
	{
		TSharedPtr<const FResolvedWorkspaceSourceProof> Proof;
		if (!TryGetResolvedWorkspaceSourceProof(WorkspaceRoot, Proof, OutError)) return false;
		OutCheckpoint = [Proof](const FString&, FWorkspaceError& Error) { return Proof->ValidateAll(Error); };
		return true;
	}

	bool TryResolveKeyedPropertyValues(const FHjsonNode& Row, const FString& ExpectedType,
		const TArray<FString>& TemplateFields, FUeAgentResourceTracker& Resources,
		TMap<FString, FUeAgentCanonicalValueRef>& OutValues, FWorkspaceError& OutError,
		const TMap<FString, FString>* PublicRefs, FHjsonNodePtr* OutValueSources)
	{
		OutValues.Reset();
		if (OutValueSources) OutValueSources->Reset();
		const auto* Type = Row.Get(TEXT("type"));
		const auto* Value = Row.Get(TEXT("value"));
		if (!Type || Type->GetKind() != EHjsonNodeKind::String)
			return FailField(OutError, TEXT("semantic_workspace_property_type_missing"), TEXT("The property requires a string type field."),
				Row, TEXT("type"), MakeShared<FJsonValueString>(TEXT("string")));
		if (!Value)
			return FailField(OutError, TEXT("semantic_workspace_property_value_missing"), TEXT("The property requires a value field."), Row, TEXT("value"));
		if (!Type->GetText().TrimStartAndEnd().Equals(ExpectedType.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			return FailNode(OutError, TEXT("semantic_workspace_property_type_mismatch"),
				TEXT("The authored property type differs from its declared type."), *Type, MakeShared<FJsonValueString>(ExpectedType));
		TMap<FString, FUeAgentCanonicalValueRef> Result;
		// The same owner that restores native aliases also binds those aliases
		// to their original parser nodes. Only the source tree is synthesized.
		const FHjsonNodeRef* ValueRef = nullptr;
		for (const auto& Field : Row.GetEntries())
			if (Field.Key == TEXT("value")) { ValueRef = &Field.Value; break; }
		TArray<TPair<FString, FHjsonNodeRef>> SourceFields;
		// Current exports use the native type. Legacy captured workspaces may still
		// carry a definition alias here; preserve their captured text encoding.
		const bool bTextType = ExpectedType == TEXT("text")
			|| ExpectedType.StartsWith(TEXT("text#"));
		if (bTextType && Value->GetKind() == EHjsonNodeKind::String)
		{
			FUeAgentCanonicalValuePtr Parsed;
			if (!TryCanonicalValue(*Value, Resources, Parsed, OutError, PublicRefs)) return false;
			// Pin defaults capture text in a direct value wrapper; native FText
			// properties use literal/localized. Preserve the captured owner shape.
			const FString TextField = TemplateFields.Num() == 1
				&& TemplateFields[0] == TEXT("value") ? TEXT("value") : TEXT("literal");
			Result.Add(TextField, Parsed.ToSharedRef());
			if (OutValueSources && ValueRef) SourceFields.Emplace(TextField, *ValueRef);
		}
		else if (!bTextType && TemplateFields.Num() == 1
			&& UeAgentAuthoringTypeDescriptors::IsDirectPropertyValueWrapper(TemplateFields[0])
			&& !Value->Get(TemplateFields[0])
			&& !(TemplateFields[0] == TEXT("path") && Value->Get(TEXT("owned_object"))))
		{
			// This is the captured PropertySet's outer values envelope, never a
			// recursive business object. Native descendants are restored by the
			// type-aware semantic overlay; this boundary consumes no nested names.
			FUeAgentCanonicalValuePtr Parsed;
			if (!TryCanonicalValue(*Value, Resources, Parsed, OutError, PublicRefs)) return false;
			Result.Add(TemplateFields[0], Parsed.ToSharedRef());
			if (OutValueSources && ValueRef) SourceFields.Emplace(TemplateFields[0], *ValueRef);
		}
		else
		{
			if (Value->GetKind() != EHjsonNodeKind::Object)
				return FailNode(OutError, TEXT("semantic_workspace_unit_values_invalid"),
					TEXT("A complex property value must be an object."), *Value, MakeShared<FJsonValueString>(TEXT("object")));
			for (const auto& Field : Value->GetEntries())
			{
				FUeAgentCanonicalValuePtr Parsed;
				if (Result.Contains(Field.Key))
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_unit_values_invalid"),
						TEXT("The semantic values contain a duplicate field name."), TEXT("workspace_resolve"));
					Diagnostic.CandidateName = Field.Key;
					return FailAtNode(OutError, MoveTemp(Diagnostic), *Field.Value);
				}
				if (!TryCanonicalValue(*Field.Value, Resources, Parsed, OutError, PublicRefs)) return false;
				Result.Add(Field.Key, Parsed.ToSharedRef());
				if (OutValueSources) SourceFields.Emplace(Field.Key, Field.Value);
			}
		}
		OutValues = MoveTemp(Result);
		if (OutValueSources)
		{
			// Existing document bindings may name /value; retain that alias too.
			if (ValueRef && !SourceFields.ContainsByPredicate([](const auto& Field) { return Field.Key == TEXT("value"); }))
				SourceFields.Emplace(TEXT("value"), *ValueRef);
			*OutValueSources = MakeShared<FHjsonNode>(EHjsonNodeKind::Object, Row.GetProvenance(),
				FString(), FString(), false, TArray<FHjsonNodeRef>(), MoveTemp(SourceFields));
		}
		return true;
	}



	bool TryProjectHjsonCanonicalValue(const FHjsonNode& Root, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError, bool bInterpretSemanticValues)
	{
		return TryCanonicalValue(Root, Resources, OutValue, OutError, nullptr, bInterpretSemanticValues);
	}

	bool TryResolveSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		return TryResolveSemanticWorkspaceInternal(
			Source,
			ExpectedMode,
			false,
			Resources,
			OutWorkspace,
			OutError);
	}

	bool TryResolveEditedSemanticWorkspace(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError,
		TSharedPtr<const FResolvedWorkspaceSourceProof>* OutUnchangedImportProof)
	{
		return TryResolveSemanticWorkspaceInternal(
			Source,
			ExpectedMode,
			true,
			Resources,
			OutWorkspace,
			OutError, OutUnchangedImportProof);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSemanticLayoutDestinationIsolationTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.DestinationIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutDestinationIsolationTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;
	ResetCompiledAuthoringReuse();
	ON_SCOPE_EXIT { ResetCompiledAuthoringReuse(); };
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	const FString Target(TEXT("target:authoring_target"));
	const auto MakeModel = [&](const FString& Label, int32 ItemCount, FAuthoringModelPtr& Out) {
		TArray<FSemanticUnit> Units; TArray<FString> Members;
		for (int32 I = 0; I < ItemCount; ++I) Members.Add(TEXT("item:") + Label + FString::FromInt(I));
		Units.Emplace(Target, ESemanticUnitKind::TargetState, FString(), TEXT("ue.fixture.asset"),
			TEXT("authoring_target"), FString(), TMap<FString, FUeAgentCanonicalValueRef>(), Members,
			TArray<FString>(), FString(), FString(), TArray<FString>(), TArray<FString>{TEXT("state")},
			TEXT("fixture:") + Label, TEXT("/Fixture/") + Label);
		for (const auto& Id : Members) Units.Emplace(Id, ESemanticUnitKind::ItemState, Target,
			TEXT("uai.fixture.item"), TEXT("fixture_item"), FString(), TMap<FString, FUeAgentCanonicalValueRef>(),
			TArray<FString>(), TArray<FString>(), FString(), FString(), TArray<FString>(),
			TArray<FString>{TEXT("state")}, TEXT("fixture:") + Label);
		return TryCompileAuthoringModel(TEXT("uai.fixture.destination"), 1, TEXT("state"), Label,
			MoveTemp(Units), {Target}, Resources, Out, Error);
	};
	FAuthoringModelPtr First, Current;
	if (!TestTrue(TEXT("first fixture compiles"), MakeModel(TEXT("first"), 1, First))) return false;
	FWorkspaceLayoutPlanPtr FirstLayout;
	if (!TestTrue(TEXT("first real allocator output"), TryPlanWorkspaceLayout(First.ToSharedRef(), 2,
		EWorkspaceSourceMode::Directory, {}, Resources, FirstLayout, Error))) return false;
	class FCountingValues final : public FResolvedWorkspaceUnitBindings {
	public:
		FAuthoringModelRef Values; mutable int32 Reads = 0;
		explicit FCountingValues(FAuthoringModelRef In) : Values(MoveTemp(In)) {}
		const FSourceProvenance* FindSource(const FString&) const override { return nullptr; }
		const FSemanticUnit* FindBaseline(const FString& Id) const override { ++Reads; return Values->FindUnit(Id); }
		FHjsonDocumentPtr FindSourceDocument(const FString&) const override { return nullptr; }
		void CopySources(TMap<FString, FSourceProvenance>&) const override {}
		void CopyBaselines(TMap<FString, FSemanticUnit>&) const override { ++Reads; }
	};
	const auto OldValues = MakeShared<FCountingValues>(First.ToSharedRef());
	const auto Directory = FAuthoringModel::MakePublicationDirectory(*First, OldValues);
	auto Retired = FirstLayout->WithPublicationDirectory(Directory);
	OldValues->Reads = 0;
	if (!TestTrue(TEXT("different asset with the same local target ID compiles"), MakeModel(TEXT("current"), 2, Current))) return false;
	class FCurrentBindings final : public FSemanticProjectionBindings {
		FAuthoringModelRef Current;
	public:
		explicit FCurrentBindings(FAuthoringModelRef In) : Current(MoveTemp(In)) {}
		const FAuthoringModel& GetModel() const override { return Current.Get(); }
		bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)>) const override { return true; }
	} Bindings(Current.ToSharedRef());
	// Pin the former automatic selection explicitly to reproduce its real
	// membership/name consumer. No parser, file IO or UE object is substituted.
	FWorkspaceLayoutPlanPtr Witness;
	if (!TestTrue(TEXT("former selected-layout consumer runs"), TryPlanWorkspaceLayout(Current.ToSharedRef(), 2,
		EWorkspaceSourceMode::Directory, {}, Resources, Witness, Error, &Bindings, nullptr, &Retired.Get()))) return false;
	const int32 ForeignReads = OldValues->Reads;
	TestTrue(TEXT("old selected layout really demands foreign values"), ForeignReads > 0);
	if (!TestTrue(TEXT("seed the first destination's real last-layout memo again"), TryPlanWorkspaceLayout(First.ToSharedRef(), 2,
		EWorkspaceSourceMode::Directory, {}, Resources, FirstLayout, Error))) return false;
	Retired = FirstLayout->WithPublicationDirectory(Directory);
	OldValues->Reads = 0;
	FWorkspaceLayoutPlanPtr Result;
	if (!TestTrue(TEXT("ordinary layout uses its own bindings"), TryPlanWorkspaceLayout(Current.ToSharedRef(), 2,
		EWorkspaceSourceMode::Directory, {}, Resources, Result, Error, &Bindings))) return false;
	TestEqual(TEXT("no previous-destination value read"), OldValues->Reads, 0);
	TestEqual(TEXT("all current members retained"), Result->GetPhysicalPlacements().Num(), Current->GetUnitCount());
	for (const auto& Placement : Result->GetPhysicalPlacements())
		TestEqual(TEXT("same visible output without foreign value reads"), Placement.Value,
			Witness->GetPhysicalPlacements().FindRef(Placement.Key));
	AddInfo(FString::Printf(TEXT("original membership consumer foreign_value_reads=%d; ordinary destination reads=%d; current_units=%d"),
		ForeignReads, OldValues->Reads, Current->GetUnitCount()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentSourceCheckpointTest,
	"UeAgentInterface.Core.HjsonWorkspace.SourceCheckpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSourceCheckpointTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ArtifactPublisher;
	const FString ExistingRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_CHECKPOINT_WORKSPACE"));
	if (!ExistingRoot.IsEmpty()) {
		// Read the existing formal Source directly, without native capture,
		// formatter, publisher, source-file mutation or an alternate reader.
		FSemanticWorkspaceReadScope Scope; FWorkspaceError Error; auto Resources = MakeSemanticWorkspaceResourceTracker();
		const double Started = FPlatformTime::Seconds(); FResolvedSemanticWorkspacePtr Source;
		if (!BeginExplicitWorkspaceFileObservation(ExistingRoot, Error)
			|| !TryResolveEditedSemanticWorkspace(FWorkspaceSource::Directory(ExistingRoot), ESemanticWorkspaceMode::State, Resources, Source, Error)) {
			AddError(Error.GetDiagnostic().PublicMessage); return false;
		}
		TSharedPtr<const FResolvedWorkspaceSourceProof> Proof;
		if (!TryGetResolvedWorkspaceSourceProof(ExistingRoot, Proof, Error)) { AddError(Error.GetDiagnostic().PublicMessage); return false; }
		if (FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_CHECKPOINT_RETAINED_HASHES")) == TEXT("1")) {
			FHjsonOperationCheckpoint Accepted;
			if (!TryMakeSemanticWorkspaceSourceCheckpoint(ExistingRoot, Accepted, Error)
				|| !Accepted(TEXT("before_test_source_consumer"), Error)) { AddError(Error.GetDiagnostic().PublicMessage); return false; }
			AcceptResolvedSemanticWorkspace(Source, MoveTemp(Accepted));
			FString Consumer, Owner; FArtifactPublishError Failure; TMap<FString, FString> Retained;
			if (!TryReadWorkspaceConsumerState(ExistingRoot, FString(), Consumer, Owner, Failure, &Retained)) { AddError(Failure.PublicMessage); return false; }
			FString Root = FPaths::ConvertRelativePathToFull(ExistingRoot); FPaths::NormalizeDirectoryName(Root);
			const FString Guide = Root / TEXT("examples/index.hjson");
			TestTrue(TEXT("fixture has a real unrelated generated guide"), Retained.Contains(TEXT("examples/index.hjson")));
			TestFalse(TEXT("Source did not read unrelated guide"), ObservedSourceFiles.Contains(Guide));
			const int32 ObservationsBefore = ObservedSourceFiles.Num();
			if (!TryUpdateAcceptedRetainedFileHashes(Root, Retained, Error)) { AddError(Error.GetDiagnostic().PublicMessage); return false; }
			TestFalse(TEXT("retained header does not turn guide ownership into a Source read"), ObservedSourceFiles.Contains(Guide));
			AddInfo(FString::Printf(TEXT("actual retained Source consumer retained_files=%d added_observations=%d unrelated_guide_read=false native_capture=false input_writes=0"),
				Retained.Num(), ObservedSourceFiles.Num() - ObservationsBefore));
		}
		if (FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_CHECKPOINT_PUBLISHED_READS")) == TEXT("1")) {
			FString Consumer, Owner; FArtifactPublishError Failure; TMap<FString, FString> Published;
			if (!TryReadWorkspaceConsumerState(ExistingRoot, FString(), Consumer, Owner, Failure, &Published)) { AddError(Failure.PublicMessage); return false; }
			int32 ReadFiles = 0;
			for (const auto& File : Published) if (File.Key.StartsWith(TEXT(".uai/baselines/"), ESearchCase::CaseSensitive)) {
				TArray<uint8> Bytes;
				if (!TryReadPublishedWorkspaceBytes(ExistingRoot, File.Key, File.Value, Bytes, Error)) { AddError(Error.GetDiagnostic().PublicMessage); return false; }
				++ReadFiles;
			}
			AddInfo(FString::Printf(TEXT("original published-byte consumer appended baseline_files=%d native_capture=false input_writes=0"), ReadFiles));
		}
		const double Resolved = FPlatformTime::Seconds();
		ResetWorkspaceFileProofs(); // Only this owned worker: optional leases must not be business qualifications.
		FGenerationFileGuard CurrentGuard; FArtifactPublishError GuardError; FString GuardedPath;
		FWorkspaceCurrentFileScope Current([&](const FString& Path) -> void* {
			if (GuardedPath != Path) { CurrentGuard.Close(); GuardedPath = Path;
				TArray<uint8> ReadBeforeCheckpoint;
				if (!CurrentGuard.Open(Path, false, false, GuardError) || !CurrentGuard.Read(ReadBeforeCheckpoint, GuardError)) return nullptr; }
			return CurrentGuard.GetCurrentObservationHandle(Path);
		});
		const bool Valid = Proof->ValidateAll(Error);
		AddInfo(FString::Printf(TEXT("actual Source to checkpoint: source_s=%.6f checkpoint_s=%.6f valid=%s native_capture=false file_writes=0 lease_reset=owned_worker_only"),
			Resolved - Started, FPlatformTime::Seconds() - Resolved, Valid ? TEXT("true") : TEXT("false")));
		if (!Valid) AddError(Error.GetDiagnostic().PublicMessage);
		return Valid;
	}
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/uai-tests/checkpoint-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FPaths::NormalizeDirectoryName(Root); const FString Path = Root / TEXT("source.hjson");
	if (!TestTrue(TEXT("create owned file fixture"), IFileManager::Get().MakeDirectory(*Root, true))) return false;
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); IFileManager::Get().DeleteDirectory(*Root, false, false); };
	const FString Before(TEXT("{ value: false }\n")), After(TEXT("{ value: true }\n"));
	if (!TestTrue(TEXT("write fixture input"), FFileHelper::SaveStringToFile(Before, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	auto State = MakeShared<FResolvedWorkspaceSourceProof::FState>(); State->Root = Root;
	State->Files.Add(Path, {Sha256Text(Before), nullptr}); FResolvedWorkspaceSourceProof Proof(State); FWorkspaceError Error;
	TestTrue(TEXT("unchanged actual bytes with no optional lease"), Proof.ValidateAll(Error));
	TestTrue(TEXT("checkpoint hands its actual byte proof to its next consumer"), State->Files.FindChecked(Path).Proof.IsValid());
	{
		FGenerationFileGuard Guard; FArtifactPublishError Failure;
		if (!TestTrue(TEXT("same original publisher read guard"), Guard.Open(Path, false, false, Failure))) return false;
		TArray<uint8> ReadBeforeCheckpoint;
		if (!TestTrue(TEXT("publisher read before source checkpoint"), Guard.Read(ReadBeforeCheckpoint, Failure))) return false;
		FWorkspaceCurrentFileScope Current([&](const FString& Requested) { return Guard.GetCurrentObservationHandle(Requested); });
		TestTrue(TEXT("source body read from borrowed publisher guard with no lease"), Proof.ValidateAll(Error));
	}
	if (!TestTrue(TEXT("write independent changed input"), FFileHelper::SaveStringToFile(After, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	Error.Reset(); TestFalse(TEXT("real byte change rejected"), Proof.ValidateAll(Error));
	TestTrue(TEXT("consumer mismatch retains actual body/path diagnosis"), Error.GetDiagnostic().PublicMessage.Contains(Path)
		&& Error.GetDiagnostic().PublicMessage.Contains(TEXT("body_read=true")));
	IFileManager::Get().Delete(*Path); Error.Reset(); TestFalse(TEXT("missing actual file rejected"), Proof.ValidateAll(Error));
	TestTrue(TEXT("missing file diagnosis names precise path"), Error.GetDiagnostic().PublicMessage.Contains(Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPublishedInventoryProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.PublishedInventoryProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPublishedInventoryProjectionTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	FSemanticWorkspaceReadScope Scope;
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	// Exercise the same metadata string policy used by both the legacy owned
	// result and the borrowed row fields. Error output mutation is contractual.
	FHjsonNodePtr Strings;
	if (!TryParseHjsonSelectedNodes(TEXT("{ present: \" \\t中文 \\t\", empty: \" \\t\", wrong: 7 }"),
		TEXT("metadata-strings.hjson"), Resources, Strings, Error, [](const FString&, const FString&) { return true; })) { AddError(TEXT("metadata string fixture: ") + Error.GetCode()); return false; }
	FString Owned(TEXT("keep")); FStringView Borrowed;
	TestTrue(TEXT("owned metadata string retains trim policy"), RequiredString(*Strings, TEXT("present"), Owned, Error));
	TestEqual(TEXT("owned trimmed value"), Owned, FString(TEXT("中文")));
	TestTrue(TEXT("borrowed metadata string uses same validator"), ReadMetadataStringNode(*Strings, TEXT("present"), Strings->Get(TEXT("present")), true, Borrowed, Error));
	TestTrue(TEXT("borrowed trimmed value"), Borrowed.Equals(FStringView(TEXT("中文")), ESearchCase::CaseSensitive));
	TestFalse(TEXT("required empty remains an error"), RequiredString(*Strings, TEXT("empty"), Owned, Error));
	TestTrue(TEXT("required empty clears output"), Owned.IsEmpty());
	TestEqual(TEXT("empty diagnostic"), Error.GetCode(), FString(TEXT("semantic_workspace_manifest_field_invalid")));
	Owned = TEXT("keep");
	TestFalse(TEXT("required wrong type remains an error"), RequiredString(*Strings, TEXT("wrong"), Owned, Error));
	TestEqual(TEXT("required wrong type preserves output"), Owned, FString(TEXT("keep")));
	TestFalse(TEXT("optional wrong type remains an error"), OptionalString(*Strings, TEXT("wrong"), Owned, Error));
	TestTrue(TEXT("optional wrong type clears output"), Owned.IsEmpty());
	Owned = TEXT("keep");
	TestFalse(TEXT("required missing remains an error"), RequiredString(*Strings, TEXT("missing"), Owned, Error));
	TestEqual(TEXT("required missing diagnostic"), Error.GetCode(), FString(TEXT("semantic_workspace_manifest_field_missing")));
	TestEqual(TEXT("required missing preserves output"), Owned, FString(TEXT("keep")));
	TestTrue(TEXT("optional absence remains legal"), OptionalString(*Strings, TEXT("missing"), Owned, Error));
	TestTrue(TEXT("optional absence clears output"), Owned.IsEmpty());
	ESemanticUnitKind ParsedKind = ESemanticUnitKind::WorkspaceIndex;
	TestTrue(TEXT("existing case-insensitive kind parsing retained"), ParseUnitKind(FStringView(TEXT("PROPERTY_SET")), ParsedKind));
	TestTrue(TEXT("kind is actual enum value"), ParsedKind == ESemanticUnitKind::PropertySet);
	const FString CanonicalHash = TEXT("sha256:") + FString::ChrN(64, TEXT('a'));
	const FString OpaqueHash = TEXT("SHA256:") + FString::ChrN(64, TEXT('A'));
	const FString RowText = FString::Printf(TEXT("{ unit_inventory: ["
		"{ unit_id: \"u:a\", unit_kind: \"property_set\", public_ref: \"p:a\", logical_path: \"targets/a\", document_path: \"targets/doc.hjson\", unit_pointer: \"/a\", content_hash: \" %s \", envelope_ref: \".uai/baselines/a.hjson\" },"
		"{ unit_id: \"u:b\", unit_kind: \"PROPERTY_SET\", public_ref: \"p:b\", logical_path: \"targets/b\", document_path: \"targets/doc.hjson\", unit_pointer: \"/b\", content_hash: \" %s \", envelope_ref: \".uai/baselines/a.hjson\" } ] }"), *CanonicalHash, *OpaqueHash);
	FHjsonNodePtr RowRoot; TArray<FInventoryRow> TypedRows;
	const int64 ReusesBefore = ActiveReadState->InventoryValidatedPathReuses;
	if (!TryParseHjsonSelectedNodes(RowText, TEXT("metadata-rows.hjson"), Resources, RowRoot, Error,
		[](const FString&, const FString&) { return true; }) || !ParseInventory(*RowRoot, TypedRows, Error)) { AddError(TEXT("inventory row fixture: ") + Error.GetCode()); return false; }
	if (!TestEqual(TEXT("both actual rows consumed"), TypedRows.Num(), 2)) return false;
	TestTrue(TEXT("canonical digest decoded directly"), TypedRows[0].Syntax->ContentHash.bCompact);
	TestTrue(TEXT("canonical digest exact spelling"), TypedRows[0].Syntax->ContentHash.ToString().Equals(CanonicalHash, ESearchCase::CaseSensitive));
	TestFalse(TEXT("opaque digest remains opaque"), TypedRows[1].Syntax->ContentHash.bCompact);
	TestTrue(TEXT("opaque digest exact original spelling"), TypedRows[1].Syntax->ContentHash.ToString().Equals(OpaqueHash, ESearchCase::CaseSensitive));
	TestTrue(TEXT("same validated path owner transferred"), TypedRows[0].Syntax->EnvelopeRef.Get() == TypedRows[1].Syntax->EnvelopeRef.Get());
	TestEqual(TEXT("normalization not repeated for exact same input"), ActiveReadState->InventoryValidatedPathReuses - ReusesBefore, int64(1));
	RowRoot.Reset();
	TestEqual(TEXT("owned row facts outlive borrowed syntax"), TypedRows[1].Syntax->GetDocumentPath(), FString(TEXT("targets/doc.hjson")));
	const FString FixtureRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_SPAN_WORKSPACE"));
	const FString FixtureDocument = FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_SPAN_DOCUMENT"));
	FString Root = FixtureRoot, Path, Text;
	const bool bOwned = Root.IsEmpty();
	if (bOwned) {
		Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/uai-tests/inventory-projection-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Path = GetPublishedMachineEnvelopePath(TEXT("targets/a.hjson"), true).Replace(TEXT("/baselines/"), TEXT("/inventory/"));
		const FString Kind = LexToString(ESemanticUnitKind::PropertySet);
		Text = FString::Printf(TEXT("{ unit_inventory: [{ unit_id: \"unit:中文\", unit_kind: \"%s\", public_ref: \"item/a/value\", logical_path: \"targets/a/value\", document_path: \"targets/a.hjson\", unit_pointer: \"/value\", content_hash: \"hash-a\", edit_hash: \"edit-a\", envelope_ref: \".uai/baselines/fixture.hjson\", envelope_span: [0, 64], content_files: [] }] }"), *Kind);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root / Path), true);
		if (!FFileHelper::SaveStringToFile(Text, *(Root / Path), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	} else {
		if (!TestFalse(TEXT("real fixture document supplied"), FixtureDocument.IsEmpty())) return false;
		Path = GetPublishedMachineEnvelopePath(FixtureDocument, true).Replace(TEXT("/baselines/"), TEXT("/inventory/"));
		if (!FFileHelper::LoadFileToString(Text, *(Root / Path))) return false;
	}
	ON_SCOPE_EXIT { if (bOwned) { IFileManager::Get().Delete(*(Root / Path)); IFileManager::Get().DeleteDirectory(*Root, false, true); } };
	FHjsonNodePtr Oracle;
	if (!TryParseHjsonSelectedNodes(Text, Path, Resources, Oracle, Error, [](const FString&, const FString&) { return true; })) return false;
	const auto* OriginalRows = Oracle->Get(TEXT("unit_inventory"));
	if (!TestTrue(TEXT("original complete-parser rows"), OriginalRows && OriginalRows->GetKind() == EHjsonNodeKind::Array)) return false;
	TArray<FResolvedWorkspaceInventory::FRow> Rows;
	const int64 SkippedBefore = ActiveReadState->PublishedInventoryScalarNodesSkipped;
	const int64 TextsBefore = ActiveReadState->PublishedInventoryBorrowedTokenTexts;
	const int64 LexemesBefore = ActiveReadState->PublishedInventoryBorrowedTokenLexemes;
	const int64 KeysBefore = ActiveReadState->PublishedInventoryBorrowedKeys;
	const int64 SpanNodesBefore = ActiveReadState->PublishedInventorySpanNodesSkipped;
	if (!TestTrue(TEXT("published row-only production reader"), ReadPublishedInventoryDocument(Root, Path, Sha256Text(Text), Resources, Rows, Error))) { AddError(Error.GetCode()); return false; }
	if (bOwned) {
		TestEqual(TEXT("nine strings and two span integers produced no scalar AST"), ActiveReadState->PublishedInventoryScalarNodesSkipped - SkippedBefore, int64(11));
		TestEqual(TEXT("nine projected plain texts borrowed the original input"), ActiveReadState->PublishedInventoryBorrowedTokenTexts - TextsBefore, int64(9));
		TestEqual(TEXT("all projected raw lexemes borrowed the original input"), ActiveReadState->PublishedInventoryBorrowedTokenLexemes - LexemesBefore, int64(11));
		TestEqual(TEXT("success object keys borrowed original tokens"), ActiveReadState->PublishedInventoryBorrowedKeys - KeysBefore, int64(9));
		TestEqual(TEXT("same parser directly supplied both span endpoints"), ActiveReadState->PublishedInventorySpanNodesSkipped - SpanNodesBefore, int64(2));
		}
	TestEqual(TEXT("every real row handed to the directory"), Rows.Num(), OriginalRows->GetItems().Num());
	{
		FResolvedWorkspaceInventory Inventory; Inventory.Rows = Rows;
		FAcceptedWorkspaceContentHashView Saved;
		TSharedPtr<const FInventoryRowSyntax> HeldSyntax;
		if (!Inventory.Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
			if (!Saved.IsBound()) { Saved = Row.ContentHashView; HeldSyntax = Row.SourceSyntax; }
			return TestTrue(TEXT("actual parsed provider binds an owned digest view"), Row.ContentHashView.IsBound());
		})) return false;
		Inventory.Rows.Reserve(Inventory.Rows.Num() + 32);
		TestTrue(TEXT("digest view outlives stack callback and array growth"), Saved.IsBound() && HeldSyntax
			&& Saved.Matches(HeldSyntax->ContentHash.ToString()));
		TestEqual(TEXT("view keeps original canonical-versus-opaque distinction"), Saved.Canonical(),
			HeldSyntax->ContentHash.bCompact ? HeldSyntax->ContentHash.ToString() : FString());
		int32 UnexpectedRows = 0;
		TestTrue(TEXT("empty document selection was successful, not a nonempty proof"),
			Inventory.VisitDocument(TEXT("not/an/inventory/document"), [&](const auto&) { ++UnexpectedRows; return true; }));
		TestEqual(TEXT("empty successful selection emitted no row"), UnexpectedRows, 0);

	}
	{
		// Exercise the actual cold directory loader, not a stand-in readiness flag.
		FHjsonNodePtr PreparationSyntax;
		const FString Header = TEXT("{ files: [") + FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Path) + TEXT("] }");
		if (!TryParseHjsonSelectedNodes(Header, TEXT("metadata-readiness-header"), Resources, PreparationSyntax, Error,
			[](const FString&, const FString&) { return true; })) return false;
		FPublishedWorkspaceDirectory Directory;
		Directory.Root = Root; Directory.PublishedHashes.Add(Path, Sha256Text(Text));
		Directory.Publication.ScopedMachineBuckets = true; // The fixture uses the real scoped shard naming contract.
		Directory.InventoryEntries.Add(&PreparationSyntax->Get(TEXT("files"))->GetItems()[0].Get());
		const int32 Queries = ActiveReadState->PublishedInventoryDocumentQueries;
		const int64 Slots = ActiveReadState->PublishedInventoryDocumentSlots;
		FString FailedDocument;
		if (!TestTrue(TEXT("same directory metadata loader prepares the accepted shard"), Directory.PrepareDocumentsMetadata({Rows[0].Syntax->GetDocumentPath()}, FailedDocument))) {
			AddError(Directory.ReadError.GetCode()); return false;
		}
		TestEqual(TEXT("preparation produced all original parsed rows"), Directory.Loaded.Rows.Num(), Rows.Num());
		TestEqual(TEXT("preparation invoked no document row visitor"), ActiveReadState->PublishedInventoryDocumentQueries, Queries);
		TestEqual(TEXT("preparation materialized no row-visitor slots"), ActiveReadState->PublishedInventoryDocumentSlots, Slots);
	}
	if (bOwned) {
		const FString FirstDocument = TEXT("targets/parallel-a/state.hjson"), SecondDocument = TEXT("targets/parallel-b/state.hjson");
		const FString FirstPath = GetPublishedMachineEnvelopePath(FirstDocument, true).Replace(TEXT("/baselines/"), TEXT("/inventory/"));
		const FString SecondPath = GetPublishedMachineEnvelopePath(SecondDocument, true).Replace(TEXT("/baselines/"), TEXT("/inventory/"));
		if (!TestNotEqual(TEXT("independent source parser inputs use different physical shards"), FirstPath, SecondPath)) return false;
		const FString FirstText = Text.Replace(TEXT("unit_kind: \"property_set\""), TEXT("unit_kind: \"not-a-kind\""));
		const FString SecondText = TEXT("{ unit_inventory: [");
		if (!TestNotEqual(TEXT("the first row has a real semantic error"), FirstText, Text)) return false;
		for (const auto& Input : TArray<TPair<FString, FString>>{{FirstPath, FirstText}, {SecondPath, SecondText}}) {
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root / Input.Key), true);
			if (!FFileHelper::SaveStringToFile(Input.Value, *(Root / Input.Key), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		TArray<FResolvedWorkspaceInventory::FRow> InvalidRows; FWorkspaceError FirstError, SecondError;
		TestFalse(TEXT("original first input fails in row semantics"), ReadPublishedInventoryDocument(Root, FirstPath, Sha256Text(FirstText), Resources, InvalidRows, FirstError));
		TestFalse(TEXT("original second input fails in syntax"), ReadPublishedInventoryDocument(Root, SecondPath, Sha256Text(SecondText), Resources, InvalidRows, SecondError));
		TestNotEqual(TEXT("the two original stages have distinguishable diagnostics"), FirstError.GetCode(), SecondError.GetCode());
		TArray<FString> Paths{FirstPath, SecondPath}; Paths.Sort();
		const FString Header = TEXT("{ files: [") + FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Paths[0])
			+ TEXT(",") + FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Paths[1]) + TEXT("] }");
		FHjsonNodePtr HeaderSyntax;
		if (!TryParseHjsonSelectedNodes(Header, TEXT("ordered-parser-inputs"), Resources, HeaderSyntax, Error,
			[](const FString&, const FString&) { return true; })) return false;
		FPublishedWorkspaceDirectory Directory; Directory.Root = Root; Directory.Publication.ScopedMachineBuckets = true;
		Directory.PublishedHashes.Add(FirstPath, Sha256Text(FirstText)); Directory.PublishedHashes.Add(SecondPath, Sha256Text(SecondText));
		for (const auto& Item : HeaderSyntax->Get(TEXT("files"))->GetItems()) Directory.InventoryEntries.Add(&Item.Get());
		FString FailedDocument;
		TestFalse(TEXT("pure parser batches fail without choosing another parser"), Directory.PrepareDocumentsMetadata({FirstDocument, SecondDocument}, FailedDocument));
		TestEqual(TEXT("first original document remains authoritative"), FailedDocument, FirstDocument);
		TestEqual(TEXT("later lexical failure cannot mask the first document semantic error"), Directory.ReadError.GetCode(), FirstError.GetCode());
		TestTrue(TEXT("no failed shard is admitted"), Directory.LoadedShards.IsEmpty());
	}
	for (int32 I = 0; I < Rows.Num(); ++I) {
		const auto& Row = Rows[I]; const auto& Node = OriginalRows->GetItems()[I].Get();
		for (const auto& Pair : TArray<TPair<FString, FString>>{
			{TEXT("unit_id"), Row.Syntax->GetUnitId()}, {TEXT("public_ref"), Row.Syntax->GetPublicRef()},
			{TEXT("logical_path"), Row.Syntax->GetLogicalPath()}, {TEXT("document_path"), Row.Syntax->GetDocumentPath()},
			{TEXT("unit_pointer"), Row.Syntax->GetUnitPointer()}, {TEXT("envelope_ref"), Row.Syntax->GetEnvelopeRef()}}) {
			if (const auto* Original = Node.Get(Pair.Key)) TestEqual(*Pair.Key, Pair.Value, Original->GetText());
		}
		TestTrue(TEXT("same actual byte span"), Row.InventorySpan.Begin == Node.GetProvenance().GetByteStart()
			&& Row.InventorySpan.End == Node.GetProvenance().GetByteEnd());
		TestEqual(TEXT("actual ordinal preserved"), Row.InventoryOrdinal, I);
		if (const auto* Hash = Node.Get(TEXT("content_hash"))) TestTrue(TEXT("same content digest"), Row.Syntax->ContentHash.Matches(Hash->GetText()));
		if (const auto* Hash = Node.Get(TEXT("edit_hash"))) TestTrue(TEXT("same edit digest"), Row.Syntax->EditHash.Matches(Hash->GetText()));
		if (const auto* Span = Node.Get(TEXT("envelope_span"))) {
			int64 Begin = 0, End = 0;
			if (!ReadInventorySpanInteger(Span->GetItems()[0]->GetLexeme(), Begin)
				|| !ReadInventorySpanInteger(Span->GetItems()[1]->GetLexeme(), End)) return false;
			TestTrue(TEXT("same persisted baseline endpoint values"), Row.EnvelopeSpan.Begin == Begin && Row.EnvelopeSpan.End == End);
			}
	}
	TestFalse(TEXT("one-use full AST is not retained as a document"), ActiveReadState->Documents.Contains(Root / Path));
	if (!Rows.IsEmpty()) {
		const FString DocumentPath = bOwned ? TEXT("targets/a.hjson") : FixtureDocument;
		FHjsonNodePtr ManifestRoot;
		if (!TryParseHjsonSelectedNodes(TEXT("{ unit_inventory_files: [\"") + Path + TEXT("\"] }"), ManifestPath,
			Resources, ManifestRoot, Error, [](const FString&, const FString&) { return true; })) return false;
		FPublishedWorkspaceDirectory Directory;
		Directory.Root = Root; Directory.Publication.ScopedMachineBuckets = true;
		Directory.PublishedHashes.Add(Path, Sha256Text(Text));
		Directory.Manifest = MakeShared<FHjsonDocument>(ManifestPath, ManifestRoot.ToSharedRef());
		// Match the formal owner: these are borrowed entries of this exact
		// immutable manifest, consumed by ReadShard's accepted-list lookup.
		for (const auto& File : ManifestRoot->Get(TEXT("unit_inventory_files"))->GetItems())
			Directory.InventoryEntries.Add(&File.Get());
		// A different document was already loaded by an earlier consumer. Its
		// identity/value are not inputs to this requested document selection.
		auto Unrelated = MakeShared<FInventoryRowSyntax>(*Rows[0].Syntax);
		Unrelated->UnitIdentity = MakeShared<const FString, ESPMode::ThreadSafe>(TEXT("unit:unrelated"));
		Unrelated->DocumentPath = TEXT("targets/unrelated.hjson");
		Directory.Loaded.Rows.Add({Unrelated});
		const int64 BeforeSlots = ActiveReadState->PublishedInventoryDocumentSlots;
		int32 Expected = 0, Matched = 0;
		for (const auto& Row : Rows) if (Row.Syntax->GetDocumentPath() == DocumentPath) ++Expected;
		if (!TestTrue(TEXT("document selection consumes its original shard range"), Directory.VisitDocument(DocumentPath,
			[&](const FAcceptedWorkspaceInventoryRow& Row) { ++Matched; return TestEqual(TEXT("only requested document"), Row.DocumentPath, DocumentPath); }))) { AddError(Directory.ReadError.GetCode()); return false; }
		TestEqual(TEXT("all and only matching document rows"), Matched, Expected);
		TestEqual(TEXT("unrelated loaded prefix is not searched"), ActiveReadState->PublishedInventoryDocumentSlots - BeforeSlots, int64(Rows.Num()));
	}
	if (bOwned) {
		const FString DocumentPath(TEXT("targets/a.hjson"));
		{
			const FString OwnerPath = Root / TEXT(".uai-export-owner.json");
			const FString OwnerText = TEXT("{\"format\":\"uai.c8f.artifact_publish.v1\",\"publisher_owner_id\":\"local-owner\",\"final_path\":\"")
				+ Root.Replace(TEXT("\\"), TEXT("/")) + TEXT("\",\"consumer_state\":\"accepted-state\",\"files\":[]}");
			for (const FString& Input : {OwnerText, FString(TEXT("{\"format\":"))}) {
				if (!FFileHelper::SaveStringToFile(Input, *OwnerPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				FString BeforeState, BeforeOwner, ActualState, ActualOwner;
				TMap<FString, FString> BeforeHashes, ActualHashes;
				UE::UAI::ArtifactPublisher::FArtifactPublishError BeforeError, ActualError;
				const bool bBefore = UE::UAI::ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(),
					BeforeState, BeforeOwner, BeforeError, &BeforeHashes);
				const bool bActual = UE::UAI::ArtifactPublisher::TryReadObservedWorkspaceConsumerState(Root, FString(), Input,
					ActualState, ActualOwner, ActualError, &ActualHashes);
				TestTrue(TEXT("observed ownership text uses original parser, fields and errors"), bBefore == bActual
					&& BeforeState == ActualState && BeforeOwner == ActualOwner && BeforeHashes.OrderIndependentCompareEqual(ActualHashes)
					&& BeforeError.Code == ActualError.Code && BeforeError.Source.Line == ActualError.Source.Line
					&& BeforeError.Source.Column == ActualError.Source.Column);
			}
			IFileManager::Get().Delete(*OwnerPath);
		}
		const FString Original(TEXT("{ value: false }\n"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Root / DocumentPath), true);
		if (!FFileHelper::SaveStringToFile(Original, *(Root / DocumentPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		auto Directory = MakeShared<FPublishedWorkspaceDirectory>();
		Directory->Root = Root; Directory->Publication.ScopedMachineBuckets = true;
		Directory->Publication.PublishedUnitCount = Rows.Num();
		Directory->PublishedHashes.Add(Path, Sha256Text(Text));
		Directory->PublishedHashes.Add(DocumentPath, Sha256Text(Original));
		FHjsonNodePtr Header;
		if (!TryParseHjsonSelectedNodes(TEXT("{ unit_inventory_files: [\"") + Path + TEXT("\"] }"), ManifestPath,
			Resources, Header, Error, [](const FString&, const FString&) { return true; })) return false;
		Directory->Manifest = MakeShared<FHjsonDocument>(ManifestPath, Header.ToSharedRef());
		for (const auto& File : Header->Get(TEXT("unit_inventory_files"))->GetItems()) Directory->InventoryEntries.Add(&File.Get());
		auto Candidate = MakeShared<FGeneratedSourceCandidate>();
		Candidate->Inventory = Directory; Candidate->Bindings = Directory;
		Candidate->Model = FAuthoringModel::MakePropertySourceView(TEXT("fixture"), 1, TEXT("llm_authoring"),
			TEXT("fixture"), {}, {}, TEXT("fixture"));
		Candidate->FileHashes = Directory->PublishedHashes;
		TGuardValue<bool> Establishing(ActiveReadState->bEstablishFileObservation, true);
		TGuardValue<const TMap<FString, FString>*> PublishedHashes(ActiveReadState->PublishedFileHashes, &Directory->PublishedHashes);
		TGuardValue<FString> ObservationRoot(ActiveReadState->FileObservationRoot, Root);
		FParsedSource Unchanged; Unchanged.GeneratedSource = Candidate;
		TArray<FInventoryRow> UnchangedRows;
		if (!TestTrue(TEXT("same original document reader establishes unchanged bytes"), ReadGeneratedInventory(Root, Resources, Unchanged, UnchangedRows, Error))) { AddError(Error.GetCode()); return false; }
		TestTrue(TEXT("unchanged document produces no execution row wrappers"), UnchangedRows.IsEmpty());
		TestEqual(TEXT("actual accepted input hash remains recorded"), Unchanged.DocumentHashes.FindRef(DocumentPath), Sha256Text(Original));
		TestTrue(TEXT("original inventory validation still ran before skipping wrappers"), Directory->Loaded.Rows.Num() == Rows.Num());
		Candidate->PendingAuthorDocuments.Add(DocumentPath);
		FParsedSource Pending; Pending.GeneratedSource = Candidate; TArray<FInventoryRow> PendingRows;
		if (!TestTrue(TEXT("same bytes with pending author intent remain real input"), ReadGeneratedInventory(Root, Resources, Pending, PendingRows, Error))) { AddError(Error.GetCode()); return false; }
		TestEqual(TEXT("pending intent retains every addressed row"), PendingRows.Num(), Rows.Num());
		Candidate->PendingAuthorDocuments.Reset();
		if (!FFileHelper::SaveStringToFile(TEXT("{ value: true }\n"), *(Root / DocumentPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		FParsedSource Changed; Changed.GeneratedSource = Candidate; TArray<FInventoryRow> ChangedRows;
		if (!TestTrue(TEXT("changed actual bytes remain real input"), ReadGeneratedInventory(Root, Resources, Changed, ChangedRows, Error))) { AddError(Error.GetCode()); return false; }
		TestEqual(TEXT("changed document retains every addressed row"), ChangedRows.Num(), Rows.Num());
		// Exercise the real file observer against the original Engine text
		// conversion/hash, not a second normalization implementation.
		TArray<TArray<uint8>> EncodingInputs;
		for (const FString& Input : {FString(TEXT("{ value: false }\n")), FString(TEXT("{ value: 中文 }\n"))}) {
			const FTCHARToUTF8 Utf8(*Input); TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); EncodingInputs.Add(MoveTemp(Bytes));
		}
		EncodingInputs.Add({uint8('a'), 0, uint8('b')});
		EncodingInputs.Add({0xff, 0xfe, uint8('a'), 0, uint8('\n'), 0});
		for (int32 Index = 0; Index < EncodingInputs.Num(); ++Index) {
			const auto& Bytes = EncodingInputs[Index];
			const FString Relative = FString::Printf(TEXT("targets/encoding%d.hjson"), Index);
			const FString FullPath = Root / Relative;
			ON_SCOPE_EXIT { ObservedSourceFiles.Remove(FullPath); };
			if (!FFileHelper::SaveArrayToFile(Bytes, *FullPath)) return false;
			Directory->PublishedHashes.Add(Relative, Sha256(Bytes));
			FString ExpectedText; FFileHelper::BufferToString(ExpectedText, Bytes.GetData(), Bytes.Num());
			const FString ExpectedHash = Sha256Text(ExpectedText);
			const int32 BeforeDirect = ActiveReadState->DirectHashOnlyFiles;
			FString ActualText, ActualHash; int64 ActualCharacters = -1;
			if (!TestTrue(TEXT("actual encoding input is observed"), ObserveSourceFile(FullPath, false,
				ActualText, ActualHash, ActualCharacters))) return false;
			TestEqual(TEXT("same normalized text hash"), ActualHash, ExpectedHash);
			TestEqual(TEXT("same exact UTF-16 character count"), ActualCharacters, int64(ExpectedText.Len()));
			TestEqual(TEXT("only exact non-NUL ASCII skips text buffers"), ActiveReadState->DirectHashOnlyFiles - BeforeDirect, Index == 0 ? 1 : 0);
		}
	}
	TArray<FResolvedWorkspaceInventory::FRow> Wrong; Error.Reset();
	TestFalse(TEXT("different accepted bytes stop before parsing"), ReadPublishedInventoryDocument(Root, Path, TEXT("different-version"), Resources, Wrong, Error));
	TestEqual(TEXT("actual mismatch preserved"), Error.GetCode(), FString(TEXT("workspace_source_changed")));
	TestTrue(TEXT("mismatch produced no rows"), Wrong.IsEmpty());
	if (bOwned) {
		// Same original grammar/validator is the oracle for the new handoff,
		// including semantic priority and syntax failure before row compilation.
		const FString InvalidInputs[] {
			Text.Replace(TEXT("unit_id: \"unit:中文\""), TEXT("unit_id: \" \"")),
			Text.Replace(*FString::Printf(TEXT("unit_kind: \"%s\""), *LexToString(ESemanticUnitKind::PropertySet)), TEXT("unit_kind: \"not-a-kind\"")),
			Text.Replace(TEXT("unit_id: \"unit:中文\""), TEXT("unit_id: \" \"")) + TEXT(" broken: ]"),
			Text.Replace(TEXT("envelope_span: [0, 64]"), TEXT("envelope_span: [0]")),
			Text.Replace(TEXT("envelope_span: [0, 64]"), TEXT("envelope_span: [0, true]")),
			Text.Replace(TEXT("envelope_span: [0, 64]"), TEXT("envelope_span: [0, 64, 65]")),
			Text.Replace(TEXT("envelope_span: [0, 64]"), TEXT("envelope_span: [64, 0]"))};
		for (const auto& Invalid : InvalidInputs) {
			FWorkspaceError OriginalError, ProjectedError; FHjsonNodePtr OriginalRoot;
			TArray<FInventoryRow> OriginalTyped;
			const bool bSyntax = TryParseHjsonSelectedNodes(Invalid, Path, Resources, OriginalRoot, OriginalError,
				[](const FString&, const FString&) { return true; });
			if (bSyntax) TestFalse(TEXT("original semantic failure"), ParseInventory(*OriginalRoot, OriginalTyped, OriginalError));
			if (!FFileHelper::SaveStringToFile(Invalid, *(Root / Path), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			TArray<FResolvedWorkspaceInventory::FRow> InvalidRows;
			TestFalse(TEXT("projection failure is final"), ReadPublishedInventoryDocument(Root, Path, Sha256Text(Invalid), Resources, InvalidRows, ProjectedError));
			TestEqual(TEXT("same failure code"), ProjectedError.GetCode(), OriginalError.GetCode());
			TestEqual(TEXT("same failure source pointer"), ProjectedError.GetJsonPointer(), OriginalError.GetJsonPointer());
			TestEqual(TEXT("same failure line"), ProjectedError.GetLine(), OriginalError.GetLine());
			TestEqual(TEXT("same failure column"), ProjectedError.GetColumn(), OriginalError.GetColumn());
			TestTrue(TEXT("failed source handed off no rows"), InvalidRows.IsEmpty());
			const auto& A = OriginalError.GetDiagnostic().ActualValue;
			const auto& B = ProjectedError.GetDiagnostic().ActualValue;
			TestTrue(TEXT("same public actual field"), CanonicalJson::AreJsonValuesEquivalent(A, B));
			}
		if (!FFileHelper::SaveStringToFile(Text, *(Root / Path), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
	AddInfo(FString::Printf(TEXT("published compact rows=%d original_file=%s per_row_source_hash=false copied_source_backing=false"), Rows.Num(), bOwned ? TEXT("false") : TEXT("true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentExplicitPublicationSpanTest,
	"UeAgentInterface.Core.HjsonWorkspace.ExplicitPublicationSpans",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExplicitPublicationSpanTest::RunTest(const FString&)
{
	using namespace UE::UAI::HjsonWorkspace;
	FSemanticWorkspaceReadScope Scope;
	auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError Error;
	FString BaselinePath = TEXT(".uai/baselines/span-fixture.hjson");
	FString BaselineText = TEXT("{\n\"unit:a\": { captured_values: { value: false, \"object:/needle\": 123 } },\n\"unit:中文\": { captured_values: { value: [1, { nested: \"object:/needle\" }, \"中文\"] } }\n}\n");
	FString InventoryText = TEXT("{ unit_inventory: [ { unit_id: \"unit:中文\" }, { unit_id: \"unit:a\" } ] }\n");
	const FString FixtureRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_SPAN_WORKSPACE"));
	const FString FixtureDocument = FPlatformMisc::GetEnvironmentVariable(TEXT("UAI_TEST_HJSON_SPAN_DOCUMENT"));
	if (!FixtureRoot.IsEmpty() || !FixtureDocument.IsEmpty()) {
		if (!TestFalse(TEXT("both optional fixture inputs supplied"), FixtureRoot.IsEmpty() || FixtureDocument.IsEmpty())) return false;
		BaselinePath = GetPublishedMachineEnvelopePath(FixtureDocument, true);
		if (!TestTrue(TEXT("read original Large baseline bytes"), FFileHelper::LoadFileToString(BaselineText, *(FixtureRoot / BaselinePath)))
			|| !TestTrue(TEXT("read corresponding original inventory bytes"), FFileHelper::LoadFileToString(InventoryText,
				*(FixtureRoot / BaselinePath.Replace(TEXT("/baselines/"), TEXT("/inventory/")))))) return false;
		}
	const FString InventoryPath = BaselinePath.Replace(TEXT("/baselines/"), TEXT("/inventory/"));
	FHjsonNodePtr Baseline, InventoryRoot;
	const auto IncludeAll = [](const FString&, const FString&) { return true; };
	// Selected-node entry intentionally avoids persistent syntax/cache writes.
	if (!TestTrue(TEXT("independent complete original-parser oracle"), TryParseHjsonSelectedNodes(BaselineText, BaselinePath,
		Resources, Baseline, Error, IncludeAll))
		|| !TestTrue(TEXT("original inventory parser"), TryParseHjsonSelectedNodes(InventoryText, InventoryPath,
			Resources, InventoryRoot, Error, IncludeAll))) { AddError(Error.GetCode()); return false; }
	const auto* RowArray = InventoryRoot->Get(TEXT("unit_inventory"));
	if (!TestTrue(TEXT("inventory rows"), RowArray && RowArray->GetKind() == EHjsonNodeKind::Array)) return false;
	class FOriginalRows final : public FAcceptedWorkspaceInventory {
	public:
		const FHjsonNode& Rows; const FString& Envelope;
		const FHjsonNode* Captured = nullptr;
		bool bMetadata = false;
		mutable int32 VisitCalls = 0, UnusedHashReads = 0;
		FOriginalRows(const FHjsonNode& InRows, const FString& InEnvelope) : Rows(InRows), Envelope(InEnvelope) {}
		int32 Num() const override { return Rows.GetItems().Num(); }
		bool VisitBucket(const FString& Path, TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor,
			bool bRequireEnvelopeSpan = true) const override { return Path == Envelope && Visit(Visitor); }
		bool Visit(TFunctionRef<bool(const FAcceptedWorkspaceInventoryRow&)> Visitor) const override {
			++VisitCalls;
			const FString Empty; const TArray<FString> NoFiles;
			const auto UnusedHash = [&]() { ++UnusedHashReads; return FString(); };
			const auto UnusedCompare = [&](const FString&) { ++UnusedHashReads; return false; };
			// The consumer must use the real ordinal, not traversal/physical order.
			for (int32 I = Num() - 1; I >= 0; --I) {
				const auto& Node = Rows.GetItems()[I].Get(); const auto* Id = Node.Get(TEXT("unit_id"));
				if (!Id || Id->GetKind() != EHjsonNodeKind::String) return false;
				const auto& Origin = Node.GetProvenance();
				FAcceptedWorkspaceByteSpan EnvelopeSpan;
				if (Captured) {
					const auto* EnvelopeNode = Captured->Get(Id->GetText()); if (!EnvelopeNode) return false;
					const auto& Actual = EnvelopeNode->GetProvenance(); EnvelopeSpan = {Actual.GetByteStart(), Actual.GetByteEnd()};
					}
				const auto Field = [&](const TCHAR* Name, const FString& Default) {
					const auto* Value = Node.Get(Name); return Value ? Value->GetText() : Default; };
				const FString Ref = bMetadata ? Field(TEXT("public_ref"), TEXT("ref:") + Id->GetText()) : Empty;
				const FString Logical = bMetadata ? Field(TEXT("logical_path"), TEXT("targets/") + Id->GetText() + TEXT(".hjson")) : Empty;
				const FString Document = bMetadata ? Field(TEXT("document_path"), Logical) : Empty;
				const FString Pointer = bMetadata ? Field(TEXT("unit_pointer"), TEXT("/unit")) : Empty;
				const auto Hash = [&]() { return bMetadata ? Field(TEXT("content_hash"), Sha256Text(Id->GetText())) : UnusedHash(); };
				const auto Edit = [&]() { return bMetadata ? Field(TEXT("edit_hash"), Empty) : UnusedHash(); };
				TArray<FString> ContentFiles;
				if (bMetadata) if (const auto* Content = Node.Get(TEXT("content_files")))
					for (const auto& Item : Content->GetItems()) ContentFiles.Add(Item->GetText());
				ESemanticUnitKind Kind = ESemanticUnitKind::PropertySet;
				if (bMetadata) if (const auto* K = Node.Get(TEXT("unit_kind"))) if (!ParseUnitKind(K->GetText(), Kind)) return false;
				// This fixture's strings are local to its synchronous row callback,
				// so one row is the actual read chunk. Production directory chunks
				// instead borrow their inventory-owned digest and strings.
				const FString HashText = bMetadata ? Hash() : FString();
				const auto HashView = bMetadata ? FAcceptedWorkspaceContentHashView::FromCanonicalText(HashText) : FAcceptedWorkspaceContentHashView{};
				if (!Visitor({Id->GetText(), Kind, Ref, Logical, Document, Pointer, Envelope, bMetadata ? ContentFiles : NoFiles,
					Edit, UnusedCompare, Hash, {Origin.GetByteStart(), Origin.GetByteEnd()}, EnvelopeSpan, false, {}, I, false, {}, HashView, true})) return false;
				}
			return true;
			}
	} Rows(*RowArray, BaselinePath);
	const auto Bytes = [](const FString& Text) {
		const FTCHARToUTF8 Utf8(*Text); TArray<uint8> Result;
		Result.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); return Result;
		};
	TArray<FExportFilePlan> Files;
	Files.Emplace(BaselinePath, Bytes(BaselineText), Sha256Text(BaselineText));
	Files.Emplace(InventoryPath, Bytes(InventoryText), Sha256Text(InventoryText));
	const auto OriginalBytes = Files[0].GetCanonicalUtf8Identity();
	TMap<FString, FString> Hashes;
	for (const auto& File : Files) Hashes.Add(File.GetRelativePath(), File.GetContentHash());
	const TArray<FExportFilePlan> UnpreparedFiles = Files;
	if (!TestTrue(TEXT("formal original-bytes producer"), BuildExplicitPublicationSpanFiles(Files, Rows,
		[&](const FString& Path) { return Hashes.Find(Path); }, Resources, Error))) { AddError(Error.GetCode()); return false; }
	TestTrue(TEXT("same byte backing, no body copy"), OriginalBytes.Pin() == Files[0].GetCanonicalUtf8Identity().Pin());
	TestEqual(TEXT("no edit/content/unit hash requested"), Rows.UnusedHashReads, 0);
	const auto BaseSpans = Files[0].GetGeneratedFragmentSpans(), InventorySpans = Files[1].GetGeneratedFragmentSpans();
	const auto Identities = Files[0].GetGeneratedFragmentIdentities();
	if (!TestTrue(TEXT("both machine roles directly carry ranges"), BaseSpans && InventorySpans
		&& BaseSpans->Num() == Rows.Num() && InventorySpans->Num() == Rows.Num())) return false;
	if (!TestTrue(TEXT("both machine roles share actual ordinal identities"), Identities && Identities->Num() == Rows.Num()
		&& Identities == Files[1].GetGeneratedFragmentIdentities())) return false;
	for (int32 I = 0; I < Rows.Num(); ++I) {
		const auto& Row = RowArray->GetItems()[I].Get(); const auto* Id = Row.Get(TEXT("unit_id"));
		const auto* Envelope = Baseline->Get(Id->GetText());
		if (!TestTrue(TEXT("same actual envelope identity"), Envelope != nullptr)) return false;
		const auto& A = Envelope->GetProvenance(); const auto& B = Row.GetProvenance();
		TestEqual(TEXT("identity uses actual inventory ordinal, not traversal order"), *(*Identities)[I], Id->GetText());
		TestTrue(TEXT("baseline span equals complete parser by inventory ordinal"), (*BaseSpans)[I] == FIntPoint(A.GetByteStart(), A.GetByteEnd()));
		TestTrue(TEXT("inventory original range is passed through"), (*InventorySpans)[I] == FIntPoint(B.GetByteStart(), B.GetByteEnd()));
		if (const auto* Stored = Row.Get(TEXT("envelope_span"))) {
			if (!TestTrue(TEXT("current writer persisted two integer coordinates"), Stored->GetKind() == EHjsonNodeKind::Array && Stored->GetItems().Num() == 2)) return false;
			int64 Begin = -1, End = -1;
			if (!TestTrue(TEXT("current writer coordinates parse exactly"), LexTryParseString(Begin, *Stored->GetItems()[0]->GetLexeme())
				&& LexTryParseString(End, *Stored->GetItems()[1]->GetLexeme()))) return false;
			TestTrue(TEXT("on-disk writer coordinates equal independent original-parser baseline bounds"), Begin == A.GetByteStart() && End == A.GetByteEnd());
		}
		}
	Rows.Captured = Baseline.Get();
	auto PassedFiles = UnpreparedFiles; int32 ParsedKnownFiles = -1;
	if (!TestTrue(TEXT("accepted original envelope ranges are consumed before parsing"), BuildExplicitPublicationSpanFiles(PassedFiles, Rows,
		[&](const FString& Path) { return Hashes.Find(Path); }, Resources, Error, &ParsedKnownFiles))) { AddError(Error.GetCode()); return false; }
	TestEqual(TEXT("known envelope ranges produce no second parse"), ParsedKnownFiles, 0);
	TestTrue(TEXT("passed ranges match actual original-parser ranges"), PassedFiles[0].GetGeneratedFragmentSpans()
		&& *PassedFiles[0].GetGeneratedFragmentSpans() == *BaseSpans);
	TestTrue(TEXT("accepted span handoff keeps original file bytes"), OriginalBytes.Pin() == PassedFiles[0].GetCanonicalUtf8Identity().Pin());
	const int32 Visits = Rows.VisitCalls;
	if (!TestTrue(TEXT("generated/already-prepared path does not recover text"), BuildExplicitPublicationSpanFiles(Files, Rows,
		[](const FString&) -> const FString* { return nullptr; }, Resources, Error))) return false;
	TestEqual(TEXT("no repeated inventory visit"), Rows.VisitCalls, Visits);
	TestTrue(TEXT("same sealed span backing"), BaseSpans == Files[0].GetGeneratedFragmentSpans());
	// The existing record, not a second persistent index, carries the baseline
	// range. Re-read it through the production parser in a distinct file scope.
	{
		Rows.bMetadata = true; Rows.Captured = nullptr;
		auto Upgrade = UnpreparedFiles; int32 ParsedOld = -1;
		if (!TestTrue(TEXT("explicit old-record range upgrade"), BuildExplicitPublicationSpanFiles(Upgrade, Rows,
			[&](const FString& Path) { return Hashes.Find(Path); }, Resources, Error, &ParsedOld, true))) { AddError(Error.GetCode()); return false; }
		TestEqual(TEXT("old record parses original baseline exactly once"), ParsedOld, 1);
		TestTrue(TEXT("upgrade preserves actual baseline bytes"), Upgrade[0].GetCanonicalUtf8Identity().Pin() == OriginalBytes.Pin());
		const FString OwnedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/uai-tests/inventory-ranges-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString File = OwnedRoot / InventoryPath;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*File); IFileManager::Get().DeleteDirectory(*OwnedRoot, false, true); };
		if (!TestTrue(TEXT("write real upgraded inventory"), FFileHelper::SaveArrayToFile(Upgrade[1].GetCanonicalUtf8(), *File))) return false;
		FResolvedWorkspaceInventory Parsed;
		if (!TestTrue(TEXT("cold production row reader"), ReadPublishedInventoryDocument(OwnedRoot, InventoryPath, Upgrade[1].GetContentHash(), Resources, Parsed.Rows, Error))) { AddError(Error.GetCode()); return false; }
		if (!TestEqual(TEXT("same persisted membership"), Parsed.Num(), Rows.Num())) return false;
		for (int32 I = 0; I < Parsed.Num(); ++I) {
			const auto& Row = Parsed.Rows[I];
			TestEqual(TEXT("same persistent ordinal identity"), Row.Syntax->GetUnitId(), *(*Identities)[I]);
			TestTrue(TEXT("persisted range is exact original baseline range"), Row.EnvelopeSpan.Begin == (*BaseSpans)[I].X && Row.EnvelopeSpan.End == (*BaseSpans)[I].Y);
			TestTrue(TEXT("stored range provenance survives the production reader"), Row.Syntax->StoredEnvelopeSpan.IsValid());
			}
		TArray<FExportFilePlan> Cold;
		Cold.Emplace(BaselinePath, Bytes(BaselineText), Sha256Text(BaselineText));
		Cold.Emplace(InventoryPath, Upgrade[1].GetCanonicalUtf8(), Upgrade[1].GetContentHash());
		TMap<FString, FString> Current;
		for (const auto& Input : Cold) Current.Add(Input.GetRelativePath(), Input.GetContentHash());
		const auto PersistedBacking = Cold[1].GetCanonicalUtf8Identity().Pin(); int32 ParsedNew = -1;
		if (!TestTrue(TEXT("persisted original ranges establish cold file versions"), BuildExplicitPublicationSpanFiles(Cold, Parsed,
			[&](const FString& Path) { return Current.Find(Path); }, Resources, Error, &ParsedNew, true))) { AddError(Error.GetCode()); return false; }
		TestEqual(TEXT("cold persistent range needs no baseline parse"), ParsedNew, 0);
		TestTrue(TEXT("current record needs no rewrite"), Cold[1].GetCanonicalUtf8Identity().Pin() == PersistedBacking);
		AddInfo(FString::Printf(TEXT("persistent envelope ranges: rows=%d inventory_before=%d after=%d delta=%d cold_baseline_parses=%d separate_index=false"),
			Parsed.Num(), UnpreparedFiles[1].GetCanonicalUtf8().Num(), Upgrade[1].GetCanonicalUtf8().Num(),
			Upgrade[1].GetCanonicalUtf8().Num() - UnpreparedFiles[1].GetCanonicalUtf8().Num(), ParsedNew));
		Rows.bMetadata = false;
	}
	if (FixtureRoot.IsEmpty()) {
		// Consume the same formal span producer through real protected files.
		const FString OwnedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/uai-tests/captured-query-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString File = OwnedRoot / BaselinePath;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		ON_SCOPE_EXIT { IFileManager::Get().Delete(*File); IFileManager::Get().DeleteDirectory(*OwnedRoot, false, true); };
		if (!TestTrue(TEXT("write original sealed baseline"), FFileHelper::SaveArrayToFile(Files[0].GetCanonicalUtf8(), *File))) return false;
		ActiveReadState->PublishedEnvelopeSpan = [&](const FString& Id, FAcceptedWorkspaceByteSpan& Out, FWorkspaceError&) {
			for (int32 I = 0; I < Identities->Num(); ++I) if (*(*Identities)[I] == Id) {
				const auto Span = (*BaseSpans)[I]; Out = {Span.X, Span.Y}; return true;
			}
			return false;
		};
		ON_SCOPE_EXIT { ActiveReadState->PublishedEnvelopeSpan = {}; };
		bool Match = false;
		const auto IsNeedle = [](const FString& Value) { return Value == TEXT("object:/needle"); };
		if (!TryReadCapturedReferenceScope([&]() {
			if (!TestTrue(TEXT("string query reads exact captured row"), TryPublishedCapturedContainsAnyString(OwnedRoot, BaselinePath,
				Files[0].GetContentHash(), TEXT("unit:中文"), IsNeedle, Resources, Match, Error))) return false;
			TestTrue(TEXT("nested value matches"), Match);
			return TestTrue(TEXT("key with same text is not a reference value"), TryPublishedCapturedContainsAnyString(OwnedRoot, BaselinePath,
				Files[0].GetContentHash(), TEXT("unit:a"), IsNeedle, Resources, Match, Error) && !Match);
		}, Error)) { AddError(Error.GetCode()); return false; }
		TestTrue(TEXT("addressed batch owns the original root binding"), ActiveReadState->CapturedReferencePaths.IsValid());
		TestEqual(TEXT("only addressed leaf is reopened, not an N-file observation"), ActiveReadState->CapturedReferencePaths->GetOpenedCurrentHandles(), 1);
		TestTrue(TEXT("reference consumer did not restore any typed unit"), ActiveReadState->CapturedRows.IsEmpty());
		TestEqual(TEXT("only one mandatory whole-file byte observation"), ActiveReadState->CapturedByteReads, 1);
		TestEqual(TEXT("second actual row uses the sealed range under the first observation"), ActiveReadState->CapturedRangeReads, 1);
		{
			FWorkspaceReadPathScope ReadScope(OwnedRoot); TArray<uint8> Fragment;
			const auto Span = (*BaseSpans)[0];
			if (!TestTrue(TEXT("original file owner range API"), ReadScope.Observe(File, [&](void* Handle, int64, int64) {
				return FWorkspaceReadPathScope::ReadRange(Handle, Span.X, Span.Y, Fragment); }))) return false;
			TestTrue(TEXT("range bytes exactly equal immutable producer span"), Fragment.Num() == Span.Y - Span.X
				&& FMemory::Memcmp(Fragment.GetData(), Files[0].GetCanonicalUtf8().GetData() + Span.X, Fragment.Num()) == 0);
		}
		if (!TestTrue(TEXT("independent changed byte breaks the original observation"), FFileHelper::SaveStringToFile(BaselineText + TEXT("// changed\n"), *File,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		Error.Reset();
		TestFalse(TEXT("changed backing stops rather than selecting another parser"), TryPublishedCapturedContainsAnyString(OwnedRoot, BaselinePath,
			Files[0].GetContentHash(), TEXT("unit:a"), IsNeedle, Resources, Match, Error));
		TestEqual(TEXT("actual byte mismatch is preserved"), Error.GetCode(), FString(TEXT("workspace_source_changed")));
		// The original header owner travels with its immutable file, independent
		// of the optional last-parser slot being overwritten by another document.
		FHjsonDocumentPtr Header; Error.Reset();
		const FString HeaderText(TEXT("{ workspace_schema: \"uai.semantic.workspace/4\", publication_generation: \"test-generation\", publication_state: \"ready\" }"));
		if (!TryParseHjsonDocument(HeaderText, TEXT(".uai/workspace.hjson"), Resources, Header, Error)) { AddError(Error.GetCode()); return false; }
		RememberParsedHjsonRootHeader(Sha256Text(HeaderText), *Header, {});
		const FExportFilePlan HeaderFile(TEXT(".uai/workspace.hjson"), Bytes(HeaderText), Sha256Text(HeaderText));
		RememberParsedHjsonRootHeader(Files[0].GetContentHash(), FHjsonDocument(BaselinePath, Baseline.ToSharedRef()), {});
		FString Generation;
		TestTrue(TEXT("sealed header facts survive unrelated root parsing"), TryReadHjsonRootString(HeaderFile.GetRootHeaderFacts(), HeaderFile.GetContentHash(),
			TEXT("publication_generation"), Generation) && Generation == TEXT("test-generation"));
		TestFalse(TEXT("header facts cannot qualify different bytes"), TryReadHjsonRootString(HeaderFile.GetRootHeaderFacts(), TEXT("different"),
			TEXT("publication_generation"), Generation));
	}
	TArray<FExportFilePlan> Unbound; Unbound.Emplace(BaselinePath, Bytes(BaselineText), Sha256Text(BaselineText));
	TestFalse(TEXT("missing original byte binding is an explicit stop"), BuildExplicitPublicationSpanFiles(Unbound, Rows,
		[](const FString&) -> const FString* { return nullptr; }, Resources, Error));
	TestEqual(TEXT("specific original-input diagnostic"), Error.GetCode(), FString(TEXT("workspace_explicit_publication_span_bytes_unbound")));
	AddInfo(FString::Printf(TEXT("pure-file spans: rows=%d baseline_bytes=%d inventory_bytes=%d original_fixture=%s native_capture=false"),
		Rows.Num(), Files[0].GetCanonicalUtf8().Num(), Files[1].GetCanonicalUtf8().Num(), FixtureRoot.IsEmpty() ? TEXT("false") : TEXT("true")));
	return true;
}
#endif
