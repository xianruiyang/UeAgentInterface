#include "DomainAdapters/AnimationCurve/UeAgentAnimationCurveDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AnimationCurveCommands/UeAgentAnimationCurveCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::AnimationCurve
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;

	namespace
	{
		const FString SchemaId = TEXT("uai.animation_curve.edit/1");
		const FString BlendSpaceType = TEXT("/Script/Engine.BlendSpace");
		const FString SequenceType = TEXT("/Script/Engine.AnimSequence");

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

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
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
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Item, OutError);
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
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("animation_curve_domain_hjson_node_invalid"));
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
					TEXT("animation_curve_domain_json_canonicalization_failed"),
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
			const FString RequestId = TEXT("animation-curve-domain-")
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
				reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitAnimationCommand(
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
					TEXT("animation_curve_domain_command_registry_unavailable"),
					CompositionError);
			}
			const CommandContract::FUeAgentFrozenCommandContract* FrozenContract =
				Registry->FindExact(CommandId);
			if (!FrozenContract)
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_command_contract_missing"),
					CommandId);
			}
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
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
			if (!AnimationCurveCommands::TryResolveMechanics(
					CommandId, Mechanics, Scopes, MechanicsError)
				|| !Mechanics.IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_native_mechanics_unavailable"),
					CommandId,
					MechanicsError);
			}
			Scopes.AddUnique(TEXT("uai:query"));
			Scopes.AddUnique(TEXT("uai:operate"));
			const FUeAgentCommandResult Result = AnimationCurveCommands::Submit(
				Mechanics.ToSharedRef(),
				Request.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("animation-curve-domain-provider"),
					Scopes,
					Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_native_command_failed"),
					CommandId,
					Result.GetErrorJson());
			}
			OutPayload = Result.GetPayloadJson();
			return true;
		}

		bool ParsePayload(
			const FString& Payload,
			TSharedPtr<FJsonObject>& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Json::TryParseObject(Payload, Out, Error) || !Out.IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_native_payload_invalid"),
					Error);
			}
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
					TEXT("animation_curve_domain_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_resolved_source_required"));
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
					OutError,
					TEXT("animation_curve_domain_resolved_source_invalid"));
			}
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject()
					: nullptr;
				FString RelativePath;
				FString Text;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(
						TEXT("document"), Text))
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_resolved_source_invalid"));
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
					TEXT("animation_curve_domain_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:animation-curve-domain"),
					Resources,
					Document,
					OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Value =
				JsonValueFromHjson(*Document->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Parsed =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			if (!Parsed.IsValid()
				|| !Parsed->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Parsed->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid())
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_document_invalid"));
			}
			OutRoot = *Root;
			return true;
		}

		bool ObservedState(
			const TSharedPtr<FJsonObject>& Read,
			FString& OutState,
			FWorkspaceError& OutError)
		{
			OutState.Reset();
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			const TSharedPtr<FJsonObject>* Snapshot = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Read.IsValid()
				|| !Read->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid()
				|| !(*Payload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
				|| !Snapshot || !Snapshot->IsValid()
				|| !(*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
				|| !Entries || Entries->IsEmpty())
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_observed_state_missing"));
			}
			const TSharedPtr<FJsonObject> Entry = (*Entries)[0]->AsObject();
			const TSharedPtr<FJsonObject>* EntryValue = nullptr;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const TSharedPtr<FJsonObject>* State = nullptr;
			if (!Entry.IsValid()
				|| !Entry->TryGetObjectField(TEXT("value"), EntryValue)
				|| !EntryValue || !EntryValue->IsValid()
				|| !(*EntryValue)->TryGetObjectField(TEXT("fields"), Fields)
				|| !Fields || !Fields->IsValid()
				|| !(*Fields)->TryGetObjectField(TEXT("observed_state"), State)
				|| !State || !State->IsValid()
				|| !(*State)->TryGetStringField(TEXT("text"), OutState)
				|| OutState.IsEmpty())
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_observed_state_missing"));
			}
			return true;
		}

		bool NativeRevision(
			const TSharedPtr<FJsonObject>& Read,
			TSharedPtr<FJsonObject>& OutRevision,
			FWorkspaceError& OutError);

		bool StableRevision(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Read,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			FString PackageName;
			FString ObjectName;
			if (!Target.Split(TEXT("."), &PackageName, &ObjectName)
				|| !FPackageName::IsValidLongPackageName(PackageName))
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_package_identity_invalid"),
					Target);
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_persisted_package_missing"),
					PackageName);
			}
			OutRevision = Sha256(Bytes);
			return true;
		}

		bool NativeRevision(
			const TSharedPtr<FJsonObject>& Read,
			TSharedPtr<FJsonObject>& OutRevision,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* Payload = nullptr;
			const TSharedPtr<FJsonObject>* Snapshot = nullptr;
			const TSharedPtr<FJsonObject>* Revision = nullptr;
			if (!Read.IsValid()
				|| !Read->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"), Payload)
				|| !Payload || !Payload->IsValid()
				|| !(*Payload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
				|| !Snapshot || !Snapshot->IsValid()
				|| !(*Snapshot)->TryGetObjectField(TEXT("revision"), Revision)
				|| !Revision || !Revision->IsValid())
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_native_revision_missing"));
			}
			FString SourceId;
			FString Generation;
			FString Sequence;
			if (!(*Revision)->TryGetStringField(TEXT("source_id"), SourceId)
				|| !(*Revision)->TryGetStringField(TEXT("generation"), Generation))
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_native_revision_invalid"));
			}
			if (!(*Revision)->TryGetStringField(TEXT("sequence"), Sequence))
			{
				double Number = 0.0;
				if (!(*Revision)->TryGetNumberField(TEXT("sequence"), Number)
					|| Number < 0.0
					|| FMath::FloorToDouble(Number) != Number)
				{
					return Fail(
						OutError, TEXT("animation_curve_domain_native_revision_invalid"));
				}
				Sequence = FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
			}
			OutRevision = MakeShared<FJsonObject>();
			OutRevision->SetStringField(TEXT("source_id"), SourceId);
			OutRevision->SetStringField(TEXT("generation"), Generation);
			OutRevision->SetStringField(TEXT("sequence"), Sequence);
			return true;
		}

		bool ReadSequence(
			const FString& Target,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetBoolField(TEXT("include_tracks"), true);
			Input->SetBoolField(TEXT("include_keys"), true);
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			Params->SetObjectField(TEXT("input"), Input);
			return SubmitAnimationCommand(
					TEXT("animation.sequence.read"), Params, OutPayload, OutError)
				&& ParsePayload(OutPayload, OutObject, OutError);
		}

		bool ReadBlendSpace(
			const FString& Target,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			TArray<TSharedPtr<FJsonValue>> BlendParameters;
			for (const FString& Axis : {FString(TEXT("Speed")), FString(TEXT("Direction"))})
			{
				const TSharedPtr<FJsonObject> Parameter = MakeShared<FJsonObject>();
				Parameter->SetStringField(TEXT("axis_ref"), Axis);
				Parameter->SetNumberField(TEXT("value"), 0.0);
				BlendParameters.Add(MakeShared<FJsonValueObject>(Parameter));
			}
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetArrayField(TEXT("blend_parameters"), BlendParameters);
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			Params->SetObjectField(TEXT("input"), Input);
			return SubmitAnimationCommand(
					TEXT("animation.blend_space.sample"),
					Params,
					OutPayload,
					OutError)
				&& ParsePayload(OutPayload, OutObject, OutError);
		}

		bool ReadForType(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			return RootType == BlendSpaceType
				? ReadBlendSpace(Target, OutPayload, OutObject, OutError)
				: ReadSequence(Target, OutPayload, OutObject, OutError);
		}

		FString PackagePath(const FString& Target)
		{
			FString Package;
			FString Object;
			Target.Split(TEXT("."), &Package, &Object);
			FString Path;
			FString Asset;
			Package.Split(TEXT("/"), &Path, &Asset, ESearchCase::CaseSensitive,
				ESearchDir::FromEnd);
			return Path;
		}

		FString AssetName(const FString& Target)
		{
			FString Package;
			FString Object;
			Target.Split(TEXT("."), &Package, &Object);
			return Object;
		}

		bool BuildWriteResponse(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Read,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString Revision;
			if (!StableRevision(Target, Read, Revision, OutError)) return false;
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(
				TEXT("provider_id"), TEXT("animation-curve-domain-provider"));
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetBoolField(TEXT("reloaded"), true);
			Response->SetObjectField(TEXT("independent_readback"), Read);
			return CanonicalObject(Response, OutPayload, OutError);
		}

		bool ImportBlendSpace(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (ExpectedRevision != TEXT("missing"))
			{
				FString BeforePayload;
				TSharedPtr<FJsonObject> Before;
				FString Current;
				if (!ReadBlendSpace(Target, BeforePayload, Before, OutError)
					|| !StableRevision(Target, Before, Current, OutError))
				{
					return false;
				}
				if (Current != ExpectedRevision)
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_stale_revision"),
						Target,
						Current);
				}
			}
			else
			{
				FString Skeleton;
				double Dimension = 0.0;
				if (!Root->TryGetStringField(TEXT("skeleton_ref"), Skeleton)
					|| !Root->TryGetNumberField(TEXT("dimension"), Dimension)
					|| (Dimension != 1.0 && Dimension != 2.0))
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_blend_space_create_invalid"));
				}
				const TSharedPtr<FJsonObject> Create = MakeShared<FJsonObject>();
				Create->SetStringField(TEXT("asset_name"), AssetName(Target));
				Create->SetNumberField(TEXT("dimension"), Dimension);
				Create->SetStringField(TEXT("package_path"), PackagePath(Target));
				Create->SetStringField(TEXT("skeleton_ref"), Skeleton);
				FString Ignored;
				if (!SubmitAnimationCommand(
						TEXT("animation.blend_space.asset.create"),
						Create,
						Ignored,
						OutError))
				{
					return false;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
			if (!Root->TryGetArrayField(TEXT("axes"), Axes)
				|| !Axes
				|| !Root->TryGetArrayField(TEXT("samples"), Samples)
				|| !Samples)
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_blend_space_definition_invalid"));
			}
			const TSharedPtr<FJsonObject> Apply = MakeShared<FJsonObject>();
			Apply->SetStringField(TEXT("target_ref"), Target);
			Apply->SetArrayField(TEXT("axes"), *Axes);
			Apply->SetArrayField(TEXT("samples"), *Samples);
			FString Ignored;
			if (!SubmitAnimationCommand(
					TEXT("animation.blend_space.definition.apply"),
					Apply,
					Ignored,
					OutError))
			{
				return false;
			}
			FString AfterPayload;
			TSharedPtr<FJsonObject> After;
			return ReadBlendSpace(Target, AfterPayload, After, OutError)
				&& BuildWriteResponse(
					Target, BlendSpaceType, After, OutPayload, OutError);
		}

		bool ImportSequence(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (ExpectedRevision == TEXT("missing"))
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_sequence_creation_not_supported"),
					Target);
			}
			FString ReadPayload;
			TSharedPtr<FJsonObject> Read;
			FString Current;
			if (!ReadSequence(Target, ReadPayload, Read, OutError)
				|| !StableRevision(Target, Read, Current, OutError))
			{
				return false;
			}
			if (Current != ExpectedRevision)
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_stale_revision"),
					Target,
					Current);
			}

			struct FChange
			{
				const TCHAR* Field;
				const TCHAR* Command;
			};
			const FChange Changes[] = {
				{TEXT("metadata_changes"), TEXT("animation.sequence.metadata.update")},
				{TEXT("curve_changes"), TEXT("animation.sequence.curves.update")},
				{TEXT("event_changes"), TEXT("animation.sequence.events.update")},
			};
			bool bChanged = false;
			for (const FChange& Change : Changes)
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Root->TryGetArrayField(Change.Field, Values) || !Values)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Revision;
				if (!NativeRevision(Read, Revision, OutError)) return false;
				const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetArrayField(Change.Field, *Values);
				Input->SetObjectField(TEXT("expected_revision"), Revision);
				const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("target"), Target);
				Params->SetObjectField(TEXT("input"), Input);
				FString Mutation;
				if (!SubmitAnimationCommand(
						Change.Command, Params, Mutation, OutError)
					|| !ReadSequence(Target, ReadPayload, Read, OutError))
				{
					return false;
				}
				bChanged = true;
			}
			if (!bChanged)
			{
				return Fail(
					OutError, TEXT("animation_curve_domain_sequence_changes_required"));
			}
			return BuildWriteResponse(
				Target, SequenceType, Read, OutPayload, OutError);
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
				OutError, TEXT("animation_curve_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(
				OutError,
				TEXT("animation_curve_domain_target_identity_mismatch"),
				TargetIdentity);
		}
		if (RootTypeIdentity != BlendSpaceType
			&& RootTypeIdentity != SequenceType)
		{
			return Fail(
				OutError,
				TEXT("animation_curve_domain_root_type_unsupported"),
				RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_expected_revision_required"));
			}
			TSharedPtr<FJsonObject> Root;
			if (!ParseDocument(Params, Root, OutError)) return false;
			FString DocumentType;
			if (!Root->TryGetStringField(TEXT("type"), DocumentType)
				|| DocumentType != RootTypeIdentity)
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_document_type_mismatch"),
					DocumentType);
			}
			return RootTypeIdentity == BlendSpaceType
				? ImportBlendSpace(
					Target, ExpectedRevision, Root, OutPayloadJson, OutError)
				: ImportSequence(
					Target, ExpectedRevision, Root, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(
				OutError,
				TEXT("animation_curve_domain_export_not_registered"),
				Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString ReadPayload;
			TSharedPtr<FJsonObject> Read;
			if (!ReadForType(
					Target,
					RootTypeIdentity,
					ReadPayload,
					Read,
					OutError))
			{
				return false;
			}
			FString Revision;
			if (!StableRevision(Target, Read, Revision, OutError)) return false;
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootTypeIdentity);
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Read);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		return Fail(
			OutError,
			TEXT("animation_curve_domain_command_not_supported"),
			CommandId);
	}
}
