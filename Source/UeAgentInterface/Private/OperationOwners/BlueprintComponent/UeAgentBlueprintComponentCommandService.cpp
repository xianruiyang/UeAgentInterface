#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

#include "QueryOwners/BlueprintComponent/UeAgentBlueprintComponentQueryOwner.h"

namespace UE::UAI::OperationOwners::BlueprintComponent
{
	using namespace UE::UAI::QueryOwners::BlueprintComponent;

	namespace
	{
		const FString CandidateCommandId =
			TEXT("blueprint.component.candidates");

		bool Fail(
			FUeAgentBlueprintComponentCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		class FProductionService final
			: public IUeAgentBlueprintComponentCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentBlueprintComponentCommandRequest& Request,
				FUeAgentBlueprintComponentCommandResponse& OutResponse,
				FUeAgentBlueprintComponentCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_command_not_supported"),
						Request.CommandId);
				}
				return TryExecuteCandidateQuery(
					Request, OutResponse, OutError);
			}
		};

	}

	bool TryCreateProductionBlueprintComponentCommandService(
		TSharedPtr<IUeAgentBlueprintComponentCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FProductionService>();
		OutError.Reset();
		return true;
	}
}
