#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "HAL/FileManager.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
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
        void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            FString SubjectId = FString(), const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = MoveTemp(SubjectId);
            OutError.Append(Recovery);
        }

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
			bool bHasBlendDefinition = false;
			FName GraphName;
			FString CreateInputPointer;
			FString GraphNameInputPointer;
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
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				TargetRef, bNotifyAssetRegistry, Session, Backend, FencingToken,
				Resources, OutDetail);
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object),
					Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError, Error, FString());
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
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_invalid"), TEXT("target_ref does not identify the required Animation Blueprint target."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
				return false;
			}
			bool bTargetPresent = false;
			if (!HjsonWorkspace::DomainAuthoring::
				TryInspectObjectTargetPresence(
					Request.GetTargetRef(), bTargetPresent))
			{
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_invalid"), TEXT("target_ref does not identify the required Animation Blueprint target."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
				return false;
			}
			if (bTargetPresent)
			{
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_already_exists"), TEXT("target_ref already resolves to an existing animation asset."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
				OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
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
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_invalid"), TEXT("target_ref does not identify the required Animation Blueprint target."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_class_mismatch"), TEXT("target_ref resolves to an incompatible animation asset class."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_class_mismatch"), TEXT("target_ref resolves to an incompatible animation asset class."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
					SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_class_mismatch"), TEXT("target_ref resolves to an incompatible animation asset class."), TEXT("graph_compound"), FString()), Request.GetTargetRef());
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
					SetError(OutError, Entry.GetValidationDiagnostic(), Captured.SemanticId);
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
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			return true;
		}

		FString AnimationInputField(const FString& Pointer, const FString& Field)
		{
			return Pointer.IsEmpty() ? FString() : Pointer == TEXT("/") ? Field : Pointer + Field;
		}

		bool BlendValueFailure(const FUeAgentCanonicalValue* Value, const FString& Pointer,
			const FString& Expected, const FString& Code, FGraphCompoundRuntimeError& OutError)
		{
			TSharedPtr<FJsonValue> Actual;
			Contract::FDiagnostic ProjectionError;
			if (Value && !CanonicalJson::TryToJsonValue(*Value, Actual, ProjectionError))
			{
				OutError = MoveTemp(ProjectionError);
				OutError.Source.JsonPath = Pointer;
				return false;
			}
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
				MakeShared<FJsonValueString>(Expected), Actual, TEXT("input_decode"),
				TEXT("The BlendSpace value must satisfy: ") + Expected + TEXT("."));
			return false;
		}

		bool ParseBlendAxis(const FUeAgentCanonicalValue& Value,
			FAnimationRigGraphBlendAxisMutation& Axis, FGraphCompoundRuntimeError& OutError,
			const FString& Prefix)
		{
			auto Invalid = [&](const TCHAR* Field, const TCHAR* Expected) {
				return BlendValueFailure(FindField(Value, Field), AnimationInputField(Prefix, FString(TEXT("/")) + Field),
					Expected, TEXT("graph_compound_animation_blend_axis_invalid"), OutError);
			};
			FString AxisName;
			if (!TryIntegerField(Value, TEXT("axis_index"), Axis.AxisIndex)) return Invalid(TEXT("axis_index"), TEXT("a 32-bit integer"));
			if (!TryStringField(Value, TEXT("axis_name"), AxisName)) return Invalid(TEXT("axis_name"), TEXT("a non-empty string"));
			if (!TryNumberField(Value, TEXT("minimum"), Axis.Minimum)) return Invalid(TEXT("minimum"), TEXT("a finite number"));
			if (!TryNumberField(Value, TEXT("maximum"), Axis.Maximum)) return Invalid(TEXT("maximum"), TEXT("a finite number"));
			if (!TryIntegerField(Value, TEXT("grid_divisions"), Axis.GridDivisions)) return Invalid(TEXT("grid_divisions"), TEXT("a 32-bit integer"));
			Axis.AxisName = *AxisName;
			Axis.InputPointer = Prefix;
			return true;
		}

		bool ParseBlendSample(const FUeAgentCanonicalValue& Value, EAnimationRigGraphMutationIntent Intent,
			FAnimationRigGraphBlendSampleMutation& Sample, FGraphCompoundRuntimeError& OutError,
			const FString& Prefix)
		{
			FString AnimationRef;
			if (!TryStringField(Value, TEXT("animation_ref"), AnimationRef))
				return BlendValueFailure(FindField(Value, TEXT("animation_ref")), AnimationInputField(Prefix, TEXT("/animation_ref")), TEXT("a non-empty animation reference"), TEXT("graph_compound_animation_blend_sample_invalid"), OutError);
			const auto* Coordinates = FindField(Value, TEXT("coordinates"));
			if (!Coordinates || Coordinates->GetKind() != EUeAgentValueKind::Array
				|| Coordinates->GetArrayItems().IsEmpty() || Coordinates->GetArrayItems().Num() > 3)
				return BlendValueFailure(Coordinates, AnimationInputField(Prefix, TEXT("/coordinates")), TEXT("an array of one to three finite numbers"), TEXT("graph_compound_animation_blend_sample_invalid"), OutError);
			Sample.Animation = LoadObject<UAnimSequence>(nullptr, *AnimationRef);
			for (int32 Index = 0; Index < Coordinates->GetArrayItems().Num(); ++Index)
			{
				const auto& Coordinate = Coordinates->GetArrayItems()[Index].Get();
				if ((Coordinate.GetKind() != EUeAgentValueKind::Integer && Coordinate.GetKind() != EUeAgentValueKind::Decimal)
					|| !FMath::IsFinite(FCString::Atod(*Coordinate.GetLexeme())))
					return BlendValueFailure(&Coordinate, AnimationInputField(Prefix, TEXT("/coordinates/") + ::LexToString(Index)), TEXT("a finite number"), TEXT("graph_compound_animation_blend_coordinate_invalid"), OutError);
				Sample.Coordinates[Index] = FCString::Atod(*Coordinate.GetLexeme());
			}
			Sample.Intent = Intent;
			Sample.InputPointer = Prefix;
			Sample.AnimationRef = AnimationRef;
			return true;
		}

		bool ParseBlendCreationDefinition(const FUeAgentCanonicalValue& Value,
			FAnimationReadbackSpec& Spec, FGraphCompoundRuntimeError& OutError)
		{
			const FString& InputPointer = Spec.CreateInputPointer;
			const auto* Axes = FindField(Value, TEXT("axes"));
			const auto* Samples = FindField(Value, TEXT("samples"));
			Spec.bHasBlendDefinition = Axes || Samples;
			if (Axes && Axes->GetKind() != EUeAgentValueKind::Array)
				return BlendValueFailure(Axes, AnimationInputField(InputPointer, TEXT("/axes")), TEXT("an array"), TEXT("graph_compound_animation_blend_definition_invalid"), OutError);
			if (Samples && Samples->GetKind() != EUeAgentValueKind::Array)
				return BlendValueFailure(Samples, AnimationInputField(InputPointer, TEXT("/samples")), TEXT("an array"), TEXT("graph_compound_animation_blend_definition_invalid"), OutError);
			if (Axes) for (int32 Index = 0; Index < Axes->GetArrayItems().Num(); ++Index) {
				FAnimationRigGraphBlendAxisMutation Axis;
				if (!ParseBlendAxis(*Axes->GetArrayItems()[Index], Axis, OutError, AnimationInputField(InputPointer, TEXT("/axes/") + ::LexToString(Index)))) return false;
				Spec.Axes.Add(Axis);
			}
			if (Samples) for (int32 Index = 0; Index < Samples->GetArrayItems().Num(); ++Index) {
				const auto& Item = Samples->GetArrayItems()[Index];
				const FString Prefix = AnimationInputField(InputPointer, TEXT("/samples/") + ::LexToString(Index));
				FString Text;
				EAnimationRigGraphMutationIntent Intent;
				TryStringField(*Item, TEXT("intent"), Text);
				if (Text == TEXT("create")) Intent = EAnimationRigGraphMutationIntent::Create;
				else if (Text == TEXT("update")) Intent = EAnimationRigGraphMutationIntent::Update;
				else if (Text == TEXT("replace")) Intent = EAnimationRigGraphMutationIntent::Replace;
				else if (Text == TEXT("delete")) Intent = EAnimationRigGraphMutationIntent::Delete;
				else return BlendValueFailure(FindField(*Item, TEXT("intent")), AnimationInputField(Prefix, TEXT("/intent")), TEXT("create, update, replace or delete"), TEXT("graph_compound_animation_blend_sample_intent_invalid"), OutError);
				FAnimationRigGraphBlendSampleMutation Sample;
				if (!ParseBlendSample(*Item, Intent, Sample, OutError, Prefix)) return false;
				Spec.Samples.Add(Sample);
			}
			return true;
		}

		FGraphCompoundRuntimeError AnimationInputFailure(
			const FGraphCompoundAnimationOperationRequest& Request, const FGraphCompoundCanonicalEdit& Edit,
			const TCHAR* Code, const FString& Reason, const FString& Field,
			const FUeAgentCanonicalValue* Actual, const FString& Expected)
		{
			auto Error = FGraphCompoundRuntimeError::Failure(Code, Reason, TEXT("input_decode"));
			Error.TargetPath = Request.GetTargetRef();
			Error.SubjectId = Edit.GetIdentity().GetSemanticId();
			Error.ActualValuePresent = Actual != nullptr;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (Actual)
			{
				FDiagnostic ProjectionError;
				if (!CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError)) Error.Append(ProjectionError);
			}
			if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
			{
				Error.Source.JsonPath = Field.IsEmpty() ? *Pointer : *Pointer == TEXT("/") ? Field : *Pointer + Field;
				Error.SourceKind = TEXT("related_input");
			}
			return Error;
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
				OutError = FGraphCompoundRuntimeError::Failure(TEXT("graph_compound_animation_plan_invalid"),
					TEXT("The parsed animation operation contains no edits."), TEXT("graph_compound"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one edit"));
				return false;
			}
			const FString& Capability = Profile.GetCapabilityId();
			if (IsW02CreateProfile(Profile))
			{
				OutSpec.Kind = EAnimationReadbackKind::AssetCreate;
				if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
					OutSpec.CreateInputPointer = *Pointer;
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				if (!Value.IsValid())
				{
					OutError = AnimationInputFailure(Request, Edits[0], TEXT("graph_compound_animation_create_value_missing"),
						TEXT("Animation asset creation requires a proposed value."), FString(), nullptr, TEXT("an asset creation value"));
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
						const TCHAR* Field = OutSpec.SkeletonRef.IsEmpty() ? TEXT("skeleton_ref") : TEXT("parent_class_ref");
						OutError = AnimationInputFailure(Request, Edits[0], TEXT("graph_compound_animation_create_reference_missing"),
							FString::Printf(TEXT("Animation Blueprint creation requires a non-empty '%s' reference."), Field),
							FString(TEXT("/")) + Field, FindField(*Value, Field), TEXT("a non-empty asset or class reference"));
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
					if (!ParseBlendCreationDefinition(*Value, OutSpec, OutError)) return false;
					OutSpec.NativeClassPath = OutSpec.Dimension == 1
						? UBlendSpace1D::StaticClass()->GetPathName()
						: UBlendSpace::StaticClass()->GetPathName();
					if (OutSpec.SkeletonRef.IsEmpty()
						|| (OutSpec.Dimension != 1 && OutSpec.Dimension != 2))
					{
						const bool bSkeletonMissing = OutSpec.SkeletonRef.IsEmpty();
						const TCHAR* Field = bSkeletonMissing ? TEXT("skeleton_ref") : TEXT("dimension");
						OutError = AnimationInputFailure(Request, Edits[0], TEXT("graph_compound_animation_blend_create_invalid"),
							bSkeletonMissing ? TEXT("BlendSpace creation requires a non-empty skeleton reference.")
								: TEXT("BlendSpace dimension must be 1 or 2."),
							FString(TEXT("/")) + Field, FindField(*Value, Field), bSkeletonMissing ? TEXT("a non-empty skeleton reference") : TEXT("1 or 2"));
						return false;
					}
				}
				return true;
			}

			if (Capability == TEXT("animation.anim_blueprint.graph.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::AnimBlueprintGraph;
				if (Edits.Num() != 1)
				{
					OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_graph_plan_invalid"),
						TEXT("An animation graph operation requires exactly one edit."), TEXT("input_decode"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.GetTargetRef();
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Edits.Num());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					return false;
				}
				const auto& Value = Edits[0].GetProposedAfter();
				if (!Value || !TryStringField(*Value, TEXT("graph_name"), OutSpec.NativeClassPath))
				{
					OutError = AnimationInputFailure(Request, Edits[0], TEXT("graph_compound_animation_graph_plan_invalid"),
						Value ? TEXT("The animation graph name must be a non-empty string.") : TEXT("The animation graph edit requires a proposed value."),
						Value ? TEXT("/graph_name") : FString(), Value ? FindField(*Value, TEXT("graph_name")) : nullptr,
						Value ? TEXT("a non-empty graph name") : TEXT("an animation graph value"));
					return false;
				}
				if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
					OutSpec.GraphNameInputPointer = AnimationInputField(*Pointer, TEXT("/graph_name"));
				OutSpec.GraphName = *OutSpec.NativeClassPath;
				OutSpec.NativeClassPath = UAnimBlueprint::StaticClass()->GetPathName();
				return true;
			}

			if (Capability == TEXT("animation.blend_space.definition.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::BlendSpaceDefinition;
				OutSpec.NativeClassPath = UBlendSpace::StaticClass()->GetPathName();
				for (const FGraphCompoundCanonicalEdit& Edit : Edits) {
					ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
					const auto& Value = Edit.GetProposedAfter();
					const FString* InputPointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId());
					const FString Prefix = InputPointer ? *InputPointer : FString();
					if (!Value) {
						OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_blend_plan_invalid"),
							TEXT("The BlendSpace edit requires a proposed axis or sample value."), FString(), nullptr, TEXT("an axis or sample value"));
						return false;
					}
					if (Edit.GetIdentity().GetSemanticId().StartsWith(TEXT("item:blend_axis:"))) {
						FAnimationRigGraphBlendAxisMutation Axis;
						if (!ParseBlendAxis(*Value, Axis, OutError, Prefix)) return false;
						OutSpec.Axes.Add(Axis);
					} else {
						FAnimationRigGraphBlendSampleMutation Sample;
						if (!ParseBlendSample(*Value, NativeIntent(Edit.GetIntent()), Sample, OutError, Prefix)) return false;
						OutSpec.Samples.Add(Sample);
					}
				}
				if (OutSpec.Axes.IsEmpty())
				{
					OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_blend_axes_missing"),
						TEXT("The BlendSpace definition contains no axis edits."), TEXT("input_decode"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.GetTargetRef();
					OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one axis edit"));
					return false;
				}
				return true;
			}

			if (Capability == TEXT("animation.skeleton.definition.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::SkeletonDefinition;
				OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
					FAnimationRigGraphSkeletonBoneMutation Bone;
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
						Bone.InputPointer = *Pointer;
					FString BoneName;
					FString ParentName;
					if (!Edit.GetProposedAfter().IsValid())
					{
						OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_skeleton_bone_invalid"),
							TEXT("A skeleton bone edit requires a definition value."), FString(), nullptr, TEXT("bone definition object"));
						return false;
					}
					if (!TryStringField(*Edit.GetProposedAfter(), TEXT("bone_name"), BoneName))
					{
						OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_skeleton_bone_invalid"),
							TEXT("A skeleton bone definition requires a non-empty bone_name string."), TEXT("/bone_name"),
							FindField(*Edit.GetProposedAfter(), TEXT("bone_name")), TEXT("non-empty string"));
						return false;
					}
					TryStringField(
						*Edit.GetProposedAfter(), TEXT("parent_bone"), ParentName);
					const auto* Parent = FindField(*Edit.GetProposedAfter(), TEXT("parent_bone"));
					Bone.bHasParentBoneInput = Parent && Parent->GetKind() == EUeAgentValueKind::String;
					Bone.BoneName = *BoneName;
					Bone.ParentBone = *ParentName;
					Bone.Intent = NativeIntent(Edit.GetIntent());
					OutSpec.Bones.Add(Bone);
				}
				return true;
			}

			if (Capability == TEXT("animation.skeleton.compatibility.apply"))
			{
				OutSpec.Kind = EAnimationReadbackKind::SkeletonCompatibility;
				OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
				const FString Prefix = TEXT("binding:compatible_skeleton:");
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
					const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
					if (!SemanticId.StartsWith(Prefix))
					{
						OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_compatibility_identity_invalid"),
							TEXT("The edit does not identify a compatible skeleton binding."), FString(), nullptr, TEXT("compatible skeleton binding"));
						OutError.ActualEvidence = MakeShared<FJsonValueString>(SemanticId);
						OutError.ActualValuePresent.Reset();
						return false;
					}
					FAnimationRigGraphSkeletonCompatibilityMutation Mutation;
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Mutation.InputPointer = *Pointer;
					Mutation.CompatibleSkeleton = LoadObject<USkeleton>(
						nullptr, *SemanticId.Mid(Prefix.Len()));
					Mutation.Intent = NativeIntent(Edit.GetIntent());
					if (!Mutation.CompatibleSkeleton)
					{
						OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_compatible_skeleton_invalid"),
							TEXT("The compatible skeleton reference could not be loaded as a Skeleton asset."), FString(), nullptr,
							USkeleton::StaticClass()->GetPathName());
						OutError.ActualValuePresent = true;
						OutError.ActualValue = MakeShared<FJsonValueString>(SemanticId.Mid(Prefix.Len()));
						return false;
					}
					OutSpec.Compatibility.Add(Mutation);
				}
				return true;
			}

			OutSpec.Kind = EAnimationReadbackKind::SkeletonVirtualBone;
			OutSpec.NativeClassPath = USkeleton::StaticClass()->GetPathName();
			const FString Prefix = TEXT("hierarchy:virtual_bone:");
			if (Edits.Num() != 1)
			{
				OutError = FGraphCompoundRuntimeError::Failure(TEXT("graph_compound_animation_virtual_bone_invalid"),
					TEXT("A virtual bone operation requires exactly one edit."), TEXT("graph_compound"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Edits.Num());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				return false;
			}
			const FGraphCompoundCanonicalEdit& Edit = Edits[0];
			if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
				OutSpec.VirtualBone.InputPointer = *Pointer;
			if (!Edit.GetIdentity().GetSemanticId().StartsWith(Prefix))
			{
				OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_virtual_bone_invalid"),
					TEXT("The edit does not identify a virtual bone."), FString(), nullptr, TEXT("virtual bone identity"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Edit.GetIdentity().GetSemanticId());
				OutError.ActualValuePresent.Reset();
				return false;
			}
			if (!Edit.GetProposedAfter().IsValid())
			{
				OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_virtual_bone_invalid"),
					TEXT("A virtual bone edit requires a definition value."), FString(), nullptr, TEXT("virtual bone definition object"));
				return false;
			}
			FString SourceBone;
			FString TargetBone;
			if (!TryStringField(*Edit.GetProposedAfter(), TEXT("source_bone"), SourceBone))
			{
				OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_virtual_bone_invalid"),
					TEXT("A virtual bone requires a non-empty source_bone string."), TEXT("/source_bone"),
					FindField(*Edit.GetProposedAfter(), TEXT("source_bone")), TEXT("non-empty string"));
				return false;
			}
			if (!TryStringField(*Edit.GetProposedAfter(), TEXT("target_bone"), TargetBone))
			{
				OutError = AnimationInputFailure(Request, Edit, TEXT("graph_compound_animation_virtual_bone_invalid"),
					TEXT("A virtual bone requires a non-empty target_bone string."), TEXT("/target_bone"),
					FindField(*Edit.GetProposedAfter(), TEXT("target_bone")), TEXT("non-empty string"));
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
			case EAnimationReadbackKind::AnimBlueprintGraph:
				Root->SetStringField(TEXT("graph_name"), Spec.GraphName.ToString());
				break;
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
				if (!Spec.bHasBlendDefinition) break;
				[[fallthrough]];
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = FDiagnostic::Failure(TEXT("animation_readback_target_class_mismatch"),
					!Target ? TEXT("The animation readback target could not be loaded.")
						: TEXT("The animation readback target has a different native class than the requested asset."), TEXT("readback"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = TargetRef;
				if (Target) OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
				else OutError.ActualValue = MakeShared<FJsonValueNull>();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Spec.NativeClassPath);
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
				if (!Spec.bHasBlendDefinition) return true;
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
					OutError = FDiagnostic::Failure(TEXT("animation_readback_graph_missing"),
						FString::Printf(TEXT("Animation graph '%s' is missing from the reloaded Blueprint."), *Spec.GraphName.ToString()), TEXT("readback"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = TargetRef;
					OutError.ActualValue = MakeShared<FJsonValueNull>();
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Spec.GraphName.ToString());
					if (!Spec.GraphNameInputPointer.IsEmpty()) {
						OutError.Source.JsonPath = Spec.GraphNameInputPointer;
						OutError.SourceKind = TEXT("related_input");
					}
					return false;
				}
				OutRoot->SetStringField(TEXT("graph_name"), ActualGraph->GetName());
				return true;
			}
			if (Spec.Kind == EAnimationReadbackKind::BlendSpaceDefinition || Spec.bHasBlendDefinition)
			{
				const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Target);
				if (!BlendSpace)
				{
					OutError = FDiagnostic::Failure(TEXT("animation_readback_blend_missing"),
						TEXT("BlendSpace definition readback requires a BlendSpace target."), TEXT("readback"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = TargetRef;
					OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UBlendSpace::StaticClass()->GetPathName());
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
				OutError = FDiagnostic::Failure(TEXT("animation_readback_skeleton_missing"),
					TEXT("Skeleton readback requires a Skeleton asset."), TEXT("readback"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = TargetRef;
				OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName());
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
				UE::UAI::Contract::FDiagnostic UnloadDetail;
				if (Request.GetOracleId() != ReadbackOracleId || Request.GetSubjectRef() != TargetRef)
				{
					const bool bOracleMatches = Request.GetOracleId() == ReadbackOracleId;
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_readback_unload_failed"),
						!bOracleMatches ? TEXT("The readback request selects a different oracle.") : TEXT("The readback request targets a different animation asset."));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = TargetRef;
					OutError.PropertyPath = !bOracleMatches ? TEXT("oracle_matches") : TEXT("subject_ref");
					OutError.ActualValue = !bOracleMatches ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
						: MakeShared<FJsonValueString>(Request.GetSubjectRef());
					OutError.ExpectedValue = !bOracleMatches ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
						: MakeShared<FJsonValueString>(TargetRef);
					return false;
				}
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(TargetRef, UnloadDetail))
				{
					OutError = MoveTemp(UnloadDetail);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				UE::UAI::Contract::FDiagnostic ReadError;
				if (!TryBuildActualJson(TargetRef, Spec, Actual, ReadError))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(ReadError, TEXT("animation-readback:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr ActualValue;
				FGraphCompoundRuntimeError CanonicalError;
				if (!TryCanonicalObject(
						Actual, Resources, ActualValue, CanonicalError))
				{
					OutError = MoveTemp(CanonicalError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
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
				USkeleton* const Skeleton = LoadObject<USkeleton>(nullptr, *Spec.SkeletonRef);
				UClass* const ParentClass = LoadObject<UClass>(nullptr, *Spec.ParentClassRef);
				if (!Skeleton || !ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()))
				{
					OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_create_reference_invalid"),
						!Skeleton ? TEXT("The animation Blueprint skeleton reference could not be loaded as a Skeleton asset.")
							: !ParentClass ? TEXT("The animation Blueprint parent class reference could not be loaded.")
							: TEXT("The animation Blueprint parent class must derive from AnimInstance."), TEXT("native_graph"));
					OutError.TargetPath = Request.GetTargetRef();
					OutError.ActualValue = MakeShared<FJsonValueString>(!Skeleton ? Spec.SkeletonRef : Spec.ParentClassRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(!Skeleton ? USkeleton::StaticClass()->GetPathName()
						: FString(TEXT("a class derived from ")) + UAnimInstance::StaticClass()->GetPathName());
					OutError.Source.JsonPath = AnimationInputField(Spec.CreateInputPointer, !Skeleton ? TEXT("/skeleton_ref") : TEXT("/parent_class_ref"));
					OutError.SourceKind = OutError.Source.JsonPath.IsEmpty() ? TEXT("native_result") : TEXT("related_input");
					return nullptr;
				}
				UAnimBlueprintFactory* const Factory = NewObject<UAnimBlueprintFactory>();
				Factory->TargetSkeleton = Skeleton;
				Factory->ParentClass = ParentClass;
				Created = Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), &Package, Name,
					RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
			}
			else if (Profile.GetCapabilityId() == TEXT("animation.anim_blueprint.layer_interface.asset.create"))
			{
				UAnimLayerInterfaceFactory* const Factory = NewObject<UAnimLayerInterfaceFactory>();
				Created = Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), &Package, Name,
					RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
			}
			else
			{
				auto Seed = MakeShared<FJsonObject>();
				Seed->SetStringField(TEXT("skeleton"), Spec.SkeletonRef);
				Seed->SetNumberField(TEXT("dimension"), Spec.Dimension);
				UE::UAI::Contract::FDiagnostic Error;
				UeAgentAnimationAuthoring::FFactorySeedDiagnosticSource SeedSource;
				SeedSource.TargetPath = Request.GetTargetRef();
				SeedSource.Skeleton.JsonPath = AnimationInputField(Spec.CreateInputPointer, TEXT("/skeleton_ref"));
				SeedSource.Dimension.JsonPath = AnimationInputField(Spec.CreateInputPointer, TEXT("/dimension"));
				Created = UeAgentAnimationAuthoring::CreateFactorySeed(
					UeAgentAnimationAuthoring::EAssetKind::BlendSpace, &Package, Name,
					RF_Public | RF_Standalone | RF_Transactional, Seed, Error, SeedSource);
				if (!Created)
				{
					SetError(OutError, MoveTemp(Error), Request.GetTargetRef());
					return nullptr;
				}
			}
			if (!Created || Created->GetClass()->GetPathName() != Spec.NativeClassPath)
			{
				OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_factory_failed"),
					!Created ? TEXT("The Unreal animation factory did not create an asset.")
						: TEXT("The Unreal animation factory returned an unexpected asset class."), TEXT("native_graph"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.PropertyPath = TEXT("asset_class");
				OutError.ActualValue = Created ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Created->GetClass()->GetPathName()))
					: MakeShared<FJsonValueNull>();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Spec.NativeClassPath);
				return nullptr;
			}
			if (UAnimBlueprint* const Blueprint = Cast<UAnimBlueprint>(Created))
			{
				FCompilerResultsLog CompilerLog;
				FKismetEditorUtilities::CompileBlueprint(
					Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
				if (CompilerLog.NumErrors != 0 || Blueprint->Status == BS_Error)
				{
					OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("animation_rig_graph_compile_incomplete"),
						Request.GetTargetRef(), CompilerLog, Blueprint);
					OutError.SourceKind = TEXT("native_result");
					return nullptr;
				}
			}
			return Created;
		}

		bool TryApplyMutation(
			const FPcgAnimationGraphRouteProfile& Profile,
			UObject& Target,
			FAnimationReadbackSpec& Spec,
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
			UClass* ExpectedMutationClass = nullptr;
			if (Profile.GetCapabilityId()
				== TEXT("animation.anim_blueprint.graph.apply"))
			{
				ExpectedMutationClass = UAnimBlueprint::StaticClass();
				UAnimBlueprint* const Blueprint = Cast<UAnimBlueprint>(&Target);
				if (Blueprint && Blueprint->TargetSkeleton)
				{
					bApplied = Hook.TryEnsureCompileAndCaptureAnimBlueprint(
						*Blueprint, *Blueprint->TargetSkeleton, Spec.GraphName,
						Apply, Barrier, Topology, OutRevision, Evidence, NativeError, Spec.GraphNameInputPointer);
				}
				else if (Blueprint) {
					NativeError = FDiagnostic::Failure(TEXT("graph_compound_animation_skeleton_missing"),
						TEXT("The animation Blueprint has no target skeleton for graph editing."), TEXT("native_graph"));
					NativeError.SourceKind = TEXT("native_result");
					NativeError.ActualValue = MakeShared<FJsonValueNull>();
					NativeError.ExpectedValue = MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName());
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.blend_space.definition.apply") || Spec.bHasBlendDefinition)
			{
				ExpectedMutationClass = UBlendSpace::StaticClass();
				UBlendSpace* const BlendSpace = Cast<UBlendSpace>(&Target);
				if (BlendSpace && BlendSpace->GetSkeleton())
				{
					auto Axes = Spec.Axes;
					if (Axes.IsEmpty() && Spec.bHasBlendDefinition) {
						for (int32 Index = 0; Index < Spec.Dimension; ++Index) {
							const auto& Current = BlendSpace->GetBlendParameter(Index);
							FAnimationRigGraphBlendAxisMutation Axis;
							Axis.AxisIndex = Index; Axis.AxisName = *Current.DisplayName;
							Axis.Minimum = Current.Min; Axis.Maximum = Current.Max; Axis.GridDivisions = Current.GridNum;
							Axes.Add(Axis);
						}
					}
					bApplied = Hook.TryApplyAndCaptureBlendSpaceDefinition(
						*BlendSpace, *BlendSpace->GetSkeleton(), Axes,
						Spec.Samples, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
				else if (BlendSpace) {
					NativeError = FDiagnostic::Failure(TEXT("graph_compound_animation_skeleton_missing"),
						TEXT("The BlendSpace has no skeleton for definition editing."), TEXT("native_graph"));
					NativeError.SourceKind = TEXT("native_result");
					NativeError.ActualValue = MakeShared<FJsonValueNull>();
					NativeError.ExpectedValue = MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName());
				}
			}
			else if (Profile.GetCapabilityId()
				== TEXT("animation.skeleton.definition.apply"))
			{
				ExpectedMutationClass = USkeleton::StaticClass();
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
				ExpectedMutationClass = USkeleton::StaticClass();
				if (USkeleton* const Skeleton = Cast<USkeleton>(&Target))
				{
					bApplied = Hook.TryApplyAndCaptureSkeletonCompatibility(
						*Skeleton, Spec.Compatibility, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
			}
			else
			{
				ExpectedMutationClass = USkeleton::StaticClass();
				if (USkeleton* const Skeleton = Cast<USkeleton>(&Target))
				{
					bApplied = Hook.TryApplyAndCaptureSkeletonVirtualBone(
						*Skeleton, Spec.VirtualBone, Apply, Barrier, Topology,
						OutRevision, Evidence, NativeError);
				}
			}
			if (!bApplied || !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
			{
				if (NativeError.IsValid()) OutError = NativeError;
				else
				{
					const bool bWrongClass = ExpectedMutationClass && !Target.IsA(ExpectedMutationClass);
					OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_native_failed"),
						bWrongClass ? TEXT("The animation mutation target has an incompatible asset class.")
							: !bApplied ? TEXT("The native animation mutation or its topology capture did not complete.")
							: !Barrier.IsComplete() ? TEXT("The native animation refresh or compile barrier did not complete.")
							: TEXT("The native animation barrier reported errors."), TEXT("native_graph"));
					OutError.SourceKind = TEXT("native_result");
					if (bWrongClass)
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedMutationClass->GetPathName());
					}
					else if (bApplied && Barrier.IsComplete())
					{
						OutError.ActualValue = MakeShared<FJsonValueNumber>(Barrier.GetErrorCount());
						OutError.ExpectedValue = MakeShared<FJsonValueNumber>(0);
					}
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName();
				return false;
			}
			return true;
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			if (TargetRef.IsEmpty())
			{
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_invalid"), TEXT("target_ref does not identify the required Animation Blueprint target."), TEXT("graph_compound"), FString()), TargetRef);
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Object->SetStringField(TEXT("target_ref"), TargetRef);
			Object->SetBoolField(TEXT("mutation_applied"), true);
			Object->SetBoolField(TEXT("compile_settled"), true);
			Object->SetBoolField(TEXT("saved"), true);
			Object->SetBoolField(TEXT("reloaded"), true);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryCanonicalize(Object, Canonical, JsonError))
			{
				SetError(OutError, JsonError, TargetRef);
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_production_binding_unimplemented"), TEXT("No animation production binding supports the requested graph capability."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("capability");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Profile.GetCapabilityId());
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation create or mutation capability"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (IsUnavailableConcreteDeformerProfile(Profile))
				{
					return TrySetUnavailableConcreteDeformerError(Profile, OutError);
				}
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(bCancelled ? TEXT("graph_compound_animation_cancelled") : TEXT("graph_compound_animation_thread_invalid"), bCancelled ? TEXT("Animation graph capture was cancelled.") : TEXT("Animation graph capture requires the game thread."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("execution_context");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(bCancelled ? TEXT("cancelled") : TEXT("not_game_thread"));
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread with no cancellation"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
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
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_production_binding_unimplemented"), TEXT("No animation production binding supports the requested graph capability."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("capability");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Profile.GetCapabilityId());
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation create or mutation capability"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Profile.GetCapabilityId());
					return false;
				}
				if (!IsInGameThread() || Cancellation->IsCancelled()
					|| Plan->GetDescriptorIdentity()
						!= Profile.GetExecutionContract().GetDescriptorIdentity()
					|| Plan->GetChangeSet().GetChanges().IsEmpty())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("game_thread"), IsInGameThread());
					Actual->SetBoolField(TEXT("cancelled"), bCancelled);
					Actual->SetBoolField(TEXT("descriptor_matches"), Plan->GetDescriptorIdentity() == Profile.GetExecutionContract().GetDescriptorIdentity());
					Actual->SetNumberField(TEXT("change_count"), Plan->GetChangeSet().GetChanges().Num());
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(bCancelled ? TEXT("graph_compound_animation_cancelled") : TEXT("graph_compound_animation_plan_invalid"), bCancelled ? TEXT("Animation graph execution was cancelled.") : TEXT("Animation graph execution requires the game thread, a matching descriptor, and at least one planned change."), TEXT("graph_compound"));
					Diagnostic.PropertyPath = TEXT("plan");
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread=true, cancelled=false, descriptor_matches=true, change_count>0"));
					Diagnostic.SourceKind = TEXT("native_result");
					Diagnostic.TargetPath = Request.GetTargetRef();
					SetError(OutError, MoveTemp(Diagnostic), Request.GetTargetRef());
					return false;
				}
				for (const FPlannedChange& Change : Plan->GetChangeSet().GetChanges())
				{
					if (Change.GetIdentity().GetOwnerId() != Request.GetTargetRef())
					{
						auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_plan_target_mismatch"), TEXT("A planned animation change targets a different owner than the execution request."), TEXT("graph_compound"));
						Diagnostic.PropertyPath = TEXT("owner_matches_target");
						Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						Diagnostic.SourceKind = TEXT("native_result");
						Diagnostic.TargetPath = Request.GetTargetRef();
						SetError(OutError, MoveTemp(Diagnostic), Change.GetIdentity().GetSemanticId());
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Change.GetIdentity().GetOwnerId());
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
					OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
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
						TargetPackage, EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Prepared;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest, DurableBackend, JournalRoot,
						*Resources, Prepared, DurableError))
				{
					OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
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
					if (!Package)
					{
						OutError = FDiagnostic::Failure(TEXT("graph_compound_animation_factory_failed"),
							TEXT("The animation asset destination package could not be created."), TEXT("native_graph"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Request.GetTargetRef();
						OutError.PropertyPath = TEXT("package_created");
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					FAnimationRigGraphNativeMutationReceipt Apply;
					FAnimationRigGraphBarrierReceipt Barrier;
					FAnimationRigGraphAssetIdentity Identity;
					FString Evidence;
					FAnimationRigGraphAdapterError NativeError;
					bool bCaptured = Target != nullptr;
					if (bCaptured)
					{
						bCaptured = NativeHook.TryCaptureOwnedAssetObject(
							*Package, Request.GetTargetKind(), *Target->GetClass(),
							*AssetName(Request.GetTargetRef()), *Target,
							Apply, Barrier, Identity, Revision, Evidence, NativeError);
						if (!bCaptured) OutError = MoveTemp(NativeError);
					}
					if (!bCaptured || (Spec.bHasBlendDefinition && !TryApplyMutation(
						Profile, *Target, Spec, NativeHook, Revision, OutError)))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
						if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
						Target = nullptr;
						Package = nullptr;
						UE::UAI::Contract::FDiagnostic RollbackDetail;
						TryRollback(Request.GetTargetRef(), true,
							Prepared.ToSharedRef(), DurableBackend,
							FencingToken, *Resources, RollbackDetail);
						OutError.Append(RollbackDetail);
						return false;
					}
					Target->SetFlags(RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(Target);
				}
				else
				{
					Target = LoadObject<UObject>(nullptr, *Request.GetTargetRef());
					Package = Target ? Target->GetOutermost() : nullptr;
					if (!Target)
					{
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_target_unavailable"),
							TEXT("The animation mutation target could not be loaded."), TEXT("native_graph"));
						OutError.TargetPath = Request.GetTargetRef();
						OutError.SourceKind = TEXT("native_result");
						OutError.ActualValuePresent = false;
					}
					if (!Target || !TryApplyMutation(
							Profile, *Target, Spec, NativeHook, Revision, OutError))
					{
						Target = nullptr;
						Package = nullptr;
						UE::UAI::Contract::FDiagnostic RollbackDetail;
						TryRollback(Request.GetTargetRef(), false,
							Prepared.ToSharedRef(), DurableBackend,
							FencingToken, *Resources, RollbackDetail);
						OutError.Append(RollbackDetail);
						return false;
					}
				}
				Package->MarkPackageDirty();
				FDurableReceipt DurableReceipt;
				if (!TryPersistAssetDurabilityForVerification(
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources,
						DurableReceipt, DurableError))
				{
					Target = nullptr;
					Package = nullptr;
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate,
						Prepared.ToSharedRef(), DurableBackend,
						FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
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
					|| !Readback->TryObserve(
						FIndependentReadbackRequest(
							TEXT("graph-compound-animation:") + Plan->GetRequestId(),
							ReadbackOracleId, Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Resources, Outcome, VerifyError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate, Persisted,
						DurableBackend, FencingToken, *Resources, RollbackDetail);
					OutError = MoveTemp(VerifyError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
					if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
					OutError.Append(RollbackDetail);
					return false;
				}

				const FDurableVerificationAcknowledgement Acknowledgement(
					DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
					Prepared->GetInventory().GetInventoryHash(),
					Outcome.GetEvidenceId(), JournalOwner, FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
				if (!TryAcknowledgeAssetDurability(
						Persisted, Acknowledgement, FencingToken,
						*Resources, DurableError))
				{
					UE::UAI::Contract::FDiagnostic RollbackDetail;
					TryRollback(Request.GetTargetRef(), bCreate, Persisted,
						DurableBackend, FencingToken, *Resources, RollbackDetail);
					SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
					return false;
				}
				return TrySerializeResult(
					Request.GetTargetRef(),
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
		OutEnvironment = MakeShared<FAnimationProductionEnvironment>(Config);
		return OutEnvironment.IsValid();
	}
}
