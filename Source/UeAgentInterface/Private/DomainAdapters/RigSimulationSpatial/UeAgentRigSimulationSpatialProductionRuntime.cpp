#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Algo/Reverse.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "AssetToolsModule.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDescriptors.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	namespace
	{
		using UE::UAI::Contract::FDiagnostic;

		bool NativeFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = Expected.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool InputFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString())
		{
			NativeFailure(OutError, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			OutError.SourceKind = TEXT("command_input");
			return false;
		}

		bool MissingNative(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& Target)
		{
			return NativeFailure(OutError, Code, Reason, Path,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
		}

		void NativeContext(FDiagnostic& Error, const FString& Path = FString(), bool bCanonicalValue = false)
		{
			Error.SourceKind = TEXT("native_result");
			if (!Path.IsEmpty() || bCanonicalValue)
			{
				auto Locate = [&Path, bCanonicalValue](FString Pointer)
				{
					// Canonical constructors locate their value parameter at /value.
					if (bCanonicalValue && (Pointer == TEXT("/value") || Pointer.StartsWith(TEXT("/value/"))))
						Pointer.RightChopInline(6);
					if (Pointer.IsEmpty() || Pointer == TEXT("/")) return Path;
					if (!Pointer.StartsWith(TEXT("/")))
						Pointer = TEXT("/") + Pointer.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					return Path + Pointer;
				};
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.Source.JsonPath;
				Error.Source.JsonPath = Locate(Error.Source.JsonPath);
				Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Error.Source.JsonPath : Locate(Error.PropertyPath);
				if (!Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Locate(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) NativeContext(Child, Path, bCanonicalValue);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				NativeContext(*Cause, Path, bCanonicalValue);
				Error.PublicCause = MoveTemp(Cause);
			}
		}


		bool NativeCause(FDiagnostic& OutError, const FString& Code, FDiagnostic Cause, const FString& Path)
		{
			NativeContext(Cause, Path);
			OutError = FDiagnostic::WrapFailure(Code, Cause, TEXT("native_execution"));
			return false;
		}

		bool MissingFamily(FDiagnostic& OutError, const FString& Capability, const FString& Port)
		{
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				TEXT("No native implementation is available for this capability's ") + Port + TEXT(" port."),
				TEXT("/capability_id"), MakeShared<FJsonValueString>(Capability),
				MakeShared<FJsonValueString>(TEXT("a capability with a configured ") + Port + TEXT(" port")));
		}

		bool MissingRoute(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& From, const FString& To)
		{
			NativeFailure(OutError, Code, Reason, Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(From);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(To);
			return false;
		}

		bool ConfigFailure(FDiagnostic& OutError, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, const TCHAR* Expected, const FString& Target = FString())
		{
			OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_runtime_config_invalid"), Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		void PrefixConfigDiagnostic(FDiagnostic& Error, const FString& Prefix)
		{
			Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Prefix : Prefix + TEXT(".") + Error.PropertyPath;
		}

		class FProductionRetargetAssetEffectPort final
			: public IUeAgentRigSimulationSpatialAssetEffectPort
		{
		public:
			bool TryApplyRetargetAssets(
				const FUeAgentRigSimulationSpatialRetargetAssetEffectRequest& Request,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialRetargetAssetEffectResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				UAnimSequence* Source = Cast<UAnimSequence>(Request.SourceSubject.Get());
				USkeletalMesh* Target = Cast<USkeletalMesh>(Request.TargetSubject.Get());
				UIKRetargeter* Retargeter = Request.Retargeter.Get();
				UIKRetargeterController* Controller = Retargeter
					? UIKRetargeterController::GetController(Retargeter) : nullptr;
				if (!Request.TryValidate(OutError)) return false;
				if (!Source) return NativeFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_request_invalid"),
					TEXT("The asset effect source is not an animation sequence."), TEXT("/source_subject/type"),
					MakeShared<FJsonValueString>(Request.SourceSubject->GetClass()->GetName()),
					MakeShared<FJsonValueString>(TEXT("AnimSequence")), Request.DestinationRoot);
				if (!Target) return NativeFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_request_invalid"),
					TEXT("The asset effect target is not a skeletal mesh."), TEXT("/target_subject/type"),
					MakeShared<FJsonValueString>(Request.TargetSubject->GetClass()->GetName()),
					MakeShared<FJsonValueString>(TEXT("SkeletalMesh")), Request.DestinationRoot);
				if (!Controller) return MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_request_invalid"),
					TEXT("The retargeter controller is unavailable for asset generation."), TEXT("/retargeter/controller"), Request.DestinationRoot);
				if (Request.AssetRequests.Num() != 1)
					return InputFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_request_invalid"),
						TEXT("This asset effect port requires exactly one asset request."), TEXT("/asset_requests"),
						MakeShared<FJsonValueNumber>(Request.AssetRequests.Num()), MakeShared<FJsonValueNumber>(1), Request.DestinationRoot);
				const FRigSimulationSpatialAssetRequest& Item = Request.AssetRequests[0];
				if (Item.AssetKind != TEXT("animation_sequence"))
					return InputFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_policy_invalid"),
						TEXT("This asset effect port does not support the requested asset kind."), TEXT("/asset_requests/0/asset_kind"),
						MakeShared<FJsonValueString>(Item.AssetKind), MakeShared<FJsonValueString>(TEXT("animation_sequence")), Request.DestinationRoot);
				USkeletalMesh* SourceMesh = Controller->GetPreviewMesh(
					ERetargetSourceOrTarget::Source);
				if (!SourceMesh) return MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_mesh_invalid"),
					TEXT("The retargeter has no source preview skeletal mesh."), TEXT("/retargeter/source_preview_mesh"), Request.DestinationRoot);
				if (SourceMesh == Target)
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_mesh_invalid"),
						TEXT("The retarget source preview mesh and target subject are the same mesh."), TEXT("/subjects/distinct_meshes"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.DestinationRoot);

				const FString TemporaryName = Item.Name + TEXT("__UAI_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
				FIKRetargetBatchOperationContext Batch;
				Batch.AssetsToRetarget.Add(Source);
				Batch.SourceMesh = SourceMesh;
				Batch.TargetMesh = Target;
				Batch.IKRetargetAsset = Retargeter;
				Batch.NameRule.FolderPath = Request.DestinationRoot;
				Batch.NameRule.ReplaceFrom = Source->GetName();
				Batch.NameRule.ReplaceTo = TemporaryName;
				Batch.bOverwriteExistingFiles = false;
				Batch.bIncludeReferencedAssets = false;
				UIKRetargetBatchOperation* Operation =
					NewObject<UIKRetargetBatchOperation>(GetTransientPackage());
				if (!Operation)
				{
					return MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_operation_unavailable"),
						TEXT("Creating the native retarget batch operation returned no object."), TEXT("/retarget_operation"), Request.DestinationRoot);
				}
				Operation->RunRetarget(Batch);
				const FString TemporaryPath = Request.DestinationRoot + TEXT("/")
					+ TemporaryName + TEXT(".") + TemporaryName;
				UAnimSequence* Generated = FindObject<UAnimSequence>(nullptr, *TemporaryPath);
				if (!Generated)
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_generated_asset_missing"),
						TEXT("The retarget batch operation did not produce the requested animation."), TEXT("/asset_requests/0/generated_asset"), Request.DestinationRoot + TEXT("/") + Item.Name);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(TemporaryPath);
					return false;
				}
				const FString FinalPath = Request.DestinationRoot + TEXT("/")
					+ Item.Name + TEXT(".") + Item.Name;
				UAnimSequence* Final = FindObject<UAnimSequence>(nullptr, *FinalPath);
				if (Final && Item.OverwritePolicy == TEXT("fail"))
				{
					ObjectTools::DeleteSingleObject(Generated, true);
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_target_exists"),
						TEXT("The destination animation already exists while overwrite_policy is fail."), TEXT("/asset_requests/0/destination/present"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), FinalPath);
				}
				if (Final)
				{
					const IAnimationDataModel* Model = Generated->GetDataModel();
					if (!Model)
					{
						ObjectTools::DeleteSingleObject(Generated, true);
						MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_model_unavailable"),
							TEXT("The generated animation has no data model to copy into the destination."), TEXT("/asset_requests/0/generated_asset/data_model"), FinalPath);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(TemporaryPath);
						return false;
					}
					Final->Modify();
					Final->SetSkeleton(Generated->GetSkeleton());
					IAnimationDataController& FinalController = Final->GetController();
					FinalController.RemoveAllBoneTracks(false);
					FinalController.SetFrameRate(Model->GetFrameRate(), false);
					FinalController.SetNumberOfFrames(
						FFrameNumber(Model->GetNumberOfFrames()), false);
					TArray<FName> BoneTracks;
					Model->GetBoneTrackNames(BoneTracks);
					for (const FName BoneTrack : BoneTracks)
					{
						TArray<FTransform> Transforms;
						Model->GetBoneTrackTransforms(BoneTrack, Transforms);
						TArray<FVector3f> Positions;
						TArray<FQuat4f> Rotations;
						TArray<FVector3f> Scales;
						for (const FTransform& Transform : Transforms)
						{
							Positions.Add(FVector3f(Transform.GetTranslation()));
							Rotations.Add(FQuat4f(Transform.GetRotation()));
							Scales.Add(FVector3f(Transform.GetScale3D()));
						}
						FinalController.AddBoneCurve(BoneTrack, false);
						FinalController.SetBoneTrackKeys(
							BoneTrack, Positions, Rotations, Scales, false);
					}
					FinalController.NotifyPopulated();
					Final->RefreshCacheData();
					Final->MarkPackageDirty();
					ObjectTools::DeleteSingleObject(Generated, true);
				}
				else
				{
					FAssetRenameData Rename(Generated, Request.DestinationRoot, Item.Name);
					TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
					if (!FModuleManager::LoadModuleChecked<FAssetToolsModule>(
							TEXT("AssetTools")).Get().RenameAssets({Rename}))
					{
						ObjectTools::DeleteSingleObject(Generated, true);
						return NativeFailure(OutError, TEXT("rig_simulation_spatial_asset_effect_rename_failed"),
							TEXT("AssetTools rejected renaming the generated animation to the requested destination."), TEXT("/asset_requests/0/rename"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FinalPath);
					}
					Final = FindObject<UAnimSequence>(nullptr, *FinalPath);
				}
				if (!Final)
				{
					return MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_final_asset_missing"),
						TEXT("The final animation is unavailable after the asset effect completed."), TEXT("/asset_requests/0/final_asset"), FinalPath);
				}
				OutResult.GeneratedAssets.Add(Final);
				OutResult.GeneratedAssetRefs.Add(FinalPath);
				return OutResult.TryValidate(OutError);
			}
		};

		class FProductionEnvironment final
			: public IUeAgentRigSimulationSpatialNativeEnvironment
		{
		public:
			explicit FProductionEnvironment(
				const FUeAgentRigSimulationSpatialProductionRuntimeConfig& Config,
				TSharedRef<FThreadSafeCounter> InCalls)
				: RuntimeGeneration(Config.RuntimeGeneration)
				, InputRootPointer(Config.InputRootPointer)
				, ObjectBindings(Config.ObjectBindings)
				, ClockBindings(Config.ClockBindings)
				, FrameBindings(Config.FrameBindings)
				, SkeletonBindings(Config.SkeletonBindings)
				, SocketBindings(Config.SocketBindings)
				, RigBakeBindings(Config.RigBakeBindings)
				, IkRigBindings(Config.IkRigBindings)
				, RetargeterBindings(Config.RetargeterBindings)
				, PhysicsAssetBindings(Config.PhysicsAssetBindings)
				, PhysicsSubjectBindings(Config.PhysicsSubjectBindings)
				, DeformerGeometryBindings(Config.DeformerGeometryBindings)
				, DeformerTrainingInputBindings(Config.DeformerTrainingInputBindings)
				, DeformerEvaluationBindings(Config.DeformerEvaluationBindings)
				, SpatialConversions(Config.SpatialConversions)
				, TemporalConversions(Config.TemporalConversions)
				, Calls(MoveTemp(InCalls))
			{
			}

			const FString& GetInputRootPointer() const override { return InputRootPointer; }

			bool HasExplicitObject(const FString& Identity) const override
			{
				return ObjectBindings.ContainsByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialObjectBinding& Binding)
					{
						return Binding.Identity == Identity && Binding.IsValid();
					}) || ResolveDirectObjectPath(Identity) != nullptr;
			}

			bool HasTemporalPath(const FString& ClockId) const override
			{
				if (ClockBindings.ContainsByPredicate(
					[&ClockId](const FUeAgentRigSimulationSpatialClockBinding& Binding)
					{
						return Binding.ClockId == ClockId
							|| Binding.NativeClockId == ClockId;
					})) return true;
				TArray<FString> Pending = {ClockId};
				TSet<FString> Visited;
				for (int32 Index = 0; Index < Pending.Num(); ++Index)
				{
					const FString Current = Pending[Index];
					if (Visited.Contains(Current)) continue;
					Visited.Add(Current);
					for (const FUeAgentTemporalConversion& Conversion : TemporalConversions)
					{
						if (Conversion.GetSourceClockId() != Current) continue;
						const FString& Next = Conversion.GetTargetClockId();
						if (ClockBindings.ContainsByPredicate(
							[&Next](const FUeAgentRigSimulationSpatialClockBinding& Binding)
							{
								return Binding.ClockId == Next
									|| Binding.NativeClockId == Next;
							})) return true;
						Pending.AddUnique(Next);
					}
				}
				return false;
			}

			bool HasSpatialPath(const FString& FrameId) const override
			{
				TSet<FString> NativeFrames;
				for (const FUeAgentRigSimulationSpatialFrameBinding& Binding : FrameBindings)
					if (Binding.bNativeFrame) NativeFrames.Add(Binding.Frame.GetFrameId());
				if (NativeFrames.Contains(FrameId)) return true;
				TArray<FString> Pending = {FrameId};
				TSet<FString> Visited;
				for (int32 Index = 0; Index < Pending.Num(); ++Index)
				{
					const FString Current = Pending[Index];
					if (Visited.Contains(Current)) continue;
					Visited.Add(Current);
					for (const FUeAgentSpatialConversion& Conversion : SpatialConversions)
					{
						if (Conversion.GetFromFrame() != Current) continue;
						if (NativeFrames.Contains(Conversion.GetToFrame())) return true;
						Pending.AddUnique(Conversion.GetToFrame());
					}
				}
				return false;
			}

			bool TryResolveFrame(
				const FRigSimulationSpatialFrame3d& WireFrame,
				FUeAgentSpatialFrame& OutFrame,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutError.Reset();
				const FUeAgentRigSimulationSpatialFrameBinding* Found =
					FrameBindings.FindByPredicate(
						[&WireFrame](const FUeAgentRigSimulationSpatialFrameBinding& Binding)
						{
							return Binding.Frame.GetFrameId() == WireFrame.FrameId;
						});
				const auto Mismatch = [&](const TCHAR* Reason, const TCHAR* Path,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, bool bInternal = false)
				{
					OutError = FDiagnostic::Failure(TEXT("rig_simulation_spatial_frame_fact_mismatch"), Reason, TEXT("native_execution"));
					OutError.TargetPath = WireFrame.FrameId;
					OutError.PropertyPath = Path;
					if (bInternal)
					{
						OutError.ActualEvidence = MoveTemp(Actual);
						OutError.ExpectedEvidence = MoveTemp(Expected);
					}
					else
					{
						OutError.ActualValuePresent = Actual.IsValid();
						OutError.ActualValue = MoveTemp(Actual);
						OutError.ExpectedValue = MoveTemp(Expected);
					}
					return false;
				};
				if (!Found)
				{
					Mismatch(TEXT("No registered frame matches the requested frame identifier."), TEXT("frame_id"),
						MakeShared<FJsonValueString>(WireFrame.FrameId), MakeShared<FJsonValueString>(TEXT("registered frame identifier")));
					OutError.Code = TEXT("rig_simulation_spatial_frame_fact_missing");
					return false;
				}
				if (!Found->TryValidate(OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = WireFrame.FrameId;
					// The invalid fact belongs to the registered native frame, not the input frame object.
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				if (Found->Frame.GetBasisFrameId() != WireFrame.BasisFrameId)
					return Mismatch(TEXT("The requested basis frame differs from the registered frame's basis."), TEXT("basis_frame_id"),
						MakeShared<FJsonValueString>(WireFrame.BasisFrameId), MakeShared<FJsonValueString>(Found->Frame.GetBasisFrameId()));
				if (Found->Frame.GetDimension() != WireFrame.Dimension)
					return Mismatch(TEXT("The requested dimension differs from the registered frame's dimension."), TEXT("dimension"),
						MakeShared<FJsonValueNumber>(WireFrame.Dimension), MakeShared<FJsonValueNumber>(Found->Frame.GetDimension()));
				if (Found->Frame.GetUnit() != WireFrame.Unit)
					return Mismatch(TEXT("The requested unit differs from the registered frame's unit."), TEXT("unit"),
						MakeShared<FJsonValueString>(WireFrame.Unit), MakeShared<FJsonValueString>(Found->Frame.GetUnit()));
				if (Found->Frame.GetBasisEvidence() != WireFrame.BasisEvidenceId)
					return Mismatch(TEXT("The requested basis evidence does not match the registered frame."), TEXT("basis_evidence_id"),
						MakeShared<FJsonValueString>(WireFrame.BasisEvidenceId), MakeShared<FJsonValueString>(Found->Frame.GetBasisEvidence()), true);
				OutFrame = Found->Frame;
				return true;
			}

			bool TryReadRevision(
				const FString& ExplicitRef,
				FRigSimulationSpatialRevision& OutRevision,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutRevision = {};
				OutError = {};
				const FUeAgentRigSimulationSpatialObjectBinding* Found =
					ObjectBindings.FindByPredicate(
						[&ExplicitRef](const FUeAgentRigSimulationSpatialObjectBinding& Binding)
						{
							return Binding.Identity == ExplicitRef;
						});
				const UObject* Object = Found && Found->IsValid()
					? Found->Object.Get() : ResolveDirectObjectPath(ExplicitRef, &OutError);
				if (!Object)
				{
					return OutError.IsValid() ? false : NativeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_unavailable"),
						TEXT("The explicit reference does not resolve to an available object for revision readback."), TEXT("/explicit_ref"),
						MakeShared<FJsonValueString>(ExplicitRef), MakeShared<FJsonValueString>(TEXT("an available object reference")), ExplicitRef);
				}
				const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
				uint32 RevisionHash = GetTypeHash(Object->GetPathName());
				RevisionHash = HashCombine(RevisionHash, GetTypeHash(Object->GetClass()->GetPathName()));
				RevisionHash = HashCombine(RevisionHash,
					Package && Package->IsDirty() ? 1u : 0u);
				OutRevision.SourceId = ExplicitRef;
				OutRevision.Generation = RuntimeGeneration;
				OutRevision.Sequence = static_cast<int64>(RevisionHash);
				if (OutRevision.SourceId.IsEmpty() || OutRevision.Generation.IsEmpty())
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_revision_invalid"),
						OutRevision.SourceId.IsEmpty() ? TEXT("The observed revision has no source identifier.") : TEXT("The observed revision has no runtime generation."),
						OutRevision.SourceId.IsEmpty() ? TEXT("/revision/source_id") : TEXT("/revision/generation"),
						MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("a nonempty identifier")), ExplicitRef);
				if (OutRevision.Sequence < 0)
					return NativeFailure(OutError, TEXT("rig_simulation_spatial_revision_invalid"),
						TEXT("The observed revision sequence is negative."), TEXT("/revision/sequence"),
						MakeShared<FJsonValueString>(::LexToString(OutRevision.Sequence)), MakeShared<FJsonValueString>(TEXT(">= 0")), ExplicitRef);
				return true;
			}

			const TArray<FUeAgentSpatialConversion>&
			GetSpatialConversions() const override { return SpatialConversions; }
			const TArray<FUeAgentTemporalConversion>&
			GetTemporalConversions() const override { return TemporalConversions; }

			UObject* ResolveExplicitObject(const FString& Identity) const override
			{
				const FUeAgentRigSimulationSpatialObjectBinding* Found =
					ObjectBindings.FindByPredicate(
						[&Identity](const FUeAgentRigSimulationSpatialObjectBinding& Binding)
						{
							return Binding.Identity == Identity && Binding.IsValid();
						});
				return Found ? Found->Object.Get() : ResolveDirectObjectPath(Identity);
			}

			const FUeAgentRigSimulationSpatialSkeletonBinding*
			FindSkeletonBinding(const FString& Identity) const override
			{
				return SkeletonBindings.FindByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialSkeletonBinding& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialSocketBinding*
			FindSocketBinding(
				const FString& SkeletonIdentity,
				const FString& SocketIdentity) const override
			{
				return SocketBindings.FindByPredicate(
					[&](const FUeAgentRigSimulationSpatialSocketBinding& Binding)
					{
						return Binding.SkeletonIdentity == SkeletonIdentity
							&& Binding.SocketIdentity == SocketIdentity;
					});
			}

			const FUeAgentRigSimulationSpatialRigBakeBinding*
			FindRigBakeBinding(const FString& Identity) const override
			{
				return RigBakeBindings.FindByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialRigBakeBinding& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialIkRigBinding*
			FindIkRigBinding(const FString& Identity) const override
			{
				return IkRigBindings.FindByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialIkRigBinding& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialRetargeterBinding*
			FindRetargeterBinding(const FString& Identity) const override
			{
				return RetargeterBindings.FindByPredicate(
					[&Identity](
						const FUeAgentRigSimulationSpatialRetargeterBinding& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialPhysicsAssetBinding*
			FindPhysicsAssetBinding(const FString& Identity) const override
			{
				return PhysicsAssetBindings.FindByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialPhysicsSubjectBinding*
			FindPhysicsSubjectBinding(const FString& Identity) const override
			{
				if (const FUeAgentRigSimulationSpatialPhysicsSubjectBinding* Found =
					PhysicsSubjectBindings.FindByPredicate(
					[&Identity](const FUeAgentRigSimulationSpatialPhysicsSubjectBinding& Binding)
					{
						return Binding.Identity == Identity;
					}))
				{
					return Found;
				}
				FScopeLock Lock(&DynamicBindingMutex);
				if (const TSharedPtr<const
					FUeAgentRigSimulationSpatialPhysicsSubjectBinding>* Cached =
						DynamicPhysicsSubjectBindings.Find(Identity))
				{
					return Cached->Get();
				}
				UPrimitiveComponent* const Component = Cast<UPrimitiveComponent>(
					ResolveDirectObjectPath(Identity));
				if (!Component)
				{
					return nullptr;
				}
				FUeAgentRigSimulationSpatialPhysicsSubjectBinding Binding;
				Binding.Identity = Identity;
				Binding.Component = Component;
				Binding.NativeSimulationFrameId = TEXT("ue.physics.world");
				Binding.NativeSimulationClockId = TEXT("ue.physics.seconds");
				const TSharedPtr<const
					FUeAgentRigSimulationSpatialPhysicsSubjectBinding> Added =
						MakeShared<const
							FUeAgentRigSimulationSpatialPhysicsSubjectBinding>(
								MoveTemp(Binding));
				DynamicPhysicsSubjectBindings.Add(Identity, Added);
				return Added.Get();
			}

			const FUeAgentRigSimulationSpatialDeformerGeometryBinding*
			FindDeformerGeometryBinding(const FString& Identity) const override
			{
				return DeformerGeometryBindings.FindByPredicate(
					[&Identity](const auto& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialDeformerTrainingInputBinding*
			FindDeformerTrainingInputBinding(const FString& Identity) const override
			{
				return DeformerTrainingInputBindings.FindByPredicate(
					[&Identity](const auto& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			const FUeAgentRigSimulationSpatialDeformerEvaluationBinding*
			FindDeformerEvaluationBinding(const FString& Identity) const override
			{
				return DeformerEvaluationBindings.FindByPredicate(
					[&Identity](const auto& Binding)
					{
						return Binding.Identity == Identity;
					});
			}

			bool TryConvertSpatialValueToFrame(
				const FUeAgentSpatialValue& Value,
				const FString& TargetFrameId,
				FUeAgentResourceTracker& Resources,
				FUeAgentSpatialValuePtr& OutValue,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutValue.Reset();
				OutError = {};
				const FUeAgentRigSimulationSpatialFrameBinding* Target =
					FrameBindings.FindByPredicate(
						[&TargetFrameId](const FUeAgentRigSimulationSpatialFrameBinding& Binding)
						{
							return Binding.Frame.GetFrameId() == TargetFrameId;
						});
				if (!Value.IsValid())
					return NativeCause(OutError, TEXT("rig_simulation_spatial_spatial_value_invalid"),
						Value.GetValidationDiagnostic(), TEXT("/spatial_value"));
				if (!Target)
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_native_frame_unavailable"),
						TEXT("The target native frame has no registered frame binding."), TEXT("/target_frame_binding"), FString());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TargetFrameId);
					return false;
				}
				FDiagnostic FrameError;
				if (!Target->TryValidate(FrameError))
					return NativeCause(OutError, TEXT("rig_simulation_spatial_native_frame_unavailable"),
						MoveTemp(FrameError), TEXT("/target_frame_binding"));
				const FString SourceFrameId = Value.GetFrame().GetFrameId();
				if (SourceFrameId == TargetFrameId)
				{
					OutValue = MakeShared<FUeAgentSpatialValue>(Value);
					// Copying the already validated spatial value preserves its validation state.
					return true;
				}

				TArray<FString> Pending = {SourceFrameId};
				TSet<FString> Visited;
				TMap<FString, FString> ParentFrame;
				TMap<FString, int32> ParentConversion;
				bool bFound = false;
				for (int32 Index = 0; Index < Pending.Num() && !bFound; ++Index)
				{
					const FString Current = Pending[Index];
					if (Visited.Contains(Current)) continue;
					Visited.Add(Current);
					for (int32 ConversionIndex = 0;
						ConversionIndex < SpatialConversions.Num(); ++ConversionIndex)
					{
						const FUeAgentSpatialConversion& Conversion =
							SpatialConversions[ConversionIndex];
						if (Conversion.GetFromFrame() != Current) continue;
						const FString& Next = Conversion.GetToFrame();
						if (!ParentFrame.Contains(Next))
						{
							ParentFrame.Add(Next, Current);
							ParentConversion.Add(Next, ConversionIndex);
							Pending.Add(Next);
						}
						if (Next == TargetFrameId)
						{
							bFound = true;
							break;
						}
					}
				}
				if (!bFound) return MissingRoute(OutError, TEXT("rig_simulation_spatial_native_frame_conversion_missing"),
					TEXT("No registered spatial conversion route connects the source frame to the requested native frame."),
					TEXT("/spatial_conversion/path"), SourceFrameId, TargetFrameId);

				TArray<int32> Path;
				FString Cursor = TargetFrameId;
				while (Cursor != SourceFrameId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentFrame.Find(Cursor);
					if (!ConversionIndex || !Parent)
					{
						MissingNative(OutError, TEXT("rig_simulation_spatial_native_frame_path_invalid"),
							!ConversionIndex ? TEXT("The selected spatial route has no conversion index for a frame.") : TEXT("The selected spatial route has no parent frame."),
							!ConversionIndex ? TEXT("/spatial_conversion/path/conversion_index") : TEXT("/spatial_conversion/path/parent_frame"), FString());
						OutError.SubjectId = Cursor;
						return false;
					}
					Path.Add(*ConversionIndex);
					Cursor = *Parent;
				}
				Algo::Reverse(Path);
				FUeAgentSpatialValuePtr Current = MakeShared<FUeAgentSpatialValue>(Value);
				for (const int32 ConversionIndex : Path)
				{
					const FUeAgentSpatialConversion& Conversion =
						SpatialConversions[ConversionIndex];
					const FUeAgentRigSimulationSpatialFrameBinding* NextFrame =
						FrameBindings.FindByPredicate(
							[&Conversion](const FUeAgentRigSimulationSpatialFrameBinding& Binding)
							{
								return Binding.Frame.GetFrameId()
									== Conversion.GetToFrame();
							});
					FUeAgentStateSemanticError SemanticError;
					FUeAgentSpatialValuePtr Converted;
					const FString StepPath = TEXT("/spatial_conversions/") + FString::FromInt(ConversionIndex);
					if (!NextFrame)
					{
						MissingNative(OutError, TEXT("rig_simulation_spatial_native_frame_conversion_failed"),
							TEXT("The conversion step's target frame has no registered frame binding."), StepPath + TEXT("/target_frame_binding"), FString());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Conversion.GetToFrame());
						return false;
					}
					if (!TryConvertSpatialValue(*Current, NextFrame->Frame, Conversion, Resources, Converted, SemanticError))
						return NativeCause(OutError, TEXT("rig_simulation_spatial_native_frame_conversion_failed"), MoveTemp(SemanticError), StepPath);
					if (!Converted.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_frame_conversion_failed"),
						TEXT("The spatial conversion step returned no value."), StepPath + TEXT("/converted_value"), FString());
					Current = MoveTemp(Converted);
				}
				OutValue = MoveTemp(Current);
				if (!OutValue.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_frame_conversion_failed"),
					TEXT("The spatial conversion route returned no final value."), TEXT("/converted_value"), FString());
				if (!OutValue->IsValid()) return NativeCause(OutError, TEXT("rig_simulation_spatial_spatial_value_invalid"),
					OutValue->GetValidationDiagnostic(), TEXT("/converted_value"));
				return true;
			}

			bool TryConvertTemporalRangeToClock(
				const FUeAgentTemporalRange& Range,
				const FString& TargetClockId,
				FUeAgentResourceTracker& Resources,
				TOptional<FUeAgentTemporalRange>& OutRange,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutRange.Reset();
				OutError = {};
				if (!Range.IsValid()) return NativeCause(OutError, TEXT("rig_simulation_spatial_native_temporal_range_invalid"),
					Range.GetValidationDiagnostic(), TEXT("/temporal_range"));
				const FString SourceClockId = Range.GetClockId();
				if (SourceClockId == TargetClockId)
				{
					OutRange = Range;
					return true;
				}
				TArray<FString> Pending = {SourceClockId};
				TSet<FString> Visited;
				TMap<FString, FString> ParentClock;
				TMap<FString, int32> ParentConversion;
				bool bFound = false;
				for (int32 Index = 0; Index < Pending.Num() && !bFound; ++Index)
				{
					const FString Current = Pending[Index];
					if (Visited.Contains(Current)) continue;
					Visited.Add(Current);
					for (int32 ConversionIndex = 0;
						ConversionIndex < TemporalConversions.Num(); ++ConversionIndex)
					{
						const FUeAgentTemporalConversion& Conversion =
							TemporalConversions[ConversionIndex];
						if (Conversion.GetSourceClockId() != Current) continue;
						const FString& Next = Conversion.GetTargetClockId();
						if (!ParentClock.Contains(Next))
						{
							ParentClock.Add(Next, Current);
							ParentConversion.Add(Next, ConversionIndex);
							Pending.Add(Next);
						}
						if (Next == TargetClockId)
						{
							bFound = true;
							break;
						}
					}
				}
				if (!bFound) return MissingRoute(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_missing"),
					TEXT("No registered temporal conversion route connects the source clock to the requested native clock."),
					TEXT("/temporal_conversion/path"), SourceClockId, TargetClockId);
				TArray<int32> Path;
				FString Cursor = TargetClockId;
				while (Cursor != SourceClockId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentClock.Find(Cursor);
					if (!ConversionIndex || !Parent)
					{
						MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_path_invalid"),
							!ConversionIndex ? TEXT("The selected temporal route has no conversion index for a clock.") : TEXT("The selected temporal route has no parent clock."),
							!ConversionIndex ? TEXT("/temporal_conversion/path/conversion_index") : TEXT("/temporal_conversion/path/parent_clock"), FString());
						OutError.SubjectId = Cursor;
						return false;
					}
					Path.Add(*ConversionIndex);
					Cursor = *Parent;
				}
				Algo::Reverse(Path);
				TOptional<FUeAgentTemporalRange> Current = Range;
				for (const int32 ConversionIndex : Path)
				{
					FUeAgentStateSemanticError SemanticError;
					TOptional<FUeAgentTemporalRange> Converted;
					const FString StepPath = TEXT("/temporal_conversions/") + FString::FromInt(ConversionIndex);
					if (!TryConvertTemporalRange(*Current, TemporalConversions[ConversionIndex],
						EUeAgentTemporalRoundingPolicy::Exact, EUeAgentTemporalRoundingPolicy::Exact, Resources, Converted, SemanticError))
						return NativeCause(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"), MoveTemp(SemanticError), StepPath);
					if (!Converted.IsSet()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
						TEXT("The temporal conversion step returned no range."), StepPath + TEXT("/converted_range"), FString());
					Current = MoveTemp(Converted);
				}
				OutRange = MoveTemp(Current);
				if (!OutRange.IsSet()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
					TEXT("The temporal conversion route returned no final range."), TEXT("/converted_range"), FString());
				if (OutRange->GetClockId() != TargetClockId)
					return MissingRoute(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
						TEXT("The converted range uses a different clock than the requested native clock."),
						TEXT("/converted_range/clock_matches"), OutRange->GetClockId(), TargetClockId);
				return true;
			}

			bool TryConvertTemporalPointToClock(
				const FUeAgentTemporalPoint& Point,
				const FString& TargetClockId,
				FUeAgentResourceTracker& Resources,
				TOptional<FUeAgentTemporalPoint>& OutPoint,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutPoint.Reset();
				OutError = {};
				if (!Point.IsValid()) return NativeCause(OutError, TEXT("rig_simulation_spatial_native_temporal_point_invalid"),
					Point.GetValidationDiagnostic(), TEXT("/temporal_point"));
				const FString SourceClockId = Point.GetClockId();
				if (SourceClockId == TargetClockId)
				{
					OutPoint = Point;
					return true;
				}
				TArray<FString> Pending = {SourceClockId};
				TSet<FString> Visited;
				TMap<FString, FString> ParentClock;
				TMap<FString, int32> ParentConversion;
				bool bFound = false;
				for (int32 Index = 0; Index < Pending.Num() && !bFound; ++Index)
				{
					const FString Current = Pending[Index];
					if (Visited.Contains(Current)) continue;
					Visited.Add(Current);
					for (int32 ConversionIndex = 0;
						ConversionIndex < TemporalConversions.Num(); ++ConversionIndex)
					{
						const FUeAgentTemporalConversion& Conversion =
							TemporalConversions[ConversionIndex];
						if (Conversion.GetSourceClockId() != Current) continue;
						const FString& Next = Conversion.GetTargetClockId();
						if (!ParentClock.Contains(Next))
						{
							ParentClock.Add(Next, Current);
							ParentConversion.Add(Next, ConversionIndex);
							Pending.Add(Next);
						}
						if (Next == TargetClockId) { bFound = true; break; }
					}
				}
				if (!bFound) return MissingRoute(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_missing"),
					TEXT("No registered temporal conversion route connects the source clock to the requested native clock."),
					TEXT("/temporal_conversion/path"), SourceClockId, TargetClockId);
				TArray<int32> Path;
				FString Cursor = TargetClockId;
				while (Cursor != SourceClockId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentClock.Find(Cursor);
					if (!ConversionIndex || !Parent)
					{
						MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_path_invalid"),
							!ConversionIndex ? TEXT("The selected temporal route has no conversion index for a clock.") : TEXT("The selected temporal route has no parent clock."),
							!ConversionIndex ? TEXT("/temporal_conversion/path/conversion_index") : TEXT("/temporal_conversion/path/parent_clock"), FString());
						OutError.SubjectId = Cursor;
						return false;
					}
					Path.Add(*ConversionIndex);
					Cursor = *Parent;
				}
				Algo::Reverse(Path);
				TOptional<FUeAgentTemporalPoint> Current = Point;
				for (const int32 ConversionIndex : Path)
				{
					FUeAgentStateSemanticError SemanticError;
					TOptional<FUeAgentTemporalPoint> Converted;
					const FString StepPath = TEXT("/temporal_conversions/") + FString::FromInt(ConversionIndex);
					if (!TryConvertTemporalPoint(*Current, TemporalConversions[ConversionIndex],
						EUeAgentTemporalRoundingPolicy::Exact, Resources, Converted, SemanticError))
						return NativeCause(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"), MoveTemp(SemanticError), StepPath);
					if (!Converted.IsSet()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
						TEXT("The temporal conversion step returned no point."), StepPath + TEXT("/converted_point"), FString());
					Current = MoveTemp(Converted);
				}
				OutPoint = MoveTemp(Current);
				if (!OutPoint.IsSet()) return MissingNative(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
					TEXT("The temporal conversion route returned no final point."), TEXT("/converted_point"), FString());
				if (OutPoint->GetClockId() != TargetClockId)
					return MissingRoute(OutError, TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
						TEXT("The converted point uses a different clock than the requested native clock."),
						TEXT("/converted_point/clock_matches"), OutPoint->GetClockId(), TargetClockId);
				return true;
			}

			void RecordMutation(const FString& OperationId, UObject* Object) override
			{
				FScopeLock Lock(&MutationMutex);
				MutatedObjects.Add(OperationId, Object);
			}

			UObject* FindMutation(const FString& OperationId) const override
			{
				FScopeLock Lock(&MutationMutex);
				const TWeakObjectPtr<UObject>* Found = MutatedObjects.Find(OperationId);
				return Found ? Found->Get() : nullptr;
			}

			void CountFamilyDispatch() override
			{
				Calls->Increment();
			}

			void GetExplicitObjectIdentities(TArray<FString>& OutIds) const
			{
				OutIds.Reset();
				for (const FUeAgentRigSimulationSpatialObjectBinding& Binding : ObjectBindings)
					OutIds.Add(Binding.Identity);
				OutIds.Sort();
			}

		private:
			static UObject* ResolveDirectObjectPath(const FString& Identity,
				FUeAgentRigSimulationSpatialExecutionError* OutError = nullptr)
			{
				if (Identity.IsEmpty() || Identity.Contains(TEXT("://")))
				{
					return nullptr;
				}
				FString ObjectPath = Identity;
				UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
				if (!ContentDiscovery::TryResolveEditorContentReference(ObjectPath, FString(), ResolveError))
				{
					if (OutError) *OutError = ResolveError;
					return nullptr;
				}
				if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
				{
					return Existing;
				}
				return StaticLoadObject(
					UObject::StaticClass(), nullptr, *ObjectPath,
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}

			FString RuntimeGeneration;
			FString InputRootPointer;
			TArray<FUeAgentRigSimulationSpatialObjectBinding> ObjectBindings;
			TArray<FUeAgentRigSimulationSpatialClockBinding> ClockBindings;
			TArray<FUeAgentRigSimulationSpatialFrameBinding> FrameBindings;
			TArray<FUeAgentRigSimulationSpatialSkeletonBinding> SkeletonBindings;
			TArray<FUeAgentRigSimulationSpatialSocketBinding> SocketBindings;
			TArray<FUeAgentRigSimulationSpatialRigBakeBinding> RigBakeBindings;
			TArray<FUeAgentRigSimulationSpatialIkRigBinding> IkRigBindings;
			TArray<FUeAgentRigSimulationSpatialRetargeterBinding> RetargeterBindings;
			TArray<FUeAgentRigSimulationSpatialPhysicsAssetBinding> PhysicsAssetBindings;
			TArray<FUeAgentRigSimulationSpatialPhysicsSubjectBinding> PhysicsSubjectBindings;
			TArray<FUeAgentRigSimulationSpatialDeformerGeometryBinding>
				DeformerGeometryBindings;
			TArray<FUeAgentRigSimulationSpatialDeformerTrainingInputBinding>
				DeformerTrainingInputBindings;
			TArray<FUeAgentRigSimulationSpatialDeformerEvaluationBinding>
				DeformerEvaluationBindings;
			TArray<FUeAgentSpatialConversion> SpatialConversions;
			TArray<FUeAgentTemporalConversion> TemporalConversions;
			TSharedRef<FThreadSafeCounter> Calls;
			mutable FCriticalSection MutationMutex;
			TMap<FString, TWeakObjectPtr<UObject>> MutatedObjects;
			mutable FCriticalSection DynamicBindingMutex;
			mutable TMap<FString, TSharedPtr<const
				FUeAgentRigSimulationSpatialPhysicsSubjectBinding>>
				DynamicPhysicsSubjectBindings;
		};

		class FUnavailableReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FUnavailableReadback(TSharedRef<FThreadSafeCounter> InCalls)
				: Calls(MoveTemp(InCalls)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutReadback.Reset();
				Calls->Increment();
				return MissingFamily(OutError, Contract.GetCapabilityId(), TEXT("readback"));
			}

		private:
			TSharedRef<FThreadSafeCounter> Calls;
		};

		class FUnavailableMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			explicit FUnavailableMutation(TSharedRef<FThreadSafeCounter> InCalls)
				: Calls(MoveTemp(InCalls)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				Calls->Increment();
				return MissingFamily(OutError, Contract.GetCapabilityId(), TEXT("mutation"));
			}

		private:
			TSharedRef<FThreadSafeCounter> Calls;
		};

		class FUnavailableSettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return MissingFamily(OutError, Contract.GetCapabilityId(), TEXT("settle"));
			}
		};

		class FUnavailableComparator final
			: public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentRigSimulationSpatialReadbackResult& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return MissingFamily(OutError, Contract.GetCapabilityId(), TEXT("comparison"));
			}
		};
	}

	FUeAgentRigSimulationSpatialObjectBinding::
	FUeAgentRigSimulationSpatialObjectBinding(
		FString InIdentity,
		UObject* InObject)
		: Identity(MoveTemp(InIdentity)), Object(InObject)
	{
		Identity.TrimStartAndEndInline();
	}

	bool FUeAgentRigSimulationSpatialObjectBinding::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Identity.IsEmpty()) return ConfigFailure(OutError,
			TEXT("The object binding requires an object reference."), TEXT("identity"),
			MakeShared<FJsonValueString>(Identity), TEXT("non-empty object reference"));
		if (!Object.IsValid()) return ConfigFailure(OutError,
			TEXT("The bound object is unavailable."), TEXT("object"), nullptr, TEXT("valid object"), Identity);
		if (Object->IsUnreachable()) return ConfigFailure(OutError,
			TEXT("The bound object is unreachable."), TEXT("object"), nullptr, TEXT("reachable object"), Identity);
		return true;
	}

	bool FUeAgentRigSimulationSpatialObjectBinding::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	FUeAgentRigSimulationSpatialClockBinding::
	FUeAgentRigSimulationSpatialClockBinding(
		FString InClockId,
		FString InNativeClockId,
		FString InEvidenceId)
		: ClockId(MoveTemp(InClockId))
		, NativeClockId(MoveTemp(InNativeClockId))
		, EvidenceId(MoveTemp(InEvidenceId))
	{
		ClockId.TrimStartAndEndInline();
		NativeClockId.TrimStartAndEndInline();
		EvidenceId.TrimStartAndEndInline();
	}

	bool FUeAgentRigSimulationSpatialClockBinding::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (ClockId.IsEmpty()) return ConfigFailure(OutError,
			TEXT("The clock binding requires a clock identifier."), TEXT("clock_id"),
			MakeShared<FJsonValueString>(ClockId), TEXT("non-empty clock identifier"));
		if (NativeClockId.IsEmpty()) return ConfigFailure(OutError,
			TEXT("The clock binding requires a native clock identifier."), TEXT("native_clock_id"),
			MakeShared<FJsonValueString>(NativeClockId), TEXT("non-empty native clock identifier"), ClockId);
		if (EvidenceId.IsEmpty())
		{
			ConfigFailure(OutError, TEXT("The clock binding lacks validation evidence."), TEXT("evidence_id"),
				nullptr, TEXT("non-empty clock evidence"), ClockId);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(EvidenceId);
			return false;
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialClockBinding::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	FUeAgentRigSimulationSpatialFrameBinding::
	FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame InFrame,
		const bool bInNativeFrame)
		: Frame(MoveTemp(InFrame)), bNativeFrame(bInNativeFrame)
	{
	}

	bool FUeAgentRigSimulationSpatialFrameBinding::TryValidate(FDiagnostic& OutError) const
	{
		OutError = Frame.GetValidationDiagnostic();
		if (OutError.IsEmpty()) return true;
		PrefixConfigDiagnostic(OutError, TEXT("frame"));
		return false;
	}

	bool FUeAgentRigSimulationSpatialFrameBinding::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	bool TryCreateRigSimulationSpatialProductionAssetEffectPort(
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort>& OutPort,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutError = {};
		OutPort = MakeShared<FProductionRetargetAssetEffectPort>();
		return OutPort.IsValid() ? true : MissingNative(OutError, TEXT("rig_simulation_spatial_asset_effect_port_unavailable"),
			TEXT("The retarget asset effect port could not be created."), TEXT("/asset_effect_port"), FString());
	}

	bool FUeAgentRigSimulationSpatialProductionRuntimeConfig::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto RequireInternal = [&](const FString& Value, const TCHAR* Path, const TCHAR* Reason)
		{
			if (!Value.IsEmpty()) return true;
			ConfigFailure(OutError, Reason, Path, nullptr, TEXT("non-empty runtime metadata"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Value);
			return false;
		};
		if (!RequireInternal(ProjectInstanceId, TEXT("project_instance_id"), TEXT("The runtime configuration lacks a project instance identifier."))) return false;
		if (!RequireInternal(EditorInstanceId, TEXT("editor_instance_id"), TEXT("The runtime configuration lacks an editor instance identifier."))) return false;
		if (!RequireInternal(ServerSessionId, TEXT("server_session_id"), TEXT("The runtime configuration lacks a server session identifier."))) return false;
		if (!RequireInternal(RuntimeGeneration, TEXT("runtime_generation"), TEXT("The runtime configuration lacks a runtime generation."))) return false;
		if (RegistryEpoch < 0)
		{
			ConfigFailure(OutError, TEXT("The runtime registry epoch is negative."), TEXT("registry_epoch"),
				nullptr, TEXT("non-negative registry epoch"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(::LexToString(RegistryEpoch));
			return false;
		}
		const auto ValidateBinding = [&](const auto& Binding, const FString& Prefix)
		{
			if (Binding.TryValidate(OutError)) return true;
			PrefixConfigDiagnostic(OutError, Prefix);
			return false;
		};
		const auto RequireUnique = [&](const TSet<FString>& Ids, const FString& Key,
			const FString& Value, const FString& Path, const FString& Target)
		{
			return !Ids.Contains(Key) || ConfigFailure(OutError,
				TEXT("This binding reference is already registered in the collection."), Path,
				MakeShared<FJsonValueString>(Value), TEXT("unique binding reference"), Target);
		};
		const auto RequireRegistered = [&](const TSet<FString>& Ids, const FString& Value,
			const FString& Path, const TCHAR* Expected, const FString& Target)
		{
			return Ids.Contains(Value) || ConfigFailure(OutError,
				TEXT("The referenced binding is not registered in this runtime configuration."), Path,
				MakeShared<FJsonValueString>(Value), Expected, Target);
		};
		TSet<FString> ObjectIds;
		for (int32 Index = 0; Index < ObjectBindings.Num(); ++Index)
		{
			const auto& Binding = ObjectBindings[Index];
			const FString Prefix = FString::Printf(TEXT("object_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(ObjectIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)) return false;
			ObjectIds.Add(Binding.Identity);
		}
		TSet<FString> ClockIds;
		for (int32 Index = 0; Index < ClockBindings.Num(); ++Index)
		{
			const auto& Binding = ClockBindings[Index];
			const FString Prefix = FString::Printf(TEXT("clock_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(ClockIds, Binding.ClockId, Binding.ClockId, Prefix + TEXT(".clock_id"), Binding.ClockId)) return false;
			ClockIds.Add(Binding.ClockId);
		}
		const auto RequireRegisteredClock = [&](const FString& ClockId, const FString& Path, const FString& Target)
		{
			return ClockBindings.ContainsByPredicate(
				[&ClockId](const FUeAgentRigSimulationSpatialClockBinding& Clock)
				{
					return Clock.ClockId == ClockId || Clock.NativeClockId == ClockId;
				}) || ConfigFailure(OutError,
					TEXT("The referenced clock is not registered in this runtime configuration."), Path,
					MakeShared<FJsonValueString>(ClockId), TEXT("registered clock or native clock identifier"), Target);
		};
		TSet<FString> FrameIds;
		for (int32 Index = 0; Index < FrameBindings.Num(); ++Index)
		{
			const auto& Binding = FrameBindings[Index];
			const FString Prefix = FString::Printf(TEXT("frame_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(FrameIds, Binding.Frame.GetFrameId(), Binding.Frame.GetFrameId(),
					Prefix + TEXT(".frame.frame_id"), Binding.Frame.GetFrameId())) return false;
			FrameIds.Add(Binding.Frame.GetFrameId());
		}
		TSet<FString> SkeletonIds;
		for (int32 Index = 0; Index < SkeletonBindings.Num(); ++Index)
		{
			const auto& Binding = SkeletonBindings[Index];
			const FString Prefix = FString::Printf(TEXT("skeleton_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(SkeletonIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)) return false;
			SkeletonIds.Add(Binding.Identity);
		}
		TSet<FString> SocketKeys;
		for (int32 Index = 0; Index < SocketBindings.Num(); ++Index)
		{
			const auto& Binding = SocketBindings[Index];
			const FString Prefix = FString::Printf(TEXT("socket_bindings[%d]"), Index);
			const FString Key = Binding.SkeletonIdentity + TEXT("|")
				+ Binding.SocketIdentity;
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(SocketKeys, Key, Binding.SocketIdentity, Prefix + TEXT(".socket_identity"), Binding.SkeletonIdentity)
				|| !RequireRegistered(SkeletonIds, Binding.SkeletonIdentity, Prefix + TEXT(".skeleton_identity"), TEXT("registered skeleton reference"), Binding.SkeletonIdentity)
				|| !RequireRegistered(FrameIds, Binding.NativeParentFrameId, Prefix + TEXT(".native_parent_frame_id"), TEXT("registered frame identifier"), Binding.SkeletonIdentity)) return false;
			SocketKeys.Add(Key);
		}
		TSet<FString> RigBakeIds;
		for (int32 Index = 0; Index < RigBakeBindings.Num(); ++Index)
		{
			const auto& Binding = RigBakeBindings[Index];
			const FString Prefix = FString::Printf(TEXT("rig_bake_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(RigBakeIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.NativeControlFrameId, Prefix + TEXT(".native_control_frame_id"), TEXT("registered frame identifier"), Binding.Identity)) return false;
			RigBakeIds.Add(Binding.Identity);
		}
		TSet<FString> IkRigIds;
		for (int32 Index = 0; Index < IkRigBindings.Num(); ++Index)
		{
			const auto& Binding = IkRigBindings[Index];
			const FString Prefix = FString::Printf(TEXT("ik_rig_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(IkRigIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.PreviewMeshIdentity, Prefix + TEXT(".preview_mesh_identity"), TEXT("registered object reference"), Binding.Identity)) return false;
			IkRigIds.Add(Binding.Identity);
		}
		TSet<FString> RetargeterIds;
		for (int32 Index = 0; Index < RetargeterBindings.Num(); ++Index)
		{
			const auto& Binding = RetargeterBindings[Index];
			const FString Prefix = FString::Printf(TEXT("retargeter_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(RetargeterIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(IkRigIds, Binding.SourceRigIdentity, Prefix + TEXT(".source_rig_identity"), TEXT("registered IK rig reference"), Binding.Identity)
				|| !RequireRegistered(IkRigIds, Binding.TargetRigIdentity, Prefix + TEXT(".target_rig_identity"), TEXT("registered IK rig reference"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.SourceNativePoseFrameId, Prefix + TEXT(".source_native_pose_frame_id"), TEXT("registered frame identifier"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.TargetNativePoseFrameId, Prefix + TEXT(".target_native_pose_frame_id"), TEXT("registered frame identifier"), Binding.Identity)) return false;
			RetargeterIds.Add(Binding.Identity);
		}
		TSet<FString> PhysicsAssetIds;
		for (int32 Index = 0; Index < PhysicsAssetBindings.Num(); ++Index)
		{
			const auto& Binding = PhysicsAssetBindings[Index];
			const FString Prefix = FString::Printf(TEXT("physics_asset_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(PhysicsAssetIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)) return false;
			PhysicsAssetIds.Add(Binding.Identity);
		}
		TSet<FString> PhysicsSubjectIds;
		for (int32 Index = 0; Index < PhysicsSubjectBindings.Num(); ++Index)
		{
			const auto& Binding = PhysicsSubjectBindings[Index];
			const FString Prefix = FString::Printf(TEXT("physics_subject_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(PhysicsSubjectIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.NativeSimulationFrameId, Prefix + TEXT(".native_simulation_frame_id"), TEXT("registered frame identifier"), Binding.Identity)
				|| !RequireRegisteredClock(Binding.NativeSimulationClockId, Prefix + TEXT(".native_simulation_clock_id"), Binding.Identity)) return false;
			PhysicsSubjectIds.Add(Binding.Identity);
		}
		TSet<FString> GeometryIds;
		for (int32 Index = 0; Index < DeformerGeometryBindings.Num(); ++Index)
		{
			const auto& Binding = DeformerGeometryBindings[Index];
			const FString Prefix = FString::Printf(TEXT("deformer_geometry_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(GeometryIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.NativeFrame.GetFrameId(), Prefix + TEXT(".native_frame.frame_id"), TEXT("registered frame identifier"), Binding.Identity)
				|| !RequireRegisteredClock(Binding.NativeClockId, Prefix + TEXT(".native_clock_id"), Binding.Identity)) return false;
			GeometryIds.Add(Binding.Identity);
		}
		TSet<FString> TrainingIds;
		for (int32 Index = 0; Index < DeformerTrainingInputBindings.Num(); ++Index)
		{
			const auto& Binding = DeformerTrainingInputBindings[Index];
			const FString Prefix = FString::Printf(TEXT("deformer_training_input_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(TrainingIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.NativeFrame.GetFrameId(), Prefix + TEXT(".native_frame.frame_id"), TEXT("registered frame identifier"), Binding.Identity)
				|| !RequireRegisteredClock(Binding.NativeClockId, Prefix + TEXT(".native_clock_id"), Binding.Identity)) return false;
			TrainingIds.Add(Binding.Identity);
		}
		TSet<FString> EvaluationIds;
		for (int32 Index = 0; Index < DeformerEvaluationBindings.Num(); ++Index)
		{
			const auto& Binding = DeformerEvaluationBindings[Index];
			const FString Prefix = FString::Printf(TEXT("deformer_evaluation_bindings[%d]"), Index);
			if (!ValidateBinding(Binding, Prefix)
				|| !RequireUnique(EvaluationIds, Binding.Identity, Binding.Identity, Prefix + TEXT(".identity"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.Identity, Prefix + TEXT(".identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(ObjectIds, Binding.SubjectIdentity, Prefix + TEXT(".subject_identity"), TEXT("registered object reference"), Binding.Identity)
				|| !RequireRegistered(FrameIds, Binding.NativeFrame.GetFrameId(), Prefix + TEXT(".native_frame.frame_id"), TEXT("registered frame identifier"), Binding.Identity)
				|| !RequireRegisteredClock(Binding.NativeClockId, Prefix + TEXT(".native_clock_id"), Binding.Identity)) return false;
			EvaluationIds.Add(Binding.Identity);
		}
		for (int32 Index = 0; Index < SpatialConversions.Num(); ++Index)
		{
			OutError = SpatialConversions[Index].GetValidationDiagnostic();
			if (OutError.IsEmpty()) continue;
			PrefixConfigDiagnostic(OutError, FString::Printf(TEXT("spatial_conversions[%d]"), Index));
			return false;
		}
		for (int32 Index = 0; Index < TemporalConversions.Num(); ++Index)
		{
			OutError = TemporalConversions[Index].GetValidationDiagnostic();
			if (OutError.IsEmpty()) continue;
			PrefixConfigDiagnostic(OutError, FString::Printf(TEXT("temporal_conversions[%d]"), Index));
			return false;
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialProductionRuntimeConfig::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	FUeAgentRigSimulationSpatialProductionRuntime::
	FUeAgentRigSimulationSpatialProductionRuntime(
		TSharedPtr<const FUeAgentDescriptorRegistry> InDescriptors,
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> InQueries,
		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>> InOperations,
		TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> InEnvironment,
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> InDecoder,
		TSharedRef<FThreadSafeCounter> InFamilyDispatchCounter)
		: Descriptors(MoveTemp(InDescriptors))
		, Queries(MoveTemp(InQueries))
		, Operations(MoveTemp(InOperations))
		, Environment(MoveTemp(InEnvironment))
		, Decoder(MoveTemp(InDecoder))
		, FamilyDispatchCounter(MoveTemp(InFamilyDispatchCounter))
	{
	}

	bool FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
		const FUeAgentRigSimulationSpatialProductionRuntimeConfig& Config,
		TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime>& OutRuntime,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.TryValidate(OutError)) return false;

		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		UE::UAI::Contract::FDiagnostic DescriptorError;
		const bool bDescriptorCreated = TryCreateRigSimulationSpatialDescriptorRegistry(DescriptorRegistry, DescriptorError);
		if (!bDescriptorCreated || !DescriptorRegistry.IsValid())
		{
			OutError = MoveTemp(DescriptorError);
			if (!OutError.IsEmpty()) return false;
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_descriptor_registry_unavailable"),
				!bDescriptorCreated ? TEXT("The descriptor registry factory failed without a diagnostic.") : TEXT("The descriptor registry factory returned no registry."),
				!bDescriptorCreated ? TEXT("/descriptor_registry/factory_succeeded") : TEXT("/descriptor_registry/present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		TSharedRef<FThreadSafeCounter> Calls = MakeShared<FThreadSafeCounter>();
		TSharedRef<FProductionEnvironment> ProductionEnvironment =
			MakeShared<FProductionEnvironment>(Config, Calls);
		TSharedPtr<IUeAgentRigSimulationSpatialRequestDecoder> DecoderPtr;
		const bool bDecoderCreated = TryCreateRigSimulationSpatialRequestDecoder(ProductionEnvironment, DecoderPtr, OutError);
		if (!bDecoderCreated || !DecoderPtr.IsValid())
		{
			if (!OutError.IsEmpty()) return false;
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_request_decoder_unavailable"),
				!bDecoderCreated ? TEXT("The request decoder factory failed without a diagnostic.") : TEXT("The request decoder factory returned no decoder."),
				!bDecoderCreated ? TEXT("/request_decoder/factory_succeeded") : TEXT("/request_decoder/present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		FUeAgentRigSimulationSpatialNativePorts SkeletalRigBakePorts;
		if (!TryCreateRigSimulationSpatialSkeletalRigBakeNativePorts(
				ProductionEnvironment, SkeletalRigBakePorts, OutError)) return false;
		TArray<FUeAgentRigSimulationSpatialNativePorts> FamilyPorts;
		FamilyPorts.Add(MoveTemp(SkeletalRigBakePorts));
		if (!Config.IkRigBindings.IsEmpty()
			|| !Config.RetargeterBindings.IsEmpty()
			|| Config.AssetEffectPort.IsValid())
		{
			FUeAgentRigSimulationSpatialNativePorts IkRetargetPorts;
			if (!TryCreateRigSimulationSpatialIkRetargetNativePorts(
					ProductionEnvironment, Config.AssetEffectPort,
					IkRetargetPorts, OutError)) return false;
			FamilyPorts.Add(MoveTemp(IkRetargetPorts));
		}
		if (!Config.PhysicsAssetBindings.IsEmpty()
			|| !Config.PhysicsSubjectBindings.IsEmpty()
			|| Config.PhysicsSessionPort.IsValid())
		{
			FUeAgentRigSimulationSpatialNativePorts PhysicsPorts;
			if (!TryCreateRigSimulationSpatialPhysicsNativePorts(
					ProductionEnvironment, Config.PhysicsSessionPort,
					PhysicsPorts, OutError)) return false;
			FamilyPorts.Add(MoveTemp(PhysicsPorts));
		}
		if (!Config.DeformerGeometryBindings.IsEmpty()
			|| !Config.DeformerTrainingInputBindings.IsEmpty()
			|| !Config.DeformerEvaluationBindings.IsEmpty())
		{
			FUeAgentRigSimulationSpatialNativePorts DeformerPcgPorts;
			if (!TryCreateRigSimulationSpatialDeformerPcgNativePorts(
					ProductionEnvironment, DeformerPcgPorts, OutError)) return false;
			FamilyPorts.Add(MoveTemp(DeformerPcgPorts));
		}
		FUeAgentRigSimulationSpatialNativePorts Ports;
		if (!TryComposeRigSimulationSpatialNativePorts(
				MoveTemp(FamilyPorts),
				Ports, OutError)) return false;

		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> QueryRegistry;
		const bool bQueryCreated = TryCreateRigSimulationSpatialQueryProviderRegistry(
			DecoderPtr.ToSharedRef(), Ports.Readback.ToSharedRef(), QueryRegistry, OutError);
		if (!bQueryCreated || !QueryRegistry.IsValid())
		{
			if (!OutError.IsEmpty()) return false;
			return NativeFailure(OutError, TEXT("rig_simulation_spatial_query_registry_unavailable"),
				!bQueryCreated ? TEXT("The query provider factory failed without a diagnostic.") : TEXT("The query provider factory returned no registry."),
				!bQueryCreated ? TEXT("/query_registry/factory_succeeded") : TEXT("/query_registry/present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>> Chains;
		if (!TryCreateRigSimulationSpatialOperationChains(
				DecoderPtr.ToSharedRef(), Ports.Readback.ToSharedRef(),
				Ports.Mutation.ToSharedRef(), Ports.Settle.ToSharedRef(),
				Ports.Comparator.ToSharedRef(),
				Chains, OutError)) return false;

		OutRuntime = MakeShareable(new FUeAgentRigSimulationSpatialProductionRuntime(
			MoveTemp(DescriptorRegistry), MoveTemp(QueryRegistry), MoveTemp(Chains),
			ProductionEnvironment, DecoderPtr.ToSharedRef(), Calls));
		return OutRuntime.IsValid() || MissingNative(OutError, TEXT("rig_simulation_spatial_production_runtime_unavailable"),
			TEXT("The production runtime could not be created."), TEXT("/production_runtime"), FString());
	}

	const TSharedPtr<const FUeAgentDescriptorRegistry>&
	FUeAgentRigSimulationSpatialProductionRuntime::GetDescriptorRegistry() const
	{
		return Descriptors;
	}

	const TSharedPtr<const FUeAgentDomainQueryProviderRegistry>&
	FUeAgentRigSimulationSpatialProductionRuntime::GetQueryProviderRegistry() const
	{
		return Queries;
	}

	const TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>&
	FUeAgentRigSimulationSpatialProductionRuntime::GetOperationChains() const
	{
		return Operations;
	}

	bool FUeAgentRigSimulationSpatialProductionRuntime::TryReadRevision(
		const FString& ExplicitRef,
		FRigSimulationSpatialRevision& OutRevision,
		FUeAgentRigSimulationSpatialExecutionError& OutError) const
	{
		return Environment->TryReadRevision(ExplicitRef, OutRevision, OutError);
	}

	bool FUeAgentRigSimulationSpatialProductionRuntime::TryGetIdentityFields(
		const FString& CapabilityId,
		TArray<FString>& OutFields,
		FUeAgentRigSimulationSpatialExecutionError& OutError) const
	{
		return TryGetRigSimulationSpatialIdentityFields(
			CapabilityId, OutFields, OutError);
	}

	bool FUeAgentRigSimulationSpatialProductionRuntime::TryDecodeForExecution(
		const FString& CapabilityId,
		const FUeAgentContractValue& CanonicalInput,
		TSharedPtr<const FUeAgentRigSimulationSpatialRequest>& OutRequest,
		FUeAgentRigSimulationSpatialExecutionError& OutError) const
	{
		OutRequest.Reset();
		OutError = {};
		const FUeAgentRigSimulationSpatialExecutionChainContract* Contract =
			GetRigSimulationSpatialExecutionChainContracts().FindByPredicate(
				[&CapabilityId](const FUeAgentRigSimulationSpatialExecutionChainContract& Candidate)
				{
					return Candidate.GetCapabilityId() == CapabilityId;
				});
		if (!Contract) return NativeFailure(OutError, TEXT("rig_simulation_spatial_request_spec_unknown"),
			TEXT("No execution-chain contract is registered for this capability."), TEXT("/capability_id"),
			MakeShared<FJsonValueString>(CapabilityId), MakeShared<FJsonValueString>(TEXT("a registered RigSimulationSpatial capability")));
		FUeAgentResourceTracker Resources;
		FUeAgentRigSimulationSpatialTypedRequestPtr Typed;
		if (!Decoder->TryDecode(*Contract, CanonicalInput, Resources, Typed, OutError)) return false;
		if (!Typed.IsValid()) return MissingNative(OutError, TEXT("rig_simulation_spatial_request_decoder_result_missing"),
			TEXT("The request decoder reported success but returned no typed request."), TEXT("/typed_request"), FString());
		OutRequest = StaticCastSharedPtr<const FUeAgentRigSimulationSpatialRequest>(Typed);
		return true;
	}

	int32 FUeAgentRigSimulationSpatialProductionRuntime::
	GetFamilyDispatchCallCount() const
	{
		return FamilyDispatchCounter->GetValue();
	}

	void FUeAgentRigSimulationSpatialProductionRuntime::GetExplicitObjectIdentities(
		TArray<FString>& OutIds) const
	{
		StaticCastSharedRef<FProductionEnvironment>(Environment)
			->GetExplicitObjectIdentities(OutIds);
	}
}
