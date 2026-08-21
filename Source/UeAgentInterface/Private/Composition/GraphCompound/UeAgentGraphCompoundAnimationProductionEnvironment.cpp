#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundDeformerProduction.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRigIkProduction.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::AnimationRigGraph;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		const FString ReadbackOracleId =
			TEXT("uai.readback.graph_compound.animation.assets");

		enum class EAnimationReadbackKind : uint8
		{
			AssetCreate,
			AnimBlueprintGraph,
			BlendSpaceDefinition,
			SkeletonDefinition,
			SkeletonCompatibility,
			SkeletonVirtualBone,
			Deformer,
			RigIk,
		};

		struct FAnimationReadbackSpec final
		{
			EAnimationReadbackKind Kind = EAnimationReadbackKind::AssetCreate;
			FString TargetKind;
			FString NativeClassPath;
			FString SkeletonRef;
			FString ParentClassRef;
			int32 Dimension = 0;
			FName GraphName;
			TArray<FAnimationRigGraphBlendAxisMutation> Axes;
			TArray<FAnimationRigGraphBlendSampleMutation> Samples;
			TArray<FAnimationRigGraphSkeletonBoneMutation> Bones;
			TArray<FAnimationRigGraphSkeletonCompatibilityMutation> Compatibility;
			FAnimationRigGraphSkeletonVirtualBoneMutation VirtualBone;
			FGraphCompoundDeformerReadbackSpec Deformer;
			FRigIkReadbackSpec RigIk;
		};

		struct FCapturedValue final
		{
			FString SemanticId;
			FUeAgentCanonicalValuePtr Value;
		};

		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool IsW02CreateProfile(const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetCapabilityId()
					== TEXT("animation.anim_blueprint.asset.create")
				|| Profile.GetCapabilityId()
					== TEXT("animation.anim_blueprint.layer_interface.asset.create")
				|| Profile.GetCapabilityId()
					== TEXT("animation.blend_space.asset.create");
		}

		bool IsW02MutationProfile(const FPcgAnimationGraphRouteProfile& Profile)
		{
			return Profile.GetCapabilityId()
					== TEXT("animation.anim_blueprint.graph.apply")
				|| Profile.GetCapabilityId()
					== TEXT("animation.blend_space.definition.apply")
				|| Profile.GetCapabilityId()
					== TEXT("animation.skeleton.definition.apply")
				|| Profile.GetCapabilityId()
					== TEXT("animation.skeleton.compatibility.apply")
				|| Profile.GetCapabilityId()
					== TEXT("animation.skeleton.virtual_bone.apply");
		}

		FString PackageName(const FString& TargetRef)
		{
			return FPackageName::ObjectPathToPackageName(TargetRef);
		}

		FString AssetName(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? FPackageName::ObjectPathToObjectName(TargetRef)
				: FPackageName::GetLongPackageAssetName(TargetRef);
		}

		FString ObjectPath(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? TargetRef : TargetRef + TEXT(".") + AssetName(TargetRef);
		}

		bool TryPackageFilename(
			const FString& InPackageName,
			FString& OutFilename)
		{
			OutFilename.Reset();
			return FPackageName::IsValidLongPackageName(InPackageName)
				&& !FPackageName::GetPackageMountPoint(InPackageName).IsNone()
				&& FPackageName::TryConvertLongPackageNameToFilename(
					InPackageName, OutFilename,
					FPackageName::GetAssetPackageExtension());
		}

		bool TryUnloadTarget(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString TargetPackage = PackageName(TargetRef);
			UPackage* const Package = FindPackage(nullptr, *TargetPackage);
			if (!Package)
			{
				return true;
			}
			if (bNotifyAssetRegistry)
			{
				if (UObject* const Asset = FindObject<UObject>(
						Package, *AssetName(TargetRef)))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *TargetPackage))
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
		}

		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(), Prepared->GetInventory(),
				Prepared->GetJournalId(), Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(), Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(), Receipt.GetPhaseHistory());
		}

		bool TryRollback(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadTarget(
				TargetRef, bNotifyAssetRegistry, UnloadDetail);
			FDurableReceipt Receipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, Receipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail : Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryStringField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value || Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		bool TryIntegerField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			const double Number = FCString::Atod(*Value->GetLexeme());
			const double Rounded = FMath::RoundToDouble(Number);
			if (!FMath::IsFinite(Number)
				|| !FMath::IsNearlyEqual(Number, Rounded)
				|| Rounded < static_cast<double>(MIN_int32)
				|| Rounded > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Rounded);
			return true;
		}

		bool TryNumberField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			double& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			OutValue = FCString::Atod(*Value->GetLexeme());
			return FMath::IsFinite(OutValue);
		}

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FString Error;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object),
					Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_animation_value_invalid"),
					FString(), Error);
				return false;
			}
			return true;
		}

		EAnimationRigGraphMutationIntent NativeIntent(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EAnimationRigGraphMutationIntent::Create;
			case EChangeIntent::Update:
				return EAnimationRigGraphMutationIntent::Update;
			case EChangeIntent::Replace:
				return EAnimationRigGraphMutationIntent::Replace;
			case EChangeIntent::Delete:
				return EAnimationRigGraphMutationIntent::Delete;
			}
			return EAnimationRigGraphMutationIntent::Update;
		}

		bool TryCreateAbsentSnapshot(
			const FGraphCompoundAnimationTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			FString Filename;
			if (!TryPackageFilename(Request.GetTargetRef(), Filename))
			{
				SetError(OutError,
					TEXT("graph_compound_animation_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (FindPackage(nullptr, *Request.GetTargetRef())
				|| IFileManager::Get().FileExists(*Filename))
			{
				SetError(OutError,
					TEXT("graph_compound_animation_target_already_exists"),
					Request.GetTargetRef());
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:animation-absent:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:animation-create"),
						TEXT("absent:") + Request.GetTargetRef(), 0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("file-absence:") + Filename),
					{}, Resources, OutSnapshot, Error)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, Error.Code,
					Request.GetTargetRef(), Error.Detail);
				return false;
			}
			return true;
		}

		bool AddCapturedObject(
			const FString& SemanticId,
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			TArray<FCapturedValue>& OutValues,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalObject(Object, Resources, Value, OutError))
			{
				return false;
			}
			OutValues.Add({SemanticId, MoveTemp(Value)});
			return true;
		}

		bool TryCaptureMutationSnapshot(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundAnimationTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UObject* const Target = LoadObject<UObject>(
				nullptr, *Request.GetTargetRef());
			if (!Target || Target->GetPathName() != Request.GetTargetRef())
			{
				SetError(OutError,
					TEXT("graph_compound_animation_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			TArray<FCapturedValue> Values;
			const FString& Capability = Profile.GetCapabilityId();
			if (IsDeformerMutationProfile(Profile))
			{
				TArray<FGraphCompoundDeformerCapturedValue> DeformerValues;
				if (!TryCaptureDeformerValues(
						Profile, *Target, DeformerValues, OutError))
				{
					return false;
				}
				for (const FGraphCompoundDeformerCapturedValue& Captured
					: DeformerValues)
				{
					if (!AddCapturedObject(
							Captured.SemanticId, Captured.Value,
							Resources, Values, OutError))
					{
						return false;
					}
				}
			}
			else if (IsRigIkMutationProfile(Profile))
			{
				TArray<FRigIkCapturedValue> RigValues;
				if (!TryCaptureRigIkValues(Profile, *Target, RigValues, OutError))
				{
					return false;
				}
				for (const FRigIkCapturedValue& Captured : RigValues)
				{
					if (!AddCapturedObject(
						Captured.SemanticId, Captured.Value,
						Resources, Values, OutError))
					{
						return false;
					}
				}
			}
			else if (Capability == TEXT("animation.anim_blueprint.graph.apply"))
			{
				UAnimBlueprint* const Blueprint = Cast<UAnimBlueprint>(Target);
				if (!Blueprint)
				{
					SetError(OutError,
						TEXT("graph_compound_animation_target_class_mismatch"),
						Request.GetTargetRef());
					return false;
				}
				TArray<UEdGraph*> Graphs;
				Blueprint->GetAllGraphs(Graphs);
				for (const UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("graph_name"),
						Graph->GetName());
					if (!AddCapturedObject(
							TEXT("structure:anim_graph:") + Graph->GetName(),
							Value, Resources, Values, OutError))
					{
						return false;
					}
				}
			}
			else if (Capability
				== TEXT("animation.blend_space.definition.apply"))
			{
				UBlendSpace* const BlendSpace = Cast<UBlendSpace>(Target);
				if (!BlendSpace)
				{
					SetError(OutError,
						TEXT("graph_compound_animation_target_class_mismatch"),
						Request.GetTargetRef());
					return false;
				}
				for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
				{
					const FBlendParameter& Axis =
						BlendSpace->GetBlendParameter(AxisIndex);
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetNumberField(TEXT("axis_index"), AxisIndex);
					Value->SetStringField(TEXT("axis_name"), Axis.DisplayName);
					Value->SetNumberField(TEXT("minimum"), Axis.Min);
					Value->SetNumberField(TEXT("maximum"), Axis.Max);
					Value->SetNumberField(TEXT("grid_divisions"), Axis.GridNum);
					if (!AddCapturedObject(
							FString::Printf(TEXT("item:blend_axis:%d"), AxisIndex),
							Value, Resources, Values, OutError))
					{
						return false;
					}
				}
			}
			else
			{
				USkeleton* const Skeleton = Cast<USkeleton>(Target);
				if (!Skeleton)
				{
					SetError(OutError,
						TEXT("graph_compound_animation_target_class_mismatch"),
						Request.GetTargetRef());
					return false;
				}
				if (Capability == TEXT("animation.skeleton.definition.apply"))
				{
					const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
					for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
					{
						const FName BoneName = Ref.GetBoneName(Index);
						const int32 ParentIndex = Ref.GetParentIndex(Index);
						const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
						Value->SetStringField(TEXT("bone_name"), BoneName.ToString());
						Value->SetStringField(TEXT("parent_bone"),
							ParentIndex == INDEX_NONE ? FString()
								: Ref.GetBoneName(ParentIndex).ToString());
						if (!AddCapturedObject(
								TEXT("hierarchy:bone:") + BoneName.ToString(),
								Value, Resources, Values, OutError))
						{
							return false;
						}
					}
				}
				else if (Capability
					== TEXT("animation.skeleton.compatibility.apply"))
				{
					for (const TSoftObjectPtr<USkeleton>& Compatible
						: Skeleton->GetCompatibleSkeletons())
					{
						const FString Ref =
							Compatible.ToSoftObjectPath().ToString();
						if (!AddCapturedObject(
								TEXT("binding:compatible_skeleton:") + Ref,
								MakeShared<FJsonObject>(), Resources,
								Values, OutError))
						{
							return false;
						}
					}
				}
				else
				{
					for (const FVirtualBone& Bone : Skeleton->GetVirtualBones())
					{
						const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
						Value->SetStringField(TEXT("source_bone"),
							Bone.SourceBoneName.ToString());
						Value->SetStringField(TEXT("target_bone"),
							Bone.TargetBoneName.ToString());
						if (!AddCapturedObject(
								TEXT("hierarchy:virtual_bone:")
									+ Bone.VirtualBoneName.ToString(),
								Value, Resources, Values, OutError))
						{
							return false;
						}
					}
				}
			}

			FString Filename;
			TryPackageFilename(PackageName(Request.GetTargetRef()), Filename);
			const FString Generation = TEXT("md5:") + FMD5::HashAnsiString(
				*(Request.GetTargetRef() + TEXT("|")
					+ Target->GetClass()->GetPathName() + TEXT("|")
					+ IFileManager::Get().GetTimeStamp(*Filename).ToIso8601()));
			TArray<FUeAgentStateEntry> Entries;
			for (const FCapturedValue& Captured : Values)
			{
				FUeAgentStateEntry Entry(
					FUeAgentCanonicalIdentity(
						Captured.SemanticId, Request.GetTargetKind(),
						Request.GetTargetRef(), Captured.SemanticId,
						TEXT("graph_compound"), Generation),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.animation.state.v1"),
						Captured.Value->GetKind(), false,
						Captured.Value->GetKind() == EUeAgentValueKind::Object
							? TEXT("object") : TEXT("scalar")),
					Captured.Value.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("independent animation readback"),
						Captured.Value.ToSharedRef()),
					FUeAgentStateDetails(
						Captured.SemanticId,
						TEXT("GraphCompound Animation current state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:animation-mutation"),
						Generation, 0),
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
				if (!Entry.IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_animation_state_entry_invalid"),
						Captured.SemanticId);
					return false;
				}
				Entries.Add(MoveTemp(Entry));
			}
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:animation-mutation:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:c4:animation-mutation"),
						Generation, 0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("reloadable-native-state:") + Generation),
					MoveTemp(Entries), Resources, OutSnapshot, StateError)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			return true;
		}

		bool TryParseReadbackSpec(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundAnimationOperationRequest& Request,
			FAnimationReadbackSpec& OutSpec,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSpec = {};
			OutSpec.TargetKind = Request.GetTargetKind();
			if (IsDeformerProductionProfile(Profile))
			{
				OutSpec.Kind = EAnimationReadbackKind::Deformer;
				if (!TryParseDeformerReadbackSpec(
						Profile, Request, OutSpec.Deformer, OutError))
				{
					return false;
				}
				OutSpec.NativeClassPath = OutSpec.Deformer.NativeClassPath;
				return true;
			}
			if (IsRigIkCreateProfile(Profile) || IsRigIkMutationProfile(Profile))
			{
				OutSpec.Kind = EAnimationReadbackKind::RigIk;
				if (!TryParseRigIkReadbackSpec(
					Profile, Request, OutSpec.RigIk, OutError))
				{
					return false;
				}
				OutSpec.NativeClassPath = OutSpec.RigIk.NativeClassPath;
				return true;
			}
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			if (Edits.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_animation_plan_invalid"),
					Request.GetTargetRef());
				return false;
			}
			const FString& Capability = Profile.GetCapabilityId();
			if (IsW02CreateProfile(Profile))
			{
				OutSpec.Kind = EAnimationReadbackKind::AssetCreate;
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				if (!Value.IsValid())
				{
					SetError(OutError,
						TEXT("graph_compound_animation_create_value_missing"));
					return false;
				}
				TryStringField(*Value, TEXT("skeleton_ref"), OutSpec.SkeletonRef);
				TryStringField(*Value, TEXT("parent_class_ref"), OutSpec.ParentClassRef);
				TryIntegerField(*Value, TEXT("dimension"), OutSpec.Dimension);
				if (Capability
					== TEXT("animation.anim_blueprint.asset.create"))
				{
					OutSpec.NativeClassPath = UAnimBlueprint::StaticClass()->GetPathName();
					if (OutSpec.SkeletonRef.IsEmpty()
						|| OutSpec.ParentClassRef.IsEmpty())
					{
						SetError(OutError,
							TEXT("graph_compound_animation_create_reference_missing"));
						return false;
					}
				}
				else if (Capability
					== TEXT("animation.anim_blueprint.layer_interface.asset.create"))
				{
					OutSpec.NativeClassPath = UAnimBlueprint::StaticClass()->GetPathName();
				}
				else
				{
					OutSpec.NativeClassPath = OutSpec.Dimension == 1
						? UBlendSpace1D::StaticClass()->GetPathName()
						: UBlendSpace::StaticClass()->GetPathName();
					if (OutSpec.SkeletonRef.IsEmpty()
						|| (OutSpec.Dimension != 1 && OutSpec.Dimension != 2))
					{
						SetError(OutError,
							TEXT("graph_compound_animation_blend_create_invalid"));
						return false;
					}
				}
				return true;
			}

			if (Capability == TEXT("animation.anim_blueprint.graph.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::AnimBlueprintGraph;
				if (Edits.Num() != 1 || !Edits[0].GetProposedAfter().IsValid()
					|| !TryStringField(
						*Edits[0].GetProposedAfter(), TEXT("graph_name"),
						OutSpec.NativeClassPath))
				{
					SetError(OutError,
						TEXT("graph_compound_animation_graph_plan_invalid"));
					return false;
				}
				OutSpec.GraphName = *OutSpec.NativeClassPath;
				OutSpec.NativeClassPath = UAnimBlueprint::StaticClass()->GetPathName();
				return true;
			}

			if (Capability == TEXT("animation.blend_space.definition.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::BlendSpaceDefinition;
				OutSpec.NativeClassPath = UBlendSpace::StaticClass()->GetPathName();
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					if (!Value.IsValid())
					{
						SetError(OutError,
							TEXT("graph_compound_animation_blend_plan_invalid"));
						return false;
					}
					if (Edit.GetIdentity().GetSemanticId().StartsWith(
							TEXT("item:blend_axis:")))
					{
						FAnimationRigGraphBlendAxisMutation Axis;
						FString AxisName;
						if (!TryIntegerField(*Value, TEXT("axis_index"), Axis.AxisIndex)
							|| !TryStringField(*Value, TEXT("axis_name"), AxisName)
							|| !TryNumberField(*Value, TEXT("minimum"), Axis.Minimum)
							|| !TryNumberField(*Value, TEXT("maximum"), Axis.Maximum)
							|| !TryIntegerField(
								*Value, TEXT("grid_divisions"), Axis.GridDivisions))
						{
							SetError(OutError,
								TEXT("graph_compound_animation_blend_axis_invalid"));
							return false;
						}
						Axis.AxisName = *AxisName;
						OutSpec.Axes.Add(Axis);
					}
					else
					{
						FAnimationRigGraphBlendSampleMutation Sample;
						FString AnimationRef;
						const FUeAgentCanonicalValue* Coordinates =
							FindField(*Value, TEXT("coordinates"));
						if (!TryStringField(
								*Value, TEXT("animation_ref"), AnimationRef)
							|| !Coordinates
							|| Coordinates->GetKind() != EUeAgentValueKind::Array
							|| Coordinates->GetArrayItems().IsEmpty()
							|| Coordinates->GetArrayItems().Num() > 3)
						{
							SetError(OutError,
								TEXT("graph_compound_animation_blend_sample_invalid"));
							return false;
						}
						Sample.Animation = LoadObject<UAnimSequence>(
							nullptr, *AnimationRef);
						for (int32 Index = 0;
							Index < Coordinates->GetArrayItems().Num(); ++Index)
						{
							const FUeAgentCanonicalValue& Coordinate =
								Coordinates->GetArrayItems()[Index].Get();
							if (Coordinate.GetKind() != EUeAgentValueKind::Integer
								&& Coordinate.GetKind() != EUeAgentValueKind::Decimal)
							{
								SetError(OutError,
									TEXT("graph_compound_animation_blend_coordinate_invalid"));
								return false;
							}
							Sample.Coordinates[Index] =
								FCString::Atod(*Coordinate.GetLexeme());
						}
						Sample.Intent = NativeIntent(Edit.GetIntent());
						OutSpec.Samples.Add(Sample);
					}
				}
				return !OutSpec.Axes.IsEmpty();
			}

			if (Capability == TEXT("animation.skeleton.definition.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::SkeletonDefinition;
				OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					FAnimationRigGraphSkeletonBoneMutation Bone;
					FString BoneName;
					FString ParentName;
					if (!Edit.GetProposedAfter().IsValid()
						|| !TryStringField(
							*Edit.GetProposedAfter(), TEXT("bone_name"), BoneName))
					{
						SetError(OutError,
							TEXT("graph_compound_animation_skeleton_bone_invalid"));
						return false;
					}
					TryStringField(
						*Edit.GetProposedAfter(), TEXT("parent_bone"), ParentName);
					Bone.BoneName = *BoneName;
					Bone.ParentBone = *ParentName;
					Bone.Intent = NativeIntent(Edit.GetIntent());
					OutSpec.Bones.Add(Bone);
				}
				return !OutSpec.Bones.IsEmpty();
			}

			if (Capability == TEXT("animation.skeleton.compatibility.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::SkeletonCompatibility;
				OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
				const FString Prefix = TEXT("binding:compatible_skeleton:");
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
					if (!SemanticId.StartsWith(Prefix))
					{
						SetError(OutError,
							TEXT("graph_compound_animation_compatibility_identity_invalid"));
						return false;
					}
					FAnimationRigGraphSkeletonCompatibilityMutation Mutation;
					Mutation.CompatibleSkeleton = LoadObject<USkeleton>(
						nullptr, *SemanticId.Mid(Prefix.Len()));
					Mutation.Intent = NativeIntent(Edit.GetIntent());
					if (!Mutation.CompatibleSkeleton)
					{
						SetError(OutError,
							TEXT("graph_compound_animation_compatible_skeleton_invalid"));
						return false;
					}
					OutSpec.Compatibility.Add(Mutation);
				}
				return !OutSpec.Compatibility.IsEmpty();
			}

			OutSpec.Kind = EAnimationReadbackKind::SkeletonVirtualBone;
			OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
			const FString Prefix = TEXT("hierarchy:virtual_bone:");
			const FGraphCompoundCanonicalEdit& Edit = Edits[0];
			if (Edits.Num() != 1
				|| !Edit.GetIdentity().GetSemanticId().StartsWith(Prefix)
				|| !Edit.GetProposedAfter().IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_animation_virtual_bone_invalid"));
				return false;
			}
			FString SourceBone;
			FString TargetBone;
			if (!TryStringField(
					*Edit.GetProposedAfter(), TEXT("source_bone"), SourceBone)
				|| !TryStringField(
						*Edit.GetProposedAfter(), TEXT("target_bone"), TargetBone))
			{
				SetError(OutError,
					TEXT("graph_compound_animation_virtual_bone_invalid"));
				return false;
			}
			OutSpec.VirtualBone.Name =
				*Edit.GetIdentity().GetSemanticId().Mid(Prefix.Len());
			OutSpec.VirtualBone.SourceBone = *SourceBone;
			OutSpec.VirtualBone.TargetBone = *TargetBone;
			OutSpec.VirtualBone.Intent = NativeIntent(Edit.GetIntent());
			return true;
		}

		TSharedRef<FJsonObject> ExpectedJson(
			const FAnimationReadbackSpec& Spec)
		{
			if (Spec.Kind == EAnimationReadbackKind::RigIk)
			{
				return RigIkExpectedJson(Spec.RigIk);
			}
			if (Spec.Kind == EAnimationReadbackKind::Deformer)
			{
				return DeformerExpectedJson(Spec.Deformer);
			}
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_kind"), Spec.TargetKind);
			Root->SetStringField(TEXT("native_class"), Spec.NativeClassPath);
			switch (Spec.Kind)
			{
			case EAnimationReadbackKind::AssetCreate:
				if (Spec.TargetKind == TEXT("anim_layer_interface"))
				{
					Root->SetStringField(TEXT("blueprint_type"), TEXT("interface"));
				}
				else
				{
					Root->SetStringField(TEXT("skeleton_ref"), Spec.SkeletonRef);
					if (Spec.TargetKind == TEXT("anim_blueprint"))
					{
						Root->SetStringField(
							TEXT("parent_class_ref"), Spec.ParentClassRef);
					}
					else
					{
						Root->SetNumberField(TEXT("dimension"), Spec.Dimension);
					}
				}
				break;
			case EAnimationReadbackKind::AnimBlueprintGraph:
				Root->SetStringField(TEXT("graph_name"), Spec.GraphName.ToString());
				break;
			case EAnimationReadbackKind::BlendSpaceDefinition:
			{
				TArray<TSharedPtr<FJsonValue>> Axes;
				for (const FAnimationRigGraphBlendAxisMutation& Axis : Spec.Axes)
				{
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetNumberField(TEXT("axis_index"), Axis.AxisIndex);
					Value->SetStringField(TEXT("axis_name"), Axis.AxisName.ToString());
					Value->SetNumberField(TEXT("minimum"), Axis.Minimum);
					Value->SetNumberField(TEXT("maximum"), Axis.Maximum);
					Value->SetNumberField(TEXT("grid_divisions"), Axis.GridDivisions);
					Axes.Add(MakeShared<FJsonValueObject>(Value));
				}
				Root->SetArrayField(TEXT("axes"), MoveTemp(Axes));
				TArray<TSharedPtr<FJsonValue>> Samples;
				for (const FAnimationRigGraphBlendSampleMutation& Sample : Spec.Samples)
				{
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("animation_ref"),
						Sample.Animation ? Sample.Animation->GetPathName() : FString());
					Value->SetArrayField(TEXT("coordinates"), {
						MakeShared<FJsonValueNumber>(Sample.Coordinates.X),
						MakeShared<FJsonValueNumber>(Sample.Coordinates.Y),
						MakeShared<FJsonValueNumber>(Sample.Coordinates.Z)});
					Value->SetBoolField(TEXT("present"),
						Sample.Intent != EAnimationRigGraphMutationIntent::Delete);
					Samples.Add(MakeShared<FJsonValueObject>(Value));
				}
				Root->SetArrayField(TEXT("samples"), MoveTemp(Samples));
				break;
			}
			case EAnimationReadbackKind::SkeletonDefinition:
			{
				TArray<TSharedPtr<FJsonValue>> Bones;
				for (const FAnimationRigGraphSkeletonBoneMutation& Bone : Spec.Bones)
				{
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("bone_name"), Bone.BoneName.ToString());
					Value->SetStringField(TEXT("parent_bone"), Bone.ParentBone.ToString());
					Value->SetBoolField(TEXT("present"),
						Bone.Intent != EAnimationRigGraphMutationIntent::Delete);
					Bones.Add(MakeShared<FJsonValueObject>(Value));
				}
				Root->SetArrayField(TEXT("bones"), MoveTemp(Bones));
				break;
			}
			case EAnimationReadbackKind::SkeletonCompatibility:
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FAnimationRigGraphSkeletonCompatibilityMutation& Item
					: Spec.Compatibility)
				{
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("skeleton_ref"),
						Item.CompatibleSkeleton
							? Item.CompatibleSkeleton->GetPathName() : FString());
					Value->SetBoolField(TEXT("present"),
						Item.Intent != EAnimationRigGraphMutationIntent::Delete);
					Values.Add(MakeShared<FJsonValueObject>(Value));
				}
				Root->SetArrayField(TEXT("compatibility"), MoveTemp(Values));
				break;
			}
			case EAnimationReadbackKind::SkeletonVirtualBone:
				Root->SetStringField(TEXT("name"), Spec.VirtualBone.Name.ToString());
				Root->SetStringField(TEXT("source_bone"),
					Spec.VirtualBone.SourceBone.ToString());
				Root->SetStringField(TEXT("target_bone"),
					Spec.VirtualBone.TargetBone.ToString());
				Root->SetBoolField(TEXT("present"),
					Spec.VirtualBone.Intent
						!= EAnimationRigGraphMutationIntent::Delete);
				break;
			case EAnimationReadbackKind::Deformer:
			case EAnimationReadbackKind::RigIk:
				break;
			}
			return Root;
		}

		bool TryBuildActualJson(
			const FString& TargetRef,
			const FAnimationReadbackSpec& Spec,
			TSharedRef<FJsonObject>& OutRoot,
			FString& OutError)
		{
			OutError.Reset();
			if (Spec.Kind == EAnimationReadbackKind::RigIk)
			{
				return TryBuildRigIkActualJson(
					TargetRef, Spec.RigIk, OutRoot, OutError);
			}
			if (Spec.Kind == EAnimationReadbackKind::Deformer)
			{
				return TryBuildDeformerActualJson(
					TargetRef, Spec.Deformer, OutRoot, OutError);
			}
			OutRoot = MakeShared<FJsonObject>();
			UObject* const Target = LoadObject<UObject>(
				nullptr, *ObjectPath(TargetRef));
			if (!Target || Target->GetClass()->GetPathName() != Spec.NativeClassPath)
			{
				OutError = TEXT("animation_readback_target_class_mismatch");
				return false;
			}
			OutRoot->SetStringField(TEXT("target_kind"), Spec.TargetKind);
			OutRoot->SetStringField(TEXT("native_class"),
				Target->GetClass()->GetPathName());
			if (Spec.Kind == EAnimationReadbackKind::AssetCreate)
			{
				if (Spec.TargetKind == TEXT("anim_layer_interface"))
				{
					const UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Target);
					OutRoot->SetStringField(TEXT("blueprint_type"),
						Blueprint && Blueprint->BlueprintType == BPTYPE_Interface
							? TEXT("interface") : TEXT("other"));
					return true;
				}
				if (const UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Target))
				{
					OutRoot->SetStringField(TEXT("skeleton_ref"),
						Blueprint->TargetSkeleton
							? Blueprint->TargetSkeleton->GetPathName() : FString());
					OutRoot->SetStringField(TEXT("parent_class_ref"),
						Blueprint->ParentClass
							? Blueprint->ParentClass->GetPathName() : FString());
				}
				else if (const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Target))
				{
					OutRoot->SetStringField(TEXT("skeleton_ref"),
						BlendSpace->GetSkeleton()
							? BlendSpace->GetSkeleton()->GetPathName() : FString());
					OutRoot->SetNumberField(TEXT("dimension"),
						Target->IsA<UBlendSpace1D>() ? 1 : 2);
				}
				return true;
			}
			if (Spec.Kind == EAnimationReadbackKind::AnimBlueprintGraph)
			{
				const UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Target);
				TArray<UEdGraph*> Graphs;
				if (Blueprint)
				{
					Blueprint->GetAllGraphs(Graphs);
				}
				UEdGraph* const* FoundGraph = Graphs.FindByPredicate(
					[&Spec](const UEdGraph* Graph)
					{
						return Graph && Graph->GetFName() == Spec.GraphName;
					});
				const UEdGraph* const ActualGraph = FoundGraph ? *FoundGraph : nullptr;
				if (!ActualGraph)
				{
					OutError = TEXT("animation_readback_graph_missing");
					return false;
				}
				OutRoot->SetStringField(TEXT("graph_name"), ActualGraph->GetName());
				return true;
			}
			if (Spec.Kind == EAnimationReadbackKind::BlendSpaceDefinition)
			{
				const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Target);
				if (!BlendSpace)
				{
					OutError = TEXT("animation_readback_blend_missing");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> AxisValues;
				for (const FAnimationRigGraphBlendAxisMutation& Axis : Spec.Axes)
				{
					const FBlendParameter& Actual =
						BlendSpace->GetBlendParameter(Axis.AxisIndex);
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetNumberField(TEXT("axis_index"), Axis.AxisIndex);
					Value->SetStringField(TEXT("axis_name"), Actual.DisplayName);
					Value->SetNumberField(TEXT("minimum"), Actual.Min);
					Value->SetNumberField(TEXT("maximum"), Actual.Max);
					Value->SetNumberField(TEXT("grid_divisions"), Actual.GridNum);
					AxisValues.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutRoot->SetArrayField(TEXT("axes"), MoveTemp(AxisValues));
				TArray<TSharedPtr<FJsonValue>> SampleValues;
				for (const FAnimationRigGraphBlendSampleMutation& Sample : Spec.Samples)
				{
					const FBlendSample* Actual = BlendSpace->GetBlendSamples().FindByPredicate(
						[&Sample](const FBlendSample& Actual)
						{
							return Actual.Animation == Sample.Animation
								&& Actual.SampleValue.Equals(
									Sample.Coordinates, UE_DOUBLE_SMALL_NUMBER);
						});
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("animation_ref"), Actual && Actual->Animation
						? Actual->Animation->GetPathName()
						: (Sample.Animation ? Sample.Animation->GetPathName() : FString()));
					const FVector Coordinates = Actual
						? Actual->SampleValue : Sample.Coordinates;
					Value->SetArrayField(TEXT("coordinates"), {
						MakeShared<FJsonValueNumber>(Coordinates.X),
						MakeShared<FJsonValueNumber>(Coordinates.Y),
						MakeShared<FJsonValueNumber>(Coordinates.Z)});
					Value->SetBoolField(TEXT("present"), Actual != nullptr);
					SampleValues.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutRoot->SetArrayField(TEXT("samples"), MoveTemp(SampleValues));
				return true;
			}
			const USkeleton* Skeleton = Cast<USkeleton>(Target);
			if (!Skeleton)
			{
				OutError = TEXT("animation_readback_skeleton_missing");
				return false;
			}
			if (Spec.Kind == EAnimationReadbackKind::SkeletonDefinition)
			{
				const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FAnimationRigGraphSkeletonBoneMutation& Bone : Spec.Bones)
				{
					const int32 Index = Ref.FindBoneIndex(Bone.BoneName);
					const bool bPresent = Index != INDEX_NONE;
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("bone_name"), Bone.BoneName.ToString());
					Value->SetStringField(TEXT("parent_bone"), bPresent
						&& Ref.GetParentIndex(Index) != INDEX_NONE
							? Ref.GetBoneName(Ref.GetParentIndex(Index)).ToString()
							: (bPresent ? FString() : Bone.ParentBone.ToString()));
					Value->SetBoolField(TEXT("present"), bPresent);
					Values.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutRoot->SetArrayField(TEXT("bones"), MoveTemp(Values));
				return true;
			}
			if (Spec.Kind == EAnimationReadbackKind::SkeletonCompatibility)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FAnimationRigGraphSkeletonCompatibilityMutation& Item
					: Spec.Compatibility)
				{
					const FString Ref = Item.CompatibleSkeleton->GetPathName();
					const bool bPresent = Skeleton->GetCompatibleSkeletons()
						.ContainsByPredicate([&Ref](const TSoftObjectPtr<USkeleton>& Value)
						{
							return Value.ToSoftObjectPath().ToString() == Ref;
						});
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("skeleton_ref"), Ref);
					Value->SetBoolField(TEXT("present"), bPresent);
					Values.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutRoot->SetArrayField(TEXT("compatibility"), MoveTemp(Values));
				return true;
			}
			const FVirtualBone* Actual = Skeleton->GetVirtualBones().FindByPredicate(
				[&Spec](const FVirtualBone& Bone)
				{
					return Bone.VirtualBoneName == Spec.VirtualBone.Name;
				});
			OutRoot->SetStringField(TEXT("name"), Spec.VirtualBone.Name.ToString());
			OutRoot->SetStringField(TEXT("source_bone"), Actual
				? Actual->SourceBoneName.ToString() : Spec.VirtualBone.SourceBone.ToString());
			OutRoot->SetStringField(TEXT("target_bone"), Actual
				? Actual->TargetBoneName.ToString() : Spec.VirtualBone.TargetBone.ToString());
			OutRoot->SetBoolField(TEXT("present"), Actual != nullptr);
			return true;
		}

		class FAnimationReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FAnimationReadbackOracle(
				FString InTargetRef,
				FAnimationReadbackSpec InSpec)
				: TargetRef(MoveTemp(InTargetRef))
				, Spec(MoveTemp(InSpec))
			{
			}

			const FString& GetOracleId() const override
			{
				return ReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				const TStrongObjectPtr<USkeleton> SkeletonAnchor(
					Spec.SkeletonRef.IsEmpty()
						? nullptr
						: LoadObject<USkeleton>(nullptr, *Spec.SkeletonRef));
				const TStrongObjectPtr<UClass> ParentClassAnchor(
					Spec.ParentClassRef.IsEmpty()
						? nullptr
						: LoadObject<UClass>(nullptr, *Spec.ParentClassRef));
				TArray<TStrongObjectPtr<UObject>> DependencyAnchors;
				if (Spec.Kind == EAnimationReadbackKind::RigIk)
				{
					AddRigIkDependencyAnchors(Spec.RigIk, DependencyAnchors);
				}
				else if (Spec.Kind == EAnimationReadbackKind::Deformer)
				{
					AddDeformerDependencyAnchors(
						Spec.Deformer, DependencyAnchors);
				}
				for (const FAnimationRigGraphBlendSampleMutation& Sample
					: Spec.Samples)
				{
					if (Sample.Animation)
					{
						DependencyAnchors.Emplace(Sample.Animation);
					}
				}
				for (const FAnimationRigGraphSkeletonCompatibilityMutation& Item
					: Spec.Compatibility)
				{
					if (Item.CompatibleSkeleton)
					{
						DependencyAnchors.Emplace(Item.CompatibleSkeleton);
					}
				}
				FString UnloadDetail;
				if (Request.GetOracleId() != ReadbackOracleId
					|| Request.GetSubjectRef() != TargetRef
					|| !TryUnloadTarget(TargetRef, false, UnloadDetail))
				{
					OutError = FVerificationError(
						TEXT("animation_readback_unload_failed"),
						TargetRef + TEXT(":") + UnloadDetail);
					return false;
				}
				TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				FString ReadError;
				if (!TryBuildActualJson(TargetRef, Spec, Actual, ReadError))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						ReadError, TEXT("animation-readback:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr ActualValue;
				FGraphCompoundRuntimeError CanonicalError;
				if (!TryCanonicalObject(
						Actual, Resources, ActualValue, CanonicalError))
				{
					OutError = FVerificationError(
						CanonicalError.Code,
						TargetRef + TEXT(":") + CanonicalError.Detail);
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					ActualValue.ToSharedRef(),
					TEXT("animation-readback:") + TargetRef + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
			FAnimationReadbackSpec Spec;
		};

		UObject* TryCreateAsset(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundAnimationOperationRequest& Request,
			const FAnimationReadbackSpec& Spec,
			UPackage& Package,
			FGraphCompoundRuntimeError& OutError)
		{
			if (Spec.Kind == EAnimationReadbackKind::RigIk)
			{
				return TryCreateRigIkAsset(
					Profile, Request, Spec.RigIk, Package, OutError);
			}
			if (Spec.Kind == EAnimationReadbackKind::Deformer)
			{
				return TryCreateDeformerAsset(
					Profile, Request, Spec.Deformer, Package, OutError);
			}
			const FName Name(*AssetName(Request.GetTargetRef()));
			UObject* Created = nullptr;
			if (Profile.GetCapabilityId()
				== TEXT("animation.anim_blueprint.asset.create"))
			{
				USkeleton* const Skeleton = LoadObject<USkeleton>(
					nullptr, *Spec.SkeletonRef);
				UClass* const ParentClass = LoadObject<UClass>(
					nullptr, *Spec.ParentClassRef);
				if (!Skeleton || !ParentClass
					|| !ParentClass->IsChildOf(UAnimInstance::StaticClass()))
				{
					SetError(OutError,
						TEXT("graph_compound_animation_create_reference_invalid"),
						Request.GetTargetRef());
					return nullptr;
				}
				UAnimBlueprintFactory* const Factory =
					NewObject<UAnimBlueprintFactory>();
				Factory->TargetSkeleton = Skeleton;
				Factory->ParentClass = ParentClass;
				Created = Factory->FactoryCreateNew(
					UAnimBlueprint::StaticClass(), &Package, Name,
					RF_Public | RF_Standalone | RF_Transactional,
					nullptr, GWarn);
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.anim_blueprint.layer_interface.asset.create"))
			{
				UAnimLayerInterfaceFactory* const Factory =
					NewObject<UAnimLayerInterfaceFactory>();
				Created = Factory->FactoryCreateNew(
					UAnimBlueprint::StaticClass(), &Package, Name,
					RF_Public | RF_Standalone | RF_Transactional,
					nullptr, GWarn);
			}
			else
			{
				USkeleton* const Skeleton = LoadObject<USkeleton>(
					nullptr, *Spec.SkeletonRef);
				if (!Skeleton)
				{
					SetError(OutError,
						TEXT("graph_compound_animation_skeleton_ref_invalid"),
						Spec.SkeletonRef);
					return nullptr;
				}
				if (Spec.Dimension == 1)
				{
					UBlendSpaceFactory1D* const Factory =
						NewObject<UBlendSpaceFactory1D>();
					Factory->TargetSkeleton = Skeleton;
					Created = Factory->FactoryCreateNew(
						UBlendSpace1D::StaticClass(), &Package, Name,
						RF_Public | RF_Standalone | RF_Transactional,
						nullptr, GWarn);
				}
				else
				{
					UBlendSpaceFactoryNew* const Factory =
						NewObject<UBlendSpaceFactoryNew>();
					Factory->TargetSkeleton = Skeleton;
					Created = Factory->FactoryCreateNew(
						UBlendSpace::StaticClass(), &Package, Name,
						RF_Public | RF_Standalone | RF_Transactional,
						nullptr, GWarn);
				}
			}
			if (!Created || Created->GetClass()->GetPathName() != Spec.NativeClassPath)
			{
				SetError(OutError,
					TEXT("graph_compound_animation_factory_failed"),
					Request.GetTargetRef(), Spec.NativeClassPath);
				return nullptr;
			}
			if (UAnimBlueprint* const Blueprint = Cast<UAnimBlueprint>(Created))
			{
				FCompilerResultsLog CompilerLog;
				FKismetEditorUtilities::CompileBlueprint(
					Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
				if (CompilerLog.NumErrors != 0 || Blueprint->Status == BS_Error)
				{
					SetError(OutError,
						TEXT("animation_rig_graph_compile_incomplete"),
						Request.GetTargetRef(), FString::FromInt(CompilerLog.NumErrors));
					return nullptr;
				}
			}
			return Created;
		}

		bool TryApplyMutation(
			const FPcgAnimationGraphRouteProfile& Profile,
			UObject& Target,
			const FAnimationReadbackSpec& Spec,
			FAnimationRigGraphNativeHook& Hook,
			FString& OutRevision,
			FGraphCompoundRuntimeError& OutError)
		{
			if (Spec.Kind == EAnimationReadbackKind::RigIk)
			{
				return TryApplyRigIkMutation(
					Profile, Target, Spec.RigIk, Hook, OutRevision, OutError);
			}
			if (Spec.Kind == EAnimationReadbackKind::Deformer)
			{
				return TryApplyDeformerMutation(
					Profile, Target, Spec.Deformer, Hook, OutRevision, OutError);
			}
			FAnimationRigGraphNativeMutationReceipt Apply;
			FAnimationRigGraphBarrierReceipt Barrier;
			FAnimationRigGraphTopology Topology;
			FString Evidence;
			FAnimationRigGraphAdapterError NativeError;
			bool bApplied = false;
			if (Profile.GetCapabilityId()
				== TEXT("animation.anim_blueprint.graph.apply"))
			{
				UAnimBlueprint* const Blueprint = Cast<UAnimBlueprint>(&Target);
				if (Blueprint && Blueprint->TargetSkeleton)
				{
					bApplied = Hook.TryEnsureCompileAndCaptureAnimBlueprint(
						*Blueprint, *Blueprint->TargetSkeleton, Spec.GraphName,
						Apply, Barrier, Topology, OutRevision, Evidence, NativeError);
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.blend_space.definition.apply"))
			{
				UBlendSpace* const BlendSpace = Cast<UBlendSpace>(&Target);
				if (BlendSpace && BlendSpace->GetSkeleton())
				{
					bApplied = Hook.TryApplyAndCaptureBlendSpaceDefinition(
						*BlendSpace, *BlendSpace->GetSkeleton(), Spec.Axes,
						Spec.Samples, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.skeleton.definition.apply"))
			{
				if (USkeleton* const Skeleton = Cast<USkeleton>(&Target))
				{
					bApplied = Hook.TryApplyAndCaptureSkeletonDefinition(
						*Skeleton, Spec.Bones, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.skeleton.compatibility.apply"))
			{
				if (USkeleton* const Skeleton = Cast<USkeleton>(&Target))
				{
					bApplied = Hook.TryApplyAndCaptureSkeletonCompatibility(
						*Skeleton, Spec.Compatibility, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
			}
			else if (USkeleton* const Skeleton = Cast<USkeleton>(&Target))
			{
				bApplied = Hook.TryApplyAndCaptureSkeletonVirtualBone(
					*Skeleton, Spec.VirtualBone, Apply, Barrier, Topology,
					OutRevision, Evidence, NativeError);
			}
			if (!bApplied || !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
			{
				SetError(OutError,
					NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_animation_native_failed")
						: NativeError.GetCode(),
					Target.GetPathName(), NativeError.GetDetail());
				return false;
			}
			return true;
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			const FString& Revision,
			const FString& PersistenceReceiptId,
			const FString& ReadbackEvidenceId,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			if (TargetRef.IsEmpty()
				|| Revision.IsEmpty()
				|| PersistenceReceiptId.IsEmpty()
				|| ReadbackEvidenceId.IsEmpty())
			{
				SetError(
					OutError,
					TEXT("graph_compound_animation_lifecycle_receipt_invalid"),
					TargetRef);
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Object->SetStringField(TEXT("target_ref"), TargetRef);
			Object->SetStringField(TEXT("revision"), Revision);
			Object->SetBoolField(TEXT("mutation_applied"), true);
			Object->SetBoolField(TEXT("compile_settled"), true);
			Object->SetBoolField(TEXT("saved"), true);
			Object->SetBoolField(TEXT("reloaded"), true);
			Object->SetStringField(
				TEXT("persistence_receipt_id"), PersistenceReceiptId);
			Object->SetStringField(
				TEXT("independent_readback_evidence_id"), ReadbackEvidenceId);
			FString Canonical;
			FString JsonError;
			if (!Json::TryCanonicalize(Object, Canonical, JsonError))
			{
				SetError(OutError,
					TEXT("graph_compound_animation_result_invalid"),
					TargetRef, JsonError);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FAnimationProductionEnvironment final
			: public IGraphCompoundAnimationEnvironment
		{
		public:
			explicit FAnimationProductionEnvironment(
				const FGraphCompoundAnimationProductionConfig& Config)
				: JournalRoot(Config.DurableJournalRoot.IsEmpty()
					? FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/GraphCompoundDurable")
					: Config.DurableJournalRoot)
			{
			}

			bool TryCaptureSnapshot(
				const FPcgAnimationGraphRouteProfile& Profile,
				const FGraphCompoundAnimationTargetRequest& Request,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!IsW02CreateProfile(Profile)
					&& !IsW02MutationProfile(Profile)
					&& !IsDeformerProductionProfile(Profile)
					&& !IsRigIkCreateProfile(Profile)
					&& !IsRigIkMutationProfile(Profile))
				{
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}
				if (IsUnavailableConcreteDeformerProfile(Profile))
				{
					return TrySetUnavailableConcreteDeformerError(Profile, OutError);
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_animation_cancelled")
							: TEXT("graph_compound_animation_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				return (IsW02CreateProfile(Profile)
						|| IsDeformerCreateProfile(Profile)
						|| IsRigIkCreateProfile(Profile))
					? TryCreateAbsentSnapshot(
						Request, *Resources, OutSnapshot, OutError)
					: TryCaptureMutationSnapshot(
						Profile, Request, *Resources, OutSnapshot, OutError);
			}

			bool TryExecute(
				const FPcgAnimationGraphRouteProfile& Profile,
				const FGraphCompoundAnimationOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				const bool bCreate = IsW02CreateProfile(Profile)
					|| IsDeformerCreateProfile(Profile)
					|| IsRigIkCreateProfile(Profile);
				if (!bCreate && !IsW02MutationProfile(Profile)
					&& !IsDeformerMutationProfile(Profile)
					&& !IsRigIkMutationProfile(Profile))
				{
					SetError(OutError,
						TEXT("graph_compound_production_binding_unimplemented"),
						Profile.GetCapabilityId());
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled()
					|| Plan->GetDescriptorIdentity()
						!= Profile.GetExecutionContract().GetDescriptorIdentity()
					|| Plan->GetChangeSet().GetChanges().IsEmpty())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_animation_cancelled")
							: TEXT("graph_compound_animation_plan_invalid"),
						Request.GetTargetRef());
					return false;
				}
				for (const FPlannedChange& Change : Plan->GetChangeSet().GetChanges())
				{
					if (Change.GetIdentity().GetOwnerId() != Request.GetTargetRef())
					{
						SetError(OutError,
							TEXT("graph_compound_animation_plan_target_mismatch"),
							Change.GetIdentity().GetOwnerId());
						return false;
					}
				}

				FAnimationReadbackSpec Spec;
				if (!TryParseReadbackSpec(Profile, Request, Spec, OutError))
				{
					return false;
				}
				FUeAgentCanonicalSnapshotPtr Current;
				if (!(bCreate
						? TryCreateAbsentSnapshot(
							Request.GetTarget(), *Resources, Current, OutError)
						: TryCaptureMutationSnapshot(
							Profile, Request.GetTarget(), *Resources,
							Current, OutError)))
				{
					return false;
				}
				FPlanningPreflightError RevalidateError;
				if (!TryRevalidatePlan(
						Plan, Current.ToSharedRef(), CallerContext,
						*Resources, RevalidateError))
				{
					SetError(OutError, RevalidateError.GetCode(),
						RevalidateError.GetIdentityKey(),
						RevalidateError.GetDetail());
					return false;
				}

				const int64 FencingToken = ++LastFencingToken;
				const FString JournalOwner =
					TEXT("graph-compound-animation:") + Plan->GetRequestId();
				const FString TargetPackage = PackageName(Request.GetTargetRef());
				const FAssetDurableRequest DurableRequest(
					Plan,
					{FAssetDurableTarget(
						Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
						TargetPackage, EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Prepared;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest, DurableBackend, JournalRoot,
						*Resources, Prepared, DurableError)
					|| !Prepared.IsValid())
				{
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(), DurableError.GetDetail());
					return false;
				}

				UPackage* Package = nullptr;
				UObject* Target = nullptr;
				FString Revision;
				if (bCreate)
				{
					Package = CreatePackage(*TargetPackage);
					Target = Package
						? TryCreateAsset(Profile, Request, Spec, *Package, OutError)
						: nullptr;
					FAnimationRigGraphNativeMutationReceipt Apply;
					FAnimationRigGraphBarrierReceipt Barrier;
					FAnimationRigGraphAssetIdentity Identity;
					FString Evidence;
					FAnimationRigGraphAdapterError NativeError;
					if (!Target
						|| !NativeHook.TryCaptureOwnedAssetObject(
							*Package, Request.GetTargetKind(), *Target->GetClass(),
							*AssetName(Request.GetTargetRef()), *Target,
							Apply, Barrier, Identity, Revision, Evidence, NativeError)
						|| !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
					{
						Target = nullptr;
						Package = nullptr;
						FString RollbackDetail;
						TryRollback(Request.GetTargetRef(), true,
							Prepared.ToSharedRef(), DurableBackend,
							FencingToken, *Resources, RollbackDetail);
						if (OutError.Code.IsEmpty())
						{
							SetError(OutError,
								NativeError.GetCode().IsEmpty()
									? TEXT("graph_compound_animation_factory_failed")
									: NativeError.GetCode(),
								Request.GetTargetRef(), NativeError.GetDetail()
									+ TEXT(";") + RollbackDetail);
						}
						return false;
					}
					Target->SetFlags(RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(Target);
				}
				else
				{
					Target = LoadObject<UObject>(nullptr, *Request.GetTargetRef());
					Package = Target ? Target->GetOutermost() : nullptr;
					if (!Target || !TryApplyMutation(
							Profile, *Target, Spec, NativeHook, Revision, OutError))
					{
						Target = nullptr;
						Package = nullptr;
						FString RollbackDetail;
						TryRollback(Request.GetTargetRef(), false,
							Prepared.ToSharedRef(), DurableBackend,
							FencingToken, *Resources, RollbackDetail);
						OutError.Detail += TEXT(";") + RollbackDetail;
						return false;
					}
				}
				Package->MarkPackageDirty();
				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurability(
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources,
						DurableReceipt, DurableError))
				{
					Target = nullptr;
					Package = nullptr;
					FString RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate,
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(), DurableError.GetDetail()
							+ TEXT(";") + RollbackDetail);
					return false;
				}
				Package->SetDirtyFlag(false);
				Target = nullptr;
				Package = nullptr;
				const FAssetDurableSessionRef Persisted =
					PersistedSession(Prepared.ToSharedRef(), DurableReceipt);

				FUeAgentCanonicalValuePtr Expected;
				if (!TryCanonicalObject(
						ExpectedJson(Spec), *Resources, Expected, OutError))
				{
					return false;
				}
				TSharedPtr<FIndependentReadbackService> Readback;
				FVerificationError VerifyError;
				const TSharedRef<FAnimationReadbackOracle, ESPMode::ThreadSafe> Oracle =
					MakeShared<FAnimationReadbackOracle, ESPMode::ThreadSafe>(
						Request.GetTargetRef(), Spec);
				FIndependentReadbackOutcome Outcome;
				if (!FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback.IsValid()
					|| !Readback->TryVerify(
						FIndependentReadbackRequest(
							TEXT("graph-compound-animation:") + Plan->GetRequestId(),
							ReadbackOracleId, Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError)
					|| Outcome.GetStatus() != EVerificationStatus::Verified)
				{
					FString RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate, Persisted,
						DurableBackend, FencingToken, *Resources, RollbackDetail);
					SetError(OutError,
						VerifyError.GetCode().IsEmpty()
							? TEXT("graph_compound_animation_readback_failed")
							: VerifyError.GetCode(),
						Request.GetTargetRef(), Outcome.GetDiagnosticCode()
							+ TEXT(";") + RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(), JournalOwner, FencingToken);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					FString RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate, Persisted,
						DurableBackend, FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError.GetCode(),
						Request.GetTargetRef(), DurableError.GetDetail()
							+ TEXT(";") + RollbackDetail);
					return false;
				}
				return TrySerializeResult(
					Request.GetTargetRef(), Revision,
					DurableReceipt.GetJournalId(), Outcome.GetEvidenceId(),
					OutResult, OutError);
			}

		private:
			FString JournalRoot;
			FAnimationRigGraphNativeHook NativeHook;
			FPackageAssetDurableBackend DurableBackend;
			int64 LastFencingToken = 0;
		};
	}

	bool TryCreateAnimationGraphProductionEnvironment(
		const FGraphCompoundAnimationProductionConfig& Config,
		TSharedPtr<IGraphCompoundAnimationEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError)
	{
		OutEnvironment.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_production_config_invalid"),
				TEXT("animation_rig_graph"));
			return false;
		}
		OutEnvironment = MakeShared<FAnimationProductionEnvironment>(Config);
		return OutEnvironment.IsValid();
	}
}
