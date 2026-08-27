#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"

namespace UE::UAI::CommandLifecycle
{
	using namespace CommandContract;

	const TArray<FCommandLifecyclePolicy>& GetPolicies()
	{
		static const TArray<FCommandLifecyclePolicy> Policies = {
			{
				TEXT("world.actors"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.structure"),
				 TEXT("content_graph.items")},
				TEXT("Persisted World Actor discovery belongs to World HJSON."),
				true,
			},
			{
				TEXT("world.actor_components"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.items")},
				TEXT("Persisted Actor component structure belongs to World HJSON."),
				true,
			},
			{
				TEXT("world.property"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.properties")},
				TEXT("Registered persisted Actor and Component properties belong to World HJSON."),
				true,
			},
			{
				TEXT("world.object_transform.read"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.structure"),
				 TEXT("content_graph.items")},
				TEXT("Persisted Actor and Component transform readback belongs to World HJSON."),
				true,
			},
			{
				TEXT("world.object_transform.update"),
				EUeAgentCommandAvailability::Dormant,
				{TEXT("content_graph.import_workspace")},
				TEXT("Persisted Actor and Component transform mutation belongs to World HJSON."),
				true,
			},
		};
		return Policies;
	}

	const FCommandLifecyclePolicy* FindExact(const FString& CommandId)
	{
		return GetPolicies().FindByPredicate(
			[&CommandId](const FCommandLifecyclePolicy& Policy)
			{
				return Policy.CommandId == CommandId;
			});
	}

	bool TryApply(
		TArray<FUeAgentCommandDeclaration>& Declarations,
		FString& OutError)
	{
		OutError.Reset();
		TSet<FString> Applied;
		for (FUeAgentCommandDeclaration& Declaration : Declarations)
		{
			const FCommandLifecyclePolicy* Policy =
				FindExact(Declaration.CommandId);
			if (!Policy)
			{
				continue;
			}
			if (Applied.Contains(Policy->CommandId))
			{
				OutError = TEXT("command_lifecycle_policy_duplicate:")
					+ Policy->CommandId;
				return false;
			}
			if (Declaration.Availability
				!= EUeAgentCommandAvailability::Enabled)
			{
				OutError = TEXT("command_lifecycle_policy_source_not_enabled:")
					+ Policy->CommandId;
				return false;
			}
			Declaration.Availability = Policy->Availability;
			Applied.Add(Policy->CommandId);
		}
		return true;
	}

	const TCHAR* LexToString(const EUeAgentCommandAvailability Availability)
	{
		switch (Availability)
		{
		case EUeAgentCommandAvailability::Enabled:
			return TEXT("active");
		case EUeAgentCommandAvailability::Dormant:
			return TEXT("dormant");
		case EUeAgentCommandAvailability::Disabled:
			return TEXT("disabled");
		default:
			return TEXT("invalid");
		}
	}

	bool IsExecutable(const EUeAgentCommandAvailability Availability)
	{
		return Availability == EUeAgentCommandAvailability::Enabled
			|| Availability == EUeAgentCommandAvailability::Dormant;
	}
}
