#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Components/PoseableMeshComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineGlobals.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/RigSpatialCommands/UeAgentRigSpatialCommandOperationOwner.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace HjsonWorkspace;

	namespace
	{
		const FString SchemaId = TEXT("uai.rig_simulation_spatial.edit/1");
		const FString RootType = TEXT("/Script/Engine.PoseableMeshComponent");
		const FString PoseCommandId = TEXT("animation.subject.pose.update");

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

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 262144},
				{EUeAgentResourceDimension::ReferenceEdges, 262144},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					256 * 1024 * 1024},
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
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Value =
						JsonValueFromHjson(*Item, OutError);
					if (!Value.IsValid()) return nullptr;
					Values.Add(MoveTemp(Value));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Value =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Value.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("rig_simulation_spatial_hjson_node_invalid"));
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
				return Fail(OutError,
					TEXT("rig_simulation_spatial_json_invalid"), Error);
			}
			return true;
		}

		FString Number(const double Value)
		{
			return FString::Printf(TEXT("%.17g"), Value);
		}

		FString FrameId(const FString& Target)
		{
			return TEXT("t58://subject-frame/") + Target;
		}

		FString BasisFrameId(const FString& Target)
		{
			return TEXT("t25://component-local/") + Target;
		}

		FString ClockId(const FString& Target)
		{
			return TEXT("t25://editor-frame/") + Target;
		}

		FString BasisEvidenceId(const FString& Target)
		{
			return TEXT("t25:component-local:") + Target;
		}

		TSharedPtr<FJsonObject> FrameContract(const FString& Target)
		{
			const TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
			Frame->SetStringField(TEXT("frame_id"), FrameId(Target));
			Frame->SetStringField(TEXT("basis_frame_id"), BasisFrameId(Target));
			Frame->SetNumberField(TEXT("dimension"), 3);
			Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
			Frame->SetStringField(TEXT("basis_evidence_id"),
				BasisEvidenceId(Target));
			return Frame;
		}

		TSharedPtr<FJsonObject> CapturedClock(const FString& Target)
		{
			const TSharedPtr<FJsonObject> Rate = MakeShared<FJsonObject>();
			Rate->SetStringField(TEXT("numerator"), TEXT("1"));
			Rate->SetStringField(TEXT("denominator"), TEXT("1"));
			const TSharedPtr<FJsonObject> Clock = MakeShared<FJsonObject>();
			Clock->SetStringField(TEXT("clock_id"), ClockId(Target));
			Clock->SetStringField(TEXT("tick"),
				FString::Printf(TEXT("%llu"), GFrameCounter));
			Clock->SetObjectField(TEXT("rate"), Rate);
			Clock->SetStringField(TEXT("state"), TEXT("captured"));
			return Clock;
		}

		TArray<TSharedPtr<FJsonValue>> TransformComponents(
			const FTransform& Transform)
		{
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const double Value : {
				Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Scale.X, Scale.Y, Scale.Z})
			{
				Values.Add(MakeShared<FJsonValueString>(Number(Value)));
			}
			return Values;
		}

		UPoseableMeshComponent* ResolveSubject(const FString& Target)
		{
			UPoseableMeshComponent* Subject =
				FindObject<UPoseableMeshComponent>(nullptr, *Target);
			if (!Subject)
			{
				Subject = Cast<UPoseableMeshComponent>(StaticLoadObject(
					UPoseableMeshComponent::StaticClass(), nullptr, *Target));
			}
			return Subject && Subject->GetPathName() == Target ? Subject : nullptr;
		}

		bool CaptureSubject(
			const FString& Target,
			TSharedPtr<FJsonObject>& OutCapture,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			OutCapture.Reset();
			OutRevision.Reset();
			UPoseableMeshComponent* Subject = ResolveSubject(Target);
			if (!Subject)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_subject_not_found"), Target);
			}

			const TSharedPtr<FJsonObject> Capture = MakeShared<FJsonObject>();
			Capture->SetStringField(TEXT("target"), Target);
			Capture->SetStringField(TEXT("root_type"), RootType);
			Capture->SetStringField(TEXT("state_plane"),
				TEXT("runtime_non_persistent"));
			Capture->SetStringField(TEXT("binding_identity"), Subject->GetPathName());
			Capture->SetStringField(TEXT("skeletal_mesh"),
				Subject->GetSkinnedAsset()
					? Subject->GetSkinnedAsset()->GetPathName()
					: FString());
			Capture->SetObjectField(TEXT("frame"), FrameContract(Target));

			TArray<TSharedPtr<FJsonValue>> Bones;
			for (int32 Index = 0; Index < Subject->GetNumBones(); ++Index)
			{
				const FName BoneName = Subject->GetBoneName(Index);
				if (BoneName.IsNone()) continue;
				const TSharedPtr<FJsonObject> Bone = MakeShared<FJsonObject>();
				Bone->SetStringField(TEXT("bone_ref"), BoneName.ToString());
				const TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
				Transform->SetStringField(TEXT("subject"), BoneName.ToString());
				Transform->SetStringField(TEXT("quantity"), TEXT("transform"));
				Transform->SetObjectField(TEXT("frame"), FrameContract(Target));
				Transform->SetArrayField(TEXT("components"), TransformComponents(
					Subject->GetBoneTransformByName(
						BoneName, EBoneSpaces::ComponentSpace)));
				Transform->SetArrayField(TEXT("conversion_evidence_ids"), {
					MakeShared<FJsonValueString>(BasisEvidenceId(Target))});
				Bone->SetObjectField(TEXT("transform"), Transform);
				Bones.Add(MakeShared<FJsonValueObject>(Bone));
			}
			Capture->SetArrayField(TEXT("bones"), MoveTemp(Bones));

			FString StableJson;
			if (!CanonicalObject(Capture, StableJson, OutError)) return false;
			OutRevision = TEXT("rig-simulation-spatial:")
				+ FMD5::HashAnsiString(*StableJson);
			Capture->SetStringField(TEXT("revision"), OutRevision);
			Capture->SetObjectField(TEXT("clock"), CapturedClock(Target));
			Capture->SetBoolField(TEXT("complete"), true);
			OutCapture = Capture;
			return true;
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_legacy_source_contract_rejected"));
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
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_resolved_source_invalid"));
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
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(
						TEXT("document"), DocumentText))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					Text = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || Text.IsEmpty())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_resolved_entry_invalid"));
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Parsed;
			if (!TryParseHjsonDocument(
				Text, TEXT("inline:rig-simulation-spatial-domain"),
				Resources, Parsed, OutError)
				|| !Parsed.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Converted =
				JsonValueFromHjson(*Parsed->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Root =
				Converted.IsValid() ? Converted->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* DomainRoot = nullptr;
			const TSharedPtr<FJsonObject>* Document = nullptr;
			FString Schema;
			FString Type;
			if (!Root.IsValid()
				|| !Root->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Root->TryGetObjectField(TEXT("root"), DomainRoot)
				|| !DomainRoot || !DomainRoot->IsValid()
				|| !(*DomainRoot)->TryGetStringField(TEXT("type"), Type)
				|| Type != RootType
				|| !(*DomainRoot)->TryGetObjectField(TEXT("document"), Document)
				|| !Document || !Document->IsValid())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_schema_or_type_invalid"), Type);
			}
			OutDocument = *Document;
			return true;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FWorkspaceError& OutError,
			const FString& Pointer)
		{
			Out.Reset();
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, Out)
				|| Out.IsEmpty())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_field_required"), Field, Pointer);
			}
			return true;
		}

		bool ValidateFrame(
			const TSharedPtr<FJsonObject>& Frame,
			const FString& Target,
			FWorkspaceError& OutError,
			const FString& Pointer)
		{
			FString FrameIdentity;
			FString BasisIdentity;
			FString Unit;
			FString Evidence;
			double Dimension = 0.0;
			if (!RequiredString(Frame, TEXT("frame_id"), FrameIdentity,
					OutError, Pointer)
				|| !RequiredString(Frame, TEXT("basis_frame_id"), BasisIdentity,
					OutError, Pointer)
				|| !RequiredString(Frame, TEXT("unit"), Unit,
					OutError, Pointer)
				|| !RequiredString(Frame, TEXT("basis_evidence_id"), Evidence,
					OutError, Pointer)
				|| !Frame->TryGetNumberField(TEXT("dimension"), Dimension)
				|| FrameIdentity != FrameId(Target)
				|| BasisIdentity != BasisFrameId(Target)
				|| Unit != TEXT("centimeter")
				|| Evidence != BasisEvidenceId(Target)
				|| Dimension != 3.0)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_frame_contract_invalid"),
					Target, Pointer);
			}
			return true;
		}

		bool ValidateInput(
			UPoseableMeshComponent& Subject,
			const FString& Target,
			const TSharedPtr<FJsonObject>& Input,
			TArray<FName>& OutBones,
			FWorkspaceError& OutError)
		{
			OutBones.Reset();
			const TSharedPtr<FJsonObject>* EvaluationPoint = nullptr;
			const TSharedPtr<FJsonObject>* Rate = nullptr;
			const TSharedPtr<FJsonObject>* PoseFrame = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Pose = nullptr;
			FString Clock;
			FString Tick;
			FString State;
			FString Numerator;
			FString Denominator;
			int64 ParsedTick = 0;
			int64 ParsedNumerator = 0;
			int64 ParsedDenominator = 0;
			if (!Input.IsValid()
				|| !Input->TryGetObjectField(TEXT("evaluation_point"),
					EvaluationPoint)
				|| !EvaluationPoint || !EvaluationPoint->IsValid()
				|| !RequiredString(*EvaluationPoint, TEXT("clock_id"), Clock,
					OutError, TEXT("/provider_extension/evaluation_point"))
				|| !RequiredString(*EvaluationPoint, TEXT("tick"), Tick,
					OutError, TEXT("/provider_extension/evaluation_point"))
				|| !RequiredString(*EvaluationPoint, TEXT("state"), State,
					OutError, TEXT("/provider_extension/evaluation_point"))
				|| !(*EvaluationPoint)->TryGetObjectField(TEXT("rate"), Rate)
				|| !Rate || !Rate->IsValid()
				|| !RequiredString(*Rate, TEXT("numerator"), Numerator,
					OutError, TEXT("/provider_extension/evaluation_point/rate"))
				|| !RequiredString(*Rate, TEXT("denominator"), Denominator,
					OutError, TEXT("/provider_extension/evaluation_point/rate"))
				|| !LexTryParseString(ParsedTick, *Tick)
				|| !LexTryParseString(ParsedNumerator, *Numerator)
				|| !LexTryParseString(ParsedDenominator, *Denominator)
				|| ParsedDenominator <= 0
				|| Clock != ClockId(Target)
				|| State != TEXT("authored"))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_evaluation_clock_invalid"), Target);
			}
			if (!Input->TryGetObjectField(TEXT("pose_frame"), PoseFrame)
				|| !PoseFrame || !ValidateFrame(
					*PoseFrame, Target, OutError, TEXT("/provider_extension/pose_frame")))
			{
				return false;
			}
			if (!Input->TryGetArrayField(TEXT("pose"), Pose)
				|| !Pose || Pose->IsEmpty())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_pose_required"), Target);
			}

			TSet<FName> UniqueBones;
			for (int32 Index = 0; Index < Pose->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Pose)[Index]->AsObject();
				const TSharedPtr<FJsonObject>* Transform = nullptr;
				const TSharedPtr<FJsonObject>* Frame = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Evidence = nullptr;
				FString Bone;
				FString SubjectIdentity;
				FString Quantity;
				if (!RequiredString(Item, TEXT("bone_ref"), Bone,
						OutError, TEXT("/provider_extension/pose"))
					|| !Item->TryGetObjectField(TEXT("transform"), Transform)
					|| !Transform || !Transform->IsValid()
					|| !RequiredString(*Transform, TEXT("subject"), SubjectIdentity,
						OutError, TEXT("/provider_extension/pose/transform"))
					|| !RequiredString(*Transform, TEXT("quantity"), Quantity,
						OutError, TEXT("/provider_extension/pose/transform"))
					|| SubjectIdentity != Bone
					|| Quantity != TEXT("transform")
					|| Subject.GetBoneIndex(FName(*Bone)) == INDEX_NONE
					|| !(*Transform)->TryGetObjectField(TEXT("frame"), Frame)
					|| !Frame || !ValidateFrame(*Frame, Target, OutError,
						TEXT("/provider_extension/pose/transform/frame"))
					|| !(*Transform)->TryGetArrayField(TEXT("components"), Components)
					|| !Components || Components->Num() != 10
					|| !(*Transform)->TryGetArrayField(
						TEXT("conversion_evidence_ids"), Evidence)
					|| !Evidence || Evidence->IsEmpty())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_pose_item_invalid"),
						Bone, FString::FromInt(Index));
				}
				for (const TSharedPtr<FJsonValue>& Component : *Components)
				{
					FString Text;
					double Parsed = 0.0;
					if (!Component.IsValid()
						|| !Component->TryGetString(Text)
						|| !LexTryParseString(Parsed, *Text)
						|| !FMath::IsFinite(Parsed))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_transform_invalid"),
							Bone, FString::FromInt(Index));
					}
				}
				const FName BoneName(*Bone);
				if (UniqueBones.Contains(BoneName))
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_duplicate_bone"), Bone);
				}
				UniqueBones.Add(BoneName);
				OutBones.Add(BoneName);
			}
			return true;
		}

		bool DecodeRequest(
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("rig-simulation-spatial-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId, *RequestId, *PoseCommandId, *ContractHash, *ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()),
				Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_internal_request_invalid"),
					Diagnostic.Code, Diagnostic.Message);
			}
			return true;
		}

		bool SubmitPose(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Input,
			TSharedPtr<FJsonObject>& OutMutation,
			FWorkspaceError& OutError)
		{
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry;
			TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
			FString Error;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, Dispatch, Error)
				|| !Registry.IsValid() || !Dispatch.IsValid())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_command_registry_unavailable"),
					Error);
			}
			const FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(PoseCommandId);
			if (!Contract)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_pose_contract_missing"),
					PoseCommandId);
			}
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"), Target);
			Params->SetObjectField(TEXT("input"), Input);
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(
				Contract->GetContractHash(), ParamsJson, Request, OutError))
			{
				return false;
			}

			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> Scopes;
			if (!OperationOwners::RigSpatialCommands::TryResolveMechanics(
					PoseCommandId, Mechanics, Scopes, Error)
				|| !Mechanics.IsValid())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_pose_mechanics_unavailable"),
					PoseCommandId, Error);
			}
			Scopes.AddUnique(TEXT("uai:query"));
			Scopes.AddUnique(TEXT("uai:operate"));
			const FUeAgentCommandResult Result =
				OperationOwners::RigSpatialCommands::Submit(
					Mechanics.ToSharedRef(), Request.GetValue(),
					Contract::FUeAgentCallerContext(
						TEXT("rig-simulation-spatial-domain-provider"),
						Scopes, Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_pose_mutation_failed"),
					PoseCommandId, Result.GetErrorJson());
			}
			if (!Json::TryParseObject(
				Result.GetPayloadJson(), OutMutation, Error)
				|| !OutMutation.IsValid())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_pose_result_invalid"),
					PoseCommandId, Error);
			}
			return true;
		}

		bool TryRead(
			const FString& CommandId,
			const FString& Target,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> Capture;
			FString Revision;
			if (!CaptureSubject(Target, Capture, Revision, OutError)) return false;
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("command_id"), CommandId);
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"),
				TEXT("rig-simulation-spatial-domain-provider"));
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("revision"), Revision);
			Result->SetStringField(TEXT("state_plane"),
				TEXT("runtime_non_persistent"));
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("reloaded"), false);
			Result->SetObjectField(TEXT("independent_readback"), Capture);
			return CanonicalObject(Result, OutPayload, OutError);
		}

		bool TryImport(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			UPoseableMeshComponent* Subject = ResolveSubject(Target);
			if (!Subject)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_subject_not_found"), Target);
			}
			UPackage* SubjectPackage = Subject->GetOutermost();
			if (!SubjectPackage || SubjectPackage->IsDirty())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_dirty_subject_forbidden"),
					SubjectPackage ? SubjectPackage->GetName() : Target);
			}
			TSharedPtr<FJsonObject> Before;
			FString BeforeRevision;
			if (!CaptureSubject(Target, Before, BeforeRevision, OutError)) return false;
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision)
				|| ExpectedRevision != BeforeRevision)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_stale_revision"),
					ExpectedRevision, BeforeRevision);
			}

			TSharedPtr<FJsonObject> Document;
			if (!ParseDocument(Params, Document, OutError)) return false;
			FString Operation;
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!Document->TryGetStringField(TEXT("operation"), Operation)
				|| Operation != TEXT("update_runtime_pose")
				|| !Document->TryGetObjectField(TEXT("provider_extension"), Input)
				|| !Input || !Input->IsValid())
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_operation_invalid"), Operation);
			}
			TArray<FName> Bones;
			if (!ValidateInput(*Subject, Target, *Input, Bones, OutError))
			{
				return false;
			}

			TMap<FName, FTransform> OriginalTransforms;
			for (const FName Bone : Bones)
			{
				OriginalTransforms.Add(Bone, Subject->GetBoneTransformByName(
					Bone, EBoneSpaces::ComponentSpace));
			}
			auto Rollback = [&]()
			{
				for (const TPair<FName, FTransform>& Entry : OriginalTransforms)
				{
					Subject->SetBoneTransformByName(
						Entry.Key, Entry.Value, EBoneSpaces::ComponentSpace);
				}
				SubjectPackage->SetDirtyFlag(false);
			};
			TSharedPtr<FJsonObject> MechanicsResult;
			if (!SubmitPose(Target, *Input, MechanicsResult, OutError))
			{
				Rollback();
				return false;
			}
			SubjectPackage->SetDirtyFlag(false);

			TSharedPtr<FJsonObject> Readback;
			FString Revision;
			if (!CaptureSubject(Target, Readback, Revision, OutError))
			{
				Rollback();
				return false;
			}
			if (SubjectPackage->IsDirty())
			{
				Rollback();
				return Fail(OutError,
					TEXT("rig_simulation_spatial_dirty_state_restore_failed"),
					SubjectPackage->GetName());
			}

			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(TEXT("operation"), Operation);
			Mutation->SetNumberField(TEXT("bones_updated"), Bones.Num());
			Mutation->SetStringField(TEXT("delegated_command_id"), PoseCommandId);
			Mutation->SetObjectField(TEXT("mechanics_result"), MechanicsResult);
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("command_id"),
				TEXT("content_graph.import_workspace"));
			Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("provider_id"),
				TEXT("rig-simulation-spatial-domain-provider"));
			Result->SetStringField(TEXT("target"), Target);
			Result->SetStringField(TEXT("revision"), Revision);
			Result->SetStringField(TEXT("state_plane"),
				TEXT("runtime_non_persistent"));
			Result->SetStringField(TEXT("settle_mode"),
				TEXT("synchronous_component_space_readback"));
			Result->SetBoolField(TEXT("settled"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("reloaded"), false);
			Result->SetBoolField(TEXT("package_dirty"), false);
			Result->SetObjectField(TEXT("independent_readback"), Readback);
			Result->SetObjectField(TEXT("mutation"), Mutation);
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
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError,
				TEXT("rig_simulation_spatial_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError,
				TEXT("rig_simulation_spatial_target_identity_mismatch"),
				TargetIdentity, Target);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(Target, Params, OutPayloadJson, OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			return Fail(OutError,
				TEXT("rig_simulation_spatial_export_unsupported"), Target);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			bOutReadOnly = true;
			return TryRead(CommandId, Target, OutPayloadJson, OutError);
		}
		return Fail(OutError,
			TEXT("rig_simulation_spatial_command_not_supported"), CommandId);
	}
}
