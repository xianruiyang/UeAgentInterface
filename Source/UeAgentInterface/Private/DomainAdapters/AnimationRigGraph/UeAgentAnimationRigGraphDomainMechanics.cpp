#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "UObject/ObjectMacros.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringExport.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringRegistration.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringState.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringWorkspace.h"
#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoring.h"
#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoringApply.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/PackageName.h"
#include "OptimusDeformer.h"
#include "OperationOwners/AnimationRigCommands/UeAgentAnimationRigCommandOperationOwner.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.animation_rig_graph.edit/1");
		const FString ControlRigBlueprintType = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
		const FString IKRigDefinitionType = TEXT("/Script/IKRig.IKRigDefinition");
		const FString IKRetargeterType = TEXT("/Script/IKRig.IKRetargeter");
		const FString SkeletonType = TEXT("/Script/Engine.Skeleton");
		const FString DeformerGraphType = TEXT("/Script/OptimusCore.OptimusDeformer");

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            OutError.SetFailure(MoveTemp(Diagnostic));
            return false;
        }

        UE::UAI::Contract::FDiagnostic WithTarget(UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Target)
        {
            if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
            return Diagnostic;
        }

        UE::UAI::Contract::FDiagnostic InputFailure(const TCHAR* Code, const FString& Reason, const FString& Pointer,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected, const FString& Target = FString())
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual, TEXT("input_decode"), Reason);
            Error.SourceKind = TEXT("authored_value");
            Error.Source.JsonPath = Pointer;
            Error.Source.InternalJsonPath = Pointer;
            Error.TargetPath = Target;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        UE::UAI::Contract::FDiagnostic CommandFailure(const TCHAR* Code, const FString& Reason, const FString& Pointer,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected, const FString& Target)
        {
            auto Error = InputFailure(Code, Reason, Pointer, Actual, Expected, Target);
            Error.SourceKind = TEXT("command_input");
            return Error;
        }

        UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const FString& Reason, const FString& Target,
            const FString& Property, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = InputFailure(Code, Reason, Property, Actual, Expected, Target);
            Error.SourceKind = TEXT("native_result");
            Error.Source = {};
            Error.Stage = TEXT("native_graph");
            return Error;
        }

		bool IsSupportedRootType(const FString& Type)
		{
			return Type == ControlRigBlueprintType
				|| Type == IKRigDefinitionType
				|| Type == IKRetargeterType
				|| Type == SkeletonType
				|| Type == DeformerGraphType;
		}

		const UeAgentRiggingAuthoring::FProfile* ProfileForRootType(
			const FString& RootType)
		{
			if (RootType == ControlRigBlueprintType)
			{
				return UeAgentRiggingAuthoring::FindProfile(
					UeAgentRiggingAuthoring::EAssetKind::ControlRig);
			}
			if (RootType == IKRigDefinitionType)
			{
				return UeAgentRiggingAuthoring::FindProfile(
					UeAgentRiggingAuthoring::EAssetKind::IKRig);
			}
			if (RootType == IKRetargeterType)
			{
				return UeAgentRiggingAuthoring::FindProfile(
					UeAgentRiggingAuthoring::EAssetKind::IKRetargeter);
			}
			return nullptr;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentRiggingAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value);
			}
			return true;
		}

		bool EnsureDeformerAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered = UeAgentDeformerAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			return Registration.Key || Fail(OutError, Registration.Value);
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool SubmitAnimationRigCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			const FResolvedDomainHjsonDocument& Document, const FString& SourcePrefix)
		{
			FScopedDomainHjsonDiagnosticSource NativeSource(Document, OutError, SourcePrefix);
			UE::UAI::Contract::FDiagnostic MechanicsError;
			if (!Params.IsValid())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_domain_internal_request_invalid"),
					TEXT("The delegated animation rig command has no parameter object."), TEXT("native_graph"));
				Error.SourceKind = TEXT("native_result");
				Error.SubjectId = CommandId;
				Error.ActualValuePresent = false;
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("command parameter object"));
				OutError.SetFailure(MoveTemp(Error));
				return false;
			}
			if (!OperationOwners::AnimationRigCommands::TrySubmitMechanic(
					CommandId,
					Params.ToSharedRef(),
					FString(),
					OutPayload,
					MechanicsError))
			{
				if (MechanicsError.Code.StartsWith(
						TEXT("animation_rig_internal_request_")))
				{
					if (MechanicsError.SubjectId.IsEmpty()) MechanicsError.SubjectId = CommandId;
					OutError.SetFailure(MoveTemp(MechanicsError));
					return false;
				}
				if (MechanicsError.Code.StartsWith(
						TEXT("animation_rig_internal_mechanics_unavailable")))
				{
					if (MechanicsError.SubjectId.IsEmpty()) MechanicsError.SubjectId = CommandId;
					OutError.SetFailure(MoveTemp(MechanicsError));
					return false;
				}
				SetWorkspaceErrorFromCommandFailure(
					MechanicsError,
					TEXT("animation_rig_graph_domain_native_command_failed"),
					CommandId,
					OutError);
				return false;
			}
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target,
					TArray<FString>{ControlRigBlueprintType, IKRigDefinitionType,
						IKRetargeterType, SkeletonType, DeformerGraphType},
					Owner,
					Capture,
					OutError))
			{
				return false;
			}
			const FString RootType = Owner->GetClass()->GetPathName();
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool TryReadDeformerSemanticState(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			UOptimusDeformer* Deformer = LoadObject<UOptimusDeformer>(
				nullptr, *ObjectPathFromPackageIdentity(Target));
			if (!Deformer)
			{
				return Fail(OutError, NativeFailure(TEXT("deformer_semantic_readback_target_missing"),
                    TEXT("The Deformer asset could not be loaded for semantic readback."), Target, TEXT("asset_available"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			if (!EnsureDeformerAuthoringRegistered(OutError)) return false;

			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentDeformerAuthoring::BuildExportSnapshotReadRequests(
					*Deformer, Requests, CaptureError))
			{
				OutError.SetFailure(WithTarget(CaptureError, Target));
				return false;
			}
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError.SetFailure(WithTarget(Capture.Diagnostic, Target));
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildCanonicalAuthoringCapture(
					UeAgentDeformerAuthoring::CompositionKind,
					TEXT("llm_authoring"),
					Capture.Snapshot,
					Resources,
					CanonicalSnapshot,
					Model,
					OutError)
				|| !CanonicalSnapshot.IsValid()
				|| !Model.IsValid())
			{
				if (OutError.GetCode().IsEmpty())
				{
					auto Actual = MakeShared<FJsonObject>();
                    Actual->SetBoolField(TEXT("canonical_snapshot_available"), CanonicalSnapshot.IsValid());
                    Actual->SetBoolField(TEXT("authoring_model_available"), Model.IsValid());
                    return Fail(OutError, NativeFailure(TEXT("deformer_semantic_readback_capture_failed"),
                        TEXT("Semantic readback did not produce both a canonical snapshot and an authoring model."), Target, TEXT("capture"),
                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("canonical snapshot and authoring model"))));
				}
				return false;
			}
			Capture.Snapshot.ReleaseNativeValues();
			OutRevision = CanonicalSnapshot->GetRevision().GetGeneration();
			const TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
			return TryCanonicalizeObject(Readback, OutPayload, OutError);
		}

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_target_invalid"),
                    TEXT("The target is not a valid object or package identity."), TEXT("/target"),
                    MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid asset identity")), Target));
			}
			if (bPresent)
			{
				return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_stale_revision"),
                    TEXT("The request expects a missing target, but the target already exists."), TEXT("/expected_revision"),
                    MakeShared<FJsonValueString>(TEXT("missing")), MakeShared<FJsonValueString>(TEXT("current target revision")), Target));
			}
			return true;
		}

		bool TryBuildCreateParams(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Create,
			FString& OutCommandId,
			TSharedPtr<FJsonObject>& OutNative,
			FWorkspaceError& OutError)
		{
			if (RootType == SkeletonType)
			{
				return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_skeleton_create_not_supported"),
                    TEXT("Skeleton creation is not supported by this workspace create operation."), TEXT("/type"),
                    MakeShared<FJsonValueString>(RootType), MakeShared<FJsonValueString>(TEXT("ControlRigBlueprint, IKRigDefinition or IKRetargeter")), Target));
			}
			if (RootType == ControlRigBlueprintType)
			{
				OutCommandId = TEXT("animation.control_rig.asset.create");
			}
			else if (RootType == IKRigDefinitionType)
			{
				OutCommandId = TEXT("animation.ik_rig.asset.create");
			}
			else if (RootType == IKRetargeterType)
			{
				OutCommandId = TEXT("animation.ik_retargeter.asset.create");
			}
			else
			{
				return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_type_invalid"),
                    TEXT("The requested root type has no workspace creation command."), TEXT("/type"),
                    MakeShared<FJsonValueString>(RootType), MakeShared<FJsonValueString>(TEXT("ControlRigBlueprint, IKRigDefinition or IKRetargeter")), Target));
			}
			const FString Package = PackageIdentity(Target);
			const FString ExpectedName =
				FPackageName::GetLongPackageAssetName(Package);
			const FString ExpectedPath =
				FPackageName::GetLongPackagePath(Package);
			if (!Create.IsValid()
				|| ExpectedName.IsEmpty()
				|| ExpectedPath.IsEmpty()
				|| !FPackageName::IsValidLongPackageName(Package))
			{
				if (!Create.IsValid())
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_contract_invalid"),
                        TEXT("Creation requires a create object."), TEXT("/create"), nullptr,
                        MakeShared<FJsonValueString>(TEXT("create object")), Target));
                return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_create_contract_invalid"),
                    TEXT("The creation target must provide a valid long package path and asset name."), TEXT("/target"),
                    MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid asset package identity")), Target));
			}
			FString Declared;
			if (Create->TryGetStringField(TEXT("asset_name"), Declared)
				&& Declared != ExpectedName)
			{
				return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_name_mismatch"),
                    TEXT("The declared asset_name does not match the target identity."), TEXT("/create/asset_name"),
                    Create->TryGetField(TEXT("asset_name")), MakeShared<FJsonValueString>(ExpectedName), Target));
			}
			if (Create->TryGetStringField(TEXT("package_path"), Declared))
			{
				while (Declared.EndsWith(TEXT("/")))
				{
					Declared.LeftChopInline(1);
				}
				if (Declared != ExpectedPath)
				{
					return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_path_mismatch"),
                        TEXT("The declared package_path does not match the target package directory after trimming trailing slashes."), TEXT("/create/package_path"),
                        Create->TryGetField(TEXT("package_path")), MakeShared<FJsonValueString>(ExpectedPath), Target));
				}
			}
			OutNative = MakeShared<FJsonObject>(*Create);
			OutNative->SetStringField(TEXT("asset_name"), ExpectedName);
			OutNative->SetStringField(TEXT("package_path"), ExpectedPath);
			return true;
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			return TryReadNativeMutationPayload(Target, MutationPayload, OutMutation, OutError, &OutReceipt);
		}

		bool IsAllowedOperation(const FString& RootType, const FString& CommandId)
		{
			if (RootType == ControlRigBlueprintType)
			{
				return CommandId == TEXT("animation.control_rig.hierarchy_graph.apply")
					|| CommandId == TEXT("animation.control_rig.shape_library.apply");
			}
			if (RootType == IKRigDefinitionType)
			{
				return CommandId == TEXT("animation.ik_rig.definition.apply")
					|| CommandId == TEXT("animation.ik_rig.definition.auto_generate");
			}
			if (RootType == IKRetargeterType)
			{
				return CommandId == TEXT("animation.ik_retargeter.definition.apply")
					|| CommandId == TEXT("animation.ik_retargeter.settings.apply");
			}
			return RootType == SkeletonType
				&& (CommandId == TEXT("animation.skeleton.compatibility.apply")
					|| CommandId == TEXT("animation.skeleton.definition.apply")
					|| CommandId == TEXT("animation.skeleton.virtual_bone.apply"));
		}

		bool IsIdentityVector(
			const TSharedPtr<FJsonObject>& Transform,
			const TCHAR* Field,
			const double Expected)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Transform.IsValid()
				|| !Transform->TryGetArrayField(Field, Values)
				|| !Values || Values->Num() != 3)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid()
					|| Value->Type != EJson::Number
					|| !FMath::IsNearlyEqual(Value->AsNumber(), Expected))
				{
					return false;
				}
			}
			return true;
		}

        bool ValidateExplicitSpaceContract(
            const TSharedPtr<FJsonObject>& Root,
            const FString& CommandId,
            const TSharedPtr<FJsonObject>& NativeParams,
            const FString& Target,
            FWorkspaceError& OutError)
        {
            if (CommandId != TEXT("animation.control_rig.hierarchy_graph.apply")) return true;
            const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
            if (!NativeParams.IsValid() || !NativeParams->TryGetArrayField(TEXT("edits"), Edits)
                || !Edits || Edits->IsEmpty())
                return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_edit_required"),
                    TEXT("Hierarchy graph apply requires a non-empty edits array."), TEXT("/operation/params/edits"),
                    NativeParams.IsValid() ? NativeParams->TryGetField(TEXT("edits")) : nullptr,
                    MakeShared<FJsonValueString>(TEXT("non-empty array")), Target));

            bool bNeedsHierarchySpace = false;
            for (int32 Index = 0; Index < Edits->Num(); ++Index)
            {
                const TSharedPtr<FJsonValue>& EditValue = (*Edits)[Index];
                const TSharedPtr<FJsonObject> Edit = EditValue.IsValid() && EditValue->Type == EJson::Object
                    ? EditValue->AsObject() : nullptr;
                const FString Pointer = FString::Printf(TEXT("/operation/params/edits/%d"), Index);
                FString Intent;
                FString SemanticId;
                if (!Edit.IsValid())
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_edit_invalid"),
                        TEXT("Each hierarchy edit must be an object."), Pointer, EditValue,
                        MakeShared<FJsonValueString>(TEXT("object")), Target));
                if (!Edit->TryGetStringField(TEXT("intent"), Intent))
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_edit_invalid"),
                        TEXT("The hierarchy edit intent must be a string."), Pointer + TEXT("/intent"),
                        Edit->TryGetField(TEXT("intent")), MakeShared<FJsonValueString>(TEXT("string")), Target));
                if (!Edit->TryGetStringField(TEXT("semantic_id"), SemanticId))
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_edit_invalid"),
                        TEXT("The hierarchy edit semantic_id must be a string."), Pointer + TEXT("/semantic_id"),
                        Edit->TryGetField(TEXT("semantic_id")), MakeShared<FJsonValueString>(TEXT("string")), Target));
                bNeedsHierarchySpace |= Intent != TEXT("delete") && SemanticId.StartsWith(TEXT("rig_element:control:"));
            }
            if (!bNeedsHierarchySpace) return true;

            const TSharedPtr<FJsonObject>* Space = nullptr;
            const TSharedPtr<FJsonObject>* Transform = nullptr;
            FString SubjectRef;
            FString SemanticSpace;
            FString WriteSpaceRef;
            FString ReadbackSpaceRef;
            FString Units;
            const auto ContractFailure = [&](const FString& Field, const FString& Reason,
                const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
            {
                return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_contract_required"), Reason,
                    TEXT("/space_contract") + Field, Actual, MakeShared<FJsonValueString>(Expected), Target));
            };
            if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("space_contract"), Space) || !Space || !Space->IsValid())
                return ContractFailure(FString(), TEXT("Control edits require a space_contract object."),
                    Root.IsValid() ? Root->TryGetField(TEXT("space_contract")) : nullptr, TEXT("object"));
            if (!(*Space)->TryGetStringField(TEXT("subject_ref"), SubjectRef) || SubjectRef.IsEmpty())
                return ContractFailure(TEXT("/subject_ref"), TEXT("The space contract requires a non-empty subject_ref."),
                    (*Space)->TryGetField(TEXT("subject_ref")), TEXT("non-empty string"));
            if (!(*Space)->TryGetStringField(TEXT("semantic_space"), SemanticSpace) || SemanticSpace != TEXT("parent"))
                return ContractFailure(TEXT("/semantic_space"), TEXT("Control edits support parent semantic space."),
                    (*Space)->TryGetField(TEXT("semantic_space")), TEXT("parent"));
            if (!(*Space)->TryGetStringField(TEXT("write_space_ref"), WriteSpaceRef) || !WriteSpaceRef.StartsWith(TEXT("Bone:")))
                return ContractFailure(TEXT("/write_space_ref"), TEXT("The write space must reference a bone."),
                    (*Space)->TryGetField(TEXT("write_space_ref")), TEXT("string starting with Bone:"));
            if (!(*Space)->TryGetStringField(TEXT("readback_space_ref"), ReadbackSpaceRef) || ReadbackSpaceRef != WriteSpaceRef)
                return ContractFailure(TEXT("/readback_space_ref"), TEXT("The readback space must equal the write space."),
                    (*Space)->TryGetField(TEXT("readback_space_ref")), WriteSpaceRef);
            if (!(*Space)->TryGetStringField(TEXT("units"), Units) || Units != TEXT("centimeters"))
                return ContractFailure(TEXT("/units"), TEXT("The space contract requires centimeters."),
                    (*Space)->TryGetField(TEXT("units")), TEXT("centimeters"));
            if (!(*Space)->TryGetObjectField(TEXT("transform"), Transform) || !Transform || !Transform->IsValid())
                return ContractFailure(TEXT("/transform"), TEXT("The space contract requires a transform object."),
                    (*Space)->TryGetField(TEXT("transform")), TEXT("object"));

            for (const TCHAR* Field : {TEXT("translation"), TEXT("rotation_degrees"), TEXT("scale")})
            {
                const double Expected = FCString::Strcmp(Field, TEXT("scale")) == 0 ? 1.0 : 0.0;
                if (!IsIdentityVector(*Transform, Field, Expected))
                {
                    TArray<TSharedPtr<FJsonValue>> Values;
                    for (int32 Index = 0; Index < 3; ++Index) Values.Add(MakeShared<FJsonValueNumber>(Expected));
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_transform_unsupported"),
                        TEXT("Control edits currently require an identity space transform; the indicated vector must contain three matching numbers."),
                        TEXT("/space_contract/transform/") + FString(Field), (*Transform)->TryGetField(Field),
                        MakeShared<FJsonValueArray>(MoveTemp(Values)), Target));
                }
            }
            for (int32 Index = 0; Index < Edits->Num(); ++Index)
            {
                // Edit shape and identity fields were validated before the space contract.
                const TSharedPtr<FJsonObject> Edit = (*Edits)[Index]->AsObject();
                if (Edit->GetStringField(TEXT("intent")) == TEXT("delete")
                    || !Edit->GetStringField(TEXT("semantic_id")).StartsWith(TEXT("rig_element:control:"))) continue;
                const FString Pointer = FString::Printf(TEXT("/operation/params/edits/%d/value"), Index);
                const TSharedPtr<FJsonObject>* Value = nullptr;
                FString ParentKey;
                if (!Edit->TryGetObjectField(TEXT("value"), Value) || !Value || !Value->IsValid())
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_parent_mismatch"),
                        TEXT("A control edit requires a value object containing parent_native_key."), Pointer,
                        Edit->TryGetField(TEXT("value")), MakeShared<FJsonValueString>(TEXT("object")), Target));
                if (!(*Value)->TryGetStringField(TEXT("parent_native_key"), ParentKey) || ParentKey != WriteSpaceRef)
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_space_parent_mismatch"),
                        TEXT("The control parent must match the space contract write_space_ref."), Pointer + TEXT("/parent_native_key"),
                        (*Value)->TryGetField(TEXT("parent_native_key")), MakeShared<FJsonValueString>(WriteSpaceRef), Target));
            }
            return true;
        }

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (!Document.SemanticWorkspace.IsValid())
			{
                return Fail(OutError, NativeFailure(TEXT("animation_rig_graph_semantic_workspace_missing"),
                    TEXT("The resolved workspace does not contain a semantic workspace document."), Target, TEXT("semantic_workspace_available"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			UObject* Asset = nullptr;
			if (!TryLoadObjectTarget(Target,
				TArray<FString>{ControlRigBlueprintType, IKRigDefinitionType,
					IKRetargeterType, SkeletonType, DeformerGraphType},
				Asset, OutError)) return false;
			if (UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(Asset))
			{
				if (!EnsureDeformerAuthoringRegistered(OutError)) return false;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentDeformerAuthoring::BuildExportSnapshotReadRequests(
						*Deformer, Requests, CaptureError))
					{
                OutError.SetFailure(WithTarget(CaptureError, Target));
						return false;
					}
				SelectRootPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
					{
                OutError.SetFailure(WithTarget(Capture.Diagnostic, Target));
						return false;
					}
				ContextResource::FUeAgentResourceTracker Resources = MakeSemanticWorkspaceResourceTracker();
				StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
				FAuthoringModelPtr Model;
				if (!TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(), UeAgentDeformerAuthoring::CompositionKind,
					TEXT("llm_authoring"), Capture.Snapshot, Resources,
					CanonicalSnapshot, Model, OutError)) return false;
				Capture.Snapshot.ReleaseNativeValues();
				const FString TargetPackage = PackageIdentity(Target);
				const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
				// The public transaction releases and reloads the target before final readback.
				Deformer = nullptr;
				Asset = nullptr;
				return TryCoordinateSemanticWorkspaceImport(
					Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
					CanonicalSnapshot.ToSharedRef(),
					BuildSemanticWorkspaceImportDescriptor(TEXT("deformer.semantic_workspace"), *Model), TargetObjectPath, Resources,
					{TEXT("deformer-domain-provider"), Target, TargetObjectPath,
						TEXT("deformer-semantic"), TEXT("deformer_semantic_prepare_failed"),
						TEXT("deformer_semantic_apply_failed"),
						TEXT("deformer_semantic_readback_payload_invalid"),
						TEXT("animation_rig_graph_domain_compile_not_settled"),
						[TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
						{
							UOptimusDeformer* Native = LoadObject<UOptimusDeformer>(nullptr, *TargetObjectPath);
                            if (!Native)
                            {
                                Error = NativeFailure(TEXT("deformer_semantic_readback_target_missing"),
                                    TEXT("The deformer target could not be loaded for final snapshot capture."), TargetObjectPath,
                                    TEXT("asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                                return false;
                            }
                            return UeAgentDeformerAuthoring::BuildExportSnapshotReadRequests(*Native, Requests, Error);
						}},
					[TargetPackage, TargetObjectPath](const FString& RequestId,
						const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
						Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
						FRootPropertyApplyOutcome& OutOutcome)
					{
						UeAgentAuthoringOperationApply::FApplyWorkspaceResult Result;
						UeAgentAuthoringOperationApply::FBackendOptions Options;
						Options.FinalReadback = OutOutcome.FinalReadback;
						UE::UAI::Contract::FDiagnostic Error;
						const bool bApplied = UeAgentDeformerAuthoring::ApplySemanticWorkspace(
							TargetObjectPath, RequestId, Prepared, Projection, Options, Result, Error);
						OutOutcome.bApplied = bApplied;
						OutOutcome.Phase = Result.Execution.Phase;
						UeAgentAuthoringOperationApply::FApplyFailureProjection Failure;
						if (UeAgentAuthoringOperationApply::TryProjectApplyFailure(Result, Failure))
						{
							OutOutcome.Phase = Failure.Phase;
							OutOutcome.Diagnostic = Failure.Diagnostic;
						}
						else if (!bApplied)
						{
							OutOutcome.Phase = TEXT("semantic_prepare");
							OutOutcome.Diagnostic = Error;
						}
						return bApplied;
					},
					[&Target](FString& Payload, FString& Revision, FWorkspaceError& Error)
					{ return TryReadDeformerSemanticState(Target, Payload, Revision, Error); },
					OutPayload, OutError);
			}
			const FString RootType = Asset->GetClass()->GetPathName();
			if (RootType == SkeletonType)
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!UeAgentAnimationAuthoring::RegisterAuthoring(Error))
                    return Fail(OutError, WithTarget(Error, Target));
				const auto* SkeletonProfile = UeAgentAnimationAuthoring::FindProfile(UeAgentAnimationAuthoring::EAssetKind::Skeleton);
				return UeAgentAnimationAuthoring::ImportAssetWorkspace(
					*SkeletonProfile, *Asset, Document.SemanticWorkspace.ToSharedRef(),
					[&Target](FString& Payload, FString& Revision, FWorkspaceError& ReadError)
					{ return TryRead(Target, Payload, Revision, nullptr, ReadError); },
					OutPayload, OutError);
			}
			const UeAgentRiggingAuthoring::FProfile* Profile =
				ProfileForRootType(RootType);
			if (!Profile)
			{
                return Fail(OutError, NativeFailure(TEXT("animation_rig_graph_domain_schema_or_type_invalid"),
                    TEXT("The captured asset class has no rigging authoring profile."), Target, TEXT("class"),
                    MakeShared<FJsonValueString>(RootType),
                    MakeShared<FJsonValueString>(TEXT("ControlRigBlueprint, IKRigDefinition or IKRetargeter"))));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;

			const FString TargetPackage = PackageIdentity(Target);
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentRiggingAuthoring::BuildAssetExportSnapshotReadRequests(
					*Profile, *Asset, Requests, CaptureError))
			{
                OutError.SetFailure(WithTarget(CaptureError, Target));
				return false;
			}
			SelectRootPropertyImportReadRequests(*Document.SemanticWorkspace, Requests);
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CapturePropertyImportState(*Document.SemanticWorkspace, Requests, Capture))
			{
                OutError.SetFailure(WithTarget(Capture.Diagnostic, Target));
				return false;
			}

			ContextResource::FUeAgentResourceTracker Resources =
				MakeSemanticWorkspaceResourceTracker();
			StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildPropertyImportCurrentCapture(Document.SemanticWorkspace.Get(),
					ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
					TEXT("llm_authoring"), Capture.Snapshot, Resources,
					CanonicalSnapshot, Model, OutError)
				|| !CanonicalSnapshot.IsValid() || !Model.IsValid())
			{
				if (OutError.GetCode().IsEmpty())
				{
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetBoolField(TEXT("canonical_snapshot_available"), CanonicalSnapshot.IsValid());
                    Actual->SetBoolField(TEXT("authoring_model_available"), Model.IsValid());
                    auto Expected = MakeShared<FJsonObject>();
                    Expected->SetBoolField(TEXT("canonical_snapshot_available"), true);
                    Expected->SetBoolField(TEXT("authoring_model_available"), true);
                    return Fail(OutError, NativeFailure(TEXT("animation_rig_graph_semantic_capture_failed"),
                        TEXT("Canonical capture did not produce both a snapshot and an authoring model."), Target,
                        TEXT("capture"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected)));
				}
				return false;
			}

			Capture.Snapshot.ReleaseNativeValues();
			const FString TargetObjectPath = ObjectPathFromPackageIdentity(Target);
			const UeAgentRiggingAuthoring::EAssetKind AssetKind = Profile->Kind;
			return TryCoordinateSemanticWorkspaceImport(
				Document.SemanticWorkspace.ToSharedRef(), Model.ToSharedRef(),
				CanonicalSnapshot.ToSharedRef(),
				BuildSemanticWorkspaceImportDescriptor(TEXT("animation_rig_graph.semantic_workspace"), *Model),
				TargetObjectPath, Resources,
				{TEXT("animation-rig-graph-domain-provider"), Target,
					TargetObjectPath, TEXT("animation-rig-graph-semantic"),
					TEXT("animation_rig_graph_semantic_prepare_failed"),
					TEXT("animation_rig_graph_semantic_apply_failed"),
					TEXT("animation_rig_graph_semantic_readback_payload_invalid"),
					TEXT("animation_rig_graph_domain_compile_not_settled"),
					[Profile, TargetObjectPath](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
					{
						UObject* Native = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetObjectPath);
                        if (!Native)
                        {
                            Error = NativeFailure(TEXT("animation_rig_graph_semantic_readback_target_missing"),
                                TEXT("The rigging target could not be loaded for final snapshot capture."), TargetObjectPath,
                                TEXT("asset_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                            return false;
                        }
                        return UeAgentRiggingAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, *Native, Requests, Error);
					}},
				[AssetKind, TargetPackage, DocumentSemanticId = Profile->DocumentSemanticId,
					&Asset](const FString& RequestId,
					const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
					FRootPropertyApplyOutcome& OutOutcome)
				{
					UeAgentRiggingAuthoring::FAssetApplyRequest Request;
					Request.Kind = AssetKind;
					Request.Operation = UeAgentApplyInputContract::EOperation::Update;
					Request.AssetPath = TargetPackage;
					Request.Properties = MoveTemp(Projection.RootObjectProperties);
					for (const PlanningPreflight::FChangeDraft& Change :
						Prepared.GetChangeSetDraft().GetChanges())
					{
						if (Change.GetIdentity().GetSemanticId() != DocumentSemanticId
							|| !Change.GetProposedAfter().IsValid()) continue;
						TSharedPtr<FJsonValue> Json;
						if (!StateSemantic::CanonicalJson::TryToJsonValue(
								*Change.GetProposedAfter(), Json, OutOutcome.Diagnostic)
							|| !Json.IsValid() || Json->Type != EJson::Object)
						{
                            if (OutOutcome.Diagnostic.IsEmpty())
                                OutOutcome.Diagnostic = NativeFailure(TEXT("rigging_document_projection_failed"),
                                    TEXT("The proposed rigging document did not project to a JSON object."), TargetPackage,
                                    DocumentSemanticId, Json, MakeShared<FJsonValueString>(TEXT("object")));
							OutOutcome.Phase = TEXT("semantic_prepare");
							return false;
						}
						Request.Document = Json->AsObject();
						if (Change.GetExpectedBefore().IsValid())
						{
							TSharedPtr<FJsonValue> Before;
							if (!StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetExpectedBefore(), Before, OutOutcome.Diagnostic)
								|| !Before.IsValid() || Before->Type != EJson::Object)
                            {
                                if (OutOutcome.Diagnostic.IsEmpty())
                                    OutOutcome.Diagnostic = NativeFailure(TEXT("rigging_captured_document_invalid"),
                                        TEXT("The captured baseline rigging document did not project to a JSON object."), TargetPackage,
                                        DocumentSemanticId, Before, MakeShared<FJsonValueString>(TEXT("object")));
                                return false;
                            }
							Request.CapturedDocument = Before->AsObject();
						}
					const TSharedPtr<FJsonObject>* GraphDocument = nullptr;
					const TSharedPtr<FJsonObject>* Graphs = nullptr;
					if (Request.Document->TryGetObjectField(TEXT("graphs"), GraphDocument)
						&& (*GraphDocument)->TryGetObjectField(TEXT("graphs"), Graphs))
					{
						for (const auto& GraphPair : (*Graphs)->Values)
						{
							if (!GraphPair.Value.IsValid() || GraphPair.Value->Type != EJson::Object) continue;
							const auto Graph = GraphPair.Value->AsObject();
							const FString GraphPointer = TEXT("/graphs/graphs/")
								+ GraphPair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
							Prepared.GetWorkspace()->AttachDocumentPropertySources(
								Change.GetIdentity().GetCanonicalKey(), GraphPointer,
								Projection.DocumentSourceBindings, *Graph, FString());
							const TSharedPtr<FJsonObject>* Nodes = nullptr;
							if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes)) continue;
							for (const auto& NodePair : (*Nodes)->Values)
							{
								if (!NodePair.Value.IsValid() || NodePair.Value->Type != EJson::Object) continue;
								const FString NodePointer = GraphPointer + TEXT("/nodes/")
									+ NodePair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
								Prepared.GetWorkspace()->AttachDocumentPropertySources(
									Change.GetIdentity().GetCanonicalKey(), NodePointer,
									Projection.DocumentSourceBindings, *NodePair.Value->AsObject());
							}
						}
					}
					}
					Request.bCompile = true;
					Request.bSave = true;
					Request.FinalReadback = OutOutcome.FinalReadback;
					UeAgentRiggingAuthoring::FAssetApplyResult Result;
					const bool bApplied = UeAgentRiggingAuthoring::ApplyAsset(
						Request, Result, RequestId);
					OutOutcome.bApplied = bApplied && Result.IsApplied();
					OutOutcome.Phase = MoveTemp(Result.Phase);
					OutOutcome.Diagnostic = MoveTemp(Result.Diagnostic);
					Result.Asset = nullptr;
					Asset = nullptr;
					return bApplied;
				},
				[&Target](FString& Payload, FString& Revision, FWorkspaceError& Error)
				{ return TryRead(Target, Payload, Revision, nullptr, Error); },
				OutPayload, OutError);
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("animation_rig_graph"),
						SchemaId,
						TArray<FString>{ControlRigBlueprintType,
							IKRigDefinitionType, IKRetargeterType, SkeletonType,
							DeformerGraphType}),
					Document,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target, Document, OutPayload, OutError);
			}
			FString RootType;
			if (!Document.Root.IsValid()
				|| !Document.Root->TryGetStringField(TEXT("type"), RootType)
				|| !IsSupportedRootType(RootType))
			{
                return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_schema_or_type_invalid"),
                    TEXT("The workspace root type is missing or is not supported by AnimationRigGraph."), TEXT("/type"),
                    Document.Root.IsValid() ? Document.Root->TryGetField(TEXT("type")) : nullptr,
                    MakeShared<FJsonValueString>(TEXT("ControlRigBlueprint, IKRigDefinition, IKRetargeter, Skeleton or OptimusDeformer")), Target));
			}

			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_contract_required"),
                        TEXT("Creating a missing target requires a create object."), TEXT("/create"),
                        Document.Root->TryGetField(TEXT("create")), MakeShared<FJsonValueString>(TEXT("object")), Target));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_then_edit_not_atomic"),
                        TEXT("Creation and a subsequent operation cannot be submitted in the same workspace import."), TEXT("/operation"),
                        Document.Root->TryGetField(TEXT("operation")), MakeShared<FJsonValueString>(TEXT("field absent during creation")), Target));
				}
				FString CreateCommandId;
				TSharedPtr<FJsonObject> Native;
				if (!TryBuildCreateParams(
						Target, RootType, *Create,
						CreateCommandId, Native, OutError)
					|| !SubmitAnimationRigCommand(
						CreateCommandId, Native, MutationPayload, OutError, Document, TEXT("/root/create")))
				{
					return false;
				}
			}
			else
			{
				FString BeforePayload;
				FString CurrentRevision;
				if (!TryRead(
						Target, BeforePayload, CurrentRevision, nullptr, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
                    return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_stale_revision"),
                        TEXT("expected_revision does not match the current target revision."), TEXT("/expected_revision"),
                        MakeShared<FJsonValueString>(Document.ExpectedRevision), MakeShared<FJsonValueString>(CurrentRevision), Target));
				}
				if (Document.Root->HasField(TEXT("create")))
				{
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_create_requires_missing"),
                        TEXT("The create field is only valid when expected_revision is missing."), TEXT("/create"),
                        Document.Root->TryGetField(TEXT("create")), MakeShared<FJsonValueString>(TEXT("field absent when updating an existing target")), Target));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* NativeParams = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(
						TEXT("operation"), Operation)
					|| !Operation || !Operation->IsValid()
					|| !(*Operation)->TryGetStringField(
						TEXT("command_id"), CommandId)
					|| !(*Operation)->TryGetObjectField(
						TEXT("params"), NativeParams)
					|| !NativeParams || !NativeParams->IsValid())
				{
                    if (!Operation || !Operation->IsValid())
                        return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_operation_contract_required"),
                            TEXT("Updating an existing target requires an operation object."), TEXT("/operation"),
                            Document.Root->TryGetField(TEXT("operation")), MakeShared<FJsonValueString>(TEXT("object")), Target));
                    FString Ignored;
                    const bool bCommandValid = (*Operation)->TryGetStringField(TEXT("command_id"), Ignored);
                    const FString Field = bCommandValid ? TEXT("params") : TEXT("command_id");
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_operation_contract_required"),
                        bCommandValid ? TEXT("Operation params must be an object.") : TEXT("Operation command_id must be a string."),
                        TEXT("/operation/") + Field, (*Operation)->TryGetField(Field),
                        MakeShared<FJsonValueString>(bCommandValid ? TEXT("object") : TEXT("string")), Target));
				}
				if (!IsAllowedOperation(RootType, CommandId))
				{
                    const FString Allowed = RootType == ControlRigBlueprintType
                        ? TEXT("animation.control_rig.hierarchy_graph.apply, animation.control_rig.shape_library.apply")
                        : RootType == IKRigDefinitionType
                        ? TEXT("animation.ik_rig.definition.apply, animation.ik_rig.definition.auto_generate")
                        : RootType == IKRetargeterType
                        ? TEXT("animation.ik_retargeter.definition.apply, animation.ik_retargeter.settings.apply")
                        : RootType == SkeletonType
                        ? TEXT("animation.skeleton.compatibility.apply, animation.skeleton.definition.apply, animation.skeleton.virtual_bone.apply")
                        : TEXT("semantic workspace import; no native operation is supported for this root type");
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_operation_not_supported"),
                        TEXT("The operation is not supported for root type ") + RootType + TEXT("."), TEXT("/operation/command_id"),
                        MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(Allowed), Target));
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**NativeParams);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
                    return Fail(OutError, InputFailure(TEXT("animation_rig_graph_domain_operation_target_mismatch"),
                        TEXT("Operation target_ref resolves to a different asset than the workspace target."), TEXT("/operation/params/target_ref"),
                        MakeShared<FJsonValueString>(NativeTarget), MakeShared<FJsonValueString>(ObjectPathFromPackageIdentity(Target)), Target));
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!ValidateExplicitSpaceContract(
                        Document.Root, CommandId, Native, Target, OutError)
					|| !TryNormalizeAnimationRigGraphCandidateBindings(
						Target, Native, OutError)
					|| !SubmitAnimationRigCommand(
						CommandId, Native, MutationPayload, OutError, Document, TEXT("/root/operation/params")))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target, MutationPayload, Mutation,
					LifecycleReceipt, OutError))
			{
				return false;
			}
			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterSnapshot;
			if (!TryRead(
					Target, AfterPayload, AfterRevision, &AfterSnapshot, OutError))
			{
				return false;
			}
			if (!TryValidateReadbackDiagnostics(AfterSnapshot, Target,
				TEXT("animation_rig_graph_domain_compile_not_settled"), OutError))
				return false;
			return TryBuildImportSuccessPayload(
				TEXT("animation-rig-graph-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterSnapshot,
				Mutation,
				OutPayload,
				OutError);
		}

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			FPreparedWorkspaceExport* PreparedExport)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
                return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_export_path_required"),
                    TEXT("Export requires a non-empty workspace_path."), TEXT("/workspace_path"),
                    Params->TryGetField(TEXT("workspace_path")), MakeShared<FJsonValueString>(TEXT("non-empty string")), Target));
			}
			UObject* Asset = nullptr;
			if (!TryLoadObjectTarget(
					Target,
					TArray<FString>{ControlRigBlueprintType, IKRigDefinitionType,
						IKRetargeterType, SkeletonType, DeformerGraphType},
					Asset, OutError))
			{
				return false;
			}
			if (UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(Asset))
			{
				if (!EnsureDeformerAuthoringRegistered(OutError)) return false;
				UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
				FString ExportError;
				if (!UeAgentDeformerAuthoring::ExportWorkspace(*Deformer, WorkspacePath, TEXT("llm_authoring"), ExportResult, PreparedExport))
					{
                OutError.SetFailure(WithTarget(ExportResult.Diagnostic, Target));
						return false;
					}
				return TryCanonicalizeObject(ExportResult.ToPublicJsonObject(), OutPayload, OutError);
			}
			if (Asset && Asset->GetClass()->GetPathName() == SkeletonType)
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!UeAgentAnimationAuthoring::RegisterAuthoring(Error))
                    return Fail(OutError, WithTarget(Error, Target));
				const auto* SkeletonProfile = UeAgentAnimationAuthoring::FindProfile(UeAgentAnimationAuthoring::EAssetKind::Skeleton);
				UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
				if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*SkeletonProfile, *Asset, WorkspacePath, TEXT("llm_authoring"), Result, PreparedExport))
                    return Fail(OutError, WithTarget(Result.Diagnostic, Target));
				return TryCanonicalizeObject(Result.ToPublicJsonObject(), OutPayload, OutError);
			}
			const UeAgentRiggingAuthoring::FProfile* Profile = Asset
				? ProfileForRootType(Asset->GetClass()->GetPathName()) : nullptr;
			if (!Profile)
			{
                return Fail(OutError, NativeFailure(TEXT("animation_rig_graph_domain_target_type_invalid"),
                    TEXT("The export target class has no rigging authoring profile."), Target, TEXT("class"),
                    Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : nullptr,
                    MakeShared<FJsonValueString>(TEXT("ControlRigBlueprint, IKRigDefinition or IKRetargeter"))));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			FString ExportError;
			if (!UeAgentRiggingAuthoring::ExportAssetWorkspace(*Profile, *Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult, PreparedExport))
			{
                OutError.SetFailure(WithTarget(ExportResult.Diagnostic, Target));
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError,
		FPreparedWorkspaceExport* PreparedExport)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError) || !Params.IsValid())
		{
            return Fail(OutError, JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target) || Target != TargetIdentity)
		{
            return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_target_identity_mismatch"),
                TEXT("The command target must match the dispatched target identity."), TEXT("/target"),
                Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError, PreparedExport);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, nullptr, OutError);
		}
        return Fail(OutError, CommandFailure(TEXT("animation_rig_graph_domain_command_not_supported"),
            TEXT("AnimationRigGraph handles content_graph commands."), TEXT("/command_id"),
            MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*")), TargetIdentity));
	}
}
