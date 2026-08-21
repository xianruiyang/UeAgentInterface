#include "DomainAdapters/AudioGraph/UeAgentAudioGraphDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AudioGraph/UeAgentAudioGraphCandidateBindingNormalizer.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AudioCommands/UeAgentAudioCommandOperationOwner.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AudioGraph
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	namespace
	{
		const FString SchemaId = TEXT("uai.audio_graph.edit/1");
		const FString MetaSoundSourceType = TEXT("/Script/MetasoundEngine.MetaSoundSource");
		const FString SoundCueType = TEXT("/Script/Engine.SoundCue");

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
			return Type == MetaSoundSourceType || Type == SoundCueType;
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
			const FString RequestId = TEXT("audio-graph-domain-")
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
				return Fail(OutError, TEXT("audio_graph_domain_internal_request_invalid"), Diagnostic.Code, Diagnostic.Message);
			}
			return true;
		}

		bool SubmitAudioCommand(
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
				return Fail(OutError, TEXT("audio_graph_domain_registry_unavailable"), CompositionError);
			}
			const FUeAgentFrozenCommandContract* Contract = Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(OutError, TEXT("audio_graph_domain_command_missing"), CommandId);
			}
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(CommandId, Contract->GetContractHash(), ParamsJson, Request, OutError)) return false;

			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
			FString MechanicsError;
			if (!OperationOwners::AudioCommands::TryCreateMechanicsHandlers(Mechanics, MechanicsError))
			{
				return Fail(OutError, TEXT("audio_graph_domain_mechanics_unavailable"), CommandId, MechanicsError);
			}
			const TSharedRef<IUeAgentCommandHandler>* Handler = Mechanics.Find(CommandId);
			if (!Handler)
			{
				return Fail(OutError, TEXT("audio_graph_domain_mechanics_unavailable"), CommandId);
			}
			const TArray<FString> AuthorityScopes = {
				TEXT("uai:query"),
				TEXT("uai:operate"),
				TEXT("uai:authoring:hjson:import"),
				TEXT("audio_media_graph.write"),
			};
			const FUeAgentCommandResult Result = OperationOwners::AudioCommands::Submit(
				*Handler,
				Request.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("audio-graph-domain-provider"), AuthorityScopes, Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				SetWorkspaceErrorFromCommandFailure(
					Result.GetErrorJson(),
					TEXT("audio_graph_domain_native_command_failed"),
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
					TArray<FString>{MetaSoundSourceType, SoundCueType},
					Owner,
					Capture,
					OutError))
			{
				return false;
			}
			const FString RootType = Owner->GetClass()->GetPathName();
			if (!IsSupportedRootType(RootType))
			{
				return Fail(
					OutError, TEXT("audio_graph_domain_target_type_invalid"),
					RootType);
			}
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
					OutError, TEXT("audio_graph_domain_target_invalid"), Target);
			}
			if (bPresent)
			{
				return Fail(
					OutError, TEXT("audio_graph_domain_stale_revision"),
					Target, TEXT("existing"));
			}
			return true;
		}

		bool TryBuildCreateParams(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Create,
			TSharedPtr<FJsonObject>& OutNative,
			FWorkspaceError& OutError)
		{
			const FString AssetKind = RootType == MetaSoundSourceType
				? TEXT("metasound_source") : TEXT("sound_cue");
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
					OutError, TEXT("audio_graph_domain_create_contract_invalid"),
					Target);
			}
			FString Declared;
			if (Create->TryGetStringField(TEXT("asset_kind"), Declared)
				&& Declared != AssetKind)
			{
				return Fail(
					OutError, TEXT("audio_graph_domain_create_kind_mismatch"),
					Declared, AssetKind);
			}
			if (Create->TryGetStringField(TEXT("asset_name"), Declared)
				&& Declared != ExpectedName)
			{
				return Fail(
					OutError, TEXT("audio_graph_domain_create_name_mismatch"),
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
						OutError, TEXT("audio_graph_domain_create_path_mismatch"),
						Declared, ExpectedPath);
				}
			}
			OutNative = MakeShared<FJsonObject>(*Create);
			OutNative->SetStringField(TEXT("asset_kind"), AssetKind);
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
					OutError, TEXT("audio_graph_domain_lifecycle_receipt_invalid"),
					MutationTarget, JsonError);
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
			if (RootType == MetaSoundSourceType)
			{
				return CommandId == TEXT("metasound.graph.interface.mutate")
					|| CommandId == TEXT("metasound.graph.node.mutate")
					|| CommandId == TEXT("metasound.graph.link.mutate")
					|| CommandId == TEXT("metasound.graph.default.mutate")
					|| CommandId == TEXT("metasound.graph.compile_settle");
			}
			return RootType == SoundCueType
				&& (CommandId == TEXT("sound_cue.graph.node.mutate")
					|| CommandId == TEXT("sound_cue.graph.relation.mutate")
					|| CommandId == TEXT("sound_cue.graph.root.mutate")
					|| CommandId == TEXT("sound_cue.graph.compile_settle"));
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
						TEXT("audio_graph"),
						SchemaId,
						TArray<FString>{MetaSoundSourceType, SoundCueType}),
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
					OutError, TEXT("audio_graph_domain_schema_or_type_invalid"),
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
						OutError, TEXT("audio_graph_domain_create_contract_required"));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError, TEXT("audio_graph_domain_create_then_edit_not_atomic"));
				}
				TSharedPtr<FJsonObject> Native;
				if (!TryBuildCreateParams(
						Target, RootType, *Create, Native, OutError)
					|| !SubmitAudioCommand(
						TEXT("audio_media.asset.create"), Native,
						MutationPayload, OutError))
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
						OutError, TEXT("audio_graph_domain_stale_revision"),
						Target, CurrentRevision);
				}
				if (Document.Root->HasField(TEXT("create")))
				{
					return Fail(
						OutError, TEXT("audio_graph_domain_create_requires_missing"));
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
						OutError, TEXT("audio_graph_domain_operation_contract_required"));
				}
				if (!IsAllowedOperation(RootType, CommandId))
				{
					return Fail(
						OutError, TEXT("audio_graph_domain_operation_not_supported"),
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
						OutError, TEXT("audio_graph_domain_operation_target_mismatch"),
						CommandId);
				}
				Native->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				if (!TryNormalizeAudioGraphCandidateBindings(
						Target, Native, OutError)
					|| !SubmitAudioCommand(
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
					OutError, TEXT("audio_graph_domain_compile_not_settled"),
					Target, CompileStatus);
			}
			return TryBuildImportSuccessPayload(
				TEXT("audio-graph-domain-provider"),
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
			return Fail(OutError, TEXT("audio_graph_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target) || Target != TargetIdentity)
		{
			return Fail(OutError, TEXT("audio_graph_domain_target_identity_mismatch"), TargetIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(OutError, TEXT("audio_graph_domain_export_not_registered"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, nullptr, OutError);
		}
		return Fail(OutError, TEXT("audio_graph_domain_command_not_supported"), CommandId);
	}
}
