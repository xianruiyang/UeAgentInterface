#include "Composition/DestructiveImport/UeAgentDestructiveImportExternalCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace DomainAdapters::ExternalDataImport;

	namespace
	{


		bool PayloadJson(const FUeAgentExternalDataImportResult& Result, FString& OutText, UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("command_id"), Result.CommandId);
			Root->SetStringField(TEXT("source_ref"), Result.SourceRef);
			Root->SetStringField(TEXT("source_sha256"), Result.SourceSha256);
			Root->SetStringField(TEXT("result_ref"), Result.ResultRef);
			Root->SetStringField(TEXT("class_path"), Result.ClassPath);
			Root->SetStringField(TEXT("terminal_state"), Result.TerminalState);
			Root->SetStringField(
				TEXT("persistence_scope"), Result.PersistenceScope);
			Root->SetStringField(TEXT("journal_id"), Result.JournalId);
			Root->SetStringField(
				TEXT("verification_id"), Result.VerificationId);
			Root->SetBoolField(TEXT("changed"), Result.bChanged);
			Root->SetBoolField(TEXT("persisted"), Result.bPersisted);
			Root->SetBoolField(
				TEXT("readback_verified"), Result.bReadbackVerified);
			return Json::TryCanonicalize(Root, OutText, OutError);
		}



		bool ParameterFailure(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			const TCHAR* Constraint, FDiagnostic& OutError)
		{
			const auto* Found = UeAgentJsonDiagnostics::FindFieldValue(Params, Field);
			const TSharedPtr<FJsonValue> Actual = Found ? *Found : nullptr;
			OutError = FDiagnostic::Failure(TEXT("external_import_params_invalid"),
				FString::Printf(TEXT("Parameter %s must be %s; received %s."), Field, Constraint,
					*UeAgentJsonDiagnostics::JsonValueTypeToString(Actual)), TEXT("parameters"));
			OutError.SourceKind = TEXT("command_input");
			OutError.PropertyPath = FString(TEXT("/")) + Field;
			OutError.Source.JsonPath = FString(TEXT("/params/")) + Field;
			if (Actual.IsValid() && Actual->Type != EJson::Object && Actual->Type != EJson::Array)
				OutError.ActualValue = Actual;
			else if (Actual.IsValid())
				OutError.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Actual));
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Constraint);
			OutError.ExpectedValuePresent = true;
			return false;
		}

		bool TryString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			FString& Out, FDiagnostic& OutError)
		{
			if (Params.IsValid() && Params->TryGetStringField(Field, Out)
				&& !Out.TrimStartAndEnd().IsEmpty()) return true;
			return ParameterFailure(Params, Field, TEXT("a non-empty string"), OutError);
		}

		bool TryBuildRequest(const FUeAgentCommandRequest& CommandRequest,
			const FUeAgentCallerContext& CallerContext, FUeAgentExternalDataImportRequest& Out,
			FDiagnostic& OutError)
		{
			Out.RequestId = CommandRequest.GetRequestId();
			Out.CommandId = CommandRequest.GetCommandId();
			Out.CallerId = CallerContext.GetCallerId();
			const auto Params = CommandRequest.GetParams();
			if (!TryString(Params, TEXT("source_ref"), Out.SourceRef, OutError)) return false;
			if (Out.CommandId == TEXT("external.localization.po.import"))
				return TryString(Params, TEXT("target_ref"), Out.TargetRef, OutError);
			if (Out.CommandId == TEXT("external.skin_weight_profile.import"))
			{
				double LodIndex = -1;
				if (!TryString(Params, TEXT("target_ref"), Out.TargetRef, OutError)
					|| !TryString(Params, TEXT("profile_name"), Out.ProfileName, OutError)) return false;
				if (!Params->TryGetNumberField(TEXT("lod_index"), LodIndex))
					return ParameterFailure(Params, TEXT("lod_index"), TEXT("an integer within the int32 range"), OutError);
				// Preserve the integer round-trip predicate without an undefined out-of-range cast.
				if (!FMath::IsFinite(LodIndex) || LodIndex < MIN_int32 || LodIndex > MAX_int32)
				{
					ParameterFailure(Params, TEXT("lod_index"), TEXT("an integer within the int32 range"), OutError);
					if (!FMath::IsFinite(LodIndex)) OutError.ActualValue = MakeShared<FJsonValueString>(FString::SanitizeFloat(LodIndex));
					return false;
				}
				Out.LodIndex = static_cast<int32>(LodIndex);
				if (LodIndex != static_cast<double>(Out.LodIndex))
					return ParameterFailure(Params, TEXT("lod_index"), TEXT("an integer within the int32 range"), OutError);
				return true;
			}
			if (!TryString(Params, TEXT("destination_ref"), Out.DestinationRef, OutError)) return false;
			return Out.CommandId != TEXT("external.fbx.animation.import")
				|| TryString(Params, TEXT("skeleton_ref"), Out.SkeletonRef, OutError);
		}

		class FExternalDataImportCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FExternalDataImportCommandHandler(
				FString InCommandId,
				TSharedRef<IUeAgentExternalDataImportOwner> InOwner)
				: CommandId(MoveTemp(InCommandId)), Owner(MoveTemp(InOwner))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentExternalDataImportRequest DomainRequest;
				FDiagnostic ParameterError;
				if (!TryBuildRequest(Request, CallerContext, DomainRequest, ParameterError))
				{
					FUeAgentExternalDataImportResult Invalid;
					Invalid.CommandId = CommandId;
					Invalid.Diagnostic = MoveTemp(ParameterError);
					FString Payload;
					UE::UAI::Contract::FDiagnostic SerializationError;
					if (!PayloadJson(Invalid, Payload, SerializationError)) Invalid.Diagnostic.Append(SerializationError);
					return FUeAgentCommandResult::Failure(Request, Payload, Invalid.Diagnostic);
				}
				const FUeAgentExternalDataImportResult Result =
					Owner->Execute(DomainRequest, Cancellation);
				FString Payload;
				UE::UAI::Contract::FDiagnostic SerializationError;
				auto Diagnostic = Result.Diagnostic;
				Diagnostic.TerminalStatus = Result.TerminalState;
				if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Result.ResultRef;
				if (Result.bChanged) Diagnostic.MutationStarted = true;
				if (!PayloadJson(Result, Payload, SerializationError))
				{
					Diagnostic.Append(SerializationError);
					return FUeAgentCommandResult::Failure(Request, Payload, Diagnostic);
				}
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, Diagnostic);
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentExternalDataImportOwner> Owner;
		};
	}

	bool TryCreateDestructiveImportExternalCommandHandlers(
		TSharedRef<IUeAgentExternalDataImportOwner> Owner,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const TCHAR* CommandId : {
			TEXT("external.fbx.animation.import"),
			TEXT("external.fbx.skeletal_mesh.import"),
			TEXT("external.geometry_cache.import"),
			TEXT("external.texture.import"),
			TEXT("external.light_profile.import"),
			TEXT("external.sparse_volume_texture.import"),
			TEXT("external.sound_wave.import"),
			TEXT("external.localization.po.import"),
			TEXT("external.skin_weight_profile.import")})
		{
			OutHandlers.Add(MakeShared<FExternalDataImportCommandHandler>(
				CommandId, Owner));
		}
		return true;
	}
}
