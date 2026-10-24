#include "DomainAdapters/AnimationCurve/UeAgentAnimationCurveDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "DomainAdapters/AnimationCurveCommands/UeAgentAnimationCurveCommandMechanicsBoundary.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringWorkspace.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
		const FString BlendSpace1DType = TEXT("/Script/Engine.BlendSpace1D");
		const FString SequenceType = TEXT("/Script/Engine.AnimSequence");
		const FString MontageType = TEXT("/Script/Engine.AnimMontage");
		const FString PoseType = TEXT("/Script/Engine.PoseAsset");

		bool IsBlendSpaceType(const FString& Type)
		{
			return Type == BlendSpaceType || Type == BlendSpace1DType;
		}

		class FNeverCancelled final
			: public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(
			FWorkspaceError& OutError,
			UE::UAI::Contract::FDiagnostic Diagnostic,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
			return false;
		}

		UE::UAI::Contract::FDiagnostic InputValueFailure(
			const FString& Code, const FString& Reason, const FString& Pointer,
			const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual,
			const FString& Target, const bool bCommandInput = false)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected,
				Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>(), TEXT("input_decode"), Reason);
			Error.ActualValuePresent = Actual.IsValid();
			Error.TargetPath = Target;
			if (bCommandInput) Error.SourceKind = TEXT("command_input");
			return Error;
		}

		UE::UAI::Contract::FDiagnostic StaleRevisionFailure(
			const FString& Target, const FString& ExpectedRevision, const FString& CurrentRevision)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_stale_revision"),
				ExpectedRevision == TEXT("missing")
					? TEXT("The target already exists, but this import requires an absent target.")
					: CurrentRevision == TEXT("missing")
						? TEXT("The target is missing, but this import requires the previously exported asset.")
						: TEXT("The persisted asset has changed since this workspace was exported. Export it again before applying the changes."),
				TEXT("input_preflight"));
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("target_present"), ExpectedRevision != TEXT("missing"));
			Expected->SetBoolField(TEXT("revision_matches"), true);
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("target_present"), CurrentRevision != TEXT("missing"));
			Actual->SetBoolField(TEXT("revision_matches"), false);
			Error.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
			Error.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
			Error.TargetPath = Target;
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = TEXT("/expected_revision");
			Error.PropertyPath = TEXT("/expected_revision");
			return Error;
		}

		UE::UAI::Contract::FDiagnostic CreateRequiresMissingFailure(const FString& Target)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_create_requires_missing"),
				TEXT("The create definition requires expected_revision to be 'missing'; it cannot be used to update an existing asset."), TEXT("input_decode"));
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("missing"));
			Error.ActualValue = MakeShared<FJsonValueString>(TEXT("existing asset revision"));
			Error.PropertyPath = TEXT("/root/create");
			Error.Source.JsonPath = TEXT("/root/create");
			Error.SourceKind = TEXT("related_input");
			Error.TargetPath = Target;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic ProfileFailure(const FString& Target, const FString& RootType)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_semantic_profile_missing"),
				TEXT("No animation authoring profile is registered for the requested asset type."), TEXT("authoring_resolve"));
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.PropertyPath = TEXT("authoring_profile_registered");
			Error.SourceKind = TEXT("native_result");
			Error.SubjectId = RootType;
			Error.TargetPath = Target;
			Error.PublicMessage += TEXT(" Requested type: ") + RootType + TEXT(".");
			return Error;
		}

		UE::UAI::Contract::FDiagnostic TargetTypeFailure(
			const FString& Target, const FString& RootType, const UObject* Asset)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_target_type_invalid"),
				Asset ? TEXT("The loaded animation asset has a different type from the workspace root.")
					: TEXT("The animation asset could not be loaded with the requested type."), TEXT("target_resolve"));
			Error.ExpectedValue = MakeShared<FJsonValueString>(RootType);
			if (Asset) Error.ActualValue = MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName());
			else Error.ActualValue = MakeShared<FJsonValueNull>();
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			return Error;
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
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_curve_domain_internal_request_invalid"), Diagnostic, TEXT("command_decode")));
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
			UE::UAI::Contract::FDiagnostic CompositionError;
			const bool bComposed = Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, Dispatch, CompositionError);
			if (!bComposed || !Registry.IsValid())
			{
				if (CompositionError.IsEmpty())
				{
					CompositionError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_command_registry_missing"),
						TEXT("The active command composition did not provide a command registry."), TEXT("command_resolve"));
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetBoolField(TEXT("composition_succeeded"), true);
					Expected->SetBoolField(TEXT("registry_available"), true);
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("composition_succeeded"), bComposed);
					Actual->SetBoolField(TEXT("registry_available"), Registry.IsValid());
					CompositionError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
					CompositionError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					CompositionError.SourceKind = TEXT("native_result");
				}
				return Fail(OutError, CompositionError, FString(), TEXT("/"));
			}
			const CommandContract::FUeAgentFrozenCommandContract* FrozenContract =
				Registry->FindExact(CommandId);
			if (!FrozenContract)
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_command_contract_missing"),
					TEXT("The active registry has no contract for animation command ") + CommandId + TEXT("."), TEXT("command_resolve"));
				Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Error.PropertyPath = TEXT("command_contract_registered");
				Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error));
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
			UE::UAI::Contract::FDiagnostic MechanicsError;
			const bool bMechanicsResolved = AnimationCurveCommands::TryResolveMechanics(
				CommandId, Mechanics, Scopes, MechanicsError);
			if (!bMechanicsResolved || !Mechanics.IsValid())
			{
				if (MechanicsError.IsEmpty())
				{
					MechanicsError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_command_mechanics_missing"),
						TEXT("No executable handler was resolved for animation command ") + CommandId + TEXT("."), TEXT("command_resolve"));
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetBoolField(TEXT("resolved"), true);
					Expected->SetBoolField(TEXT("handler_available"), true);
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("resolved"), bMechanicsResolved);
					Actual->SetBoolField(TEXT("handler_available"), Mechanics.IsValid());
					MechanicsError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
					MechanicsError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					MechanicsError.SourceKind = TEXT("native_result");
				}
				return Fail(OutError, MoveTemp(MechanicsError));
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
					Result.GetDiagnostic(),
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryParseObject(Payload, Out, Error) || !Out.IsValid())
			{
				return Fail(OutError, Error, FString(), TEXT("/"));
			}
			return true;
		}

		bool StableRevision(
			const FString& Target,
			const TSharedPtr<FJsonObject>& Read,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(Target);
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_package_identity_invalid"),
					TEXT("The target must identify a valid long Unreal asset package name."), TEXT("/target"),
					MakeShared<FJsonValueString>(TEXT("valid long asset package name")), MakeShared<FJsonValueString>(PackageName), Target, true));
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			TArray<uint8> Bytes;
			const bool bLoaded = FFileHelper::LoadFileToArray(Bytes, *Filename);
			if (!bLoaded || Bytes.IsEmpty())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_persisted_package_missing"),
					bLoaded ? TEXT("The persisted asset package is empty, so its revision cannot be read.")
						: TEXT("The persisted asset package could not be read to determine its revision."), TEXT("revision_read"));
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("read_succeeded"), bLoaded);
				Actual->SetNumberField(TEXT("byte_count"), Bytes.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable, non-empty persisted asset package"));
				Error.SubjectId = Filename;
				Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error), Target);
			}
			OutRevision = Sha256(Bytes);
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

		bool ReadSemanticAsset(
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			UObject* Object = nullptr;
			FUeAgentContentGraphCapture Capture;
			return TryCaptureObject(
					Target,
					RootType,
					Object,
					Capture,
					OutError)
				&& TryBuildCaptureSnapshot(
					Target,
					RootType,
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
			return IsBlendSpaceType(RootType) || RootType == MontageType || RootType == PoseType
				? ReadSemanticAsset(Target, RootType, OutPayload, OutObject, OutError)
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

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentAnimationAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value, FString(), TEXT("/"));
			}
			return true;
		}

		const UeAgentAnimationAuthoring::FProfile* ResolveAuthoringProfile(
			const FString& RootType)
		{
			using namespace UeAgentAnimationAuthoring;
			if (IsBlendSpaceType(RootType)) return FindProfile(EAssetKind::BlendSpace);
			if (RootType == MontageType) return FindProfile(EAssetKind::AnimMontage);
			if (RootType == PoseType) return FindProfile(EAssetKind::PoseAsset);
			return RootType == SequenceType ? FindProfile(EAssetKind::AnimSequence) : nullptr;
		}

		bool ParseLifecycleMutation(
			const FString& Target,
			const FString& MutationPayload,
			TSharedPtr<FJsonObject>& OutMutation,
			FDomainHjsonImportLifecycleReceipt& OutReceipt,
			FWorkspaceError& OutError)
		{
			ON_SCOPE_EXIT
			{
				if (!OutError.HasErrors()) return;
				auto Diagnostic = OutError.GetDiagnostic();
				TFunction<void(UE::UAI::Contract::FDiagnostic&)> Locate = [&](auto& Item)
				{
					Item.SourceKind = TEXT("native_result");
					if (Item.TargetPath.IsEmpty()) Item.TargetPath = Target;
					for (auto& Child : Item.Diagnostics) Locate(Child);
					if (Item.PublicCause) {
						auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Item.PublicCause);
						Locate(*Cause); Item.PublicCause = MoveTemp(Cause);
					}
				};
				Locate(Diagnostic);
				OutError.SetFailure(MoveTemp(Diagnostic));
			};
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(
					MutationPayload, OutMutation, JsonError)
				|| !OutMutation.IsValid())
			{
				return Fail(OutError, JsonError, Target, TEXT("/"));
			}
			FString TerminalStatus;
			FString MutationTarget;
			FString ReceiptId;
			bool bMutationApplied = false;
			bool bCompileSettled = false;
			bool bSaved = false;
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
					|| !bMutationApplied || !bCompileSettled
					|| !bSaved)
				{
					const TCHAR* Field = TerminalStatus != TEXT("succeeded") ? TEXT("terminal_status")
						: ObjectPathFromPackageIdentity(MutationTarget) != ObjectPathFromPackageIdentity(Target) ? TEXT("target_ref")
						: !bMutationApplied ? TEXT("mutation_applied") : !bCompileSettled ? TEXT("compile_settled") : TEXT("saved");
					TSharedPtr<FJsonValue> Expected = MakeShared<FJsonValueBoolean>(true);
					if (FString(Field) == TEXT("terminal_status")) Expected = MakeShared<FJsonValueString>(TEXT("succeeded"));
					else if (FString(Field) == TEXT("target_ref")) Expected = MakeShared<FJsonValueString>(Target);
					return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_curve_domain_lifecycle_receipt_invalid"),
						FString(TEXT("/")) + Field, Expected, OutMutation->TryGetField(Field), TEXT("response_decode"),
						FString(TEXT("The native lifecycle receipt did not provide the required ")) + Field + TEXT(" confirmation.")), Target);
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
					const bool bOperationInvalid = OperationId.IsEmpty();
					const bool bProjectionInvalid = !Projection || !Projection->IsValid();
					const bool bProfileInvalid = ProfileId.IsEmpty();
					const FString Pointer = bOperationInvalid ? TEXT("/operation_id")
						: bProjectionInvalid ? TEXT("/projection")
						: bProfileInvalid ? TEXT("/projection/profile_id") : TEXT("/projection/payload");
					const auto ActualField = bOperationInvalid ? OutMutation->TryGetField(TEXT("operation_id"))
						: bProjectionInvalid ? OutMutation->TryGetField(TEXT("projection"))
						: (*Projection)->TryGetField(bProfileInvalid ? TEXT("profile_id") : TEXT("payload"));
					const bool bStringRequired = bOperationInvalid || (!bProjectionInvalid && bProfileInvalid);
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetBoolField(TEXT("present"), true);
					Expected->SetBoolField(bStringRequired ? TEXT("is_string") : TEXT("is_object"), true);
					if (bStringRequired) Expected->SetBoolField(TEXT("non_empty"), true);
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("present"), ActualField.IsValid());
					Actual->SetBoolField(bStringRequired ? TEXT("is_string") : TEXT("is_object"), ActualField.IsValid()
						&& ActualField->Type == (bStringRequired ? EJson::String : EJson::Object));
					if (bStringRequired && ActualField.IsValid() && ActualField->Type == EJson::String)
						Actual->SetBoolField(TEXT("non_empty"), !ActualField->AsString().IsEmpty());
					return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_curve_domain_lifecycle_receipt_invalid"),
						Pointer, MakeShared<FJsonValueObject>(Expected), MakeShared<FJsonValueObject>(Actual), TEXT("response_decode"),
						TEXT("The native timeline result requires ") + Pointer
							+ (bStringRequired ? TEXT(" to be a non-empty string.") : TEXT(" to be an object."))), Target);
				}
				bMutationApplied = true;
				bCompileSettled = true;
				bSaved = true;
				ReceiptId = TEXT("timeline-operation:") + OperationId;
			}
			OutReceipt.bMutationApplied = bMutationApplied;
			OutReceipt.bNativeUpdateSettled = bCompileSettled;
			OutReceipt.bPackageSaved = bSaved;
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
			const FString& RootType,
			const FString& ExpectedRevision,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			const auto& Root = Document.Root;
			auto LocateNative = [&]()
			{
				auto Diagnostic = OutError.GetDiagnostic();
				TFunction<void(UE::UAI::Contract::FDiagnostic&)> Locate = [&](auto& Item)
				{
					for (auto& Child : Item.Diagnostics) Locate(Child);
					if (Item.PublicCause) { auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Item.PublicCause); Locate(*Cause); Item.PublicCause = MoveTemp(Cause); }
					const FString Pointer = Item.SemanticPointer.IsEmpty() ? Item.Source.JsonPath : Item.SemanticPointer;
					bool bRootDefinition = false;
					for (const TCHAR* Field : {TEXT("axes"), TEXT("samples")})
						if (Root->HasField(Field) && (Pointer == FString(TEXT("/")) + Field || Pointer.StartsWith(FString(TEXT("/")) + Field + TEXT("/")))) bRootDefinition = true;
					Document.LocateDiagnostic(Item, ExpectedRevision == TEXT("missing") && !bRootDefinition ? TEXT("/root/create") : TEXT("/root"));
				};
				Locate(Diagnostic); OutError.SetFailure(MoveTemp(Diagnostic));
			};
			bool bPresent = false;
			if (!TryInspectObjectTargetPresence(Target, bPresent))
			{
				return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_target_invalid"),
					TEXT("The import target must be a valid Unreal asset object or package path."), TEXT("/target"),
					MakeShared<FJsonValueString>(TEXT("valid asset object or package path")), MakeShared<FJsonValueString>(Target), Target, true));
			}
			FString MutationPayload;
			if (ExpectedRevision == TEXT("missing"))
			{
				if (bPresent)
				{
					return Fail(OutError, StaleRevisionFailure(Target, ExpectedRevision, TEXT("existing")));
				}
				const TSharedPtr<FJsonObject>* CreateSpec = nullptr;
				if (!Root->TryGetObjectField(TEXT("create"), CreateSpec)
					|| !CreateSpec || !CreateSpec->IsValid())
				{
					return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_blend_space_create_required"),
						TEXT("Creating a BlendSpace requires an object-valued create definition."), TEXT("/root/create"),
						MakeShared<FJsonValueString>(TEXT("object")), Root->TryGetField(TEXT("create")), Target));
				}
				if (Root->HasField(TEXT("operation")))
				{
					return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_create_operation_conflict"),
						TEXT("A BlendSpace create definition cannot also declare operation."), TEXT("/root/operation"),
						MakeShared<FJsonValueString>(TEXT("field absent during creation")), Root->TryGetField(TEXT("operation")), Target));
				}
				FString Skeleton;
				double Dimension = 0.0;
				auto InvalidCreate = [&](const TCHAR* Field, const FString& Expected, const FString& Code = TEXT("animation_curve_domain_blend_space_create_invalid"))
				{
					return Fail(OutError, InputValueFailure(Code,
						TEXT("The BlendSpace create field must satisfy: ") + Expected + TEXT("."),
						FString(TEXT("/root/create/")) + Field, MakeShared<FJsonValueString>(Expected), (*CreateSpec)->TryGetField(Field), Target));
				};
				if (!(*CreateSpec)->TryGetStringField(TEXT("skeleton_ref"), Skeleton) || Skeleton.IsEmpty())
					return InvalidCreate(TEXT("skeleton_ref"), TEXT("a non-empty skeleton reference"));
				if (!(*CreateSpec)->TryGetNumberField(TEXT("dimension"), Dimension) || (Dimension != 1.0 && Dimension != 2.0))
					return InvalidCreate(TEXT("dimension"), TEXT("1 or 2"));
				if (AssetName(Target).IsEmpty() || PackagePath(Target).IsEmpty())
				{
					return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_blend_space_create_invalid"),
						TEXT("The target must identify a valid asset package and name."), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("valid asset package and name")), MakeShared<FJsonValueString>(Target), Target, true));
				}
				FString Declared;
				if ((*CreateSpec)->TryGetStringField(
						TEXT("asset_name"), Declared)
					&& Declared != AssetName(Target))
				{
					return InvalidCreate(TEXT("asset_name"), AssetName(Target), TEXT("animation_curve_domain_create_name_mismatch"));
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
						return InvalidCreate(TEXT("package_path"), PackagePath(Target), TEXT("animation_curve_domain_create_path_mismatch"));
					}
				}
				const TSharedPtr<FJsonObject> Create =
					MakeShared<FJsonObject>(**CreateSpec);
				Create->SetStringField(TEXT("asset_name"), AssetName(Target));
				Create->SetNumberField(TEXT("dimension"), Dimension);
				Create->SetStringField(TEXT("package_path"), PackagePath(Target));
				Create->SetStringField(TEXT("skeleton_ref"), Skeleton);
				for (const TCHAR* Field : {TEXT("axes"), TEXT("samples")})
					if (Root->HasField(Field)) Create->SetField(Field, Root->TryGetField(Field));
				if (!SubmitAnimationCommand(
						TEXT("animation.blend_space.asset.create"),
						Create,
						MutationPayload,
						OutError))
				{
					LocateNative();
					return false;
				}
			}
			else
			{
				if (!bPresent)
				{
					return Fail(OutError, StaleRevisionFailure(Target, ExpectedRevision, TEXT("missing")));
				}
				if (Root->HasField(TEXT("create")))
				{
					return Fail(OutError, CreateRequiresMissingFailure(Target));
				}
				FString BeforePayload;
				TSharedPtr<FJsonObject> Before;
				FString Current;
				if (!ReadSemanticAsset(Target, RootType, BeforePayload, Before, OutError)
					|| !StableRevision(Target, Before, Current, OutError))
				{
					return false;
				}
				if (Current != ExpectedRevision)
				{
					return Fail(OutError, StaleRevisionFailure(Target, ExpectedRevision, Current));
				}
				const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
				for (const TCHAR* Field : {TEXT("axes"), TEXT("samples")})
				{
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					if (!Root->TryGetArrayField(Field, Values) || !Values)
						return Fail(OutError, UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_curve_domain_blend_space_definition_invalid"),
							FString(TEXT("/root/")) + Field, MakeShared<FJsonValueString>(TEXT("array")), Root->TryGetField(Field),
							TEXT("input_decode"), TEXT("BlendSpace definition fields must be arrays.")), Target);
					if (FString(Field) == TEXT("axes")) Axes = Values; else Samples = Values;
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
					LocateNative();
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
			return ReadSemanticAsset(Target, RootType, AfterPayload, After, OutError)
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
			const bool bCreate = ExpectedRevision == TEXT("missing");
			if (!bCreate && Root->HasField(TEXT("create")))
			{
				return Fail(OutError, CreateRequiresMissingFailure(Target));
			}
			if (!bCreate)
			{
				FString ReadPayload, Current;
				TSharedPtr<FJsonObject> Read;
				if (!ReadSequence(Target, ReadPayload, Read, OutError)
					|| !StableRevision(Target, Read, Current, OutError)) return false;
				if (Current != ExpectedRevision)
					return Fail(OutError, StaleRevisionFailure(Target, ExpectedRevision, Current));
			}

            using namespace UeAgentAnimationAuthoring;
            if (!EnsureAuthoringRegistered(OutError)) return false;
            FAssetApplyRequest Request;
            Request.Kind = EAssetKind::AnimSequence;
            Request.AssetPath = PackageIdentity(Target);
            Request.Operation = bCreate ? UeAgentApplyInputContract::EOperation::Create : UeAgentApplyInputContract::EOperation::Update;
            Request.bCompile = true;
            Request.bSave = true;
            Request.Document = MakeShared<FJsonObject>();
            if (bCreate) {
                const TSharedPtr<FJsonObject>* Create = nullptr;
                if (!Root->TryGetObjectField(TEXT("create"), Create) || !Create || !*Create)
                    return Fail(OutError, InputValueFailure(TEXT("animation_create_required"),
                        TEXT("Creating an animation sequence requires an object-valued create definition."), TEXT("/root/create"),
                        MakeShared<FJsonValueString>(TEXT("object")), Root->TryGetField(TEXT("create")), Target));
                if (const auto Skeleton = (*Create)->TryGetField(TEXT("skeleton_ref")))
                    Request.Document->SetField(TEXT("skeleton"), Skeleton);
            }
            for (const TCHAR* Field : {TEXT("metadata_changes"), TEXT("curve_changes"), TEXT("event_changes")})
                if (const auto Value = Root->TryGetField(Field)) Request.Document->SetField(Field, Value);
            if (!bCreate && Request.Document->Values.IsEmpty()) {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetNumberField(TEXT("change_field_count"), Request.Document->Values.Num());
                auto Error = InputValueFailure(TEXT("animation_curve_domain_sequence_changes_required"),
                    TEXT("An animation sequence update requires metadata_changes, curve_changes or event_changes."), TEXT("/root"),
                    MakeShared<FJsonValueString>(TEXT("at least one of metadata_changes, curve_changes, event_changes")),
                    MakeShared<FJsonValueObject>(Actual), Target);
                Error.SourceKind = TEXT("related_input");
                return Fail(OutError, MoveTemp(Error));
            }
            Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
                if (!TryReadbackImportPayload(TEXT("animation-curve-domain-provider"), PackageIdentity(Target), Target,
                    nullptr, TEXT("animation_semantic_readback_payload_invalid"), TEXT("animation_compile_not_settled"),
                    [&](FString& Payload, FString& Revision, FWorkspaceError& ReadError) {
                        TSharedPtr<FJsonObject> After;
                        return ReadSemanticAsset(Target, SequenceType, Payload, After, ReadError)
                            && StableRevision(Target, After, Revision, ReadError);
                    }, OutPayload, OutError)) {
                    Failure.Diagnostic = OutError.GetDiagnostic(); return false;
                }
                return true;
            };
            FAssetApplyResult Result;
            if (!ApplyAsset(Request, Result) || !Result.IsApplied())
                return Fail(OutError, Result.Diagnostic, Target, TEXT("/"));
            return true;
        }

		bool ImportStructuredAnimation(const FString& Target, const FString& RootType, const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root, FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace UeAgentAnimationAuthoring;
			if (!EnsureAuthoringRegistered(OutError)) return false;
			FAssetApplyRequest Request;
			Request.Kind = RootType == PoseType ? EAssetKind::PoseAsset : EAssetKind::AnimMontage;
			Request.AssetPath = PackageIdentity(Target);
			Request.bCompile = true;
			Request.bSave = true;
			Request.Document = MakeShared<FJsonObject>();
			if (ExpectedRevision == TEXT("missing")) {
				Request.Operation = UeAgentApplyInputContract::EOperation::Create;
				const TSharedPtr<FJsonObject>* Create = nullptr;
				if (!Root->TryGetObjectField(TEXT("create"), Create) || !Create || !*Create)
					return Fail(OutError, InputValueFailure(TEXT("animation_create_required"),
						TEXT("Creating this animation asset requires an object-valued create definition."), TEXT("/root/create"),
						MakeShared<FJsonValueString>(TEXT("object")), Root->TryGetField(TEXT("create")), Target));
				auto Settings = MakeShared<FJsonObject>();
				FString Path;
				if ((*Create)->TryGetStringField(TEXT("skeleton_ref"), Path)) Settings->SetField(TEXT("target_skeleton"), (*Create)->TryGetField(TEXT("skeleton_ref")));
				if ((*Create)->TryGetStringField(TEXT("source_animation_ref"), Path)) Settings->SetField(TEXT("source_animation"), (*Create)->TryGetField(TEXT("source_animation_ref")));
				Request.Document->SetObjectField(TEXT("settings"), Settings);
			} else {
				FString Payload, Revision;
				TSharedPtr<FJsonObject> Before;
				if (!ReadSemanticAsset(Target, RootType, Payload, Before, OutError) ||
					!StableRevision(Target, Before, Revision, OutError)) return false;
				if (Revision != ExpectedRevision) return Fail(OutError, StaleRevisionFailure(Target, ExpectedRevision, Revision));
				Request.Operation = UeAgentApplyInputContract::EOperation::Update;
			}
			for (const TCHAR* Field : {TEXT("slots"), TEXT("sections"), TEXT("notify_tracks"), TEXT("notifies"), TEXT("metadata")})
				if (const auto Value = Root->TryGetField(Field)) Request.Document->SetField(Field, Value);
			if (RootType == PoseType) {
				for (const TCHAR* Field : {TEXT("pose_data"), TEXT("pose_data_definitions")})
					if (const auto Value = Root->TryGetField(Field)) Request.Document->SetField(Field, Value);
				const TSharedPtr<FJsonObject>* Settings = nullptr;
				if (Root->TryGetObjectField(TEXT("settings"), Settings)) {
					const TSharedPtr<FJsonObject>* Existing = nullptr;
					auto Merged = MakeShared<FJsonObject>();
					if (Request.Document->TryGetObjectField(TEXT("settings"), Existing)) Merged->Values = (*Existing)->Values;
					for (const auto& Field : (*Settings)->Values) Merged->SetField(Field.Key, Field.Value);
					Request.Document->SetObjectField(TEXT("settings"), Merged);
				}
			}
			Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
				if (!TryReadbackImportPayload(TEXT("animation-curve-domain-provider"), PackageIdentity(Target), Target,
					nullptr, TEXT("animation_semantic_readback_payload_invalid"), TEXT("animation_compile_not_settled"),
					[&](FString& Payload, FString& Revision, FWorkspaceError& ReadError) {
						TSharedPtr<FJsonObject> Read;
						return ReadSemanticAsset(Target, RootType, Payload, Read, ReadError) && StableRevision(Target, Read, Revision, ReadError);
					}, OutPayload, OutError)) {
					Failure.Diagnostic = OutError.GetDiagnostic(); return false;
				}
				return true;
			};
			FAssetApplyResult Result;
			if (!ApplyAsset(Request, Result) || !Result.IsApplied()) return Fail(OutError, Result.Diagnostic, Target, TEXT("/"));
			return true;
		}

		bool TryImportSemanticWorkspace(
			const FString& Target,
			const FString& RootType,
			const FResolvedDomainHjsonDocument& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			using namespace UE::UAI::Composition::HjsonImport;

			if (!Document.SemanticWorkspace.IsValid())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_semantic_workspace_missing"),
					TEXT("The resolved animation workspace directory has no semantic workspace data."), TEXT("workspace_resolve"));
				Error.PropertyPath = TEXT("semantic_workspace_resolved");
				Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Error.SourceKind = TEXT("native_result");
				return Fail(OutError, MoveTemp(Error), Target);
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const UeAgentAnimationAuthoring::FProfile* Profile =
				ResolveAuthoringProfile(RootType);
			if (!Profile)
			{
				return Fail(OutError, ProfileFailure(Target, RootType));
			}

			const FString TargetPackage = PackageIdentity(Target);
			UObject* Asset = UeAgentAnimationAuthoring::LoadAsset(
				Profile->Kind, TargetPackage);
			if (!Asset || Asset->GetClass()->GetPathName() != RootType)
			{
				return Fail(OutError, TargetTypeFailure(Target, RootType, Asset));
			}

			return UeAgentAnimationAuthoring::ImportAssetWorkspace(
				*Profile, *Asset, Document.SemanticWorkspace.ToSharedRef(),
				[&Target, &RootType](
					FString& OutReadbackPayload,
					FString& OutRevision,
					FWorkspaceError& OutReadError)
				{
					TSharedPtr<FJsonObject> Read;
					return ReadSemanticAsset(
							Target, RootType, OutReadbackPayload, Read, OutReadError)
						&& StableRevision(
							Target, Read, OutRevision, OutReadError);
				},
				OutPayload,
				OutError);
		}

		bool TryExport(
			const FString& Target,
			const FString& RootType,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_export_path_required"),
					TEXT("Export requires a non-empty workspace_path string."), TEXT("/workspace_path"),
					MakeShared<FJsonValueString>(TEXT("non-empty workspace directory path")), Params->TryGetField(TEXT("workspace_path")), Target, true));
			}
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const UeAgentAnimationAuthoring::FProfile* Profile =
				ResolveAuthoringProfile(RootType);
			UObject* Asset = Profile
				? UeAgentAnimationAuthoring::LoadAsset(
					Profile->Kind, PackageIdentity(Target))
				: nullptr;
			if (!Profile || !Asset || Asset->GetClass()->GetPathName() != RootType)
			{
				return Fail(OutError, !Profile ? ProfileFailure(Target, RootType) : TargetTypeFailure(Target, RootType, Asset));
			}

			UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
			if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Asset, WorkspacePath, TEXT("llm_authoring"), ExportResult))
			{
				OutError.SetFailure(ExportResult.Diagnostic, Target);
				return false;
			}
			const TSharedPtr<FJsonObject> Result = ExportResult.ToPublicJsonObject();
			return TryCanonicalizeObject(Result, OutPayload, OutError);
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, JsonError, FString(), TEXT("/"));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_target_identity_mismatch"),
				TEXT("The command target must match the target selected for the animation domain."), TEXT("/target"),
				MakeShared<FJsonValueString>(TargetIdentity), Params->TryGetField(TEXT("target")), TargetIdentity, true));
		}
		if (!IsBlendSpaceType(RootTypeIdentity)
			&& RootTypeIdentity != SequenceType && RootTypeIdentity != MontageType && RootTypeIdentity != PoseType)
		{
			TArray<TSharedPtr<FJsonValue>> SupportedTypes;
			for (const auto& Type : {BlendSpaceType, BlendSpace1DType, SequenceType, MontageType, PoseType})
				SupportedTypes.Add(MakeShared<FJsonValueString>(Type));
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_domain_root_type_unsupported"),
				TEXT("The resolved root type is not supported by the animation curve domain."), TEXT("domain_resolve"));
			Error.ExpectedValue = MakeShared<FJsonValueArray>(SupportedTypes);
			Error.ActualValue = MakeShared<FJsonValueString>(RootTypeIdentity);
			Error.SourceKind = TEXT("native_result");
			return Fail(OutError, MoveTemp(Error), TargetIdentity);
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
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				return TryImportSemanticWorkspace(
					Target,
					RootTypeIdentity,
					Document,
					OutPayloadJson,
					OutError);
			}
			if (RootTypeIdentity == MontageType || RootTypeIdentity == PoseType)
				return ImportStructuredAnimation(Target, RootTypeIdentity, Document.ExpectedRevision, Document.Root, OutPayloadJson, OutError);
			return IsBlendSpaceType(RootTypeIdentity)
				? ImportBlendSpace(
					Target,
					RootTypeIdentity,
					Document.ExpectedRevision,
					Document,
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
			bOutReadOnly = true;
			return TryExport(
				Target,
				RootTypeIdentity,
				Params,
				OutPayloadJson,
				OutError);
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
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Read);
			bOutReadOnly = true;
			return TryCanonicalizeObject(Response, OutPayloadJson, OutError);
		}
		return Fail(OutError, InputValueFailure(TEXT("animation_curve_domain_command_not_supported"),
			TEXT("The animation domain adapter only accepts content_graph commands."), TEXT("/command_id"),
			MakeShared<FJsonValueString>(TEXT("content_graph.*")), MakeShared<FJsonValueString>(CommandId), Target, true));
	}
}
