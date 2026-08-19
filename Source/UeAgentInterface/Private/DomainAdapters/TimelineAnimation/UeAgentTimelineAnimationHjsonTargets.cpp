#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationHjsonTargets.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Verification;

	class FUeAgentTimelineAnimationHjsonOperation::FState final
	{
	public:
		explicit FState(FUeAgentHjsonTargetImportRequest InRequest)
			: Request(MoveTemp(InRequest))
		{
		}

		FUeAgentHjsonTargetImportRequest Request;
		const FUeAgentOperationContext* OperationContext = nullptr;
		uint8 CompletedStages = 0;
		UObject* Asset = nullptr;
		FString PreRevision;
		FString PersistedRevision;
		TOptional<FUeAgentCanonicalIdentity> Identity;
		FUeAgentCanonicalValuePtr BeforeValue;
		FUeAgentCanonicalValuePtr AfterValue;
		double DesiredRateScale = 0.0;
		bool bDesiredRootMotion = false;
		int32 DesiredPlaybackStart = 0;
		int32 DesiredPlaybackDuration = 0;
		bool bMutationApplied = false;
		bool bCompileSettled = false;
		bool bPackageSaved = false;
		bool bUnloadedReloaded = false;
		TMap<FString, FString> PersistedValues;
		FUeAgentHjsonTargetImportReceipt ImportReceipt;
	};

	namespace
	{
		void SetError(
			FUeAgentHjsonImportTargetError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		FUeAgentResourceTracker MakeWorkspaceTracker()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1ll << 34},
				{EUeAgentResourceDimension::OutputBytes, 1ll << 34},
				{EUeAgentResourceDimension::StringBytes, 1ll << 34},
				{EUeAgentResourceDimension::Nodes, 1ll << 28},
				{EUeAgentResourceDimension::Items, 1ll << 28},
				{EUeAgentResourceDimension::Files, 1ll << 20},
				{EUeAgentResourceDimension::ReferenceEdges, 1ll << 28},
				{EUeAgentResourceDimension::WorkUnits, 1ll << 30},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 1ll << 36},
			}));
		}

		FString CanonicalDocumentHash(const TSharedRef<FJsonObject>& Document)
		{
			FString Canonical;
			FString Error;
			if (!Contract::Json::TryCanonicalize(Document, Canonical, Error))
			{
				return FString();
			}
			return Contract::Json::Sha256Utf8(Canonical);
		}

		TSharedRef<FJsonObject> Rate(const FFrameRate& Value)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("numerator"), FString::FromInt(Value.Numerator));
			Result->SetStringField(TEXT("denominator"), FString::FromInt(Value.Denominator));
			return Result;
		}

		TSharedRef<FJsonObject> AnimationSequenceDocument(
			const UAnimSequence& Sequence)
		{
			const IAnimationDataModel* Model = Sequence.GetDataModel();
			const FFrameRate SampleRate = Model
				? Model->GetFrameRate()
				: Sequence.GetSamplingFrameRate();
			const int32 FrameCount = Model
				? Model->GetNumberOfFrames()
				: FMath::Max(0, Sequence.GetNumberOfSampledKeys() - 1);

			TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
			Metadata->SetNumberField(TEXT("rate_scale"), Sequence.RateScale);
			Metadata->SetBoolField(
				TEXT("enable_root_motion"), Sequence.bEnableRootMotion);

			TArray<TSharedPtr<FJsonValue>> BoneTracks;
			if (Model)
			{
				TArray<FName> TrackNames;
				Model->GetBoneTrackNames(TrackNames);
				TrackNames.Sort(FNameLexicalLess());
				for (const FName TrackName : TrackNames)
				{
					TSharedRef<FJsonObject> Track = MakeShared<FJsonObject>();
					Track->SetStringField(TEXT("bone_ref"), TrackName.ToString());
					Track->SetNumberField(TEXT("key_count"), Model->GetNumberOfKeys());
					BoneTracks.Add(MakeShared<FJsonValueObject>(Track));
				}
			}

			TArray<TSharedPtr<FJsonValue>> Events;
			for (const FAnimNotifyEvent& Notify : Sequence.Notifies)
			{
				TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetStringField(
					TEXT("event_ref"),
					Notify.Guid.IsValid()
						? Notify.Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
						: Notify.NotifyName.ToString());
				Event->SetStringField(TEXT("event_kind"),
					Notify.NotifyStateClass ? TEXT("notify_state") : TEXT("notify"));
				Event->SetStringField(
					TEXT("tick"),
					FString::FromInt(SampleRate.AsFrameTime(Notify.GetTime()).RoundToFrame().Value));
				Event->SetStringField(TEXT("track_ref"), FString::FromInt(Notify.TrackIndex));
				Events.Add(MakeShared<FJsonValueObject>(Event));
			}

			TSharedRef<FJsonObject> Timeline = MakeShared<FJsonObject>();
			Timeline->SetObjectField(TEXT("sample_rate"), Rate(SampleRate));
			Timeline->SetStringField(TEXT("authored_start_tick"), TEXT("0"));
			Timeline->SetStringField(
				TEXT("authored_duration_ticks"), FString::FromInt(FrameCount));
			Timeline->SetObjectField(TEXT("metadata"), Metadata);
			Timeline->SetArrayField(TEXT("bone_tracks"), MoveTemp(BoneTracks));
			Timeline->SetArrayField(TEXT("curves"), {});
			Timeline->SetArrayField(TEXT("events"), MoveTemp(Events));

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"), Sequence.GetPathName());
			Result->SetStringField(
				TEXT("skeleton_ref"),
				Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
			Result->SetObjectField(TEXT("timeline"), Timeline);
			return Result;
		}

		TSharedRef<FJsonObject> LevelSequenceDocument(
			const ULevelSequence& Sequence)
		{
			const UMovieScene* MovieScene = Sequence.GetMovieScene();
			TSharedRef<FJsonObject> Timeline = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Bindings;
			TArray<TSharedPtr<FJsonValue>> Tracks;
			if (MovieScene)
			{
				Timeline->SetObjectField(
					TEXT("display_rate"), Rate(MovieScene->GetDisplayRate()));
				Timeline->SetObjectField(
					TEXT("tick_resolution"), Rate(MovieScene->GetTickResolution()));
				const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
				const int32 Start = Playback.HasLowerBound()
					? Playback.GetLowerBoundValue().Value : 0;
				const int32 End = Playback.HasUpperBound()
					? Playback.GetUpperBoundValue().Value : Start;
				Timeline->SetStringField(TEXT("playback_start_tick"), FString::FromInt(Start));
				Timeline->SetStringField(
					TEXT("playback_duration_ticks"), FString::FromInt(FMath::Max(0, End - Start)));

				for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(
						TEXT("binding_ref"),
						Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
					Item->SetStringField(TEXT("display_name"), Binding.GetName());
					Item->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());
					Bindings.Add(MakeShared<FJsonValueObject>(Item));
				}
				for (UMovieSceneTrack* Track : MovieScene->GetTracks())
				{
					if (!Track)
					{
						continue;
					}
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("track_ref"), Track->GetSignature().ToString(EGuidFormats::DigitsWithHyphensLower));
					Item->SetStringField(TEXT("track_kind"), Track->GetClass()->GetPathName());
					Item->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
					Item->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
					Tracks.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			else
			{
				Timeline->SetObjectField(TEXT("display_rate"), Rate(FFrameRate(24, 1)));
				Timeline->SetObjectField(TEXT("tick_resolution"), Rate(FFrameRate(24000, 1)));
				Timeline->SetStringField(TEXT("playback_start_tick"), TEXT("0"));
				Timeline->SetStringField(TEXT("playback_duration_ticks"), TEXT("0"));
			}
			Timeline->SetArrayField(TEXT("bindings"), MoveTemp(Bindings));
			Timeline->SetArrayField(TEXT("tracks"), MoveTemp(Tracks));

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"), Sequence.GetPathName());
			Result->SetObjectField(TEXT("movie_scene"), Timeline);
			return Result;
		}

		UObject* LoadExactTarget(
			const FString& CompositionKind,
			const FString& AssetPath)
		{
			UClass* ExpectedClass =
				CompositionKind == TEXT("ue.animation_sequence.asset")
					? UAnimSequence::StaticClass()
					: CompositionKind == TEXT("ue.level_sequence.asset")
						? ULevelSequence::StaticClass()
						: nullptr;
			if (!ExpectedClass)
			{
				return nullptr;
			}
			return StaticLoadObject(ExpectedClass, nullptr, *AssetPath);
		}

		TSharedRef<FJsonObject> CaptureDocument(
			const FString& CompositionKind,
			UObject& Asset)
		{
			if (CompositionKind == TEXT("ue.animation_sequence.asset"))
			{
				return AnimationSequenceDocument(
					*CastChecked<UAnimSequence>(&Asset));
			}
			return LevelSequenceDocument(*CastChecked<ULevelSequence>(&Asset));
		}

		UeAgentAuthoringState::FStateEntry Entry(
			const FString& SemanticId,
			const FString& CompositionKind,
			const FString& AssetPath,
			const FString& Suffix,
			const TSharedRef<FJsonObject>& Value,
			const bool bEditable)
		{
			UeAgentAuthoringState::FStateEntry Result;
			Result.SemanticId = SemanticId;
			Result.TargetIdentity.ModelId = TEXT("timeline_animation_asset");
			Result.TargetIdentity.TargetKind = CompositionKind;
			Result.TargetIdentity.KeyPattern = TEXT("asset_path");
			Result.TargetIdentity.CanonicalKey =
				TEXT("timeline_animation_asset|") + CompositionKind
				+ TEXT("|asset_path=") + AssetPath + Suffix;
			Result.TargetIdentity.Selectors.Add(TEXT("asset_path"), AssetPath);
			Result.Type.Kind = TEXT("object");
			Result.Value = MakeShared<FJsonValueObject>(Value);
			Result.bAvailable = true;
			Result.bEditable = bEditable;
			Result.AccessorId = TEXT("timeline_animation.hjson.capture");
			Result.Provenance = TEXT("timeline_animation.native_readback");
			return Result;
		}

		bool TryPublish(
			const FUeAgentHjsonTargetExportRequest& Request,
			const TSharedRef<FJsonObject>& Document,
			const FString& Revision,
			FUeAgentHjsonTargetExportReceipt& OutReceipt,
			FUeAgentHjsonImportTargetError& OutError)
		{
			FString RegisterError;
			if (!RegisterOtherTargetCompositionContracts(RegisterError))
			{
				SetError(OutError, TEXT("timeline_composition_contract_unavailable"), Request.GetCompositionKind(), RegisterError);
				return false;
			}
			FCompositionRequest Composition;
			Composition.TargetKind = Request.GetCompositionKind();
			Composition.ProfileId = Request.GetProfileId();
			Composition.BaseRevision = Revision;
			Composition.SourceProvenance = TEXT("timeline_animation.native_readback");
			Composition.Entries = {
				Entry(
					TEXT("uai.semantic.timeline.export.target"),
					Request.GetCompositionKind(),
					Request.GetTargetAssetPath(),
					TEXT("|target"),
					MakeShared<FJsonObject>(),
					false),
				Entry(
					Request.GetCompositionKind()
						== TEXT("ue.animation_sequence.asset")
							? TEXT("uai.semantic.timeline.animation_sequence.document")
							: TEXT("uai.semantic.timeline.level_sequence.document"),
					Request.GetCompositionKind(),
					Request.GetTargetAssetPath(),
					TEXT("|document"),
					Document,
					true),
			};
			Composition.Entries[0].Value->AsObject()->SetStringField(
				TEXT("asset_path"), Request.GetTargetAssetPath());

			FUeAgentResourceTracker Resources = MakeWorkspaceTracker();
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryComposeAuthoringModel(
					Composition, Resources, Model, WorkspaceError)
				|| !Model.IsValid())
			{
				SetError(OutError, WorkspaceError.GetCode(), WorkspaceError.GetSourceUnit());
				return false;
			}
			const FCompositionSnapshotBinding SnapshotBinding{
				TEXT("timeline:") + Revision.Right(24),
				FUeAgentStateRevision(
					Request.GetTargetAssetPath(), Revision.Right(32), 1),
				TEXT("timeline_animation.native_readback:") + Revision.Right(24),
			};

			FExportWorkspacePlanPtr Plan;
			if (!TryBuildCompositionExportPlan(
					Model.ToSharedRef(),
					SnapshotBinding,
					{},
					Resources,
					Plan,
					WorkspaceError)
				|| !Plan.IsValid())
			{
				SetError(OutError, WorkspaceError.GetCode(), WorkspaceError.GetSourceUnit());
				return false;
			}
			FArtifactPublishReceipt PublishReceipt;
			FArtifactPublishError PublishError;
			if (!TryPublishExportPlan(
					FArtifactPublishRequest(
						Plan.ToSharedRef(),
						Request.GetWorkspaceRoot(),
						TEXT("timeline_animation.hjson.")
							+ Request.GetCompositionKind()),
					Resources,
					PublishReceipt,
					PublishError))
			{
				SetError(OutError, PublishError.GetCode(), Request.GetTargetAssetPath(), PublishError.GetDetail());
				return false;
			}
			OutReceipt = FUeAgentHjsonTargetExportReceipt(
				Request.GetCompositionKind(),
				Request.GetTargetAssetPath(),
				PublishReceipt.GetFinalPath(),
				Revision,
				Model->GetSemanticFingerprint());
			return OutReceipt.IsValid();
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Root,
			const FString& FieldId)
		{
			TArray<const FUeAgentCanonicalValue*> Stack{&Root};
			while (!Stack.IsEmpty())
			{
				const FUeAgentCanonicalValue* Value =
					Stack.Pop(EAllowShrinking::No);
				if (Value->GetKind() == EUeAgentValueKind::Object)
				{
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
						: Value->GetObjectItems())
					{
						if (Pair.Key == FieldId)
						{
							return &Pair.Value.Get();
						}
						Stack.Add(&Pair.Value.Get());
					}
				}
				else if (Value->GetKind() == EUeAgentValueKind::Array)
				{
					for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
					{
						Stack.Add(&Item.Get());
					}
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValue* FindField(
			const FAuthoringModel& Model,
			const FString& FieldId)
		{
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				if (const FUeAgentCanonicalValueRef* Direct =
					Unit.GetValues().Find(FieldId))
				{
					return &Direct->Get();
				}
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
					: Unit.GetValues())
				{
					if (const FUeAgentCanonicalValue* Found =
						FindField(Pair.Value.Get(), FieldId))
					{
						return Found;
					}
				}
			}
			return nullptr;
		}

		bool ValidateTargetBinding(
			const FResolvedSemanticWorkspace& Workspace,
			const FUeAgentHjsonTargetImportRequest& Request,
			FUeAgentHjsonImportTargetError& OutError)
		{
			if (Workspace.GetModel()->GetBaseRevision()
				!= Request.GetExpectedBaseRevision())
			{
				SetError(OutError, TEXT("timeline_hjson_base_revision_mismatch"), Request.GetTargetAssetPath());
				return false;
			}
			int32 TargetCount = 0;
			for (const FSemanticUnit& Unit : Workspace.GetModel()->GetUnits())
			{
				if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState)
				{
					++TargetCount;
					if (Unit.GetSemanticTypeRef() != Request.GetCompositionKind())
					{
						SetError(OutError, TEXT("timeline_hjson_composition_kind_mismatch"), Unit.GetSemanticTypeRef());
						return false;
					}
				}
			}
			const FUeAgentCanonicalValue* AssetPath =
				FindField(*Workspace.GetModel(), TEXT("asset_path"));
			if (TargetCount != 1 || !AssetPath
				|| AssetPath->GetKind() != EUeAgentValueKind::String
				|| AssetPath->GetText() != Request.GetTargetAssetPath())
			{
				SetError(OutError, TEXT("timeline_hjson_target_identity_mismatch"), Request.GetTargetAssetPath());
				return false;
			}
			return true;
		}

		bool SavePackage(
			UObject& Asset,
			const FString& AssetPath,
			FUeAgentHjsonImportTargetError& OutError)
		{
			UPackage* Package = Asset.GetOutermost();
			if (!Package
				|| !UEditorLoadingAndSavingUtils::SavePackages({Package}, false))
			{
				SetError(OutError, TEXT("timeline_hjson_package_save_failed"), AssetPath);
				return false;
			}
			return true;
		}

		bool UnloadAndReload(
			UObject*& Asset,
			const FString& AssetPath,
			UClass* ExpectedClass,
			UObject*& OutReloaded,
			FUeAgentHjsonImportTargetError& OutError)
		{
			OutReloaded = nullptr;
			UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
			if (!Package)
			{
				SetError(
					OutError,
					TEXT("timeline_hjson_package_unload_failed"),
					AssetPath,
					TEXT("package_missing"));
				return false;
			}
			const TWeakObjectPtr<UObject> Original(Asset);
			Asset = nullptr;
			FText UnloadError;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package}, UnloadError, true);
			CollectGarbage(RF_NoFlags, true);
			if (!bUnloaded || Original.IsValid())
			{
				SetError(OutError, TEXT("timeline_hjson_package_unload_failed"), AssetPath, UnloadError.ToString());
				return false;
			}
			OutReloaded = StaticLoadObject(ExpectedClass, nullptr, *AssetPath);
			if (!OutReloaded)
			{
				SetError(OutError, TEXT("timeline_hjson_package_reload_failed"), AssetPath);
				return false;
			}
			return true;
		}

		void CopyError(
			const FUeAgentHjsonImportTargetError& Source,
			FUeAgentOperationBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.SubjectId;
			OutError.Detail = Source.Detail;
		}

		bool BindStageContext(
			FUeAgentTimelineAnimationHjsonOperation::FState& State,
			const FUeAgentOperationContext& Context,
			const uint8 StageBit,
			FUeAgentOperationBindingError& OutError)
		{
			if (!State.OperationContext)
			{
				State.OperationContext = &Context;
			}
			if (State.OperationContext != &Context
				|| (State.CompletedStages & StageBit) != 0)
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_operation_context_mismatch");
				OutError.SubjectId = Context.GetRequestId();
				OutError.Detail = TEXT(
					"prepare, plan, execute and verify must share one "
					"request-scoped OperationContext exactly once.");
				return false;
			}
			State.CompletedStages |= StageBit;
			return true;
		}

		bool Checkpoint(
			const FUeAgentOperationContext& Context,
			const FString& Stage,
			FUeAgentOperationBindingError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Context.GetCancellationToken().TryCheckpoint(Cancelled))
			{
				OutError = {};
				OutError.Code = TEXT("operation_cancelled");
				OutError.SubjectId = Context.GetRequestId();
				OutError.Detail = Cancelled.Reason;
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Context.GetDeadline().TryCheckpoint(Stage, Expired))
			{
				OutError = {};
				OutError.Code = TEXT("operation_deadline_exceeded");
				OutError.SubjectId = Context.GetRequestId();
				OutError.Detail = Expired.Stage;
				return false;
			}
			FUeAgentResourceBudgetExceeded Budget;
			if (!Context.GetResourceTracker().TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					Stage,
					Budget))
			{
				OutError = {};
				OutError.Code = Budget.Code;
				OutError.SubjectId = Budget.Path;
				OutError.Detail = TEXT("HJSON operation work budget exceeded.");
				OutError.ResourceDimension = Budget.Dimension;
				OutError.ResourceLimit = Budget.Limit;
				OutError.ResourceObserved = Budget.Observed;
				return false;
			}
			return true;
		}

		bool TryCanonicalAnimationState(
			const double RateScale,
			const bool bRootMotion,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentOperationBindingError& OutError)
		{
			FUeAgentCanonicalValuePtr Rate;
			FUeAgentCanonicalValuePtr RootMotion;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(RateScale),
					Resources,
					Rate,
					Error)
				|| !FUeAgentCanonicalValue::TryBoolean(
					bRootMotion,
					Resources,
					RootMotion,
					Error)
				|| !FUeAgentCanonicalValue::TryObject(
					{
						{TEXT("enable_root_motion"), RootMotion.ToSharedRef()},
						{TEXT("rate_scale"), Rate.ToSharedRef()},
					},
					Resources,
					OutValue,
					Error))
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_canonical_state_invalid");
				OutError.SubjectId = Error.Path;
				OutError.Detail = Error.Code;
				return false;
			}
			return true;
		}

		bool TryCanonicalLevelState(
			const int32 Start,
			const int32 Duration,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentOperationBindingError& OutError)
		{
			FUeAgentCanonicalValuePtr StartValue;
			FUeAgentCanonicalValuePtr DurationValue;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Start),
					Resources,
					StartValue,
					Error)
				|| !FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Duration),
					Resources,
					DurationValue,
					Error)
				|| !FUeAgentCanonicalValue::TryObject(
					{
						{TEXT("playback_duration_ticks"), DurationValue.ToSharedRef()},
						{TEXT("playback_start_tick"), StartValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					Error))
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_canonical_state_invalid");
				OutError.SubjectId = Error.Path;
				OutError.Detail = Error.Code;
				return false;
			}
			return true;
		}

		bool TryValidateCanonicalInput(
			const FUeAgentContractValue& CanonicalInput,
			const FUeAgentHjsonTargetImportRequest& Request,
			FUeAgentOperationBindingError& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			FString Error;
			FString CompositionKind;
			FString Target;
			FString WorkspaceFingerprint;
			FString ExpectedRevision;
			FString Intent;
			bool bDryRun = false;
			if (!Contract::Json::TryParseObject(
					CanonicalInput.GetJsonValue(), Root, Error)
				|| !Root.IsValid()
				|| !Root->TryGetStringField(
					TEXT("composition_kind"), CompositionKind)
				|| !Root->TryGetStringField(TEXT("target"), Target)
				|| !Root->TryGetStringField(
					TEXT("workspace_fingerprint"), WorkspaceFingerprint)
				|| !Root->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision)
				|| !Root->TryGetStringField(TEXT("intent"), Intent)
				|| !Root->TryGetBoolField(TEXT("dry_run"), bDryRun)
				|| CompositionKind != Request.GetCompositionKind()
				|| Target != Request.GetTargetAssetPath()
				|| WorkspaceFingerprint
					!= Request.GetWorkspace()->GetWorkspaceFingerprint()
				|| ExpectedRevision != Request.GetExpectedBaseRevision()
				|| Intent != LexToString(Request.GetIntent())
				|| bDryRun != Request.IsDryRun())
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_canonical_input_mismatch");
				OutError.SubjectId = Request.GetTargetAssetPath();
				OutError.Detail = Error;
				return false;
			}
			return true;
		}

		bool TryPrepareNativeState(
			FUeAgentTimelineAnimationHjsonOperation::FState& State,
			const FUeAgentOperationContext& Context,
			FUeAgentOperationBindingError& OutError)
		{
			const FUeAgentHjsonTargetImportRequest& Request = State.Request;
			State.Asset = LoadExactTarget(
				Request.GetCompositionKind(), Request.GetTargetAssetPath());
			if (!State.Asset)
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_target_not_found");
				OutError.SubjectId = Request.GetTargetAssetPath();
				return false;
			}
			State.PreRevision = CanonicalDocumentHash(
				CaptureDocument(Request.GetCompositionKind(), *State.Asset));
			if (State.PreRevision != Request.GetExpectedBaseRevision())
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_stale_target");
				OutError.SubjectId = Request.GetTargetAssetPath();
				return false;
			}
			FUeAgentHjsonImportTargetError TargetError;
			if (!ValidateTargetBinding(
					*Request.GetWorkspace(), Request, TargetError))
			{
				CopyError(TargetError, OutError);
				return false;
			}

			if (Request.GetCompositionKind()
				== TEXT("ue.animation_sequence.asset"))
			{
				const UAnimSequence* Sequence =
					CastChecked<UAnimSequence>(State.Asset);
				const FUeAgentCanonicalValue* RateScale = FindField(
					*Request.GetWorkspace()->GetModel(), TEXT("rate_scale"));
				const FUeAgentCanonicalValue* RootMotion = FindField(
					*Request.GetWorkspace()->GetModel(),
					TEXT("enable_root_motion"));
				if (!RateScale
					|| (RateScale->GetKind() != EUeAgentValueKind::Decimal
						&& RateScale->GetKind() != EUeAgentValueKind::Integer
						&& RateScale->GetKind() != EUeAgentValueKind::String))
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_rate_scale_invalid");
					OutError.SubjectId = Request.GetTargetAssetPath();
					return false;
				}
				const FString RateText =
					RateScale->GetKind() == EUeAgentValueKind::String
						? RateScale->GetText()
						: RateScale->GetLexeme();
				if (!LexTryParseString(State.DesiredRateScale, *RateText)
					|| !FMath::IsFinite(State.DesiredRateScale)
					|| State.DesiredRateScale <= 0.0)
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_rate_scale_invalid");
					OutError.SubjectId = Request.GetTargetAssetPath();
					return false;
				}
				State.bDesiredRootMotion =
					Request.GetIntent() == EChangeIntent::Replace
						? GetDefault<UAnimSequence>()->bEnableRootMotion
						: Sequence->bEnableRootMotion;
				if (RootMotion)
				{
					if (RootMotion->GetKind() == EUeAgentValueKind::Boolean)
					{
						State.bDesiredRootMotion = RootMotion->GetBoolean();
					}
					else if (RootMotion->GetKind() == EUeAgentValueKind::String
						&& RootMotion->GetText().Equals(
							TEXT("true"), ESearchCase::IgnoreCase))
					{
						State.bDesiredRootMotion = true;
					}
					else if (RootMotion->GetKind() == EUeAgentValueKind::String
						&& RootMotion->GetText().Equals(
							TEXT("false"), ESearchCase::IgnoreCase))
					{
						State.bDesiredRootMotion = false;
					}
					else
					{
						OutError = {};
						OutError.Code =
							TEXT("timeline_hjson_root_motion_invalid");
						OutError.SubjectId = Request.GetTargetAssetPath();
						return false;
					}
				}
				if (!TryCanonicalAnimationState(
						Sequence->RateScale,
						Sequence->bEnableRootMotion,
						Context.GetResourceTracker(),
						State.BeforeValue,
						OutError)
					|| !TryCanonicalAnimationState(
						State.DesiredRateScale,
						State.bDesiredRootMotion,
						Context.GetResourceTracker(),
						State.AfterValue,
						OutError))
				{
					return false;
				}
			}
			else
			{
				const ULevelSequence* Sequence =
					CastChecked<ULevelSequence>(State.Asset);
				const UMovieScene* MovieScene = Sequence->GetMovieScene();
				const FUeAgentCanonicalValue* Start = FindField(
					*Request.GetWorkspace()->GetModel(),
					TEXT("playback_start_tick"));
				const FUeAgentCanonicalValue* Duration = FindField(
					*Request.GetWorkspace()->GetModel(),
					TEXT("playback_duration_ticks"));
				if (!MovieScene || !Start || !Duration
					|| Start->GetKind() != EUeAgentValueKind::String
					|| Duration->GetKind() != EUeAgentValueKind::String
					|| !LexTryParseString(
						State.DesiredPlaybackStart, *Start->GetText())
					|| !LexTryParseString(
						State.DesiredPlaybackDuration, *Duration->GetText())
					|| State.DesiredPlaybackDuration < 0)
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_playback_range_invalid");
					OutError.SubjectId = Request.GetTargetAssetPath();
					return false;
				}
				const TRange<FFrameNumber> Current =
					MovieScene->GetPlaybackRange();
				const int32 CurrentStart =
					Current.GetLowerBoundValue().Value;
				const int32 CurrentDuration =
					Current.GetUpperBoundValue().Value - CurrentStart;
				if (!TryCanonicalLevelState(
						CurrentStart,
						CurrentDuration,
						Context.GetResourceTracker(),
						State.BeforeValue,
						OutError)
					|| !TryCanonicalLevelState(
						State.DesiredPlaybackStart,
						State.DesiredPlaybackDuration,
						Context.GetResourceTracker(),
						State.AfterValue,
						OutError))
				{
					return false;
				}
			}

			State.Identity.Emplace(
				TEXT("uai.semantic.timeline.hjson.document"),
				Request.GetCompositionKind(),
				Request.GetTargetAssetPath(),
				Request.GetTargetAssetPath(),
				TEXT("authoring_document"),
				TEXT("hjson-import-v1"));
			return State.Identity->IsValid();
		}

		bool TryBuildBeforeSnapshot(
			FUeAgentTimelineAnimationHjsonOperation::FState& State,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentOperationBindingError& OutError)
		{
			const uint64 Upper = FCrc::StrCrc32(*State.PreRevision);
			const uint64 Lower = FCrc::StrCrc32(
				*(State.Request.GetTargetAssetPath() + State.PreRevision));
			const FUeAgentStateRevision Revision(
				State.Request.GetTargetAssetPath(),
				State.PreRevision,
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				State.Identity.GetValue(),
				FUeAgentCanonicalType(
					TEXT("uai.type.timeline.hjson.document.v1"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				State.BeforeValue.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("timeline_animation.native_readback"),
					TEXT("hjson-import-before"),
					State.BeforeValue.ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("authoring_document"),
					TEXT("independent native preflight state"),
					{TEXT("TimelineAnimation"), TEXT("HjsonImport")},
					false,
					TEXT("available"),
					{}),
				Revision);
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("timeline-hjson-before:") + Context.GetRequestId(),
					Context.GetExecutionContext().GetStatePlane(),
					Revision,
					FUeAgentTemporalPoint(
						TEXT("editor-native"),
						FString::Printf(
							TEXT("%llu"), FPlatformTime::Cycles64())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("timeline_animation.native_readback"),
						TEXT("hjson-import-preflight:")
							+ State.PreRevision),
					{Entry},
					Context.GetResourceTracker(),
					OutSnapshot,
					Error))
			{
				OutError = {};
				OutError.Code = TEXT("timeline_hjson_before_snapshot_invalid");
				OutError.SubjectId = Error.Path;
				OutError.Detail = Error.Code;
				return false;
			}
			return true;
		}

		bool TryCapturePersistedValues(
			FUeAgentTimelineAnimationHjsonOperation::FState& State,
			UObject& Asset,
			const bool bCompareDesired,
			FUeAgentOperationBindingError& OutError)
		{
			State.PersistedValues.Reset();
			if (State.Request.GetCompositionKind()
				== TEXT("ue.animation_sequence.asset"))
			{
				const UAnimSequence* Sequence =
					CastChecked<UAnimSequence>(&Asset);
				State.PersistedValues.Add(
					TEXT("rate_scale"),
					FString::SanitizeFloat(Sequence->RateScale));
				State.PersistedValues.Add(
					TEXT("enable_root_motion"),
					Sequence->bEnableRootMotion ? TEXT("true") : TEXT("false"));
				if (bCompareDesired
					&& (!FMath::IsNearlyEqual(
						static_cast<double>(Sequence->RateScale),
						State.DesiredRateScale)
					|| Sequence->bEnableRootMotion
						!= State.bDesiredRootMotion))
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_persisted_mismatch");
					OutError.SubjectId = State.Request.GetTargetAssetPath();
					return false;
				}
			}
			else
			{
				const UMovieScene* MovieScene =
					CastChecked<ULevelSequence>(&Asset)->GetMovieScene();
				if (!MovieScene)
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_movie_scene_missing");
					OutError.SubjectId = State.Request.GetTargetAssetPath();
					return false;
				}
				const TRange<FFrameNumber> Range =
					MovieScene->GetPlaybackRange();
				const int32 Start = Range.GetLowerBoundValue().Value;
				const int32 Duration =
					Range.GetUpperBoundValue().Value - Start;
				State.PersistedValues.Add(
					TEXT("playback_start_tick"), FString::FromInt(Start));
				State.PersistedValues.Add(
					TEXT("playback_duration_ticks"),
					FString::FromInt(Duration));
				if (bCompareDesired
					&& (Start != State.DesiredPlaybackStart
						|| Duration != State.DesiredPlaybackDuration))
				{
					OutError = {};
					OutError.Code = TEXT("timeline_hjson_persisted_mismatch");
					OutError.SubjectId = State.Request.GetTargetAssetPath();
					return false;
				}
			}
			return true;
		}
	}

	FUeAgentTimelineAnimationHjsonOperation::
		FUeAgentTimelineAnimationHjsonOperation(
			TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	bool FUeAgentTimelineAnimationHjsonOperation::TryCreate(
		FUeAgentHjsonTargetImportRequest Request,
		TSharedPtr<FUeAgentTimelineAnimationHjsonOperation>& OutOperation,
		FUeAgentHjsonImportTargetError& OutError)
	{
		OutOperation.Reset();
		OutError = {};
		if (!Request.IsValid()
			|| !GetHjsonImportDescriptorIdentity(
				Request.GetCompositionKind()).IsValid())
		{
			SetError(
				OutError,
				TEXT("timeline_hjson_operation_request_invalid"),
				Request.GetCompositionKind());
			return false;
		}
		OutOperation = MakeShareable(
			new FUeAgentTimelineAnimationHjsonOperation(
				MakeShared<FState, ESPMode::ThreadSafe>(
					MoveTemp(Request))));
		return true;
	}

	bool FUeAgentTimelineAnimationHjsonOperation::TryPrepare(
		const FUeAgentContractValue& CanonicalInput,
		const TArray<FUeAgentOperationTargetBinding>& RequestedTargets,
		const FUeAgentOperationContext& Context,
		TOptional<FPlanningRequest>& OutPlanning,
		FUeAgentOperationBindingError& OutError)
	{
		OutPlanning.Reset();
		OutError = {};
		if (!BindStageContext(State.Get(), Context, 0x01, OutError)
			|| !Checkpoint(Context, TEXT("hjson_import.prepare"), OutError)
			|| !RequestedTargets.IsEmpty()
			|| Context.GetDescriptorIdentity()
				!= GetHjsonImportDescriptorIdentity(
					State->Request.GetCompositionKind())
			|| !TryValidateCanonicalInput(
				CanonicalInput, State->Request, OutError)
			|| !TryPrepareNativeState(State.Get(), Context, OutError))
		{
			if (!OutError.IsValid())
			{
				OutError.Code = TEXT("timeline_hjson_preparation_invalid");
				OutError.SubjectId = State->Request.GetTargetAssetPath();
				OutError.Detail = TEXT(
					"HJSON import preparation requires no preselected target "
					"and one exact descriptor.");
			}
			return false;
		}

		FUeAgentCanonicalSnapshotPtr Before;
		if (!TryBuildBeforeSnapshot(State.Get(), Context, Before, OutError)
			|| !Before.IsValid())
		{
			return false;
		}
		FChangeDraft Change(
			State->Identity.GetValue(),
			State->Request.GetIntent(),
			State->BeforeValue,
			State->AfterValue,
			{TEXT("uai:authoring:hjson:import")},
			TEXT("hjson-import-impact:")
				+ State->Request.GetWorkspace()->GetWorkspaceFingerprint(),
			{});
		FChangeSetDraft Draft(
			Context.GetDescriptorIdentity(),
			Context.GetRequestId(),
			Context.GetIdempotencyKey(),
			Before.ToSharedRef(),
			{MoveTemp(Change)});
		OutPlanning.Emplace(
			MoveTemp(Draft),
			Context.GetCallerContext(),
			State->Request.IsDryRun());
		if (!OutPlanning->IsValid())
		{
			OutPlanning.Reset();
			OutError.Code = TEXT("timeline_hjson_planning_request_invalid");
			OutError.SubjectId = Context.GetRequestId();
			return false;
		}
		return true;
	}

	bool FUeAgentTimelineAnimationHjsonOperation::TryPlan(
		const FPlanningRequest& Request,
		const FUeAgentOperationContext& Context,
		FPlanningResult& OutResult,
		FUeAgentOperationBindingError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!BindStageContext(State.Get(), Context, 0x02, OutError)
			|| !Checkpoint(Context, TEXT("hjson_import.plan"), OutError))
		{
			return false;
		}
		FPlanningPreflightError Error;
		if (!TryPlanChanges(
				Request,
				Context.GetResourceTracker(),
				OutResult,
				Error))
		{
			OutError.Code = Error.GetCode();
			OutError.SubjectId = Error.GetIdentityKey();
			OutError.Detail = Error.GetDetail();
			return false;
		}
		return true;
	}

	bool FUeAgentTimelineAnimationHjsonOperation::TryExecute(
		const FPlanningResult& Planning,
		const TArray<FUeAgentOperationTargetBinding>& Targets,
		const FUeAgentOperationContext& Context,
		FExecutionReceipt& OutReceipt,
		FUeAgentOperationBindingError& OutError)
	{
		OutReceipt = {};
		OutError = {};
		if (!BindStageContext(State.Get(), Context, 0x04, OutError)
			|| !Checkpoint(Context, TEXT("hjson_import.execute"), OutError)
			|| !Planning.IsReady()
			|| !Planning.GetPlan().IsValid()
			|| Targets.Num() != 1
			|| Targets[0].GetBinding().GetPublicRef()
				!= State->Request.GetTargetAssetPath()
			|| !State->Asset)
		{
			if (!OutError.IsValid())
			{
				OutError.Code = TEXT("timeline_hjson_execution_contract_invalid");
				OutError.SubjectId = Context.GetRequestId();
			}
			return false;
		}

		const bool bMutate = Planning.IsMutationPermitted();
		if (bMutate)
		{
			if (State->Request.GetCompositionKind()
				== TEXT("ue.animation_sequence.asset"))
			{
				UAnimSequence* Sequence =
					CastChecked<UAnimSequence>(State->Asset);
				Sequence->Modify();
				Sequence->RateScale =
					static_cast<float>(State->DesiredRateScale);
				Sequence->bEnableRootMotion = State->bDesiredRootMotion;
				Sequence->PostEditChange();
				Sequence->WaitOnExistingCompression();
				Sequence->MarkPackageDirty();
			}
			else
			{
				ULevelSequence* Sequence =
					CastChecked<ULevelSequence>(State->Asset);
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene)
				{
					OutError.Code = TEXT("timeline_hjson_movie_scene_missing");
					OutError.SubjectId = State->Request.GetTargetAssetPath();
					return false;
				}
				Sequence->Modify();
				MovieScene->Modify();
				MovieScene->SetPlaybackRange(
					State->DesiredPlaybackStart,
					State->DesiredPlaybackDuration);
				Sequence->PostEditChange();
				UMovieSceneCompiledDataManager::GetPrecompiledData()->Compile(
					Sequence);
				Sequence->MarkPackageDirty();
			}
			State->bMutationApplied = true;
			State->bCompileSettled = true;
			FUeAgentHjsonImportTargetError PersistError;
			if (!SavePackage(
					*State->Asset,
					State->Request.GetTargetAssetPath(),
					PersistError))
			{
				CopyError(PersistError, OutError);
				return false;
			}
			State->bPackageSaved = true;
		}

		const FString OperationId =
			TEXT("hjson-import:") + Context.GetRequestId();
		OutReceipt = FExecutionReceipt(
			OperationId,
			EExecutionPhase::Terminal,
			bMutate
				? EExecutionStatus::PersistedUnverified
				: EExecutionStatus::AppliedInMemory,
			FString(),
			EExecutionCancellationMode::NotRequested,
			bMutate,
			{},
			!bMutate,
			{},
			bMutate
				? TArray<FString>({
					TEXT("executing"),
					TEXT("settling"),
					TEXT("persisting"),
					TEXT("terminal"),
				})
				: TArray<FString>({TEXT("executing"), TEXT("terminal")}));
		return OutReceipt.IsValid();
	}

	bool FUeAgentTimelineAnimationHjsonOperation::TryVerify(
		const FExecutionReceipt& Receipt,
		const TArray<FUeAgentOperationTargetBinding>& Targets,
		const FUeAgentOperationContext& Context,
		FVerificationOutcome& OutOutcome,
		FUeAgentOperationBindingError& OutError)
	{
		OutOutcome = {};
		OutError = {};
		if (!BindStageContext(State.Get(), Context, 0x08, OutError)
			|| !Checkpoint(Context, TEXT("hjson_import.verify"), OutError)
			|| !Receipt.IsValid()
			|| Targets.Num() != 1
			|| !State->Identity.IsSet()
			|| !State->Asset)
		{
			if (!OutError.IsValid())
			{
				OutError.Code = TEXT("timeline_hjson_verification_contract_invalid");
				OutError.SubjectId = Context.GetRequestId();
			}
			return false;
		}

		UObject* ObservedAsset = State->Asset;
		if (State->bMutationApplied)
		{
			UClass* ExpectedClass =
				State->Request.GetCompositionKind()
					== TEXT("ue.animation_sequence.asset")
					? UAnimSequence::StaticClass()
					: ULevelSequence::StaticClass();
			FUeAgentHjsonImportTargetError ReloadError;
			if (!UnloadAndReload(
					State->Asset,
					State->Request.GetTargetAssetPath(),
					ExpectedClass,
					ObservedAsset,
					ReloadError))
			{
				CopyError(ReloadError, OutError);
				return false;
			}
			State->Asset = ObservedAsset;
			State->bUnloadedReloaded = true;
		}

		if (!TryCapturePersistedValues(
				State.Get(),
				*ObservedAsset,
				!State->Request.IsDryRun(),
				OutError))
		{
			return false;
		}
		State->PersistedRevision = CanonicalDocumentHash(
			CaptureDocument(
				State->Request.GetCompositionKind(), *ObservedAsset));
		if (State->Request.IsDryRun()
			&& State->PersistedRevision != State->PreRevision)
		{
			OutError.Code = TEXT("timeline_hjson_dry_run_mutated_target");
			OutError.SubjectId = State->Request.GetTargetAssetPath();
			return false;
		}

		State->ImportReceipt = FUeAgentHjsonTargetImportReceipt(
			State->Request.GetCompositionKind(),
			State->Request.GetTargetAssetPath(),
			State->Request.GetWorkspace()->GetWorkspaceFingerprint(),
			State->PreRevision,
			State->PersistedRevision,
			State->bMutationApplied,
			State->bCompileSettled,
			State->bPackageSaved,
			State->bUnloadedReloaded,
			State->PersistedValues);
		if (!State->ImportReceipt.IsValid())
		{
			OutError.Code = TEXT("timeline_hjson_import_receipt_invalid");
			OutError.SubjectId = State->Request.GetTargetAssetPath();
			return false;
		}

		const FString EvidenceId =
			TEXT("hjson-import-persisted:") + State->PersistedRevision;
		OutOutcome = FVerificationOutcome(
			TEXT("hjson-import-verify:") + Context.GetRequestId(),
			EVerificationStatus::Verified,
			EEffectOutcome::Complete,
			State->Request.IsDryRun()
				? TEXT("dry_run_verified")
				: FString(),
			false,
			{
				FTargetVerification(
					State->Identity.GetValue(),
					EVerificationOracleKind::Persisted,
					EVerificationStatus::Verified,
					TEXT("hjson-import-expected:")
						+ State->Request.GetWorkspace()
							->GetWorkspaceFingerprint(),
					TEXT("hjson-import-actual:")
						+ State->PersistedRevision,
					FUeAgentSemanticComparison(),
					true,
					{EvidenceId}),
			},
			{EvidenceId, TEXT("hjson-import-context-shared")},
			{});
		return true;
	}

	const FUeAgentHjsonTargetImportReceipt&
	FUeAgentTimelineAnimationHjsonOperation::GetImportReceipt() const
	{
		return State->ImportReceipt;
	}

	bool FUeAgentTimelineAnimationHjsonOperation::
		UsedOneOperationContext() const
	{
		return State->OperationContext != nullptr
			&& State->CompletedStages == 0x0f;
	}

	bool TryExportTimelineAnimationHjsonTarget(
		const FUeAgentHjsonTargetExportRequest& Request,
		FUeAgentHjsonTargetExportReceipt& OutReceipt,
		FUeAgentHjsonImportTargetError& OutError)
	{
		OutReceipt = {};
		OutError = {};
		UObject* Asset = LoadExactTarget(
			Request.GetCompositionKind(), Request.GetTargetAssetPath());
		if (!Asset)
		{
			SetError(OutError, TEXT("timeline_hjson_target_not_found"), Request.GetTargetAssetPath());
			return false;
		}
		const TSharedRef<FJsonObject> Document =
			CaptureDocument(Request.GetCompositionKind(), *Asset);
		const FString Revision = CanonicalDocumentHash(Document);
		if (Revision.IsEmpty())
		{
			SetError(OutError, TEXT("timeline_hjson_capture_hash_failed"), Request.GetTargetAssetPath());
			return false;
		}
		return TryPublish(Request, Document, Revision, OutReceipt, OutError);
	}

}
