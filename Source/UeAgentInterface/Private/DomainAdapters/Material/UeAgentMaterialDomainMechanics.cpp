#include "DomainAdapters/Material/UeAgentMaterialDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Material
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString SchemaId = TEXT("uai.material.edit/1");
		const FString MaterialType = TEXT("/Script/Engine.Material");
		const FString FunctionType = TEXT("/Script/Engine.MaterialFunction");
		const FString InstanceType = TEXT("/Script/Engine.MaterialInstanceConstant");

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
			return Type == MaterialType || Type == FunctionType
				|| Type == InstanceType;
		}

		FString ObjectPathFromPackage(const FString& Target)
		{
			int32 Dot = INDEX_NONE;
			if (Target.FindLastChar(TEXT('.'), Dot))
			{
				return Target;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
			return AssetName.IsEmpty() ? Target : Target + TEXT(".") + AssetName;
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError, TEXT("material_domain_json_invalid"), Error);
			}
			return true;
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("material-domain-")
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
				return Fail(
					OutError,
					TEXT("material_domain_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitMaterialCommand(
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
				return Fail(OutError, TEXT("material_domain_registry_unavailable"), CompositionError);
			}
			const CommandContract::FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(OutError, TEXT("material_domain_command_missing"), CommandId);
			}
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(CommandId, Contract->GetContractHash(), ParamsJson, Request, OutError))
			{
				return false;
			}
			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			FString MechanicsError;
			if (!OperationOwners::MaterialCommands::TryResolveMechanicsHandler(
					CommandId, Mechanics, MechanicsError)
				|| !Mechanics.IsValid())
			{
				return Fail(OutError, TEXT("material_domain_mechanics_unavailable"), CommandId, MechanicsError);
			}
			const FUeAgentCommandResult Result = Mechanics->Submit(
				Request.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("material-domain-provider"),
					{TEXT("uai:query"), TEXT("uai:operate"),
						TEXT("material_niagara_graph.read"),
						TEXT("material_niagara_graph.write")},
					Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				return Fail(
					OutError,
					TEXT("material_domain_native_command_failed"),
					CommandId,
					Result.GetErrorJson());
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool TryRead(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, RootType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, RootType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot)
			{
				*OutSnapshot = Snapshot;
			}
			return TryCanonicalizeObject(Snapshot, OutPayload, OutError);
		}

		bool ValidateAndStripExpressionCandidates(
			const FString& Target,
			const TArray<TSharedPtr<FJsonValue>>& Edits,
			FWorkspaceError& OutError)
		{
			UObject* TargetObject = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPathFromPackage(Target));
			if (!TargetObject)
			{
				return Fail(
					OutError,
					TEXT("material_candidate_target_unresolved"),
					Target);
			}
			for (int32 Index = 0; Index < Edits.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Edit =
					Edits[Index].IsValid() ? Edits[Index]->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				FString Intent;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| Intent != TEXT("create")
					|| !Edit->TryGetObjectField(TEXT("value"), Value)
					|| !Value || !Value->IsValid())
				{
					continue;
				}
				FString ClassRef;
				if (!(*Value)->TryGetStringField(TEXT("class_ref"), ClassRef))
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				FString CandidateName;
				FString BindingRevision;
				if (!(*Value)->TryGetObjectField(TEXT("candidate"), Candidate)
					|| !Candidate || !Candidate->IsValid()
					|| !(*Candidate)->TryGetStringField(
						TEXT("name"), CandidateName)
					|| !(*Candidate)->TryGetStringField(
						TEXT("binding_revision"), BindingRevision))
				{
					return Fail(
						OutError,
						TEXT("material_candidate_binding_required"),
						FString::FromInt(Index));
				}
				if (CandidateName != ClassRef)
				{
					return Fail(
						OutError,
						TEXT("material_candidate_name_class_mismatch"),
						CandidateName,
						ClassRef);
				}
				QueryOwners::Material::FMaterialExpressionCandidateQueryError
					CandidateError;
				if (!QueryOwners::Material::
						IsCurrentMaterialExpressionCandidateBinding(
							*TargetObject,
							CandidateName,
							BindingRevision,
							CandidateError))
				{
					return Fail(
						OutError,
						CandidateError.Code.IsEmpty()
							? FString(TEXT("material_candidate_binding_invalid"))
							: CandidateError.Code,
						CandidateError.SubjectId,
						CandidateError.Detail);
				}
				(*Value)->RemoveField(TEXT("candidate"));
			}
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& RootType,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("material"), SchemaId, RootType),
					Document,
					OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>& Root = Document.Root;
			const FString& ExpectedRevision = Document.ExpectedRevision;

			FString BeforePayload;
			FString CurrentRevision;
			bool bExists = ExpectedRevision != TEXT("missing");
			if (bExists)
			{
				if (!TryRead(
						Target, RootType, BeforePayload, CurrentRevision,
						nullptr, OutError))
				{
					return false;
				}
			}
			else
			{
				FString Filename;
				const bool bValidPackage =
					FPackageName::IsValidLongPackageName(Target)
					&& FPackageName::TryConvertLongPackageNameToFilename(
						Target, Filename, FPackageName::GetAssetPackageExtension());
				if (!bValidPackage)
				{
					return Fail(OutError, TEXT("material_domain_target_invalid"), Target);
				}
				if (FindPackage(nullptr, *Target)
					|| IFileManager::Get().FileExists(*Filename))
				{
					return Fail(
						OutError, TEXT("material_domain_stale_revision"), Target,
						TEXT("existing"));
				}
			}
			FString MutationPayload;
			if (!bExists)
			{
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Root->TryGetObjectField(TEXT("create"), Create)
					|| !Create || !Create->IsValid())
				{
					return Fail(OutError, TEXT("material_domain_create_contract_required"));
				}
				if (RootType == MaterialType || RootType == FunctionType)
				{
					if (Root->HasField(TEXT("edits")))
					{
						return Fail(OutError, TEXT("material_domain_create_then_edit_not_atomic"));
					}
					const FString ExpectedKind = RootType == FunctionType
						? FString(TEXT("material_function"))
						: FString(TEXT("material"));
					FString DeclaredKind;
					if ((*Create)->TryGetStringField(
							TEXT("material_kind"), DeclaredKind)
						&& DeclaredKind != ExpectedKind)
					{
						return Fail(
							OutError,
							TEXT("material_domain_create_kind_mismatch"),
							DeclaredKind,
							ExpectedKind);
					}
					const TSharedPtr<FJsonObject> NativeCreate =
						MakeShared<FJsonObject>(**Create);
					NativeCreate->SetStringField(
						TEXT("material_kind"), ExpectedKind);
					if (!SubmitMaterialCommand(TEXT("material.asset.create"), NativeCreate,
							MutationPayload, OutError)) return false;
				}
				else if (RootType == InstanceType)
				{
					if (Root->HasField(TEXT("parameters")))
					{
						return Fail(OutError, TEXT("material_domain_create_then_edit_not_atomic"));
					}
					if (!SubmitMaterialCommand(TEXT("material_instance.asset.create"), *Create,
							MutationPayload, OutError)) return false;
				}
			}
			else
			{
				if (ExpectedRevision != CurrentRevision)
				{
					return Fail(OutError, TEXT("material_domain_stale_revision"), Target, CurrentRevision);
				}
				const TSharedPtr<FJsonObject> NativeParams = MakeShared<FJsonObject>();
				NativeParams->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackage(Target));
				if (RootType == InstanceType)
				{
					const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
					if (!Root->TryGetArrayField(TEXT("parameters"), Parameters) || !Parameters)
					{
						return Fail(OutError, TEXT("material_domain_parameters_required"));
					}
					NativeParams->SetArrayField(TEXT("parameters"), *Parameters);
					if (!SubmitMaterialCommand(TEXT("material_instance.parameters.apply"),
							NativeParams, MutationPayload, OutError)) return false;
				}
				else
				{
					const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
					if (!Root->TryGetArrayField(TEXT("edits"), Edits) || !Edits)
					{
						return Fail(OutError, TEXT("material_domain_edits_required"));
					}
					if (!ValidateAndStripExpressionCandidates(
							Target, *Edits, OutError))
					{
						return false;
					}
					NativeParams->SetArrayField(TEXT("edits"), *Edits);
					const FString Command = RootType == MaterialType
						? TEXT("material.graph.apply")
						: TEXT("material.function.graph.apply");
					if (!SubmitMaterialCommand(Command, NativeParams, MutationPayload, OutError))
					{
						return false;
					}
				}
			}

			FString AfterPayload;
			FString AfterRevision;
			TSharedPtr<FJsonObject> AfterObject;
			if (!TryRead(
					Target, RootType, AfterPayload, AfterRevision,
					&AfterObject, OutError))
			{
				return false;
			}
			FString CompileStatus;
			if (!AfterObject.IsValid()
				|| !AfterObject->TryGetStringField(
					TEXT("compile_status"), CompileStatus)
				|| CompileStatus != TEXT("valid"))
			{
				return Fail(
					OutError, TEXT("material_domain_compile_not_settled"), Target);
			}
			TSharedPtr<FJsonObject> Mutation;
			FString JsonError;
			FString MutationTarget;
			FString PersistenceReceiptId;
			if (!Json::TryParseObject(MutationPayload, Mutation, JsonError)
				|| !Mutation.IsValid()
				|| !Mutation->TryGetStringField(
					TEXT("target_ref"), MutationTarget)
				|| (MutationTarget != Target
					&& MutationTarget != ObjectPathFromPackage(Target))
				|| !Mutation->TryGetStringField(
					TEXT("revision"), PersistenceReceiptId)
				|| !PersistenceReceiptId.StartsWith(TEXT("reload:")))
			{
				return Fail(
					OutError,
					TEXT("material_domain_lifecycle_receipt_invalid"),
					MutationTarget,
					JsonError);
			}
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bCompileSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.bUnloadedReloaded = true;
			LifecycleReceipt.PersistenceReceiptId = PersistenceReceiptId;
			return TryBuildImportSuccessPayload(
				TEXT("material-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterObject,
				Mutation,
				OutPayload,
				OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
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
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, TEXT("material_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, TEXT("material_domain_target_identity_mismatch"), TargetIdentity);
		}
		if (!IsSupportedRootType(RootTypeIdentity))
		{
			return Fail(
				OutError, TEXT("material_domain_root_type_invalid"), RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target,
				RootTypeIdentity,
				CanonicalParamsJson,
				OutPayloadJson,
				OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError,
				TEXT("material_domain_export_not_registered"),
				Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, RootTypeIdentity, OutPayloadJson, Revision,
				nullptr, OutError);
		}
		return Fail(OutError, TEXT("material_domain_command_not_supported"), CommandId);
	}
}
