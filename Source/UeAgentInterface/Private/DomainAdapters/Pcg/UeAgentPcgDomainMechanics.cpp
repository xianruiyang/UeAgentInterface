#include "DomainAdapters/Pcg/UeAgentPcgDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"
#include "PCGGraph.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::Pcg
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.pcg.edit/1");
		const FString GraphType = TEXT("/Script/PCG.PCGGraph");
		const FString NodeCarrierType = TEXT("/Script/PCG.PCGNode");

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
			const FString RequestId = TEXT("pcg-domain-")
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
			Bytes.Append(
				reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(
					OutError,
					TEXT("pcg_domain_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitPcgCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry;
			TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
			FString CompositionError;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, Dispatch, CompositionError)
				|| !Registry.IsValid())
			{
				return Fail(
					OutError, TEXT("pcg_domain_registry_unavailable"),
					CompositionError);
			}
			const CommandContract::FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(
					OutError, TEXT("pcg_domain_command_missing"), CommandId);
			}
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(
					CommandId, Contract->GetContractHash(), ParamsJson,
					Request, OutError))
			{
				return false;
			}
			TSharedPtr<IUeAgentCommandHandler> Handler;
			TArray<FString> AuthorityScopes;
			FString MechanicsError;
			if (!OperationOwners::PcgCommands::TryResolveMechanics(
					CommandId, Handler, AuthorityScopes, MechanicsError)
				|| !Handler.IsValid())
			{
				return Fail(
					OutError, TEXT("pcg_domain_mechanics_unavailable"),
					CommandId, MechanicsError);
			}
			AuthorityScopes.AddUnique(TEXT("uai:query"));
			AuthorityScopes.AddUnique(TEXT("uai:operate"));
			AuthorityScopes.AddUnique(TEXT("uai:authoring:hjson:import"));
			const FUeAgentCommandResult Result =
				OperationOwners::PcgCommands::Submit(
					Handler.ToSharedRef(), Request.GetValue(),
					Contract::FUeAgentCallerContext(
						TEXT("pcg-domain-provider"), AuthorityScopes,
						Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				return Fail(
					OutError, TEXT("pcg_domain_native_command_failed"),
					CommandId, Result.GetErrorJson());
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>* OutSnapshot,
			UObject** OutObject,
			FWorkspaceError& OutError)
		{
			UObject* Owner = nullptr;
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
					Target, GraphType, Owner, Capture, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Snapshot;
			if (!TryBuildCaptureSnapshot(
					Target, GraphType, Capture, Snapshot, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (OutSnapshot) *OutSnapshot = Snapshot;
			if (OutObject) *OutObject = Owner;
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
					OutError, TEXT("pcg_domain_target_invalid"), Target);
			}
			if (bPresent)
			{
				return Fail(
					OutError, TEXT("pcg_domain_stale_revision"), Target,
					TEXT("existing"));
			}
			return true;
		}

		bool NormalizeNodeCandidates(
			UObject& TargetObject,
			const TArray<TSharedPtr<FJsonValue>>& Edits,
			FWorkspaceError& OutError)
		{
			UPCGGraph* const Graph = Cast<UPCGGraph>(&TargetObject);
			if (!Graph || Graph->GetClass() != UPCGGraph::StaticClass())
			{
				return Fail(
					OutError,
					TEXT("pcg_candidate_target_type_unsupported"),
					TargetObject.GetClass()->GetPathName());
			}
			for (int32 Index = 0; Index < Edits.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Edit =
					Edits[Index].IsValid() ? Edits[Index]->AsObject() : nullptr;
				FString Intent;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (!Edit.IsValid()
					|| !Edit->TryGetStringField(TEXT("intent"), Intent)
					|| Intent != TEXT("create")
					|| !Edit->TryGetObjectField(TEXT("value"), Value)
					|| !Value || !Value->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				if (!(*Value)->HasField(TEXT("candidate")))
				{
					return Fail(
						OutError,
						TEXT("pcg_candidate_selector_required"),
						FString::FromInt(Index),
						FString::Printf(
							TEXT("/root/operation/params/edits/%d/value/candidate"),
							Index));
				}
				const FString Pointer = FString::Printf(
					TEXT("/root/operation/params/edits/%d/value/candidate"),
					Index);
				if (!(*Value)->TryGetObjectField(TEXT("candidate"), Candidate)
					|| !Candidate || !Candidate->IsValid())
				{
					return Fail(
						OutError,
						TEXT("domain_hjson_candidate_binding_invalid"),
						FString(),
						Pointer);
				}
				FDomainHjsonCandidateSelector Selector;
				if (!TryReadCandidateSelector(
						*Candidate, Pointer, Selector, OutError))
				{
					return false;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
					: (*Candidate)->Values)
				{
					if (Field.Key != TEXT("name")
						&& Field.Key != TEXT("id")
						&& Field.Key != TEXT("candidate_id"))
					{
						return Fail(
							OutError,
							TEXT("pcg_candidate_selector_field_unsupported"),
							Field.Key,
							Pointer + TEXT("/") + Field.Key);
					}
				}
				QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
				QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
				if (!QueryOwners::Pcg::TryResolvePcgNodeCandidate(
						*Graph,
						Selector.Name,
						Selector.Id,
						ResolvedCandidate,
						CandidateError))
				{
					return Fail(
						OutError,
						CandidateError.Code.IsEmpty()
							? FString(TEXT("pcg_candidate_selector_invalid"))
							: CandidateError.Code,
						CandidateError.SubjectId,
						CandidateError.Detail);
				}
				if (!ResolvedCandidate.bSupported)
				{
					return Fail(
						OutError,
						TEXT("pcg_candidate_action_kind_unsupported"),
						ResolvedCandidate.ActionKind,
						Pointer);
				}
				FString Existing;
				if ((*Value)->TryGetStringField(TEXT("carrier_type_ref"), Existing)
					&& Existing != NodeCarrierType)
				{
					return Fail(
						OutError, TEXT("pcg_candidate_carrier_conflict"),
						Existing, Pointer);
				}
				if ((*Value)->HasField(TEXT("settings_type_ref")))
				{
					return Fail(
						OutError,
						TEXT("pcg_candidate_legacy_settings_type_unsupported"),
						FString(),
						Pointer + TEXT("/settings_type_ref"));
				}
				if ((*Value)->TryGetStringField(TEXT("action_kind"), Existing)
					&& Existing != ResolvedCandidate.ActionKind)
				{
					return Fail(
						OutError, TEXT("pcg_candidate_action_kind_mismatch"),
						Existing, Pointer + TEXT("/action_kind"));
				}
				(*Candidate)->SetStringField(
					TEXT("name"), ResolvedCandidate.Name);
				(*Candidate)->SetStringField(
					TEXT("id"), ResolvedCandidate.Id);
				(*Candidate)->RemoveField(TEXT("candidate_id"));
				(*Value)->RemoveField(TEXT("carrier_type_ref"));
				(*Value)->RemoveField(TEXT("action_kind"));
			}
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
			FString MutationTarget;
			FString MutationRevision;
			FString ReceiptId;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid()
				|| !OutMutation->TryGetStringField(
					TEXT("target_ref"), MutationTarget)
				|| ObjectPathFromPackageIdentity(MutationTarget)
					!= ObjectPathFromPackageIdentity(Target)
				|| !OutMutation->TryGetStringField(
					TEXT("revision"), MutationRevision)
				|| MutationRevision.IsEmpty()
				|| !OutMutation->TryGetStringField(
					TEXT("persistence_receipt_id"), ReceiptId)
				|| !ReceiptId.StartsWith(TEXT("reload:")))
			{
				return Fail(
					OutError, TEXT("pcg_domain_lifecycle_receipt_invalid"),
					MutationTarget, JsonError);
			}
			OutReceipt.bMutationApplied = true;
			OutReceipt.bCompileSettled = true;
			OutReceipt.bPackageSaved = true;
			OutReceipt.bUnloadedReloaded = true;
			OutReceipt.PersistenceReceiptId = MoveTemp(ReceiptId);
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
						TEXT("pcg"), SchemaId, GraphType),
					Document,
					OutError))
			{
				return false;
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
						OutError, TEXT("pcg_domain_create_contract_required"));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError, TEXT("pcg_domain_create_then_edit_not_atomic"));
				}
				const FString Package = PackageIdentity(Target);
				const FString ExpectedName =
					FPackageName::GetLongPackageAssetName(Package);
				const FString ExpectedPath =
					FPackageName::GetLongPackagePath(Package);
				FString Declared;
				if ((*Create)->TryGetStringField(TEXT("asset_name"), Declared)
					&& Declared != ExpectedName)
				{
					return Fail(
						OutError, TEXT("pcg_domain_create_name_mismatch"),
						Declared, ExpectedName);
				}
				if ((*Create)->TryGetStringField(TEXT("package_path"), Declared))
				{
					while (Declared.EndsWith(TEXT("/")))
					{
						Declared.LeftChopInline(1);
					}
					if (Declared != ExpectedPath)
					{
						return Fail(
							OutError, TEXT("pcg_domain_create_path_mismatch"),
							Declared, ExpectedPath);
					}
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Create);
				Native->SetStringField(TEXT("asset_name"), ExpectedName);
				Native->SetStringField(TEXT("package_path"), ExpectedPath);
				if (!SubmitPcgCommand(
						TEXT("pcg.graph.asset.create"), Native,
						MutationPayload, OutError))
				{
					return false;
				}
			}
			else
			{
				FString BeforePayload;
				FString CurrentRevision;
				UObject* TargetObject = nullptr;
				if (!TryRead(
						Target, BeforePayload, CurrentRevision, nullptr,
						&TargetObject, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
					return Fail(
						OutError, TEXT("pcg_domain_stale_revision"),
						Target, CurrentRevision);
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(
						OutError, TEXT("pcg_domain_create_requires_missing"));
				}
				const TSharedPtr<FJsonObject>* Operation = nullptr;
				const TSharedPtr<FJsonObject>* Params = nullptr;
				FString CommandId;
				if (!Document.Root->TryGetObjectField(
						TEXT("operation"), Operation)
					|| !Operation || !Operation->IsValid()
					|| !(*Operation)->TryGetStringField(
						TEXT("command_id"), CommandId)
					|| !(*Operation)->TryGetObjectField(TEXT("params"), Params)
					|| !Params || !Params->IsValid())
				{
					return Fail(
						OutError, TEXT("pcg_domain_operation_contract_required"));
				}
				if (CommandId != TEXT("pcg.graph.apply")
					&& CommandId != TEXT("pcg.graph_parameters.apply"))
				{
					return Fail(
						OutError, TEXT("pcg_domain_operation_not_supported"),
						CommandId);
				}
				const TSharedPtr<FJsonObject> Native =
					MakeShared<FJsonObject>(**Params);
				FString NativeTarget;
				if (Native->TryGetStringField(TEXT("target_ref"), NativeTarget)
					&& ObjectPathFromPackageIdentity(NativeTarget)
						!= ObjectPathFromPackageIdentity(Target))
				{
					return Fail(
						OutError, TEXT("pcg_domain_operation_target_mismatch"),
						NativeTarget, Target);
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (CommandId == TEXT("pcg.graph.apply"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Edits = nullptr;
					if (!Native->TryGetArrayField(TEXT("edits"), Edits)
						|| !Edits)
					{
						return Fail(
							OutError, TEXT("pcg_domain_edits_required"));
					}
					if (!TargetObject
						|| !NormalizeNodeCandidates(
							*TargetObject, *Edits, OutError))
					{
						return false;
					}
				}
				if (!SubmitPcgCommand(
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
					Target, AfterPayload, AfterRevision, &AfterSnapshot,
					nullptr, OutError))
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
					OutError, TEXT("pcg_domain_generate_not_settled"),
					Target, CompileStatus);
			}
			return TryBuildImportSuccessPayload(
				TEXT("pcg-domain-provider"), Target, AfterRevision,
				LifecycleReceipt, AfterSnapshot, Mutation,
				OutPayload, OutError);
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
			return Fail(
				OutError, TEXT("pcg_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| ObjectPathFromPackageIdentity(Target)
				!= ObjectPathFromPackageIdentity(TargetIdentity))
		{
			return Fail(
				OutError, TEXT("pcg_domain_target_identity_mismatch"),
				TargetIdentity, Target);
		}
		if (RootTypeIdentity != GraphType)
		{
			return Fail(
				OutError, TEXT("pcg_domain_root_type_invalid"),
				RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError, TEXT("pcg_domain_export_not_registered"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(
				Target, OutPayloadJson, Revision, nullptr, nullptr, OutError);
		}
		return Fail(
			OutError, TEXT("pcg_domain_command_not_supported"), CommandId);
	}
}
