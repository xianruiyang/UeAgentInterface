#include "Core/Contract/UeAgentExecutionContextPolicy.h"

namespace UE::UAI::Contract
{
	namespace
	{
		template <typename TValue>
		bool HasDuplicate(const TArray<TValue>& Values)
		{
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
					return true;
				}
			}
			return false;
		}
	}

	const TCHAR* LexToString(const EUeAgentStatePlane Value)
	{
		switch (Value)
		{
		case EUeAgentStatePlane::AuthoringMemory:
			return TEXT("authoring_memory");
		case EUeAgentStatePlane::Persisted:
			return TEXT("persisted");
		case EUeAgentStatePlane::CompiledGenerated:
			return TEXT("compiled_generated");
		case EUeAgentStatePlane::RuntimeInstance:
			return TEXT("runtime_instance");
		case EUeAgentStatePlane::RenderedObservation:
			return TEXT("rendered_observation");
		case EUeAgentStatePlane::RegistryIndex:
			return TEXT("registry_index");
		default:
			return TEXT("");
		}
	}

	const TCHAR* LexToString(
		const EUeAgentExecutionContextFacet Value)
	{
		switch (Value)
		{
		case EUeAgentExecutionContextFacet::World:
			return TEXT("world");
		case EUeAgentExecutionContextFacet::Level:
			return TEXT("level");
		case EUeAgentExecutionContextFacet::DataLayer:
			return TEXT("data_layer");
		case EUeAgentExecutionContextFacet::WorldPartition:
			return TEXT("world_partition");
		case EUeAgentExecutionContextFacet::PieInstance:
			return TEXT("pie_instance");
		case EUeAgentExecutionContextFacet::NetMode:
			return TEXT("net_mode");
		case EUeAgentExecutionContextFacet::NetRole:
			return TEXT("net_role");
		case EUeAgentExecutionContextFacet::AssetEditor:
			return TEXT("asset_editor");
		case EUeAgentExecutionContextFacet::Viewport:
			return TEXT("viewport");
		case EUeAgentExecutionContextFacet::Selection:
			return TEXT("selection");
		default:
			return TEXT("");
		}
	}

	const TCHAR* LexToString(
		const EUeAgentContextResolutionPolicy Value)
	{
		switch (Value)
		{
		case EUeAgentContextResolutionPolicy::ExplicitOnly:
			return TEXT("explicit_only");
		case EUeAgentContextResolutionPolicy::
				ResolveCurrentThenFreeze:
			return TEXT("resolve_current_then_freeze");
		default:
			return TEXT("");
		}
	}

	const TCHAR* LexToString(
		const EUeAgentTransientContextPolicy Value)
	{
		switch (Value)
		{
		case EUeAgentTransientContextPolicy::ReadOnly:
			return TEXT("read_only");
		case EUeAgentTransientContextPolicy::
				TemporaryMutationAllowed:
			return TEXT("temporary_mutation_allowed");
		default:
			return TEXT("");
		}
	}

	bool TryParseStatePlane(
		const FString& Value,
		EUeAgentStatePlane& OutStatePlane)
	{
		static const EUeAgentStatePlane StatePlanes[] = {
			EUeAgentStatePlane::AuthoringMemory,
			EUeAgentStatePlane::Persisted,
			EUeAgentStatePlane::CompiledGenerated,
			EUeAgentStatePlane::RuntimeInstance,
			EUeAgentStatePlane::RenderedObservation,
			EUeAgentStatePlane::RegistryIndex,
		};
		for (const EUeAgentStatePlane StatePlane : StatePlanes)
		{
			if (Value == LexToString(StatePlane))
			{
				OutStatePlane = StatePlane;
				return true;
			}
		}
		return false;
	}

	FUeAgentExecutionContextPolicy::
	FUeAgentExecutionContextPolicy(
		TArray<EUeAgentStatePlane> InAllowedStatePlanes,
		TArray<EUeAgentExecutionContextFacet> InRequiredFacets,
		const EUeAgentContextResolutionPolicy InResolutionPolicy,
		const EUeAgentTransientContextPolicy
			InTransientContextPolicy)
		: AllowedStatePlanes(MoveTemp(InAllowedStatePlanes))
		, RequiredFacets(MoveTemp(InRequiredFacets))
		, ResolutionPolicy(InResolutionPolicy)
		, TransientContextPolicy(InTransientContextPolicy)
	{
		AllowedStatePlanes.Sort(
			[](const EUeAgentStatePlane Left,
			   const EUeAgentStatePlane Right)
			{
				return FCString::Strcmp(
					LexToString(Left),
					LexToString(Right)) < 0;
			});
		RequiredFacets.Sort(
			[](const EUeAgentExecutionContextFacet Left,
			   const EUeAgentExecutionContextFacet Right)
			{
				return FCString::Strcmp(
					LexToString(Left),
					LexToString(Right)) < 0;
			});
	}

	const TArray<EUeAgentStatePlane>&
	FUeAgentExecutionContextPolicy::
	GetAllowedStatePlanes() const
	{
		return AllowedStatePlanes;
	}

	const TArray<EUeAgentExecutionContextFacet>&
	FUeAgentExecutionContextPolicy::GetRequiredFacets() const
	{
		return RequiredFacets;
	}

	EUeAgentContextResolutionPolicy
	FUeAgentExecutionContextPolicy::GetResolutionPolicy() const
	{
		return ResolutionPolicy;
	}

	EUeAgentTransientContextPolicy
	FUeAgentExecutionContextPolicy::
	GetTransientContextPolicy() const
	{
		return TransientContextPolicy;
	}

	bool FUeAgentExecutionContextPolicy::IsValid() const
	{
		if (AllowedStatePlanes.IsEmpty()
			|| HasDuplicate(AllowedStatePlanes)
			|| HasDuplicate(RequiredFacets)
			|| FString(LexToString(ResolutionPolicy)).IsEmpty()
			|| FString(LexToString(
				TransientContextPolicy)).IsEmpty())
		{
			return false;
		}
		for (const EUeAgentStatePlane StatePlane :
			AllowedStatePlanes)
		{
			if (FString(LexToString(StatePlane)).IsEmpty())
			{
				return false;
			}
		}
		for (const EUeAgentExecutionContextFacet Facet :
			RequiredFacets)
		{
			if (FString(LexToString(Facet)).IsEmpty())
			{
				return false;
			}
		}
		return true;
	}
}
