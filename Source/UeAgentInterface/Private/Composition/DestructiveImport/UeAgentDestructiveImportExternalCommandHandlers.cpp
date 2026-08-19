#include "Composition/DestructiveImport/UeAgentDestructiveImportExternalCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace DomainAdapters::ExternalDataImport;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error) ? Text : TEXT("{}");
		}

		FString PayloadJson(const FUeAgentExternalDataImportResult& Result)
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
			return JsonText(Root);
		}

		FString ErrorJson(const FUeAgentExternalDataImportResult& Result)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Result.ErrorCode);
			Error->SetStringField(TEXT("detail"), Result.ErrorDetail);
			Error->SetStringField(TEXT("source_ref"), Result.SourceRef);
			Error->SetStringField(TEXT("result_ref"), Result.ResultRef);
			return JsonText(Error);
		}

		bool TryString(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* Field,
			FString& Out)
		{
			return Params.IsValid()
				&& Params->TryGetStringField(Field, Out)
				&& !Out.TrimStartAndEnd().IsEmpty();
		}

		bool TryBuildRequest(
			const FUeAgentCommandRequest& CommandRequest,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentExternalDataImportRequest& Out)
		{
			Out.RequestId = CommandRequest.GetRequestId();
			Out.CommandId = CommandRequest.GetCommandId();
			Out.CallerId = CallerContext.GetCallerId();
			const TSharedPtr<FJsonObject> Params = CommandRequest.GetParams();
			if (!TryString(Params, TEXT("source_ref"), Out.SourceRef))
			{
				return false;
			}
			if (Out.CommandId == TEXT("external.localization.po.import"))
			{
				return TryString(Params, TEXT("target_ref"), Out.TargetRef);
			}
			if (Out.CommandId == TEXT("external.skin_weight_profile.import"))
			{
				double LodIndex = -1;
				if (!TryString(Params, TEXT("target_ref"), Out.TargetRef)
					|| !TryString(Params, TEXT("profile_name"), Out.ProfileName)
					|| !Params->TryGetNumberField(TEXT("lod_index"), LodIndex))
				{
					return false;
				}
				Out.LodIndex = static_cast<int32>(LodIndex);
				return LodIndex == static_cast<double>(Out.LodIndex);
			}
			if (!TryString(Params, TEXT("destination_ref"), Out.DestinationRef))
			{
				return false;
			}
			return Out.CommandId != TEXT("external.fbx.animation.import")
				|| TryString(Params, TEXT("skeleton_ref"), Out.SkeletonRef);
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
				if (!TryBuildRequest(Request, CallerContext, DomainRequest))
				{
					FUeAgentExternalDataImportResult Invalid;
					Invalid.CommandId = CommandId;
					Invalid.ErrorCode = TEXT("external_import_params_invalid");
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorJson(Invalid));
				}
				const FUeAgentExternalDataImportResult Result =
					Owner->Execute(DomainRequest, Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorJson(Result));
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentExternalDataImportOwner> Owner;
		};
	}

	bool TryCreateDestructiveImportExternalCommandHandlers(
		TSharedRef<IUeAgentExternalDataImportOwner> Owner,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
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
