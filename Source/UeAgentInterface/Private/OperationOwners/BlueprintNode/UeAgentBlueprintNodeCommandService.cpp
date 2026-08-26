#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

#include "QueryOwners/BlueprintNode/UeAgentBlueprintNodeQueryOwner.h"

namespace UE::UAI::OperationOwners::BlueprintNode
{
	using namespace UE::UAI::QueryOwners::BlueprintNode;

	namespace
	{
		const FString CandidateCommandId = TEXT("blueprint.node.candidates");

		bool Fail(
			FUeAgentBlueprintNodeCommandError& OutError,
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
			: public IUeAgentBlueprintNodeCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentBlueprintNodeCommandRequest& Request,
				FUeAgentBlueprintNodeCommandResponse& OutResponse,
				FUeAgentBlueprintNodeCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_command_not_supported"),
						Request.CommandId);
				}
				return TryExecuteCandidateQuery(
					Request, OutResponse, OutError);
			}
		};

	}

	bool TryCreateProductionBlueprintNodeCommandService(
		TSharedPtr<IUeAgentBlueprintNodeCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FProductionService>();
		OutError.Reset();
		return true;
	}
}
