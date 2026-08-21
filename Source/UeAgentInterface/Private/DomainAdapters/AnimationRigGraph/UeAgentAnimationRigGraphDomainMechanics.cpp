#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AnimationRigCommands/UeAgentAnimationRigCommandOperationOwner.h"
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

		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		bool IsSupportedRootType(const FString& Type)
		{
			return Type == ControlRigBlueprintType
				|| Type == IKRigDefinitionType
				|| Type == IKRetargeterType
				|| Type == SkeletonType;
		}

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("animation-rig-graph-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId, *RequestId, *CommandId, *ContractHash, *ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(OutError, TEXT("animation_rig_graph_domain_internal_request_invalid"), Diagnostic.Code, Diagnostic.Message);
			}
			return true;
		}

		bool SubmitAnimationRigCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
			TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
			FString CompositionError;
			if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(
					Registry, Dispatch, CompositionError)
				|| !Registry.IsValid())
			{
				return Fail(OutError, TEXT("animation_rig_graph_domain_registry_unavailable"), CompositionError);
			}
			const FUeAgentFrozenCommandContract* Contract = Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(OutError, TEXT("animation_rig_graph_domain_command_missing"), CommandId);
			}
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(CommandId, Contract->GetContractHash(), ParamsJson, Request, OutError)) return false;

			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> AuthorityScopes;
			FString MechanicsError;
			if (!OperationOwners::AnimationRigCommands::TryResolveMechanics(
					CommandId, Mechanics, AuthorityScopes, MechanicsError)
				|| !Mechanics.IsValid())
			{
				return Fail(OutError, TEXT("animation_rig_graph_domain_mechanics_unavailable"), CommandId, MechanicsError);
			}
			const FUeAgentCommandResult Result = OperationOwners::AnimationRigCommands::Submit(
				Mechanics.ToSharedRef(),
				Request.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("animation-rig-graph-domain-provider"), AuthorityScopes, Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				SetWorkspaceErrorFromCommandFailure(
					Result.GetErrorJson(),
					TEXT("animation_rig_graph_domain_native_command_failed"),
					CommandId,
					OutError);
				return false;
			}
			OutPayload = Result.GetPayloadJson();
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
						IKRetargeterType, SkeletonType},
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

		bool ValidateMissingTarget(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(
					OutError, TEXT("animation_rig_graph_domain_target_invalid"),
					Target);
			}
			if (bPresent)
			{
				return Fail(
					OutError, TEXT("animation_rig_graph_domain_stale_revision"),
					Target, TEXT("existing"));
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
				return Fail(
					OutError,
					TEXT("animation_rig_graph_domain_skeleton_create_not_supported"),
					Target);
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
				return Fail(
					OutError, TEXT("animation_rig_graph_domain_create_type_invalid"),
					RootType);
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
				return Fail(
					OutError, TEXT("animation_rig_graph_domain_create_contract_invalid"),
					Target);
			}
			FString Declared;
			if (Create->TryGetStringField(TEXT("asset_name"), Declared)
				&& Declared != ExpectedName)
			{
				return Fail(
					OutError, TEXT("animation_rig_graph_domain_create_name_mismatch"),
					Declared, ExpectedName);
			}
			if (Create->TryGetStringField(TEXT("package_path"), Declared))
			{
				while (Declared.EndsWith(TEXT("/")))
				{
					Declared.LeftChopInline(1);
				}
				if (Declared != ExpectedPath)
				{
					return Fail(
						OutError, TEXT("animation_rig_graph_domain_create_path_mismatch"),
						Declared, ExpectedPath);
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
			FString JsonError;
			FString TerminalStatus;
			FString MutationTarget;
			FString MutationRevision;
			FString ReceiptId;
			bool bMutationApplied = false;
			bool bCompileSettled = false;
			bool bSaved = false;
			bool bReloaded = false;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid()
				|| !OutMutation->TryGetStringField(
					TEXT("terminal_status"), TerminalStatus)
				|| TerminalStatus != TEXT("succeeded")
				|| !OutMutation->TryGetStringField(
					TEXT("target_ref"), MutationTarget)
				|| ObjectPathFromPackageIdentity(MutationTarget)
					!= ObjectPathFromPackageIdentity(Target)
				|| !OutMutation->TryGetStringField(
					TEXT("revision"), MutationRevision)
				|| MutationRevision.IsEmpty()
				|| !OutMutation->TryGetBoolField(
					TEXT("mutation_applied"), bMutationApplied)
				|| !OutMutation->TryGetBoolField(
					TEXT("compile_settled"), bCompileSettled)
				|| !OutMutation->TryGetBoolField(TEXT("saved"), bSaved)
				|| !OutMutation->TryGetBoolField(TEXT("reloaded"), bReloaded)
				|| !OutMutation->TryGetStringField(
					TEXT("persistence_receipt_id"), ReceiptId)
				|| ReceiptId.IsEmpty()
				|| !bMutationApplied || !bCompileSettled || !bSaved || !bReloaded)
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_domain_lifecycle_receipt_invalid"),
					MutationTarget,
					JsonError);
			}
			OutReceipt.bMutationApplied = bMutationApplied;
			OutReceipt.bCompileSettled = bCompileSettled;
			OutReceipt.bPackageSaved = bSaved;
			OutReceipt.bUnloadedReloaded = bReloaded;
			OutReceipt.PersistenceReceiptId = MoveTemp(ReceiptId);
			return true;
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
			FWorkspaceError& OutError)
		{
			if (CommandId != TEXT("animation.control_rig.hierarchy_graph.apply"))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
			if (!NativeParams.IsValid()
				|| !NativeParams->TryGetArrayField(TEXT("edits"), Edits)
				|| !Edits || Edits->IsEmpty())
			{
				return Fail(OutError,
					TEXT("animation_rig_graph_domain_space_edit_required"),
					CommandId);
			}
			bool bNeedsHierarchySpace = false;
			for (const TSharedPtr<FJsonValue>& EditValue : *Edits)
			{
				const TSharedPtr<FJsonObject> Edit = EditValue.IsValid()
					? EditValue->AsObject() : nullptr;
				FString Intent;
				FString SemanticId;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| !Edit->TryGetStringField(TEXT("semantic_id"), SemanticId))
				{
					return Fail(OutError,
						TEXT("animation_rig_graph_domain_space_edit_invalid"),
						CommandId);
				}
				bNeedsHierarchySpace |= Intent != TEXT("delete")
					&& SemanticId.StartsWith(TEXT("rig_element:control:"));
			}
			if (!bNeedsHierarchySpace)
			{
				return true;
			}
			const TSharedPtr<FJsonObject>* Space = nullptr;
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			FString SubjectRef;
			FString SemanticSpace;
			FString WriteSpaceRef;
			FString ReadbackSpaceRef;
			FString Units;
			if (!Root.IsValid()
				|| !Root->TryGetObjectField(TEXT("space_contract"), Space)
				|| !Space || !Space->IsValid()
				|| !(*Space)->TryGetStringField(TEXT("subject_ref"), SubjectRef)
				|| SubjectRef.IsEmpty()
				|| !(*Space)->TryGetStringField(TEXT("semantic_space"), SemanticSpace)
				|| SemanticSpace != TEXT("parent")
				|| !(*Space)->TryGetStringField(TEXT("write_space_ref"), WriteSpaceRef)
				|| !WriteSpaceRef.StartsWith(TEXT("Bone:"))
				|| !(*Space)->TryGetStringField(TEXT("readback_space_ref"), ReadbackSpaceRef)
				|| ReadbackSpaceRef != WriteSpaceRef
				|| !(*Space)->TryGetStringField(TEXT("units"), Units)
				|| Units != TEXT("centimeters")
				|| !(*Space)->TryGetObjectField(TEXT("transform"), Transform)
				|| !Transform || !Transform->IsValid())
			{
				return Fail(OutError,
					TEXT("animation_rig_graph_domain_space_contract_required"),
					CommandId);
			}
			if (!IsIdentityVector(*Transform, TEXT("translation"), 0.0)
				|| !IsIdentityVector(*Transform, TEXT("rotation_degrees"), 0.0)
				|| !IsIdentityVector(*Transform, TEXT("scale"), 1.0))
			{
				return Fail(OutError,
					TEXT("animation_rig_graph_domain_space_transform_unsupported"),
					SubjectRef);
			}
			for (const TSharedPtr<FJsonValue>& EditValue : *Edits)
			{
				const TSharedPtr<FJsonObject> Edit = EditValue.IsValid()
					? EditValue->AsObject() : nullptr;
				FString Intent;
				FString SemanticId;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| !Edit->TryGetStringField(TEXT("semantic_id"), SemanticId))
				{
					return Fail(OutError,
						TEXT("animation_rig_graph_domain_space_edit_invalid"),
						CommandId);
				}
				if (Intent == TEXT("delete")
					|| !SemanticId.StartsWith(TEXT("rig_element:control:")))
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* Value = nullptr;
				FString ParentKey;
				if (!Edit->TryGetObjectField(TEXT("value"), Value)
					|| !Value || !Value->IsValid()
					|| !(*Value)->TryGetStringField(
						TEXT("parent_native_key"), ParentKey)
					|| ParentKey != WriteSpaceRef)
				{
					return Fail(OutError,
						TEXT("animation_rig_graph_domain_space_parent_mismatch"),
						SubjectRef);
				}
			}
			return true;
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
							IKRigDefinitionType, IKRetargeterType, SkeletonType}),
					Document,
					OutError))
			{
				return false;
			}
			FString RootType;
			if (!Document.Root.IsValid()
				|| !Document.Root->TryGetStringField(TEXT("type"), RootType)
				|| !IsSupportedRootType(RootType))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_domain_schema_or_type_invalid"),
					RootType);
			}

			FString MutationPayload;
			if (Document.ExpectedRevision == TEXT("missing"))
			{
				if (!ValidateMissingTarget(Target, OutError)) return false;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Document.Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_create_contract_required"));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_create_then_edit_not_atomic"));
				}
				FString CreateCommandId;
				TSharedPtr<FJsonObject> Native;
				if (!TryBuildCreateParams(
						Target, RootType, *Create,
						CreateCommandId, Native, OutError)
					|| !SubmitAnimationRigCommand(
						CreateCommandId, Native, MutationPayload, OutError))
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
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_stale_revision"),
						Target,
						CurrentRevision);
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_create_requires_missing"));
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
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_operation_contract_required"));
				}
				if (!IsAllowedOperation(RootType, CommandId))
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_operation_not_supported"),
						CommandId);
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**NativeParams);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(
						OutError,
						TEXT("animation_rig_graph_domain_operation_target_mismatch"),
						CommandId);
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!ValidateExplicitSpaceContract(
						Document.Root, CommandId, Native, OutError)
					|| !TryNormalizeAnimationRigGraphCandidateBindings(
						Target, Native, OutError)
					|| !SubmitAnimationRigCommand(
						CommandId, Native, MutationPayload, OutError))
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
			FString CompileStatus;
			if (!AfterSnapshot.IsValid()
				|| !AfterSnapshot->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError,
					TEXT("animation_rig_graph_domain_compile_not_settled"),
					Target,
					CompileStatus);
			}
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
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError) || !Params.IsValid())
		{
			return Fail(OutError, TEXT("animation_rig_graph_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target) || Target != TargetIdentity)
		{
			return Fail(OutError, TEXT("animation_rig_graph_domain_target_identity_mismatch"), TargetIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(OutError, TEXT("animation_rig_graph_domain_export_not_registered"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, nullptr, OutError);
		}
		return Fail(OutError, TEXT("animation_rig_graph_domain_command_not_supported"), CommandId);
	}
}
