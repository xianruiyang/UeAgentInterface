#include "DomainAdapters/AnimationCurve/UeAgentAnimationCurveDomainMechanics.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
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
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

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
			if (!TryCanonicalizeObject(Params, ParamsJson, OutError)) return false;
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
				SetWorkspaceErrorFromCommandFailure(
					Result.GetErrorJson(),
					TEXT("animation_curve_domain_native_command_failed"),
					CommandId,
					OutError);
				return false;
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
			UObject* Object = nullptr;
			FUeAgentContentGraphCapture Capture;
			return TryCaptureObject(
					Target,
					BlendSpaceType,
					Object,
					Capture,
					OutError)
				&& TryBuildCaptureSnapshot(
					Target,
					BlendSpaceType,
					Capture,
					OutObject,
					OutError)
				&& TryCanonicalizeObject(OutObject, OutPayload, OutError);
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

		FString PackageIdentity(const FString& Target)
		{
			return Target.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Target)
				: Target;
		}

		FString PackagePath(const FString& Target)
		{
			return FPackageName::GetLongPackagePath(PackageIdentity(Target));
		}

		FString AssetName(const FString& Target)
		{
			return FPackageName::GetLongPackageAssetName(PackageIdentity(Target));
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			FString JsonError;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_lifecycle_receipt_invalid"),
					Target,
					JsonError);
			}
			FString TerminalStatus;
			FString MutationTarget;
			FString ReceiptId;
			bool bMutationApplied = false;
			bool bCompileSettled = false;
			bool bSaved = false;
			bool bReloaded = false;
			if (OutMutation->TryGetStringField(
					TEXT("terminal_status"), TerminalStatus))
			{
				if (TerminalStatus != TEXT("succeeded")
					|| !OutMutation->TryGetStringField(
						TEXT("target_ref"), MutationTarget)
					|| ObjectPathFromPackageIdentity(MutationTarget)
						!= ObjectPathFromPackageIdentity(Target)
					|| !OutMutation->TryGetBoolField(
						TEXT("mutation_applied"), bMutationApplied)
					|| !OutMutation->TryGetBoolField(
						TEXT("compile_settled"), bCompileSettled)
					|| !OutMutation->TryGetBoolField(TEXT("saved"), bSaved)
					|| !OutMutation->TryGetBoolField(TEXT("reloaded"), bReloaded)
					|| !OutMutation->TryGetStringField(
						TEXT("persistence_receipt_id"), ReceiptId)
					|| ReceiptId.IsEmpty()
					|| !bMutationApplied || !bCompileSettled
					|| !bSaved || !bReloaded)
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_lifecycle_receipt_invalid"),
						MutationTarget);
				}
			}
			else
			{
				FString OperationId;
				FString ProfileId;
				const TSharedPtr<FJsonObject>* Projection = nullptr;
				const TSharedPtr<FJsonObject>* Payload = nullptr;
				if (!OutMutation->TryGetStringField(
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
						TEXT("animation_curve_domain_lifecycle_receipt_invalid"),
						OperationId);
				}
				bMutationApplied = true;
				bCompileSettled = true;
				bSaved = true;
				bReloaded = true;
				ReceiptId = TEXT("timeline-operation:") + OperationId;
			}
			OutReceipt.bMutationApplied = bMutationApplied;
			OutReceipt.bCompileSettled = bCompileSettled;
			OutReceipt.bPackageSaved = bSaved;
			OutReceipt.bUnloadedReloaded = bReloaded;
			OutReceipt.PersistenceReceiptId = MoveTemp(ReceiptId);
			return true;
		}

		bool BuildWriteResponse(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Read,
			const TSharedPtr<FJsonObject>& Mutation,
			const FDomainHjsonImportLifecycleReceipt& LifecycleReceipt,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString Revision;
			if (!StableRevision(Target, Read, Revision, OutError)) return false;
			return TryBuildImportSuccessPayload(
				TEXT("animation-curve-domain-provider"),
				Target,
				Revision,
				LifecycleReceipt,
				Read,
				Mutation,
				OutPayload,
				OutError);
		}

		bool ImportBlendSpace(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_target_invalid"),
					Target);
			}
			FString MutationPayload;
			if (ExpectedRevision == TEXT("missing"))
			{
				if (bPresent)
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_stale_revision"),
						Target,
						TEXT("existing"));
				}
				const TSharedPtr<FJsonObject>* CreateSpec = nullptr;
				if (!Root->TryGetObjectField(TEXT("create"), CreateSpec)
					|| !CreateSpec || !CreateSpec->IsValid())
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_blend_space_create_required"));
				}
				if (Root->HasField(TEXT("axes"))
					|| Root->HasField(TEXT("samples"))
					|| Root->HasField(TEXT("operation")))
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_create_then_edit_not_atomic"));
				}
				FString Skeleton;
				double Dimension = 0.0;
				if (!(*CreateSpec)->TryGetStringField(
						TEXT("skeleton_ref"), Skeleton)
					|| !(*CreateSpec)->TryGetNumberField(
						TEXT("dimension"), Dimension)
					|| (Dimension != 1.0 && Dimension != 2.0)
					|| AssetName(Target).IsEmpty()
					|| PackagePath(Target).IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_blend_space_create_invalid"));
				}
				FString Declared;
				if ((*CreateSpec)->TryGetStringField(
						TEXT("asset_name"), Declared)
					&& Declared != AssetName(Target))
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_create_name_mismatch"),
						Declared,
						AssetName(Target));
				}
				if ((*CreateSpec)->TryGetStringField(
						TEXT("package_path"), Declared))
				{
					while (Declared.EndsWith(TEXT("/")))
					{
						Declared.LeftChopInline(1);
					}
					if (Declared != PackagePath(Target))
					{
						return Fail(
							OutError,
							TEXT("animation_curve_domain_create_path_mismatch"),
							Declared,
							PackagePath(Target));
					}
				}
				const TSharedPtr<FJsonObject> Create =
					MakeShared<FJsonObject>(**CreateSpec);
				Create->SetStringField(TEXT("asset_name"), AssetName(Target));
				Create->SetNumberField(TEXT("dimension"), Dimension);
				Create->SetStringField(TEXT("package_path"), PackagePath(Target));
				Create->SetStringField(TEXT("skeleton_ref"), Skeleton);
				if (!SubmitAnimationCommand(
						TEXT("animation.blend_space.asset.create"),
						Create,
						MutationPayload,
						OutError))
				{
					return false;
				}
			}
			else
			{
				if (!bPresent)
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_stale_revision"),
						Target,
						TEXT("missing"));
				}
				if (Root->HasField(TEXT("create")))
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_create_requires_missing"));
				}
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
				const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
				if (!Root->TryGetArrayField(TEXT("axes"), Axes)
					|| !Axes
					|| !Root->TryGetArrayField(TEXT("samples"), Samples)
					|| !Samples)
				{
					return Fail(
						OutError,
						TEXT("animation_curve_domain_blend_space_definition_invalid"));
				}
				const TSharedPtr<FJsonObject> Apply = MakeShared<FJsonObject>();
				Apply->SetStringField(
					TEXT("target_ref"), ObjectPathFromPackageIdentity(Target));
				Apply->SetArrayField(TEXT("axes"), *Axes);
				Apply->SetArrayField(TEXT("samples"), *Samples);
				if (!SubmitAnimationCommand(
						TEXT("animation.blend_space.definition.apply"),
						Apply,
						MutationPayload,
						OutError))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> Mutation;
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			if (!ParseLifecycleMutation(
					Target,
					MutationPayload,
					Mutation,
					LifecycleReceipt,
					OutError))
			{
				return false;
			}
			FString AfterPayload;
			TSharedPtr<FJsonObject> After;
			return ReadBlendSpace(Target, AfterPayload, After, OutError)
				&& BuildWriteResponse(
					Target,
					After,
					Mutation,
					LifecycleReceipt,
					OutPayload,
					OutError);
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
			if (Root->HasField(TEXT("create")))
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
			TArray<TSharedPtr<FJsonValue>> MutationValues;
			FString ReceiptSeed;
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
				FString MutationPayload;
				if (!SubmitAnimationCommand(
						Change.Command, Params, MutationPayload, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Mutation;
				FDomainHjsonImportLifecycleReceipt CommandReceipt;
				if (!ParseLifecycleMutation(
						Target,
						MutationPayload,
						Mutation,
						CommandReceipt,
						OutError)
					|| !ReadSequence(Target, ReadPayload, Read, OutError))
				{
					return false;
				}
				MutationValues.Add(MakeShared<FJsonValueObject>(Mutation));
				ReceiptSeed += CommandReceipt.PersistenceReceiptId + TEXT("|");
			}
			if (MutationValues.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_curve_domain_sequence_changes_required"));
			}
			const TSharedPtr<FJsonObject> MutationBatch =
				MakeShared<FJsonObject>();
			MutationBatch->SetArrayField(
				TEXT("operations"), MoveTemp(MutationValues));
			FDomainHjsonImportLifecycleReceipt BatchReceipt;
			BatchReceipt.bMutationApplied = true;
			BatchReceipt.bCompileSettled = true;
			BatchReceipt.bPackageSaved = true;
			BatchReceipt.bUnloadedReloaded = true;
			BatchReceipt.PersistenceReceiptId =
				TEXT("animation-curve-batch:") + Sha256(ReceiptSeed);
			return BuildWriteResponse(
				Target,
				Read,
				MutationBatch,
				BatchReceipt,
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
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("animation_curve"),
						SchemaId,
						RootTypeIdentity),
					Document,
					OutError))
			{
				return false;
			}
			return RootTypeIdentity == BlendSpaceType
				? ImportBlendSpace(
					Target,
					Document.ExpectedRevision,
					Document.Root,
					OutPayloadJson,
					OutError)
				: ImportSequence(
					Target,
					Document.ExpectedRevision,
					Document.Root,
					OutPayloadJson,
					OutError);
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
			return TryCanonicalizeObject(Response, OutPayloadJson, OutError);
		}
		return Fail(
			OutError,
			TEXT("animation_curve_domain_command_not_supported"),
			CommandId);
	}
}
