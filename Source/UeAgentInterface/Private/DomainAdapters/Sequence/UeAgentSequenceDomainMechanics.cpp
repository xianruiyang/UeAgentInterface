#include "DomainAdapters/Sequence/UeAgentSequenceDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
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

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(
					FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Node.GetItems().Num());
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid())
					{
						return nullptr;
					}
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid())
					{
						return nullptr;
					}
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("sequence_domain_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_json_canonicalization_failed"),
					Error);
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
			if (!CanonicalObject(Params, ParamsJson, OutError))
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
				return Fail(
					OutError,
					TEXT("sequence_domain_native_command_failed"),
					CommandId,
					Result.GetErrorJson());
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutRoot,
			FWorkspaceError& OutError)
		{
			OutRoot.Reset();
			FString DocumentText;
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(
					OutError, TEXT("sequence_domain_resolved_source_required"));
			}
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(
					OutError, TEXT("sequence_domain_resolved_source_invalid"));
			}
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> Document = Value.IsValid()
					? Value->AsObject()
					: nullptr;
				FString RelativePath;
				FString Text;
				if (!Document.IsValid()
					|| !Document->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !Document->TryGetStringField(TEXT("document"), Text))
				{
					return Fail(
						OutError,
						TEXT("sequence_domain_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					DocumentText = MoveTemp(Text);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || DocumentText.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:sequence-domain"),
					Resources,
					Document,
					OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> CanonicalValue =
				JsonValueFromHjson(*Document->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> CanonicalDocument =
				CanonicalValue.IsValid() ? CanonicalValue->AsObject() : nullptr;
			if (!CanonicalDocument.IsValid())
			{
				return OutError.GetCode().IsEmpty()
					? Fail(
						OutError,
						TEXT("sequence_domain_canonical_hjson_invalid"))
					: false;
			}
			FString Schema;
			if (!CanonicalDocument->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId)
			{
				return Fail(
					OutError, TEXT("sequence_domain_schema_invalid"), Schema);
			}
			const TSharedPtr<FJsonObject>* Root = nullptr;
			FString Type;
			if (!CanonicalDocument->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid()
				|| !(*Root)->TryGetStringField(TEXT("type"), Type)
				|| Type != RootType)
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_root_type_invalid"),
					Type);
			}
			OutRoot = *Root;
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

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayloadJson,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			if (!ParseDocument(Params, Root, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Structure = nullptr;
			if (!Root->TryGetObjectField(TEXT("structure"), Structure)
				|| !Structure || !Structure->IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_structure_required"),
					Target,
					TEXT("/root/structure"));
			}
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(
					OutError,
					TEXT("sequence_domain_expected_revision_required"));
			}

			FString BeforePayload;
			TSharedPtr<FJsonObject> BeforeObject;
			FWorkspaceError ReadError;
			const bool bExists = TryRead(
				Target, BeforePayload, BeforeObject, ReadError);
			FString MutationPayload;
			if (!bExists)
			{
				if (ExpectedRevision != TEXT("missing"))
				{
					return Fail(
						OutError,
						TEXT("sequence_domain_stale_revision"),
						Target,
						TEXT("missing"));
				}
				const TSharedPtr<FJsonObject>* DisplayRate = nullptr;
				const TSharedPtr<FJsonObject>* TickResolution = nullptr;
				if (!(*Structure)->TryGetObjectField(
						TEXT("display_rate"), DisplayRate)
					|| !DisplayRate || !DisplayRate->IsValid()
					|| !(*Structure)->TryGetObjectField(
						TEXT("tick_resolution"), TickResolution)
					|| !TickResolution || !TickResolution->IsValid())
				{
					return Fail(
						OutError,
						TEXT("sequence_domain_rates_required"));
				}
				const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetStringField(
					TEXT("destination_ref"), PackageRefFromObjectPath(Target));
				Input->SetObjectField(TEXT("initial_structure"), *Structure);
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
				FString CurrentRevision;
				if (!StableRevision(BeforeObject, CurrentRevision, OutError))
				{
					return false;
				}
				if (ExpectedRevision != CurrentRevision)
				{
					return Fail(
						OutError,
						TEXT("sequence_domain_stale_revision"),
						Target,
						CurrentRevision);
				}
				TSharedPtr<FJsonObject> NativeRevision;
				if (!ExtractRevision(BeforeObject, NativeRevision, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetObjectField(TEXT("structure"), *Structure);
				Input->SetObjectField(TEXT("expected_revision"), NativeRevision);
				const TSharedPtr<FJsonObject> Replace = MakeShared<FJsonObject>();
				Replace->SetStringField(TEXT("target"), Target);
				Replace->SetObjectField(TEXT("input"), Input);
				if (!SubmitTimelineCommand(
						TEXT("timeline.sequence.structure.replace"),
						Replace,
						MutationPayload,
						OutError))
				{
					return false;
				}
			}

			FString AfterPayload;
			TSharedPtr<FJsonObject> AfterObject;
			if (!TryRead(Target, AfterPayload, AfterObject, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(TEXT("provider_id"), TEXT("sequence-domain-provider"));
			Response->SetStringField(TEXT("target"), Target);
			FString AfterRevision;
			if (!StableRevision(AfterObject, AfterRevision, OutError))
			{
				return false;
			}
			Response->SetStringField(TEXT("revision"), AfterRevision);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetBoolField(TEXT("reloaded"), true);
			Response->SetObjectField(TEXT("independent_readback"), AfterObject);
			TSharedPtr<FJsonObject> MutationObject;
			if (ParsePayload(MutationPayload, MutationObject, OutError))
			{
				Response->SetObjectField(TEXT("mutation"), MutationObject);
			}
			return CanonicalObject(Response, OutPayloadJson, OutError);
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
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError,
				TEXT("sequence_domain_export_not_registered"),
				Target);
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
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		return Fail(
			OutError,
			TEXT("sequence_domain_command_not_supported"),
			CommandId);
	}
}
