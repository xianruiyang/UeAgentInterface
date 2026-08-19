#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;

	namespace
	{
		FUeAgentWorldSpatialCompiledValue MakeGraphValue(
			const FString& Subject,
			const FString& FrameId,
			double X,
			double Y,
			const FString& Evidence)
		{
			return {
				Subject,
				TEXT("point"),
				FrameId,
				2,
				TEXT("graph_unit"),
				{X, Y},
				Evidence,
			};
		}

		bool TryMakeStringValue(
			const FString& Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
				Value, Context.GetResourceTracker(), OutValue, Error)
					? true
					: Fail(
						OutError,
						TEXT("world_spatial_graph_string_failed"),
						Value,
						Error.Code);
		}

		bool TryResolveGraphEditor(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<SGraphEditor>& OutEditor,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FString& Identity =
				Context.GetExecutionContext().GetAssetEditorIdentity();
			return !Identity.IsEmpty()
				&& Environment.TryResolveGraphEditor(Identity, OutEditor, OutError)
				&& OutEditor.IsValid()
					? true
					: OutError.IsValid()
						? false
						: Fail(
							OutError,
							TEXT("world_spatial_graph_editor_unresolved"),
							Identity);
		}

		bool TryResolveBlueprintGraph(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			UEdGraph*& OutGraph,
			UBlueprint*& OutBlueprint,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutGraph = nullptr;
			OutBlueprint = nullptr;
			const FString& Identity =
				Context.GetExecutionContext().GetAssetEditorIdentity();
			UObject* Object = nullptr;
			if (!Environment.TryResolveObject(Identity, Object, OutError))
			{
				return false;
			}
			OutGraph = Cast<UEdGraph>(Object);
			if (!OutGraph)
			{
				return Fail(OutError,
					TEXT("world_spatial_graph_identity_type_invalid"), Identity);
			}
			OutBlueprint = OutGraph->GetTypedOuter<UBlueprint>();
			return true;
		}

		FVector2f SnapGraphViewLocation(
			const FVector2f Location,
			const float Zoom)
		{
			const float SafeZoom = FMath::Max(FMath::Abs(Zoom), 0.0001f);
			return FVector2f(
				FMath::RoundToFloat(Location.X * SafeZoom) / SafeZoom,
				FMath::RoundToFloat(Location.Y * SafeZoom) / SafeZoom);
		}

		bool TryFindDocumentState(
			const UBlueprint& Blueprint,
			const UEdGraph& Graph,
			FVector2f& OutLocation,
			float& OutZoom)
		{
			const FSoftObjectPath GraphPath(&Graph);
			for (int32 Index = Blueprint.LastEditedDocuments.Num() - 1;
				Index >= 0; --Index)
			{
				const FEditedDocumentInfo& Info =
					Blueprint.LastEditedDocuments[Index];
				if (Info.EditedObjectPath == GraphPath)
				{
					OutZoom = FMath::Max(FMath::Abs(Info.SavedZoomAmount), 0.0001f);
					OutLocation = SnapGraphViewLocation(
						Info.SavedViewOffset, OutZoom);
					return true;
				}
			}
			return false;
		}

		class FGraphViewNativeFamily final : public IWorldSpatialNativeFamily
		{
		public:
			explicit FGraphViewNativeFamily(
				const FUeAgentWorldSpatialNativeDependencies& InDependencies)
				: Environment(InDependencies.Environment.ToSharedRef())
				, SpaceCompiler(InDependencies.SpaceCompiler.ToSharedRef())
				, RevisionSource(InDependencies.RevisionSource.ToSharedRef())
			{
			}

			const FString& GetFamilyId() const override
			{
				static const FString Id = TEXT("graph_view");
				return Id;
			}

			bool TryRead(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				TSharedPtr<SGraphEditor> Editor;
				if (!TryResolveGraphEditor(*Environment, Context, Editor, OutError))
				{
					return false;
				}
				FString Subject = Context.GetExecutionContext().GetAssetEditorIdentity();
				if (Request.GetPayload().IsType<FGraphViewUpdateRequestInput>())
				{
					Subject = Request.GetPayload()
						.Get<FGraphViewUpdateRequestInput>()
						.ViewState.Location.Subject;
				}
				FVector2f Location = FVector2f::ZeroVector;
				float Zoom = 0.0f;
				Editor->GetViewLocation(Location, Zoom);
				FUeAgentCanonicalValuePtr LocationValue;
				FUeAgentCanonicalValuePtr ZoomValue;
				FUeAgentCanonicalValuePtr RevisionSourceValue;
				FUeAgentCanonicalValuePtr RevisionGenerationValue;
				FUeAgentCanonicalValuePtr RevisionSequenceValue;
				FWorldSpatialRevision GraphRevision;
				const FUeAgentWorldSpatialCompiledValue Compiled = MakeGraphValue(
					Subject,
					TEXT("graph.editor.view.local"),
					Location.X,
					Location.Y,
					TEXT("graph-editor-view-readback"));
				if (!TryMakeCompiledValue(Compiled, Context, LocationValue, OutError)
					|| !TryMakeDecimalValue(Zoom, Context, ZoomValue, OutError)
					|| !RevisionSource->TryReadRevision(
						Subject, GraphRevision, OutError)
					|| !TryMakeStringValue(
						GraphRevision.SourceId,
						Context,
						RevisionSourceValue,
						OutError)
					|| !TryMakeStringValue(
						GraphRevision.Generation,
						Context,
						RevisionGenerationValue,
						OutError)
					|| !TryMakeIntegerValue(
						GraphRevision.Sequence,
						Context,
						RevisionSequenceValue,
						OutError))
				{
					return false;
				}
				TArray<FObservedEntry> Observations = {
					{
						TEXT("world-spatial:graph-view:location"),
						TEXT("graph_view"),
						Context.GetExecutionContext().GetAssetEditorIdentity(),
						Subject,
						TEXT("graph_view.location"),
						TEXT("world_spatial.compiled_value.v1"),
						LocationValue,
					},
					{
						TEXT("world-spatial:graph-view:zoom"),
						TEXT("graph_view"),
						Context.GetExecutionContext().GetAssetEditorIdentity(),
						Subject,
						TEXT("graph_view.zoom"),
						TEXT("core.decimal"),
						ZoomValue,
					},
					{
						TEXT("world-spatial:graph-view:revision-source"),
						TEXT("graph_view"),
						Context.GetExecutionContext().GetAssetEditorIdentity(),
						Subject,
						TEXT("graph_view.revision.source_id"),
						TEXT("core.string"),
						RevisionSourceValue,
					},
					{
						TEXT("world-spatial:graph-view:revision-generation"),
						TEXT("graph_view"),
						Context.GetExecutionContext().GetAssetEditorIdentity(),
						Subject,
						TEXT("graph_view.revision.generation"),
						TEXT("core.string"),
						RevisionGenerationValue,
					},
					{
						TEXT("world-spatial:graph-view:revision-sequence"),
						TEXT("graph_view"),
						Context.GetExecutionContext().GetAssetEditorIdentity(),
						Subject,
						TEXT("graph_view.revision.sequence"),
						TEXT("core.integer"),
						RevisionSequenceValue,
					},
				};
				FString Fingerprint = FString::Printf(
					TEXT("%s|%.9g|%.9g|%.9g|%s|%s|%lld"),
					*Subject, Location.X, Location.Y, Zoom,
					*GraphRevision.SourceId, *GraphRevision.Generation,
					GraphRevision.Sequence);
				const bool bIncludeDocumentState =
					!Request.GetPayload().IsType<FGraphViewReadRequestInput>()
					|| Request.GetPayload().Get<FGraphViewReadRequestInput>()
						.bIncludeDocumentState;
				if (bIncludeDocumentState)
				{
					UEdGraph* Graph = nullptr;
					UBlueprint* Blueprint = nullptr;
					if (!TryResolveBlueprintGraph(
							*Environment, Context, Graph, Blueprint, OutError))
					{
						return false;
					}
					if (Blueprint)
					{
						FVector2f PersistedLocation = FVector2f::ZeroVector;
						float PersistedZoom = 0.0f;
						if (TryFindDocumentState(
								*Blueprint, *Graph,
								PersistedLocation, PersistedZoom))
						{
							FUeAgentCanonicalValuePtr PersistedLocationValue;
							FUeAgentCanonicalValuePtr PersistedZoomValue;
							if (!TryMakeCompiledValue(
									MakeGraphValue(
										Subject,
										TEXT("graph.editor.document.local"),
										PersistedLocation.X,
										PersistedLocation.Y,
										TEXT("blueprint-last-edited-document-readback")),
									Context, PersistedLocationValue, OutError)
								|| !TryMakeDecimalValue(
									PersistedZoom, Context,
									PersistedZoomValue, OutError))
							{
								return false;
							}
							Observations.Add({
								TEXT("world-spatial:graph-view:document-location"),
								TEXT("graph_view"), Subject, Subject,
								TEXT("graph_view.document.location"),
								TEXT("world_spatial.compiled_value.v1"),
								PersistedLocationValue});
							Observations.Add({
								TEXT("world-spatial:graph-view:document-zoom"),
								TEXT("graph_view"), Subject, Subject,
								TEXT("graph_view.document.zoom"),
								TEXT("core.decimal"), PersistedZoomValue});
							Fingerprint += FString::Printf(
								TEXT("|document|%.9g|%.9g|%.9g"),
								PersistedLocation.X,
								PersistedLocation.Y,
								PersistedZoom);
						}
					}
				}
				return TryMakeSnapshot(
					Contract,
					Phase,
					Fingerprint,
					Observations,
					Context,
					OutSnapshot,
					OutError);
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (!Request.GetPayload().IsType<FGraphViewUpdateRequestInput>())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_graph_view_request_mismatch"),
						Contract.GetCapabilityId());
				}
				const FGraphViewUpdateRequestInput& Input = Request.GetPayload()
					.Get<FGraphViewUpdateRequestInput>();
				const FWorldSpatialGraphViewState& State = Input.ViewState;
				const FString& GraphIdentity =
					Context.GetExecutionContext().GetAssetEditorIdentity();
				if (State.Location.Subject != GraphIdentity)
				{
					return Fail(OutError,
						TEXT("world_spatial_graph_view_subject_mismatch"),
						State.Location.Subject, GraphIdentity);
				}
				if (!TryCheckRevision(
						*RevisionSource,
						State.Location.Subject,
						State.GraphRevision,
						OutError))
				{
					return false;
				}
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						State.Location,
						TEXT("graph.editor.view.local"),
						TEXT("graph_unit"),
						Compiled,
						OutError))
				{
					return false;
				}
				TSharedPtr<SGraphEditor> Editor;
				if (!TryResolveGraphEditor(*Environment, Context, Editor, OutError))
				{
					return false;
				}
				Editor->SetViewLocation(
					FVector2f(Compiled.Components[0], Compiled.Components[1]),
					State.Zoom);
				if (Input.bUpdateDocumentState)
				{
					UEdGraph* Graph = nullptr;
					UBlueprint* Blueprint = nullptr;
					if (!TryResolveBlueprintGraph(
							*Environment, Context, Graph, Blueprint, OutError)
						|| !Blueprint)
					{
						return OutError.IsValid()
							? false
							: Fail(OutError,
								TEXT("world_spatial_graph_document_state_unsupported"),
								GraphIdentity);
					}
					const TArray<FEditedDocumentInfo> BeforeDocuments =
						Blueprint->LastEditedDocuments;
					const FSoftObjectPath GraphPath(Graph);
					Blueprint->Modify();
					Blueprint->LastEditedDocuments.RemoveAll(
						[&GraphPath](const FEditedDocumentInfo& Info)
						{
							return Info.EditedObjectPath == GraphPath;
						});
					const FVector2f Snapped = SnapGraphViewLocation(
						FVector2f(
							Compiled.Components[0], Compiled.Components[1]),
						State.Zoom);
					Blueprint->LastEditedDocuments.Add(
						FEditedDocumentInfo(Graph, Snapped, State.Zoom));
					Blueprint->MarkPackageDirty();
					if (Input.bSaveAfterUpdate)
					{
						if (!UEditorLoadingAndSavingUtils::SavePackages(
								{Blueprint->GetOutermost()}, false))
						{
							Blueprint->LastEditedDocuments = BeforeDocuments;
							Blueprint->MarkPackageDirty();
							return Fail(OutError,
								TEXT("world_spatial_graph_document_save_failed"),
								GraphIdentity);
						}
						const FString Filename =
							FPackageName::LongPackageNameToFilename(
								Blueprint->GetOutermost()->GetName(),
								FPackageName::GetAssetPackageExtension());
						if (Blueprint->GetOutermost()->IsDirty()
							|| !IFileManager::Get().FileExists(*Filename))
						{
							return Fail(OutError,
								TEXT("world_spatial_graph_document_save_readback_failed"),
								Filename);
						}
					}
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("graph-editor-view-write:") + Compiled.EvidenceId);
				return OutResult.IsValid();
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				TSharedPtr<SGraphEditor> Editor;
				if (!Mutation.IsValid()
					|| !TryResolveGraphEditor(*Environment, Context, Editor, OutError))
				{
					return false;
				}
				FVector2f Location;
				float Zoom = 0.0f;
				Editor->GetViewLocation(Location, Zoom);
				return FMath::IsFinite(Location.X)
					&& FMath::IsFinite(Location.Y)
					&& FMath::IsFinite(Zoom)
						? true
						: Fail(
							OutError,
							TEXT("world_spatial_graph_view_not_settled"),
							Contract.GetSettleBarrier());
			}

			bool TryCompare(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (!Request.GetPayload().IsType<FGraphViewUpdateRequestInput>())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_graph_view_compare_request_mismatch"),
						Contract.GetCapabilityId());
				}
				const FWorldSpatialGraphViewState& ExpectedState = Request.GetPayload()
					.Get<FGraphViewUpdateRequestInput>().ViewState;
				FUeAgentWorldSpatialCompiledValue Expected;
				FUeAgentWorldSpatialCompiledValue Actual;
				double ActualZoom = 0.0;
				if (!SpaceCompiler->TryConvertToNative(
						ExpectedState.Location,
						TEXT("graph.editor.view.local"),
						TEXT("graph_unit"),
						Expected,
						OutError)
					|| !TryReadCompiledValue(
						After, TEXT("graph_view.location"), Actual, OutError)
					|| !TryReadDecimalValue(
						After, TEXT("graph_view.zoom"), ActualZoom, OutError))
				{
					return false;
				}
				SetComparisonOutcome(
					Contract,
					[&]()
					{
						if (!NearlyEqualComponents(
								Expected.Components, Actual.Components)
							|| !FMath::IsNearlyEqual(
								ExpectedState.Zoom, ActualZoom, 0.001))
						{
							return false;
						}
						const FGraphViewUpdateRequestInput& Input =
							Request.GetPayload().Get<FGraphViewUpdateRequestInput>();
						if (!Input.bUpdateDocumentState)
						{
							return true;
						}
						FUeAgentWorldSpatialCompiledValue DocumentLocation;
						double DocumentZoom = 0.0;
						const FVector2f Snapped = SnapGraphViewLocation(
							FVector2f(
								Expected.Components[0], Expected.Components[1]),
							ExpectedState.Zoom);
						if (!TryReadCompiledValue(
								After, TEXT("graph_view.document.location"),
								DocumentLocation, OutError)
							|| !TryReadDecimalValue(
								After, TEXT("graph_view.document.zoom"),
								DocumentZoom, OutError))
						{
							return false;
						}
						if (!NearlyEqualComponents(
								{Snapped.X, Snapped.Y},
								DocumentLocation.Components)
							|| !FMath::IsNearlyEqual(
								ExpectedState.Zoom, DocumentZoom, 0.001))
						{
							return false;
						}
						if (Input.bSaveAfterUpdate)
						{
							UEdGraph* Graph = nullptr;
							UBlueprint* Blueprint = nullptr;
							if (!TryResolveBlueprintGraph(
									*Environment, Context,
									Graph, Blueprint, OutError)
								|| !Blueprint)
							{
								return false;
							}
							const FString Filename =
								FPackageName::LongPackageNameToFilename(
									Blueprint->GetOutermost()->GetName(),
									FPackageName::GetAssetPackageExtension());
							return !Blueprint->GetOutermost()->IsDirty()
								&& IFileManager::Get().FileExists(*Filename);
						}
						return true;
					}(),
					TEXT("graph-editor-live-and-document-state-independent-readback"),
					OutOutcome);
				return true;
			}

		private:
			TSharedRef<IUeAgentWorldSpatialNativeEnvironment> Environment;
			TSharedRef<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
			TSharedRef<IUeAgentWorldSpatialRevisionSource> RevisionSource;
		};

		class FGraphLayoutNativeFamily final : public IWorldSpatialNativeFamily
		{
		public:
			explicit FGraphLayoutNativeFamily(
				const FUeAgentWorldSpatialNativeDependencies& InDependencies)
				: Environment(InDependencies.Environment.ToSharedRef())
				, SpaceCompiler(InDependencies.SpaceCompiler.ToSharedRef())
				, RevisionSource(InDependencies.RevisionSource.ToSharedRef())
			{
			}

			const FString& GetFamilyId() const override
			{
				static const FString Id = TEXT("graph_layout");
				return Id;
			}

			bool TryRead(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (!Request.GetPayload().IsType<FGraphLayoutApplyRequestInput>())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_graph_layout_request_mismatch"),
						Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				TArray<FObservedEntry> Entries;
				FString Fingerprint = Input.GraphRef;
				for (const FWorldSpatialNodePlacement& Placement : Input.NodePlacements)
				{
					UObject* Object = nullptr;
					if (!Environment->TryResolveObject(
							Placement.NodeRef, Object, OutError))
					{
						return false;
					}
					UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
					if (!Node)
					{
						return Fail(
							OutError,
							TEXT("world_spatial_graph_node_type_mismatch"),
							Placement.NodeRef);
					}
					FUeAgentCanonicalValuePtr Value;
					if (!TryMakeCompiledValue(
							MakeGraphValue(
								Placement.NodeRef,
								TEXT("graph.node.integer.local"),
								Node->NodePosX,
								Node->NodePosY,
								TEXT("graph-node-position-readback")),
							Context,
							Value,
							OutError))
					{
						return false;
					}
					Entries.Add({
						TEXT("world-spatial:graph-node:") + Placement.NodeRef,
						TEXT("graph_node"),
						Input.GraphRef,
						Placement.NodeRef,
						TEXT("graph_node:") + Placement.NodeRef,
						TEXT("world_spatial.compiled_value.v1"),
						Value,
					});
					Fingerprint += FString::Printf(
						TEXT("|%s|%d|%d"),
						*Placement.NodeRef,
						Node->NodePosX,
						Node->NodePosY);
				}
				return TryMakeSnapshot(
					Contract,
					Phase,
					Fingerprint,
					Entries,
					Context,
					OutSnapshot,
					OutError);
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (!Request.GetPayload().IsType<FGraphLayoutApplyRequestInput>())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_graph_layout_request_mismatch"),
						Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				if (!TryCheckRevision(
						*RevisionSource,
						Input.GraphRef,
						Input.ExpectedRevision,
						OutError))
				{
					return false;
				}
				UObject* GraphObject = nullptr;
				if (!Environment->TryResolveObject(Input.GraphRef, GraphObject, OutError)
					|| !Cast<UEdGraph>(GraphObject))
				{
					return OutError.IsValid()
						? false
						: Fail(
							OutError,
							TEXT("world_spatial_graph_type_mismatch"),
							Input.GraphRef);
				}
				for (const FWorldSpatialNodePlacement& Placement : Input.NodePlacements)
				{
					FUeAgentWorldSpatialCompiledValue Compiled;
					UObject* Object = nullptr;
					if (!SpaceCompiler->TryConvertToNative(
							Placement.Position,
							TEXT("graph.node.integer.local"),
							TEXT("graph_unit"),
							Compiled,
							OutError)
						|| !Environment->TryResolveObject(
							Placement.NodeRef, Object, OutError))
					{
						return false;
					}
					UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
					if (!Node
						|| !FMath::IsNearlyEqual(
							Compiled.Components[0],
							FMath::RoundToDouble(Compiled.Components[0]))
						|| !FMath::IsNearlyEqual(
							Compiled.Components[1],
							FMath::RoundToDouble(Compiled.Components[1])))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_graph_node_position_invalid"),
							Placement.NodeRef);
					}
					Node->NodePosX = FMath::RoundToInt(Compiled.Components[0]);
					Node->NodePosY = FMath::RoundToInt(Compiled.Components[1]);
				}
				CastChecked<UEdGraph>(GraphObject)->NotifyGraphChanged();
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("graph-node-position-write"));
				return OutResult.IsValid();
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				return Mutation.IsValid()
					? true
					: Fail(
						OutError,
						TEXT("world_spatial_graph_layout_not_settled"),
						Contract.GetSettleBarrier());
			}

			bool TryCompare(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (!Request.GetPayload().IsType<FGraphLayoutApplyRequestInput>())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_graph_layout_compare_request_mismatch"),
						Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				bool bMatches = true;
				TMap<FString, FUeAgentWorldSpatialCompiledValue> ActualByRef;
				for (const FWorldSpatialNodePlacement& Placement : Input.NodePlacements)
				{
					FUeAgentWorldSpatialCompiledValue Expected;
					FUeAgentWorldSpatialCompiledValue Actual;
					if (!SpaceCompiler->TryConvertToNative(
							Placement.Position,
							TEXT("graph.node.integer.local"),
							TEXT("graph_unit"),
							Expected,
							OutError)
						|| !TryReadCompiledValue(
							After,
							TEXT("graph_node:") + Placement.NodeRef,
							Actual,
							OutError))
					{
						return false;
					}
					bMatches &= NearlyEqualComponents(
						Expected.Components, Actual.Components);
					ActualByRef.Add(Placement.NodeRef, Actual);
				}
				for (const FWorldSpatialLayoutRelation& Relation : Input.LayoutRelations)
				{
					const FUeAgentWorldSpatialCompiledValue* Source =
						ActualByRef.Find(Relation.SourceNodeRef);
					const FUeAgentWorldSpatialCompiledValue* Target =
						ActualByRef.Find(Relation.TargetNodeRef);
					if (!Source || !Target || Relation.Relation != TEXT("right_of"))
					{
						bMatches = false;
						continue;
					}
					bMatches &= Target->Components[0] > Source->Components[0];
					if (Relation.Gap.IsSet())
					{
						bMatches &= Target->Components[0] - Source->Components[0]
							>= Relation.Gap.GetValue();
					}
				}
				SetComparisonOutcome(
					Contract,
					bMatches,
					TEXT("graph-node-position-independent-readback"),
					OutOutcome);
				return true;
			}

		private:
			TSharedRef<IUeAgentWorldSpatialNativeEnvironment> Environment;
			TSharedRef<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
			TSharedRef<IUeAgentWorldSpatialRevisionSource> RevisionSource;
		};
	}

	FWorldSpatialNativeFamilyRef MakeGraphViewNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies)
	{
		return MakeShared<FGraphViewNativeFamily>(Dependencies);
	}

	FWorldSpatialNativeFamilyRef MakeGraphLayoutNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies)
	{
		return MakeShared<FGraphLayoutNativeFamily>(Dependencies);
	}
}
