#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"

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
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = TEXT("rig_simulation_spatial.production_runtime");
			OutError.Detail = Detail;
			return false;
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
				if (!Request.IsValid() || !Source || !Target || !Controller
					|| Request.AssetRequests.Num() != 1)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_request_invalid"),
						Request.DestinationRoot);
				}
				const FRigSimulationSpatialAssetRequest& Item = Request.AssetRequests[0];
				if (Item.AssetKind != TEXT("animation_sequence")
					|| (Item.OverwritePolicy != TEXT("replace")
						&& Item.OverwritePolicy != TEXT("fail_if_exists")))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_policy_invalid"),
						Item.OverwritePolicy);
				}
				USkeletalMesh* SourceMesh = Controller->GetPreviewMesh(
					ERetargetSourceOrTarget::Source);
				if (!SourceMesh || SourceMesh == Target)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_mesh_invalid"),
						Retargeter->GetPathName());
				}

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
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_operation_unavailable"));
				}
				Operation->RunRetarget(Batch);
				const FString TemporaryPath = Request.DestinationRoot + TEXT("/")
					+ TemporaryName + TEXT(".") + TemporaryName;
				UAnimSequence* Generated = FindObject<UAnimSequence>(nullptr, *TemporaryPath);
				if (!Generated)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_generated_asset_missing"),
						TemporaryPath);
				}
				const FString FinalPath = Request.DestinationRoot + TEXT("/")
					+ Item.Name + TEXT(".") + Item.Name;
				UAnimSequence* Final = FindObject<UAnimSequence>(nullptr, *FinalPath);
				if (Final && Item.OverwritePolicy == TEXT("fail_if_exists"))
				{
					ObjectTools::DeleteSingleObject(Generated, true);
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_target_exists"), FinalPath);
				}
				if (Final)
				{
					const IAnimationDataModel* Model = Generated->GetDataModel();
					if (!Model)
					{
						ObjectTools::DeleteSingleObject(Generated, true);
						return Fail(OutError,
							TEXT("rig_simulation_spatial_asset_effect_model_unavailable"),
							TemporaryPath);
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
						return Fail(OutError,
							TEXT("rig_simulation_spatial_asset_effect_rename_failed"),
							FinalPath);
					}
					Final = FindObject<UAnimSequence>(nullptr, *FinalPath);
				}
				if (!Final)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_final_asset_missing"),
						FinalPath);
				}
				OutResult.GeneratedAssets.Add(Final);
				OutResult.GeneratedAssetRefs.Add(FinalPath);
				return OutResult.IsValid();
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
				, PcgGenerationBindings(Config.PcgGenerationBindings)
				, SpatialConversions(Config.SpatialConversions)
				, TemporalConversions(Config.TemporalConversions)
				, Calls(MoveTemp(InCalls))
			{
			}

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
				FUeAgentSpatialFrame& OutFrame) const override
			{
				const FUeAgentRigSimulationSpatialFrameBinding* Found =
					FrameBindings.FindByPredicate(
						[&WireFrame](const FUeAgentRigSimulationSpatialFrameBinding& Binding)
						{
							return Binding.Frame.GetFrameId() == WireFrame.FrameId;
						});
				if (!Found || !Found->IsValid()
					|| Found->Frame.GetBasisFrameId() != WireFrame.BasisFrameId
					|| Found->Frame.GetDimension() != WireFrame.Dimension
					|| Found->Frame.GetUnit() != WireFrame.Unit
					|| Found->Frame.GetBasisEvidence() != WireFrame.BasisEvidenceId)
				{
					return false;
				}
				OutFrame = Found->Frame;
				return true;
			}

			bool TryReadRevision(
				const FString& ExplicitRef,
				FRigSimulationSpatialRevision& OutRevision,
				FUeAgentRigSimulationSpatialExecutionError& OutError) const override
			{
				OutRevision = {};
				const FUeAgentRigSimulationSpatialObjectBinding* Found =
					ObjectBindings.FindByPredicate(
						[&ExplicitRef](const FUeAgentRigSimulationSpatialObjectBinding& Binding)
						{
							return Binding.Identity == ExplicitRef;
						});
				const UObject* Object = Found && Found->IsValid()
					? Found->Object.Get() : ResolveDirectObjectPath(ExplicitRef);
				if (!Object)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_unavailable"),
						ExplicitRef);
				}
				const UPackage* Package = Object ? Object->GetOutermost() : nullptr;
				uint32 RevisionHash = GetTypeHash(Object->GetPathName());
				RevisionHash = HashCombine(RevisionHash, GetTypeHash(Object->GetClass()->GetPathName()));
				RevisionHash = HashCombine(RevisionHash,
					Package && Package->IsDirty() ? 1u : 0u);
				OutRevision.SourceId = ExplicitRef;
				OutRevision.Generation = RuntimeGeneration;
				OutRevision.Sequence = static_cast<int64>(RevisionHash);
				return OutRevision.IsValid();
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

			const FUeAgentRigSimulationSpatialPcgGenerationBinding*
			FindPcgGenerationBinding(const FString& SourceIdentity) const override
			{
				return PcgGenerationBindings.FindByPredicate(
					[&SourceIdentity](const auto& Binding)
					{
						return Binding.Identity.SourceRef == SourceIdentity;
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
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_spatial_value_invalid"),
						Value.GetFrame().GetFrameId());
				}
				if (!Target || !Target->IsValid())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_frame_unavailable"),
						TargetFrameId);
				}
				const FString SourceFrameId = Value.GetFrame().GetFrameId();
				if (SourceFrameId == TargetFrameId)
				{
					OutValue = MakeShared<FUeAgentSpatialValue>(Value);
					return OutValue->IsValid();
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
				if (!bFound)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_frame_conversion_missing"),
						SourceFrameId + TEXT("->") + TargetFrameId);
				}

				TArray<int32> Path;
				FString Cursor = TargetFrameId;
				while (Cursor != SourceFrameId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentFrame.Find(Cursor);
					if (!ConversionIndex || !Parent)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_frame_path_invalid"), Cursor);
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
					if (!NextFrame || !TryConvertSpatialValue(*Current,
							NextFrame->Frame, Conversion, Resources, Converted,
							SemanticError) || !Converted.IsValid())
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_frame_conversion_failed"),
							SemanticError.Code + TEXT(":") + SemanticError.Path);
					}
					Current = MoveTemp(Converted);
				}
				OutValue = MoveTemp(Current);
				return OutValue.IsValid() && OutValue->IsValid();
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
				if (!Range.IsValid())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_temporal_range_invalid"));
				}
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
				if (!bFound)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_native_clock_conversion_missing"),
						SourceClockId + TEXT("->") + TargetClockId);
				}
				TArray<int32> Path;
				FString Cursor = TargetClockId;
				while (Cursor != SourceClockId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentClock.Find(Cursor);
					if (!ConversionIndex || !Parent)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_clock_path_invalid"), Cursor);
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
					if (!TryConvertTemporalRange(*Current,
							TemporalConversions[ConversionIndex],
							EUeAgentTemporalRoundingPolicy::Exact,
							EUeAgentTemporalRoundingPolicy::Exact,
							Resources, Converted, SemanticError)
						|| !Converted.IsSet())
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
							SemanticError.Code + TEXT(":") + SemanticError.Path);
					}
					Current = MoveTemp(Converted);
				}
				OutRange = MoveTemp(Current);
				return OutRange.IsSet() && OutRange->GetClockId() == TargetClockId;
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
				if (!Point.IsValid()) return Fail(OutError,
					TEXT("rig_simulation_spatial_native_temporal_point_invalid"));
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
				if (!bFound) return Fail(OutError,
					TEXT("rig_simulation_spatial_native_clock_conversion_missing"),
					SourceClockId + TEXT("->") + TargetClockId);
				TArray<int32> Path;
				FString Cursor = TargetClockId;
				while (Cursor != SourceClockId)
				{
					const int32* ConversionIndex = ParentConversion.Find(Cursor);
					const FString* Parent = ParentClock.Find(Cursor);
					if (!ConversionIndex || !Parent) return Fail(OutError,
						TEXT("rig_simulation_spatial_native_clock_path_invalid"), Cursor);
					Path.Add(*ConversionIndex);
					Cursor = *Parent;
				}
				Algo::Reverse(Path);
				TOptional<FUeAgentTemporalPoint> Current = Point;
				for (const int32 ConversionIndex : Path)
				{
					FUeAgentStateSemanticError SemanticError;
					TOptional<FUeAgentTemporalPoint> Converted;
					if (!TryConvertTemporalPoint(*Current,
							TemporalConversions[ConversionIndex],
							EUeAgentTemporalRoundingPolicy::Exact, Resources,
							Converted, SemanticError) || !Converted.IsSet())
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_clock_conversion_failed"),
							SemanticError.Code + TEXT(":") + SemanticError.Path);
					}
					Current = MoveTemp(Converted);
				}
				OutPoint = MoveTemp(Current);
				return OutPoint.IsSet() && OutPoint->GetClockId() == TargetClockId;
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
			static UObject* ResolveDirectObjectPath(const FString& Identity)
			{
				if (Identity.IsEmpty() || Identity.Contains(TEXT("://")))
				{
					return nullptr;
				}
				if (UObject* Existing = FindObject<UObject>(nullptr, *Identity))
				{
					return Existing;
				}
				return StaticLoadObject(
					UObject::StaticClass(), nullptr, *Identity,
					nullptr, LOAD_NoWarn | LOAD_Quiet);
			}

			FString RuntimeGeneration;
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
			TArray<FUeAgentRigSimulationSpatialPcgGenerationBinding>
				PcgGenerationBindings;
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
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				Calls->Increment();
				return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
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
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
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

	bool FUeAgentRigSimulationSpatialObjectBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Object.IsValid()
			&& !Object->IsUnreachable();
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

	bool FUeAgentRigSimulationSpatialClockBinding::IsValid() const
	{
		return !ClockId.IsEmpty() && !NativeClockId.IsEmpty() && !EvidenceId.IsEmpty();
	}

	FUeAgentRigSimulationSpatialFrameBinding::
	FUeAgentRigSimulationSpatialFrameBinding(
		FUeAgentSpatialFrame InFrame,
		const bool bInNativeFrame)
		: Frame(MoveTemp(InFrame)), bNativeFrame(bInNativeFrame)
	{
	}

	bool FUeAgentRigSimulationSpatialFrameBinding::IsValid() const
	{
		return Frame.IsValid();
	}

	bool TryCreateRigSimulationSpatialProductionAssetEffectPort(
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort>& OutPort,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutError = {};
		OutPort = MakeShared<FProductionRetargetAssetEffectPort>();
		return OutPort.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_asset_effect_port_unavailable"));
	}

	bool FUeAgentRigSimulationSpatialProductionRuntimeConfig::IsValid() const
	{
		if (ProjectInstanceId.IsEmpty() || EditorInstanceId.IsEmpty()
			|| ServerSessionId.IsEmpty() || RuntimeGeneration.IsEmpty()
			|| RegistryEpoch < 0) return false;
		TSet<FString> ObjectIds;
		for (const FUeAgentRigSimulationSpatialObjectBinding& Binding : ObjectBindings)
		{
			if (!Binding.IsValid() || ObjectIds.Contains(Binding.Identity)) return false;
			ObjectIds.Add(Binding.Identity);
		}
		TSet<FString> ClockIds;
		for (const FUeAgentRigSimulationSpatialClockBinding& Binding : ClockBindings)
		{
			if (!Binding.IsValid() || ClockIds.Contains(Binding.ClockId)) return false;
			ClockIds.Add(Binding.ClockId);
		}
		const auto HasRegisteredClock = [this](const FString& ClockId)
		{
			return ClockBindings.ContainsByPredicate(
				[&ClockId](const FUeAgentRigSimulationSpatialClockBinding& Clock)
				{
					return Clock.ClockId == ClockId || Clock.NativeClockId == ClockId;
				});
		};
		TSet<FString> FrameIds;
		for (const FUeAgentRigSimulationSpatialFrameBinding& Binding : FrameBindings)
		{
			if (!Binding.IsValid() || FrameIds.Contains(Binding.Frame.GetFrameId()))
				return false;
			FrameIds.Add(Binding.Frame.GetFrameId());
		}
		TSet<FString> SkeletonIds;
		for (const FUeAgentRigSimulationSpatialSkeletonBinding& Binding
			: SkeletonBindings)
		{
			if (!Binding.IsValid() || SkeletonIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)) return false;
			SkeletonIds.Add(Binding.Identity);
		}
		TSet<FString> SocketKeys;
		for (const FUeAgentRigSimulationSpatialSocketBinding& Binding
			: SocketBindings)
		{
			const FString Key = Binding.SkeletonIdentity + TEXT("|")
				+ Binding.SocketIdentity;
			if (!Binding.IsValid() || SocketKeys.Contains(Key)
				|| !SkeletonIds.Contains(Binding.SkeletonIdentity)
				|| !FrameIds.Contains(Binding.NativeParentFrameId)) return false;
			SocketKeys.Add(Key);
		}
		TSet<FString> RigBakeIds;
		for (const FUeAgentRigSimulationSpatialRigBakeBinding& Binding
			: RigBakeBindings)
		{
			if (!Binding.IsValid() || RigBakeIds.Contains(Binding.Identity)
				|| !FrameIds.Contains(Binding.NativeControlFrameId)) return false;
			RigBakeIds.Add(Binding.Identity);
		}
		TSet<FString> IkRigIds;
		for (const FUeAgentRigSimulationSpatialIkRigBinding& Binding : IkRigBindings)
		{
			if (!Binding.IsValid() || IkRigIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.PreviewMeshIdentity)) return false;
			IkRigIds.Add(Binding.Identity);
		}
		TSet<FString> RetargeterIds;
		for (const FUeAgentRigSimulationSpatialRetargeterBinding& Binding
			: RetargeterBindings)
		{
			if (!Binding.IsValid() || RetargeterIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !IkRigIds.Contains(Binding.SourceRigIdentity)
				|| !IkRigIds.Contains(Binding.TargetRigIdentity)
				|| Binding.SourceRigIdentity == Binding.TargetRigIdentity
				|| !FrameIds.Contains(Binding.SourceNativePoseFrameId)
				|| !FrameIds.Contains(Binding.TargetNativePoseFrameId)
				|| Binding.SourceNativePoseFrameId
					== Binding.TargetNativePoseFrameId) return false;
			RetargeterIds.Add(Binding.Identity);
		}
		TSet<FString> PhysicsAssetIds;
		for (const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding
			: PhysicsAssetBindings)
		{
			if (!Binding.IsValid() || PhysicsAssetIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)) return false;
			PhysicsAssetIds.Add(Binding.Identity);
		}
		TSet<FString> PhysicsSubjectIds;
		for (const FUeAgentRigSimulationSpatialPhysicsSubjectBinding& Binding
			: PhysicsSubjectBindings)
		{
			if (!Binding.IsValid() || PhysicsSubjectIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !FrameIds.Contains(Binding.NativeSimulationFrameId)
				|| !ClockBindings.ContainsByPredicate(
					[&Binding](const FUeAgentRigSimulationSpatialClockBinding& Clock)
					{
						return Clock.ClockId == Binding.NativeSimulationClockId
							|| Clock.NativeClockId == Binding.NativeSimulationClockId;
					})) return false;
			PhysicsSubjectIds.Add(Binding.Identity);
		}
		TSet<FString> GeometryIds;
		for (const FUeAgentRigSimulationSpatialDeformerGeometryBinding& Binding
			: DeformerGeometryBindings)
		{
			if (!Binding.IsValid() || GeometryIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !FrameIds.Contains(Binding.NativeFrame.GetFrameId())
				|| !HasRegisteredClock(Binding.NativeClockId)) return false;
			GeometryIds.Add(Binding.Identity);
		}
		TSet<FString> TrainingIds;
		for (const FUeAgentRigSimulationSpatialDeformerTrainingInputBinding& Binding
			: DeformerTrainingInputBindings)
		{
			if (!Binding.IsValid() || TrainingIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !FrameIds.Contains(Binding.NativeFrame.GetFrameId())
				|| !HasRegisteredClock(Binding.NativeClockId)) return false;
			TrainingIds.Add(Binding.Identity);
		}
		TSet<FString> EvaluationIds;
		for (const FUeAgentRigSimulationSpatialDeformerEvaluationBinding& Binding
			: DeformerEvaluationBindings)
		{
			if (!Binding.IsValid() || EvaluationIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.Identity)
				|| !ObjectIds.Contains(Binding.SubjectIdentity)
				|| !FrameIds.Contains(Binding.NativeFrame.GetFrameId())
				|| !HasRegisteredClock(Binding.NativeClockId)) return false;
			EvaluationIds.Add(Binding.Identity);
		}
		TSet<FString> PcgIds;
		for (const FUeAgentRigSimulationSpatialPcgGenerationBinding& Binding
			: PcgGenerationBindings)
		{
			if (!Binding.IsValid() || PcgIds.Contains(Binding.Identity.SourceRef)
				|| !ObjectIds.Contains(Binding.Identity.SourceRef)
				|| !FrameIds.Contains(Binding.NativeFrame.GetFrameId())
				|| !HasRegisteredClock(Binding.GeneratedAt.GetClockId())) return false;
			PcgIds.Add(Binding.Identity.SourceRef);
		}
		for (const FUeAgentSpatialConversion& Conversion : SpatialConversions)
			if (!Conversion.IsValid()) return false;
		for (const FUeAgentTemporalConversion& Conversion : TemporalConversions)
			if (!Conversion.IsValid()) return false;
		return true;
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
		if (!Config.IsValid()) return Fail(OutError,
			TEXT("rig_simulation_spatial_runtime_config_invalid"));

		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		FString DescriptorError;
		if (!TryCreateRigSimulationSpatialDescriptorRegistry(
				DescriptorRegistry, DescriptorError) || !DescriptorRegistry.IsValid())
		{
			return Fail(OutError,
				TEXT("rig_simulation_spatial_descriptor_registry_unavailable"),
				DescriptorError);
		}

		TSharedRef<FThreadSafeCounter> Calls = MakeShared<FThreadSafeCounter>();
		TSharedRef<FProductionEnvironment> ProductionEnvironment =
			MakeShared<FProductionEnvironment>(Config, Calls);
		TSharedPtr<IUeAgentRigSimulationSpatialRequestDecoder> DecoderPtr;
		if (!TryCreateRigSimulationSpatialRequestDecoder(
				ProductionEnvironment, DecoderPtr, OutError) || !DecoderPtr.IsValid())
			return false;

		FUeAgentRigSimulationSpatialNativePorts SkeletalRigBakePorts;
		if (!TryCreateRigSimulationSpatialSkeletalRigBakeNativePorts(
				ProductionEnvironment, SkeletalRigBakePorts, OutError)
			|| !SkeletalRigBakePorts.IsValid()) return false;
		TArray<FUeAgentRigSimulationSpatialNativePorts> FamilyPorts;
		FamilyPorts.Add(MoveTemp(SkeletalRigBakePorts));
		if (!Config.IkRigBindings.IsEmpty()
			|| !Config.RetargeterBindings.IsEmpty()
			|| Config.AssetEffectPort.IsValid())
		{
			FUeAgentRigSimulationSpatialNativePorts IkRetargetPorts;
			if (!TryCreateRigSimulationSpatialIkRetargetNativePorts(
					ProductionEnvironment, Config.AssetEffectPort,
					IkRetargetPorts, OutError)
				|| !IkRetargetPorts.IsValid()) return false;
			FamilyPorts.Add(MoveTemp(IkRetargetPorts));
		}
		if (!Config.PhysicsAssetBindings.IsEmpty()
			|| !Config.PhysicsSubjectBindings.IsEmpty()
			|| Config.PhysicsSessionPort.IsValid())
		{
			FUeAgentRigSimulationSpatialNativePorts PhysicsPorts;
			if (!TryCreateRigSimulationSpatialPhysicsNativePorts(
					ProductionEnvironment, Config.PhysicsSessionPort,
					PhysicsPorts, OutError) || !PhysicsPorts.IsValid()) return false;
			FamilyPorts.Add(MoveTemp(PhysicsPorts));
		}
		if (!Config.DeformerGeometryBindings.IsEmpty()
			|| !Config.DeformerTrainingInputBindings.IsEmpty()
			|| !Config.DeformerEvaluationBindings.IsEmpty()
			|| !Config.PcgGenerationBindings.IsEmpty())
		{
			FUeAgentRigSimulationSpatialNativePorts DeformerPcgPorts;
			if (!TryCreateRigSimulationSpatialDeformerPcgNativePorts(
					ProductionEnvironment, DeformerPcgPorts, OutError)
				|| !DeformerPcgPorts.IsValid()) return false;
			FamilyPorts.Add(MoveTemp(DeformerPcgPorts));
		}
		FUeAgentRigSimulationSpatialNativePorts Ports;
		if (!TryComposeRigSimulationSpatialNativePorts(
				MoveTemp(FamilyPorts),
				Ports, OutError) || !Ports.IsValid()) return false;

		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> QueryRegistry;
		if (!TryCreateRigSimulationSpatialQueryProviderRegistry(
				DecoderPtr.ToSharedRef(), Ports.Readback.ToSharedRef(), QueryRegistry,
				OutError)
			|| !QueryRegistry.IsValid()) return false;

		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>> Chains;
		if (!TryCreateRigSimulationSpatialOperationChains(
				DecoderPtr.ToSharedRef(), Ports.Readback.ToSharedRef(),
				Ports.Mutation.ToSharedRef(), Ports.Settle.ToSharedRef(),
				Ports.Comparator.ToSharedRef(),
				Chains, OutError)) return false;

		OutRuntime = MakeShareable(new FUeAgentRigSimulationSpatialProductionRuntime(
			MoveTemp(DescriptorRegistry), MoveTemp(QueryRegistry), MoveTemp(Chains),
			ProductionEnvironment, DecoderPtr.ToSharedRef(), Calls));
		return OutRuntime.IsValid();
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
		if (!Contract) return Fail(OutError,
			TEXT("rig_simulation_spatial_request_spec_unknown"), CapabilityId);
		FUeAgentResourceTracker Resources(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 100000},
			{EUeAgentResourceDimension::Items, 100000},
			{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::WorkUnits, 100000},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
		}));
		FUeAgentRigSimulationSpatialTypedRequestPtr Typed;
		if (!Decoder->TryDecode(*Contract, CanonicalInput, Resources, Typed, OutError)
			|| !Typed.IsValid()) return false;
		OutRequest = StaticCastSharedPtr<const FUeAgentRigSimulationSpatialRequest>(Typed);
		return OutRequest.IsValid();
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
