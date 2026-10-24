#include "Composition/TimelineSpatial/UeAgentTimelineSpatialCommandHandlers.h"

#include "CommandEdge/UeAgentApplicationResultProjection.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
		



		class FTimelineSpatialCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FTimelineSpatialCommandHandler(
				FUeAgentTimelineSpatialActivationProfile InProfile,
				TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Profile(MoveTemp(InProfile))
				, QueryPort(MoveTemp(InQueryPort))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Profile.CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (Profile.OperationKind == TEXT("query"))
				{
					const FUeAgentQueryResult Result = QueryPort->Submit(
						FUeAgentQueryRequest(
							Profile.DescriptorIdentity,
							Request.GetRequestId(),
							FUeAgentContractValue(Request.GetParamsJson())),
						CallerContext,
						Cancellation);
					return ProjectQueryResult(Request, Profile.DescriptorIdentity, Result);
				}

				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Profile.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				return ProjectOperationResult(Request, Profile.DescriptorIdentity, Result);
			}

		private:
			const FUeAgentTimelineSpatialActivationProfile Profile;
			TSharedRef<IUeAgentQueryApplicationPort> QueryPort;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateTimelineSpatialCommandHandlers(
		const TArray<FUeAgentTimelineSpatialActivationProfile>& Profiles,
		TSharedRef<IUeAgentQueryApplicationPort> QueryPort,
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Profiles.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_activation_profile_count_invalid"),
				TEXT("At least one activation profile is required to register Timeline/Spatial handlers"), TEXT("registration"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("activation_profile_count");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT(">= 1"));
			return false;
		}

		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (int32 ProfileIndex = 0; ProfileIndex < Profiles.Num(); ++ProfileIndex)
		{
			const FUeAgentTimelineSpatialActivationProfile& Profile = Profiles[ProfileIndex];
			const FString DescriptorId =
				Profile.DescriptorIdentity.GetDescriptorId();
			if (!Profile.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("timeline_spatial_activation_profile_invalid"), OutError, TEXT("registration"));
				return false;
			}
			if (CommandIds.Contains(Profile.CommandId)
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				const bool bDuplicateCommand = CommandIds.Contains(Profile.CommandId);
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_command_handler_duplicate"),
					bDuplicateCommand ? TEXT("The activation profile repeats a command already registered in this batch")
						: TEXT("The activation profile repeats a descriptor already registered in this batch"), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Profile.CommandId;
				OutError.OperationIndex = ProfileIndex;
				OutError.PropertyPath = bDuplicateCommand ? TEXT("command_id_unique") : TEXT("descriptor_id_unique");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(bDuplicateCommand ? Profile.CommandId : DescriptorId);
				return false;
			}
			CommandIds.Add(Profile.CommandId);
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(MakeShared<FTimelineSpatialCommandHandler>(
				Profile,
				QueryPort,
				OperationPort));
		}
		return true;
	}
}
