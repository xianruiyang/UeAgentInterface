#include "DomainAdapters/Sequence/UeAgentSequenceDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/Sequence/UeAgentSequenceHjsonContract.h"
#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::Sequence
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace ArtifactPublisher;

	namespace
	{
		const FString SchemaId = TEXT("uai.sequence.edit/1");
		const FString RootType = TEXT("/Script/LevelSequence.LevelSequence");

		class FNeverCancelled final
			: public Contract::IUeAgentCancellationSignal
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

		FString Sha256(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FUeAgentResourceTracker MakeExportResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16ll * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16ll * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16ll * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					512ll * 1024 * 1024},
			}));
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("sequence-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId,
				*RequestId,
				*CommandId,
				*ContractHash,
				*ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Encoded.Get()),
				Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitTimelineCommand(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			OutPayload.Reset();
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
					OutError,
					TEXT("sequence_domain_command_registry_unavailable"),
					CompositionError);
			}
			const CommandContract::FUeAgentFrozenCommandContract* FrozenContract =
				Registry->FindExact(CommandId);
			if (!FrozenContract)
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_command_contract_missing"),
					CommandId);
			}
			FString ParamsJson;
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError))
			{
				return false;
			}
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(
					CommandId,
					FrozenContract->GetContractHash(),
					ParamsJson,
					Request,
					OutError))
			{
				return false;
			}
			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> Scopes;
			FString MechanicsError;
			if (!TimelineSequenceCommands::TryResolveMechanics(
					CommandId, Mechanics, Scopes, MechanicsError)
				|| !Mechanics.IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_native_mechanics_unavailable"),
					CommandId,
					MechanicsError);
			}
			Scopes.AddUnique(TEXT("uai:query"));
			Scopes.AddUnique(TEXT("uai:operate"));
			const FUeAgentCommandResult Result =
				TimelineSequenceCommands::Submit(
					Mechanics.ToSharedRef(),
					Request.GetValue(),
					Contract::FUeAgentCallerContext(
						TEXT("sequence-domain-provider"),
						Scopes,
						Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				SetWorkspaceErrorFromCommandFailure(
					Result.GetErrorJson(),
					TEXT("sequence_domain_native_command_failed"),
					CommandId,
					OutError);
				return false;
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		TSharedPtr<FJsonObject> ReadParams(const FString& Target)
		{
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetBoolField(TEXT("include_bindings"), true);
			Input->SetBoolField(TEXT("include_tracks"), true);
			Input->SetBoolField(TEXT("include_keys"), true);
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			Params->SetObjectField(TEXT("input"), Input);
			return Params;
		}

		bool ParsePayload(
			const FString& PayloadJson,
			TSharedPtr<FJsonObject>& OutPayload,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Json::TryParseObject(PayloadJson, OutPayload, Error)
				|| !OutPayload.IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_native_payload_invalid"),
					Error);
			}
			return true;
		}

		bool ExtractRevision(
			const TSharedPtr<FJsonObject>& ReadPayload,
			TSharedPtr<FJsonObject>& OutRevision,
			FWorkspaceError& OutError)
		{
			OutRevision.Reset();
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* ProjectedPayload = nullptr;
			const TSharedPtr<FJsonObject>* Snapshot = nullptr;
			const TSharedPtr<FJsonObject>* Revision = nullptr;
			if (!ReadPayload.IsValid()
				|| !ReadPayload->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(
					TEXT("payload"), ProjectedPayload)
				|| !ProjectedPayload || !ProjectedPayload->IsValid()
				|| !(*ProjectedPayload)->TryGetObjectField(
					TEXT("snapshot"), Snapshot)
				|| !Snapshot || !Snapshot->IsValid()
				|| !(*Snapshot)->TryGetObjectField(TEXT("revision"), Revision)
				|| !Revision || !Revision->IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_native_revision_missing"));
			}
			FString SourceId;
			FString Generation;
			FString Sequence;
			if (!(*Revision)->TryGetStringField(TEXT("source_id"), SourceId)
				|| !(*Revision)->TryGetStringField(TEXT("generation"), Generation))
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_native_revision_invalid"));
			}
			if (!(*Revision)->TryGetStringField(TEXT("sequence"), Sequence))
			{
				double NumericSequence = 0.0;
				if (!(*Revision)->TryGetNumberField(
						TEXT("sequence"), NumericSequence)
					|| NumericSequence < 0.0
					|| NumericSequence > static_cast<double>(MAX_int64)
					|| FMath::FloorToDouble(NumericSequence) != NumericSequence)
				{
					return Fail(
						OutError,
						TEXT("sequence_domain_native_revision_invalid"));
				}
				Sequence = FString::Printf(
					TEXT("%lld"), static_cast<int64>(NumericSequence));
			}
			OutRevision = MakeShared<FJsonObject>();
			OutRevision->SetStringField(TEXT("source_id"), SourceId);
			OutRevision->SetStringField(TEXT("generation"), Generation);
			OutRevision->SetStringField(TEXT("sequence"), Sequence);
			return true;
		}

		bool StableRevision(
			const TSharedPtr<FJsonObject>& ReadPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			OutRevision.Reset();
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* ProjectedPayload = nullptr;
			const TSharedPtr<FJsonObject>* Snapshot = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!ReadPayload.IsValid()
				|| !ReadPayload->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(
					TEXT("payload"), ProjectedPayload)
				|| !ProjectedPayload || !ProjectedPayload->IsValid()
				|| !(*ProjectedPayload)->TryGetObjectField(
					TEXT("snapshot"), Snapshot)
				|| !Snapshot || !Snapshot->IsValid()
				|| !(*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
				|| !Entries || Entries->IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_observed_state_missing"));
			}
			const TSharedPtr<FJsonObject> Entry = (*Entries)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Value = nullptr;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const TSharedPtr<FJsonObject>* ObservedState = nullptr;
			FString State;
			if (!Entry.IsValid()
				|| !Entry->TryGetObjectField(TEXT("value"), Value)
				|| !Value || !Value->IsValid()
				|| !(*Value)->TryGetObjectField(TEXT("fields"), Fields)
				|| !Fields || !Fields->IsValid()
				|| !(*Fields)->TryGetObjectField(
					TEXT("observed_state"), ObservedState)
				|| !ObservedState || !ObservedState->IsValid()
				|| !(*ObservedState)->TryGetStringField(TEXT("text"), State)
				|| State.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_observed_state_missing"));
			}
			OutRevision = Sha256(State);
			return true;
		}

		FString PackageRefFromObjectPath(const FString& Target)
		{
			int32 Dot = INDEX_NONE;
			return Target.FindLastChar(TEXT('.'), Dot)
				? Target.Left(Dot)
				: Target;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutPayloadObject,
			FWorkspaceError& OutError)
		{
			return SubmitTimelineCommand(
					TEXT("timeline.sequence.read"),
					ReadParams(Target),
					OutPayload,
					OutError)
				&& ParsePayload(OutPayload, OutPayloadObject, OutError);
		}

		void CopyWorkspaceError(
			const FWorkspaceError& Error,
			FArtifactPublishError& OutError)
		{
			OutError.Set(
				Error.GetCode().IsEmpty()
					? FString(TEXT("sequence_domain_export_verification_failed"))
					: Error.GetCode(),
				Error.GetSourceUnit(),
				Error.GetJsonPointer());
		}

		class FSequenceDomainExportVerification final
			: public IArtifactPublishVerification
		{
		public:
			FSequenceDomainExportVerification(
				FString InTarget,
				FString InBaseRevision,
				FString InEntryFile)
				: Target(MoveTemp(InTarget))
				, BaseRevision(MoveTemp(InBaseRevision))
				, EntryFile(MoveTemp(InEntryFile))
				, Contract(
					GetSequenceHjsonFieldOwner(),
					GetSequenceHjsonSchemaId(),
					GetSequenceHjsonRootType())
			{
			}

			virtual bool TryVerifyStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& StagedEntryFile,
				FUeAgentResourceTracker& Resources,
				FArtifactPublishError& OutError) override
			{
				if (WorkspaceKind != EWorkspaceKind::Directory
					|| StagedEntryFile != EntryFile)
				{
					OutError.Set(
						TEXT("sequence_domain_export_workspace_identity_mismatch"),
						StagedWorkspacePath,
						StagedEntryFile);
					return false;
				}
				FWorkspaceError WorkspaceError;
				if (!TryVerifyDomainHjsonStagedWorkspace(
						StagedWorkspacePath,
						Target,
						Contract,
						BaseRevision,
						EntryFile,
						Resources,
						WorkspaceError))
				{
					CopyWorkspaceError(WorkspaceError, OutError);
					return false;
				}
				return true;
			}

			virtual bool TryRecheckTargetVersion(
				FArtifactPublishError& OutError) override
			{
				FString Payload;
				TSharedPtr<FJsonObject> ReadObject;
				FWorkspaceError WorkspaceError;
				FString CurrentRevision;
				if (!TryRead(Target, Payload, ReadObject, WorkspaceError)
					|| !StableRevision(
						ReadObject, CurrentRevision, WorkspaceError))
				{
					CopyWorkspaceError(WorkspaceError, OutError);
					return false;
				}
				if (CurrentRevision != BaseRevision)
				{
					OutError.Set(
						TEXT("sequence_domain_export_target_changed"),
						Target,
						CurrentRevision);
					return false;
				}
				return true;
			}

			virtual TMap<FString, FString>
				GetSemanticVerificationFields() const override
			{
				return {
					{TEXT("schema_id"), GetSequenceHjsonSchemaId()},
					{TEXT("root_type"), GetSequenceHjsonRootType()},
					{TEXT("field_set"), GetSequenceHjsonFieldSetId()},
					{TEXT("base_revision"), BaseRevision},
					{TEXT("staged_import_parse"), TEXT("verified")},
					{TEXT("target_version_recheck"), TEXT("verified")},
					{TEXT("asset_coverage"), TEXT("sequence_settings_only")},
				};
			}

		private:
			FString Target;
			FString BaseRevision;
			FString EntryFile;
			FDomainHjsonDocumentContract Contract;
		};

		bool TryExport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayloadJson,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			FString ProfileId(TEXT("llm_authoring"));
			if (!Params.IsValid()
				|| !Params->TryGetStringField(
					TEXT("workspace_path"), WorkspacePath))
			{
				return Fail(
					OutError,
					TEXT("content_graph_export_params_invalid"),
					Target);
			}
			Params->TryGetStringField(TEXT("profile_id"), ProfileId);
			WorkspacePath.TrimStartAndEndInline();
			ProfileId.TrimStartAndEndInline();
			if (WorkspacePath.IsEmpty()
				|| FPaths::IsRelative(WorkspacePath)
				|| ProfileId.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("workspace_path_invalid"),
					WorkspacePath);
			}

			FString ReadPayload;
			TSharedPtr<FJsonObject> ReadObject;
			FString Revision;
			if (!TryRead(Target, ReadPayload, ReadObject, OutError)
				|| !StableRevision(ReadObject, Revision, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Document;
			if (!TryBuildSequenceHjsonExportDocument(
					Target, Document, OutError)
				|| !Document.IsValid())
			{
				return false;
			}

			const FString EntryFile(TEXT("sequence.hjson"));
			const FDomainHjsonDocumentContract Contract(
				GetSequenceHjsonFieldOwner(),
				GetSequenceHjsonSchemaId(),
				GetSequenceHjsonRootType());
			FUeAgentResourceTracker Resources = MakeExportResources();
			FExportWorkspacePlanPtr Plan;
			if (!TryBuildDomainHjsonDirectoryExportPlan(
					Target,
					Contract,
					Revision,
					ProfileId,
					GetSequenceHjsonFieldOwner(),
					EntryFile,
					Document,
					Resources,
					Plan,
					OutError)
				|| !Plan.IsValid())
			{
				return false;
			}

			const TSharedRef<FSequenceDomainExportVerification> Verification =
				MakeShared<FSequenceDomainExportVerification>(
					Target, Revision, EntryFile);
			FArtifactPublishReceipt Receipt;
			FArtifactPublishError PublishError;
			if (!TryPublishExportPlan(
					FArtifactPublishRequest(
						Plan.ToSharedRef(),
						WorkspacePath,
						GetSequenceHjsonFieldOwner(),
						Verification),
					Resources,
					Receipt,
					PublishError))
			{
				return Fail(
					OutError,
					PublishError.GetCode().IsEmpty()
						? FString(TEXT("sequence_domain_export_publish_failed"))
						: PublishError.GetCode(),
					PublishError.GetPath(),
					PublishError.GetDetail());
			}

			const TSharedPtr<FJsonObject> Semantic = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Field
				: Receipt.GetSemanticVerificationFields())
			{
				Semantic->SetStringField(Field.Key, Field.Value);
			}
			const TSharedPtr<FJsonObject> Scope = MakeShared<FJsonObject>();
			Scope->SetStringField(
				TEXT("field_set"), GetSequenceHjsonFieldSetId());
			Scope->SetBoolField(TEXT("field_set_complete"), true);
			Scope->SetBoolField(TEXT("asset_structure_complete"), false);
			Scope->SetBoolField(TEXT("omitted_structure_preserved"), true);

			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(
				TEXT("command_id"), TEXT("content_graph.export_workspace"));
			Response->SetStringField(TEXT("status"), TEXT("published"));
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(
				TEXT("target_kind"), TEXT("ue.level_sequence.asset"));
			Response->SetStringField(
				TEXT("schema_id"), GetSequenceHjsonSchemaId());
			Response->SetStringField(TEXT("base_revision"), Revision);
			Response->SetStringField(
				TEXT("workspace_path"), Receipt.GetFinalPath());
			Response->SetStringField(
				TEXT("entry_file"), Receipt.GetEntryFile());
			Response->SetStringField(
				TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
			Response->SetStringField(
				TEXT("inventory_hash"), Receipt.GetInventoryHash());
			Response->SetObjectField(TEXT("scope"), Scope);
			Response->SetObjectField(TEXT("semantic_verification"), Semantic);
			return TryCanonicalizeObject(
				Response, OutPayloadJson, OutError);
		}

		bool ParseTimelineLifecycleMutation(
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			FString JsonError;
			FString OperationId;
			FString ProfileId;
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid()
				|| !OutMutation->TryGetStringField(
					TEXT("operation_id"), OperationId)
				|| OperationId.IsEmpty()
				|| !OutMutation->TryGetObjectField(
					TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetStringField(
					TEXT("profile_id"), ProfileId)
				|| ProfileId.IsEmpty()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_lifecycle_receipt_invalid"),
					OperationId,
					JsonError);
			}
			OutReceipt.bMutationApplied = true;
			OutReceipt.bCompileSettled = true;
			OutReceipt.bPackageSaved = true;
			OutReceipt.bUnloadedReloaded = true;
			OutReceipt.PersistenceReceiptId =
				TEXT("timeline-operation:") + OperationId;
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayloadJson,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("sequence"), SchemaId, RootType),
					Document,
					OutError))
			{
				return false;
			}
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(
					OutError, TEXT("sequence_domain_target_invalid"), Target);
			}
			FSequenceHjsonPatch Patch;
			if (!TryResolveSequenceHjsonPatch(
					Target,
					bPresent,
					Document.Root,
					Patch,
					OutError)
				|| !Patch.IsValid())
			{
				return false;
			}
			FString MutationPayload;
			if (!bPresent)
			{
				if (Document.ExpectedRevision != TEXT("missing"))
				{
					return Fail(
						OutError, TEXT("sequence_domain_stale_revision"),
						Target, TEXT("missing"));
				}
				if (Document.Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError, TEXT("sequence_domain_create_then_edit_not_atomic"));
				}
				const TSharedPtr<FJsonObject>* DisplayRate = nullptr;
				const TSharedPtr<FJsonObject>* TickResolution = nullptr;
				if (!Patch.Structure->TryGetObjectField(
						TEXT("display_rate"), DisplayRate)
					|| !DisplayRate || !DisplayRate->IsValid()
					|| !Patch.Structure->TryGetObjectField(
						TEXT("tick_resolution"), TickResolution)
					|| !TickResolution || !TickResolution->IsValid())
				{
					return Fail(
						OutError, TEXT("sequence_domain_rates_required"));
				}
				const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetStringField(
					TEXT("destination_ref"), PackageRefFromObjectPath(Target));
				Input->SetObjectField(
					TEXT("initial_structure"), Patch.Structure.ToSharedRef());
				Input->SetObjectField(TEXT("display_rate"), *DisplayRate);
				Input->SetObjectField(TEXT("tick_resolution"), *TickResolution);
				const TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
				Create->SetObjectField(TEXT("input"), Input);
				if (!SubmitTimelineCommand(
						TEXT("timeline.sequence.create"),
						Create,
						MutationPayload,
						OutError))
				{
					return false;
				}
			}
			else
			{
				if (Document.ExpectedRevision == TEXT("missing"))
				{
					return Fail(
						OutError, TEXT("sequence_domain_stale_revision"),
						Target, TEXT("existing"));
				}
				FString BeforePayload;
				TSharedPtr<FJsonObject> BeforeObject;
				if (!TryRead(Target, BeforePayload, BeforeObject, OutError))
				{
					return false;
				}
				FString CurrentRevision;
				if (!StableRevision(BeforeObject, CurrentRevision, OutError))
				{
					return false;
				}
				if (Document.ExpectedRevision != CurrentRevision)
				{
					return Fail(
						OutError, TEXT("sequence_domain_stale_revision"),
						Target, CurrentRevision);
				}
				TSharedPtr<FJsonObject> NativeRevision;
				if (!ExtractRevision(BeforeObject, NativeRevision, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetObjectField(TEXT("expected_revision"), NativeRevision);
				FString NativeCommand =
					TEXT("timeline.sequence.structure.replace");
				if (Patch.bSparseSettings)
				{
					for (const FString& FieldId : Patch.ChangedFields)
					{
						const TSharedPtr<FJsonObject>* Value = nullptr;
						if (!Patch.Structure->TryGetObjectField(FieldId, Value)
							|| !Value || !Value->IsValid())
						{
							return Fail(
								OutError,
								TEXT("sequence_domain_settings_patch_invalid"),
								FieldId);
						}
						Input->SetObjectField(FieldId, *Value);
					}
					NativeCommand = TEXT("timeline.sequence.settings.update");
				}
				else
				{
					Input->SetObjectField(
						TEXT("structure"), Patch.Structure.ToSharedRef());
				}
				const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
				Mutation->SetStringField(TEXT("target"), Target);
				Mutation->SetObjectField(TEXT("input"), Input);
				if (!SubmitTimelineCommand(
						NativeCommand,
						Mutation,
						MutationPayload,
						OutError))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> MutationObject;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseTimelineLifecycleMutation(
					MutationPayload,
					MutationObject,
					LifecycleReceipt,
					OutError))
			{
				return false;
			}
			FString AfterPayload;
			TSharedPtr<FJsonObject> AfterObject;
			if (!TryRead(Target, AfterPayload, AfterObject, OutError))
			{
				return false;
			}
			FString AfterRevision;
			if (!StableRevision(AfterObject, AfterRevision, OutError))
			{
				return false;
			}
			if (!TryBuildImportSuccessPayload(
				TEXT("sequence-domain-provider"),
				Target,
				AfterRevision,
				LifecycleReceipt,
				AfterObject,
				MutationObject,
				OutPayloadJson,
				OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Success;
			FString SuccessError;
			if (!Json::TryParseObject(OutPayloadJson, Success, SuccessError)
				|| !Success.IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_result_invalid"),
					Target,
					SuccessError);
			}
			TArray<TSharedPtr<FJsonValue>> ChangedFields;
			for (const FString& FieldId : Patch.ChangedFields)
			{
				ChangedFields.Add(MakeShared<FJsonValueString>(FieldId));
			}
			const TSharedRef<FJsonObject> SemanticDiff =
				MakeShared<FJsonObject>();
			SemanticDiff->SetArrayField(
				TEXT("changed_fields"), MoveTemp(ChangedFields));
			SemanticDiff->SetStringField(
				TEXT("field_owner"), GetSequenceHjsonFieldOwner());
			Success->SetObjectField(TEXT("semantic_diff"), SemanticDiff);
			return TryCanonicalizeObject(Success, OutPayloadJson, OutError);
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
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(
				OutError,
				TEXT("sequence_domain_params_invalid"),
				JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(
				OutError,
				TEXT("sequence_domain_target_identity_mismatch"),
				TargetIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.authoring_schema"))
		{
			TSharedPtr<FJsonObject> Schema;
			if (!TryBuildSequenceHjsonAuthoringSchema(
					Target, Schema, OutError)
				|| !Schema.IsValid())
			{
				return false;
			}
			FString ReadPayload;
			TSharedPtr<FJsonObject> ReadObject;
			FString Revision;
			if (!TryRead(Target, ReadPayload, ReadObject, OutError)
				|| !StableRevision(ReadObject, Revision, OutError))
			{
				return false;
			}
			Schema->SetStringField(TEXT("revision"), Revision);
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetStringField(TEXT("target"), Target);
			Response->SetBoolField(TEXT("read_only"), true);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Schema.ToSharedRef());
			bOutReadOnly = true;
			return TryCanonicalizeObject(Response, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return TryExport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			TSharedPtr<FJsonObject> ReadObject;
			FString ReadPayload;
			if (!TryRead(Target, ReadPayload, ReadObject, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			FString ReadRevision;
			if (!StableRevision(ReadObject, ReadRevision, OutError))
			{
				return false;
			}
			Response->SetStringField(TEXT("revision"), ReadRevision);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), ReadObject);
			bOutReadOnly = true;
			return TryCanonicalizeObject(Response, OutPayloadJson, OutError);
		}
		return Fail(
			OutError,
			TEXT("sequence_domain_command_not_supported"),
			CommandId);
	}
}
