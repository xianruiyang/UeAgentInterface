#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error) ? Text : FString();
		}

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			return JsonText(Error);
		}

		bool TryReadApplicationError(
			const FString& PayloadJson,
			const TCHAR* ErrorField,
			FString& OutErrorJson)
		{
			OutErrorJson.Reset();
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadJson);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(ErrorField, Error)
				&& Error
				&& Error->IsValid())
			{
				OutErrorJson = JsonText(Error->ToSharedRef());
			}
			return true;
		}

		class FGraphCompoundCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FGraphCompoundCommandHandler(
				FUeAgentGraphCompoundActivationProfile InProfile,
				TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Profile(MoveTemp(InProfile))
				, QueryPort(MoveTemp(InQueryPort))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Profile.GetCommandId();
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (Profile.GetOperationKind() == TEXT("query"))
				{
					const FUeAgentQueryResult Result = QueryPort->Submit(
						FUeAgentQueryRequest(
							Profile.GetDescriptorIdentity(),
							Request.GetRequestId(),
							FUeAgentContractValue(Request.GetParamsJson())),
						CallerContext,
						Cancellation);
					if (!Result.IsValid()
						|| !(Result.GetDescriptorIdentity()
							== Profile.GetDescriptorIdentity())
						|| Result.GetRequestId() != Request.GetRequestId())
					{
						return FUeAgentCommandResult::Failure(
							Request,
							TEXT("{}"),
							ErrorJson(TEXT("command_query_result_invalid"),
								Request.GetRequestId(),
								TEXT("C3Q returned an invalid or mismatched result.")));
					}
					const FString& PayloadJson = Result.GetValue().GetJsonValue();
					FString ApplicationError;
					if (!TryReadApplicationError(
							PayloadJson, TEXT("query_error"), ApplicationError))
					{
						return FUeAgentCommandResult::Failure(
							Request,
							TEXT("{}"),
							ErrorJson(TEXT("command_query_payload_invalid"),
								Request.GetRequestId(),
								TEXT("C3Q returned a non-object JSON payload.")));
					}
					return ApplicationError.IsEmpty()
						? FUeAgentCommandResult::Success(Request, PayloadJson)
						: FUeAgentCommandResult::Failure(
							Request, PayloadJson, ApplicationError);
				}

				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Profile.GetDescriptorIdentity(),
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity()
						== Profile.GetDescriptorIdentity())
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(TEXT("command_operation_result_invalid"),
							Request.GetRequestId(),
							TEXT("C3O returned an invalid or mismatched result.")));
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				FString ApplicationError;
				if (!TryReadApplicationError(
						PayloadJson, TEXT("operation_error"), ApplicationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(TEXT("command_operation_payload_invalid"),
							Request.GetRequestId(),
							TEXT("C3O returned a non-object JSON payload.")));
				}
				return ApplicationError.IsEmpty()
					? FUeAgentCommandResult::Success(Request, PayloadJson)
					: FUeAgentCommandResult::Failure(
						Request, PayloadJson, ApplicationError);
			}

		private:
			const FUeAgentGraphCompoundActivationProfile Profile;
			TSharedRef<IUeAgentQueryApplicationPort> QueryPort;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateGraphCompoundCommandHandlers(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedRef<IUeAgentQueryApplicationPort> QueryPort,
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Profiles.IsEmpty())
		{
			OutError = TEXT("graph_compound_activation_profiles_empty");
			return false;
		}
		const TArray<FUeAgentGraphCompoundActivationProfile>& CanonicalProfiles =
			GetGraphCompoundActivationProfiles();
		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
		{
			const FUeAgentGraphCompoundActivationProfile* Canonical = nullptr;
			for (const FUeAgentGraphCompoundActivationProfile& Candidate
				: CanonicalProfiles)
			{
				if (Candidate.GetCommandId() == Profile.GetCommandId())
				{
					Canonical = &Candidate;
					break;
				}
			}
			const FString DescriptorId =
				Profile.GetDescriptorIdentity().GetDescriptorId();
			if (!Profile.IsValid() || !Canonical
				|| Canonical->GetCapabilityId() != Profile.GetCapabilityId()
				|| Canonical->GetNamespace() != Profile.GetNamespace()
				|| Canonical->GetMethod() != Profile.GetMethod()
				|| !(Canonical->GetDescriptorIdentity()
					== Profile.GetDescriptorIdentity())
				|| Canonical->GetOperationKind() != Profile.GetOperationKind())
			{
				OutHandlers.Reset();
				OutError = TEXT("graph_compound_activation_profile_not_canonical");
				return false;
			}
			if (CommandIds.Contains(Profile.GetCommandId())
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = TEXT("graph_compound_command_handler_duplicate");
				return false;
			}
			CommandIds.Add(Profile.GetCommandId());
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(MakeShared<FGraphCompoundCommandHandler>(
				Profile, QueryPort, OperationPort));
		}
		return true;
	}
}
