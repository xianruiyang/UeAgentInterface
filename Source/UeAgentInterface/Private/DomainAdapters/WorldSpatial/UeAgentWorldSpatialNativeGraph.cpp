#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;

	namespace
	{
		bool GraphFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.TargetPath = Target;
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		TSharedPtr<FJsonValue> GraphObjectType(const UObject* Object)
		{
			return Object ? MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>();
		}

		TSharedPtr<FJsonValue> GraphCoordinateFact(double Value, const FString& FrameId)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("frame_id"), FrameId);
			Fact->SetStringField(TEXT("unit"), TEXT("graph_unit"));
			Fact->SetField(TEXT("value"), ComparisonNumber(Value));
			return MakeShared<FJsonValueObject>(Fact);
		}

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
			if (FUeAgentCanonicalValue::TryString(Value, Context.GetResourceTracker(), OutValue, Error)) return true;
			OutError = MoveTemp(Error);
			return false;
		}

		bool TryResolveGraphEditor(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<SGraphEditor>& OutEditor,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FString& Identity =
				Context.GetExecutionContext().GetAssetEditorIdentity();
			if (Identity.IsEmpty())
				return GraphFailure(OutError, TEXT("world_spatial_graph_editor_unresolved"),
					TEXT("Graph editing requires an asset editor reference."), TEXT("/asset_editor_identity"),
					MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("non-empty graph editor reference")), Identity);
			if (!Environment.TryResolveGraphEditor(Identity, OutEditor, OutError))
			{
				if (OutError.IsValid()) return false;
				return GraphFailure(OutError, TEXT("world_spatial_graph_editor_unresolved"),
					TEXT("The native graph editor provider failed without returning a diagnostic."), TEXT("/asset_editor_identity"),
					MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("resolved graph editor")), Identity);
			}
			if (!OutEditor.IsValid())
			{
				if (OutError.IsValid()) return false;
				return GraphFailure(OutError, TEXT("world_spatial_graph_editor_unresolved"),
					TEXT("The native graph editor provider returned no graph editor."), TEXT("/graph_editor"),
					nullptr, MakeShared<FJsonValueString>(TEXT("valid graph editor")), Identity);
			}
			return true;
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
				return GraphFailure(OutError, TEXT("world_spatial_graph_identity_type_invalid"),
					TEXT("The graph reference resolves to an object that is not a graph."), TEXT("/asset_editor_identity"),
					GraphObjectType(Object), MakeShared<FJsonValueString>(TEXT("UEdGraph")), Identity);
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
				const FUeAgentWorldSpatialPreflightFence& Fence,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
				if (!Request.GetPayload().IsType<FGraphViewUpdateRequestInput>())
				{
					return GraphFailure(OutError, TEXT("world_spatial_graph_view_request_mismatch"),
						TEXT("Graph view mutation requires a graph view update request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()), MakeShared<FJsonValueString>(TEXT("graph view update request")), Contract.GetCapabilityId());
				}
				const FGraphViewUpdateRequestInput& Input = Request.GetPayload()
					.Get<FGraphViewUpdateRequestInput>();
				const FWorldSpatialGraphViewState& State = Input.ViewState;
				const FString& GraphIdentity =
					Context.GetExecutionContext().GetAssetEditorIdentity();
				if (State.Location.Subject != GraphIdentity)
				{
					return GraphFailure(OutError, TEXT("world_spatial_graph_view_subject_mismatch"),
						TEXT("The view location subject does not match the graph editor reference."), TEXT("/view_state/location/subject"),
						MakeShared<FJsonValueString>(State.Location.Subject), MakeShared<FJsonValueString>(GraphIdentity), GraphIdentity);
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
				OutResult = {};
				Editor->SetViewLocation(
					FVector2f(Compiled.Components[0], Compiled.Components[1]),
					State.Zoom);
				if (Input.bUpdateDocumentState)
				{
					UEdGraph* Graph = nullptr;
					UBlueprint* Blueprint = nullptr;
					if (!TryResolveBlueprintGraph(
							*Environment, Context, Graph, Blueprint, OutError))
					{
						return false;
					}
					if (!Blueprint)
						return GraphFailure(OutError, TEXT("world_spatial_graph_document_state_unsupported"),
							TEXT("The graph has no owning Blueprint in which to store document view state."), TEXT("/blueprint"),
							nullptr, MakeShared<FJsonValueString>(TEXT("owning Blueprint")), GraphIdentity);
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
						if (!UE::UAI::EditorResources::SavePackages(
								{Blueprint->GetOutermost()}, false, OutError))
						{
							Blueprint->LastEditedDocuments = BeforeDocuments;
							Blueprint->MarkPackageDirty();
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = GraphIdentity;
							return false;
						}
						const FString Filename =
							FPackageName::LongPackageNameToFilename(
								Blueprint->GetOutermost()->GetName(),
								FPackageName::GetAssetPackageExtension());
						if (Blueprint->GetOutermost()->IsDirty())
							return GraphFailure(OutError, TEXT("world_spatial_graph_document_save_readback_failed"),
								TEXT("The graph's package still has unsaved changes after saving."), TEXT("/package/dirty"),
								MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), GraphIdentity);
						if (!IFileManager::Get().FileExists(*Filename))
						{
							GraphFailure(OutError, TEXT("world_spatial_graph_document_save_readback_failed"),
								TEXT("The graph's package file is missing after saving."), TEXT("/package/header/exists"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), GraphIdentity);
							OutError.ActualEvidence = MakeShared<FJsonValueString>(Filename);
							return false;
						}
					}
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("graph-editor-view-write:") + Compiled.EvidenceId);
				return OutResult.TryValidate(OutError);
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				TSharedPtr<SGraphEditor> Editor;
				if (!Mutation.TryValidate(OutError)
					|| !TryResolveGraphEditor(*Environment, Context, Editor, OutError))
				{
					return false;
				}
				FVector2f Location;
				float Zoom = 0.0f;
				Editor->GetViewLocation(Location, Zoom);
				if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
				{
					const bool bInvalidX = !FMath::IsFinite(Location.X);
					return GraphFailure(OutError, TEXT("world_spatial_graph_view_not_settled"),
						TEXT("The graph editor returned a non-finite view coordinate."), bInvalidX ? TEXT("/view/location/0") : TEXT("/view/location/1"),
						GraphCoordinateFact(bInvalidX ? Location.X : Location.Y, TEXT("graph.editor.view.local")),
						MakeShared<FJsonValueString>(TEXT("finite graph coordinate")), Context.GetExecutionContext().GetAssetEditorIdentity());
				}
				if (!FMath::IsFinite(Zoom))
					return GraphFailure(OutError, TEXT("world_spatial_graph_view_not_settled"),
						TEXT("The graph editor returned a non-finite zoom value."), TEXT("/view/zoom"),
						ComparisonNumber(Zoom), MakeShared<FJsonValueString>(TEXT("finite zoom")), Context.GetExecutionContext().GetAssetEditorIdentity());
				return true;
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
					return GraphFailure(OutError, TEXT("world_spatial_graph_view_compare_request_mismatch"),
						TEXT("Graph view verification requires a graph view update request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()), MakeShared<FJsonValueString>(TEXT("graph view update request")), Contract.GetCapabilityId());
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
				TArray<UE::UAI::Contract::FDiagnostic> Differences;
				ON_SCOPE_EXIT { if (!OutError.IsEmpty()) for (const auto& Difference : Differences) OutError.Append(Difference); };
				[&]()
					{
						if (!CompareComponents(Expected.Components, Actual.Components, Differences,
							TEXT("graph_view.location"), Expected.Subject)) return false;
						if (!FMath::IsNearlyEqual(ExpectedState.Zoom, ActualZoom, 0.001))
						{
							AddComparisonDifference(Differences, TEXT("graph_view.zoom"), TEXT("The live graph zoom differs from the requested zoom."),
								ComparisonNumber(ExpectedState.Zoom), ComparisonNumber(ActualZoom), Expected.Subject);
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
						if (!CompareComponents({Snapped.X, Snapped.Y}, DocumentLocation.Components, Differences,
							TEXT("graph_view.document.location"), Expected.Subject)) return false;
						if (!FMath::IsNearlyEqual(ExpectedState.Zoom, DocumentZoom, 0.001))
						{
							AddComparisonDifference(Differences, TEXT("graph_view.document.zoom"), TEXT("The persisted graph view zoom differs from the requested zoom."),
								ComparisonNumber(ExpectedState.Zoom), ComparisonNumber(DocumentZoom), Expected.Subject);
							return false;
						}
						if (Input.bSaveAfterUpdate)
						{
							UEdGraph* Graph = nullptr;
							UBlueprint* Blueprint = nullptr;
							if (!TryResolveBlueprintGraph(
									*Environment, Context,
									Graph, Blueprint, OutError))
							{
								return false;
							}
							if (!Blueprint)
							{
								OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_graph_view_readback_owner_missing"),
									TEXT("The graph readback did not resolve its owning Blueprint for save verification."), TEXT("verification"));
								OutError.TargetPath = Expected.Subject;
								return false;
							}
							const FString Filename =
								FPackageName::LongPackageNameToFilename(
									Blueprint->GetOutermost()->GetName(),
									FPackageName::GetAssetPackageExtension());
							if (Blueprint->GetOutermost()->IsDirty())
							{
								AddComparisonDifference(Differences, TEXT("graph_view.document.dirty"), TEXT("The Blueprint package is still dirty after the requested save."),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Blueprint->GetPathName());
								return false;
							}
							if (!IFileManager::Get().FileExists(*Filename))
							{
								AddComparisonDifference(Differences, TEXT("graph_view.document.file_exists"), TEXT("The Blueprint package file is absent after the requested save."),
									MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Blueprint->GetPathName());
								return false;
							}
						}
						return true;
					}();
				if (!OutError.IsEmpty()) return false;
				SetComparisonOutcome(Contract, MoveTemp(Differences),
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
					return GraphFailure(OutError, TEXT("world_spatial_graph_layout_request_mismatch"),
						TEXT("Graph layout readback requires a graph layout apply request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()), MakeShared<FJsonValueString>(TEXT("graph layout apply request")), Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				TArray<FObservedEntry> Entries;
				FString Fingerprint = Input.GraphRef;
				for (int32 Index = 0; Index < Input.NodePlacements.Num(); ++Index)
				{
					const auto& Placement = Input.NodePlacements[Index];
					UObject* Object = nullptr;
					if (!Environment->TryResolveObject(
							Placement.NodeRef, Object, OutError))
					{
						return false;
					}
					UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
					if (!Node)
					{
						return GraphFailure(OutError, TEXT("world_spatial_graph_node_type_mismatch"),
							TEXT("The node reference resolves to an object that is not a graph node."),
							FString::Printf(TEXT("/node_placements/%d/node_ref"), Index), GraphObjectType(Object),
							MakeShared<FJsonValueString>(TEXT("UEdGraphNode")), Placement.NodeRef);
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
				const FUeAgentWorldSpatialPreflightFence& Fence,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
				if (!Request.GetPayload().IsType<FGraphLayoutApplyRequestInput>())
				{
					return GraphFailure(OutError, TEXT("world_spatial_graph_layout_request_mismatch"),
						TEXT("Graph layout mutation requires a graph layout apply request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()), MakeShared<FJsonValueString>(TEXT("graph layout apply request")), Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				UObject* GraphObject = nullptr;
				if (!Environment->TryResolveObject(Input.GraphRef, GraphObject, OutError))
				{
					return false;
				}
				if (!Cast<UEdGraph>(GraphObject))
					return GraphFailure(OutError, TEXT("world_spatial_graph_type_mismatch"),
						TEXT("The graph reference resolves to an object that is not a graph."), TEXT("/graph_ref"),
						GraphObjectType(GraphObject), MakeShared<FJsonValueString>(TEXT("UEdGraph")), Input.GraphRef);
				for (int32 Index = 0; Index < Input.NodePlacements.Num(); ++Index)
				{
					const auto& Placement = Input.NodePlacements[Index];
					const FString Path = FString::Printf(TEXT("/node_placements/%d"), Index);
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
					if (!Node)
						return GraphFailure(OutError, TEXT("world_spatial_graph_node_position_invalid"),
							TEXT("The placement reference resolves to an object that is not a graph node."), Path + TEXT("/node_ref"),
							GraphObjectType(Object), MakeShared<FJsonValueString>(TEXT("UEdGraphNode")), Placement.NodeRef);
					for (int32 Axis = 0; Axis < 2; ++Axis)
					{
						const double Coordinate = Compiled.Components[Axis];
						const double Rounded = FMath::RoundToDouble(Coordinate);
						if (!FMath::IsNearlyEqual(Coordinate, Rounded))
							return GraphFailure(OutError, TEXT("world_spatial_graph_node_position_invalid"),
								TEXT("Graph node placement requires integer coordinates in the graph node frame."),
								Path + FString::Printf(TEXT("/position/components/%d"), Axis),
								GraphCoordinateFact(Coordinate, Compiled.FrameId), GraphCoordinateFact(Rounded, Compiled.FrameId), Placement.NodeRef);
					}
					OutResult = {};
					Node->NodePosX = FMath::RoundToInt(Compiled.Components[0]);
					Node->NodePosY = FMath::RoundToInt(Compiled.Components[1]);
				}
				OutResult = {};
				CastChecked<UEdGraph>(GraphObject)->NotifyGraphChanged();
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("graph-node-position-write"));
				return OutResult.TryValidate(OutError);
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				return Mutation.TryValidate(OutError);
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
					return GraphFailure(OutError, TEXT("world_spatial_graph_layout_compare_request_mismatch"),
						TEXT("Graph layout verification requires a graph layout apply request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()), MakeShared<FJsonValueString>(TEXT("graph layout apply request")), Contract.GetCapabilityId());
				}
				const FGraphLayoutApplyRequestInput& Input = Request.GetPayload()
					.Get<FGraphLayoutApplyRequestInput>();
				TArray<UE::UAI::Contract::FDiagnostic> Differences;
				ON_SCOPE_EXIT { if (!OutError.IsEmpty()) for (const auto& Difference : Differences) OutError.Append(Difference); };
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
					CompareComponents(Expected.Components, Actual.Components, Differences, TEXT("graph_node.position"), Placement.NodeRef);
					ActualByRef.Add(Placement.NodeRef, Actual);
				}
				for (int32 RelationIndex = 0; RelationIndex < Input.LayoutRelations.Num(); ++RelationIndex)
				{
					const FWorldSpatialLayoutRelation& Relation = Input.LayoutRelations[RelationIndex];
					const FString Path = FString::Printf(TEXT("graph_layout.relations[%d]"), RelationIndex);
					const FUeAgentWorldSpatialCompiledValue* Source =
						ActualByRef.Find(Relation.SourceNodeRef);
					const FUeAgentWorldSpatialCompiledValue* Target =
						ActualByRef.Find(Relation.TargetNodeRef);
					if (!Source || !Target)
					{
						AddComparisonDifference(Differences, Path + (!Source ? TEXT(".source_node_ref") : TEXT(".target_node_ref")),
							TEXT("The layout relation refers to a node without a position observation."),
							MakeShared<FJsonValueString>(!Source ? Relation.SourceNodeRef : Relation.TargetNodeRef), nullptr);
						continue;
					}
					if (Relation.Relation != TEXT("right_of"))
					{
						AddComparisonDifference(Differences, Path + TEXT(".relation"), TEXT("The native layout verifier does not support the requested relation."),
							MakeShared<FJsonValueString>(TEXT("right_of")), MakeShared<FJsonValueString>(Relation.Relation));
						continue;
					}
					if (!(Target->Components[0] > Source->Components[0]))
						AddComparisonDifference(Differences, Path + TEXT(".target_x"), TEXT("The target node X position must be greater than the source node X position."),
							ComparisonNumber(Source->Components[0]), ComparisonNumber(Target->Components[0]), Relation.TargetNodeRef);
					if (Relation.Gap.IsSet())
					{
						const double Gap = Target->Components[0] - Source->Components[0];
						if (!(Gap >= Relation.Gap.GetValue()))
							AddComparisonDifference(Differences, Path + TEXT(".gap"), TEXT("The observed horizontal gap is smaller than the requested minimum."),
								ComparisonNumber(Relation.Gap.GetValue()), ComparisonNumber(Gap), Relation.TargetNodeRef);
					}
				}
				SetComparisonOutcome(
					Contract,
					MoveTemp(Differences),
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
