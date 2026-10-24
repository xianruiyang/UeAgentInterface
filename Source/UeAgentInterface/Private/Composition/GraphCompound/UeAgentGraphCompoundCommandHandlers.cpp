#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"

#include "CommandEdge/UeAgentApplicationResultProjection.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
		



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
					return ProjectQueryResult(Request, Profile.GetDescriptorIdentity(), Result);
				}

				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Profile.GetDescriptorIdentity(),
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				return ProjectOperationResult(Request, Profile.GetDescriptorIdentity(), Result);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Profiles.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("graph_compound_activation_profiles_empty"),
				TEXT("At least one activation profile is required to register Graph handlers"), TEXT("registration"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("activation_profile_count");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT(">= 1"));
			return false;
		}
		const TArray<FUeAgentGraphCompoundActivationProfile>* CanonicalProfiles = nullptr;
		if (!TryGetGraphCompoundActivationProfiles(CanonicalProfiles, OutError)) return false;
		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (int32 ProfileIndex = 0; ProfileIndex < Profiles.Num(); ++ProfileIndex)
		{
			const FUeAgentGraphCompoundActivationProfile& Profile = Profiles[ProfileIndex];
			const FUeAgentGraphCompoundActivationProfile* Canonical = nullptr;
			for (const FUeAgentGraphCompoundActivationProfile& Candidate
				: *CanonicalProfiles)
			{
				if (Candidate.GetCommandId() == Profile.GetCommandId())
				{
					Canonical = &Candidate;
					break;
				}
			}
			const FString DescriptorId =
				Profile.GetDescriptorIdentity().GetDescriptorId();
			if (!Profile.IsValid())
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("graph_compound_activation_profile_not_canonical"),
					Profile.GetValidationDiagnostic(), TEXT("registration"));
				return false;
			}
			if (!Canonical)
			{
				OutHandlers.Reset();
				OutError = FDiagnostic::Failure(TEXT("graph_compound_activation_profile_not_canonical"),
					TEXT("No registered activation profile exists for this command"), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Profile.GetCommandId();
				OutError.PropertyPath = TEXT("canonical_profile_present");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.OperationIndex = ProfileIndex;
				return false;
			}
			if (!Profile.TryMatchDefinition(*Canonical, OutError, TEXT("registration")))
			{
				OutHandlers.Reset();
				OutError.OperationIndex = ProfileIndex;
				return false;
			}
			if (CommandIds.Contains(Profile.GetCommandId())
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				const bool bCommandDuplicate = CommandIds.Contains(Profile.GetCommandId());
				OutError = FDiagnostic::Failure(TEXT("graph_compound_command_handler_duplicate"),
					bCommandDuplicate ? TEXT("The activation profile repeats an already registered command")
						: TEXT("The activation profile repeats an already registered descriptor"), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Profile.GetCommandId();
				OutError.PropertyPath = bCommandDuplicate ? TEXT("command_id_unique") : TEXT("descriptor_id_unique");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(bCommandDuplicate ? Profile.GetCommandId() : DescriptorId);
				OutError.OperationIndex = ProfileIndex;
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
