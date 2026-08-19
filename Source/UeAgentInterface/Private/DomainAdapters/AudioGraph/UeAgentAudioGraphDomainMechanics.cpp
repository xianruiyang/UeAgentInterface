#include "DomainAdapters/AudioGraph/UeAgentAudioGraphDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
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

		FString ObjectPathFromPackage(const FString& Target)
		{
			int32 Dot = INDEX_NONE;
			if (Target.FindLastChar(TEXT('.'), Dot)) return Target;
			const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
			return AssetName.IsEmpty() ? Target : Target + TEXT(".") + AssetName;
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
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("audio_graph_domain_hjson_node_invalid"));
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
				return Fail(OutError, TEXT("audio_graph_domain_json_invalid"), Error);
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
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
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
				return Fail(OutError, TEXT("audio_graph_domain_native_command_failed"), CommandId, Result.GetErrorJson());
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool TryCapture(
			const FString& Target,
			FString& OutType,
			FUeAgentContentGraphCapture& OutCapture,
			FWorkspaceError& OutError)
		{
			UObject* Owner = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPathFromPackage(Target));
			if (!Owner)
			{
				return Fail(OutError, TEXT("audio_graph_domain_target_unresolved"), Target);
			}
			OutType = Owner->GetClass()->GetPathName();
			if (!IsSupportedRootType(OutType))
			{
				return Fail(OutError, TEXT("audio_graph_domain_target_type_invalid"), OutType);
			}
			FUeAgentContentGraphReadError CaptureError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					*Owner, OutCapture, CaptureError))
			{
				return Fail(
					OutError,
					CaptureError.Code.IsEmpty()
						? FString(TEXT("audio_graph_domain_readback_failed"))
						: CaptureError.Code,
					CaptureError.SubjectId,
					CaptureError.Detail);
			}
			return true;
		}

		TSharedPtr<FJsonObject> CaptureJson(
			const FString& Target,
			const FString& Type,
			const FUeAgentContentGraphCapture& Capture)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("root_type"), Type);
			Result->SetStringField(TEXT("domain_id"), Capture.DomainId);
			Result->SetStringField(TEXT("revision"), Capture.Revision);
			Result->SetBoolField(TEXT("complete"), true);
			Result->SetBoolField(TEXT("has_more"), false);
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FUeAgentContentGraphItem& Item : Capture.Items)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Item.ItemId);
				Value->SetStringField(TEXT("kind"), Item.ItemKind);
				Value->SetStringField(TEXT("class_path"), Item.ClassPath);
				Value->SetStringField(TEXT("container_id"), Item.ContainerId);
				Value->SetNumberField(TEXT("order"), Item.Order);
				Items.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("items"), MoveTemp(Items));
			TArray<TSharedPtr<FJsonValue>> Relations;
			for (const FUeAgentContentGraphRelation& Relation : Capture.Relations)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Relation.RelationId);
				Value->SetStringField(TEXT("kind"), Relation.RelationKind);
				Value->SetStringField(TEXT("source"), Relation.SourceItemId);
				Value->SetStringField(TEXT("target"), Relation.TargetItemId);
				Relations.Add(MakeShared<FJsonValueObject>(Value));
			}
			Result->SetArrayField(TEXT("relations"), MoveTemp(Relations));
			Result->SetNumberField(TEXT("property_count"), Capture.Properties.Num());
			Result->SetNumberField(TEXT("parameter_count"), Capture.Parameters.Num());
			bool bCompileValid = true;
			TArray<TSharedPtr<FJsonValue>> Diagnostics;
			for (const FUeAgentContentGraphDiagnostic& Diagnostic : Capture.Diagnostics)
			{
				const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("id"), Diagnostic.DiagnosticId);
				Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
				Value->SetStringField(TEXT("code"), Diagnostic.Code);
				Value->SetBoolField(TEXT("resolved"), Diagnostic.bResolved);
				Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
				if (!Diagnostic.bResolved && Diagnostic.Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
				{
					bCompileValid = false;
				}
			}
			Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
			Result->SetStringField(TEXT("compile_status"), bCompileValid ? TEXT("valid") : TEXT("error"));
			return Result;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			FString Type;
			FUeAgentContentGraphCapture Capture;
			if (!TryCapture(Target, Type, Capture, OutError)) return false;
			OutRevision = Capture.Revision;
			return CanonicalObject(CaptureJson(Target, Type, Capture), OutPayload, OutError);
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutRoot,
			FString& OutType,
			FWorkspaceError& OutError)
		{
			OutRoot.Reset();
			OutType.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_domain_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid()
				|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(OutError, TEXT("audio_graph_domain_resolved_source_invalid"));
			}
			FString Text;
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject() : nullptr;
				FString RelativePath;
				FString DocumentText;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(TEXT("document"), DocumentText))
				{
					return Fail(OutError, TEXT("audio_graph_domain_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					Text = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || Text.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("audio_graph_domain_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(Text, TEXT("inline:audio-graph-domain"), Resources, Document, OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Converted = JsonValueFromHjson(*Document->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Canonical = Converted.IsValid() ? Converted->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			if (!Canonical.IsValid()
				|| !Canonical->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Canonical->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid()
				|| !(*Root)->TryGetStringField(TEXT("type"), OutType)
				|| !IsSupportedRootType(OutType))
			{
				return Fail(OutError, TEXT("audio_graph_domain_schema_or_type_invalid"), OutType);
			}
			OutRoot = *Root;
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
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Root;
			FString RootType;
			if (!ParseDocument(Params, Root, RootType, OutError)) return false;
			FString ExpectedRevision;
			if (!Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(OutError, TEXT("audio_graph_domain_expected_revision_required"));
			}
			if (ExpectedRevision == TEXT("missing"))
			{
				return Fail(OutError, TEXT("audio_graph_domain_root_create_not_registered"), Target);
			}
			FString BeforePayload;
			FString CurrentRevision;
			if (!TryRead(Target, BeforePayload, CurrentRevision, OutError)) return false;
			if (ExpectedRevision != CurrentRevision)
			{
				return Fail(OutError, TEXT("audio_graph_domain_stale_revision"), Target, CurrentRevision);
			}
			const TSharedPtr<FJsonObject>* Operation = nullptr;
			const TSharedPtr<FJsonObject>* NativeParams = nullptr;
			FString CommandId;
			if (!Root->TryGetObjectField(TEXT("operation"), Operation)
				|| !Operation || !Operation->IsValid()
				|| !(*Operation)->TryGetStringField(TEXT("command_id"), CommandId)
				|| !(*Operation)->TryGetObjectField(TEXT("params"), NativeParams)
				|| !NativeParams || !NativeParams->IsValid())
			{
				return Fail(OutError, TEXT("audio_graph_domain_operation_contract_required"));
			}
			if (!IsAllowedOperation(RootType, CommandId))
			{
				return Fail(OutError, TEXT("audio_graph_domain_operation_not_supported"), CommandId);
			}
			FString NativeTarget;
			if (!(*NativeParams)->TryGetStringField(TEXT("target_ref"), NativeTarget)
				|| ObjectPathFromPackage(NativeTarget) != ObjectPathFromPackage(Target))
			{
				return Fail(OutError, TEXT("audio_graph_domain_operation_target_mismatch"), CommandId);
			}
			FString MutationPayload;
			if (!SubmitAudioCommand(CommandId, *NativeParams, MutationPayload, OutError)) return false;

			FString AfterPayload;
			FString AfterRevision;
			if (!TryRead(Target, AfterPayload, AfterRevision, OutError)) return false;
			TSharedPtr<FJsonObject> AfterObject;
			FString JsonError;
			if (!Json::TryParseObject(AfterPayload, AfterObject, JsonError) || !AfterObject.IsValid())
			{
				return Fail(OutError, TEXT("audio_graph_domain_readback_payload_invalid"), JsonError);
			}
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"), TEXT("audio-graph-domain-provider"));
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("revision"), AfterRevision);
			Result->SetBoolField(TEXT("saved"), true);
			Result->SetBoolField(TEXT("reloaded"), true);
			Result->SetObjectField(TEXT("independent_readback"), AfterObject);
			TSharedPtr<FJsonObject> Mutation;
			if (Json::TryParseObject(MutationPayload, Mutation, JsonError) && Mutation.IsValid())
			{
				Result->SetObjectField(TEXT("mutation"), Mutation);
			}
			return CanonicalObject(Result, OutPayload, OutError);
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
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(OutError, TEXT("audio_graph_domain_export_not_registered"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			bOutReadOnly = true;
			return TryRead(Target, OutPayloadJson, Revision, OutError);
		}
		return Fail(OutError, TEXT("audio_graph_domain_command_not_supported"), CommandId);
	}
}
