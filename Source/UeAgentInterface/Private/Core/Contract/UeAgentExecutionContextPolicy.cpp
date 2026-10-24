#include "Core/Contract/UeAgentExecutionContextPolicy.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::Contract
{
	namespace
	{
		FDiagnostic PolicyFailure(const TCHAR* Code, const TCHAR* Reason, FString Path)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("contract_validation"));
			Error.PropertyPath = TEXT("/execution_context_policy/") + Path;
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		template <typename TValue>
		FDiagnostic ValidatePolicyValues(const TArray<TValue>& Values, const TCHAR* Field, const TCHAR* AllowedValues)
		{
			TSet<TValue> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FString Name = LexToString(Values[Index]);
				const FString Path = FString::Printf(TEXT("%s/%d"), Field, Index);
				if (Name.IsEmpty())
				{
					auto Error = PolicyFailure(TEXT("execution_context_policy_enum_invalid"),
						TEXT("The execution context policy contains an unsupported enum value."), Path);
					Error.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Values[Index]));
					Error.ExpectedValue = MakeShared<FJsonValueString>(AllowedValues);
					return Error;
				}
				if (Seen.Contains(Values[Index]))
				{
					auto Error = PolicyFailure(TEXT("execution_context_policy_value_duplicate"),
						TEXT("The execution context policy repeats a value in the same field."), Path);
					Error.ActualValue = MakeShared<FJsonValueString>(Name);
					return Error;
				}
				Seen.Add(Values[Index]);
			}
			return {};
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
		if (AllowedStatePlanes.IsEmpty())
		{
			ValidationDiagnostic = PolicyFailure(TEXT("execution_context_policy_state_planes_empty"),
				TEXT("The execution context policy requires at least one allowed state plane."), TEXT("allowed_state_planes"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(0);
		}
		else
		{
			ValidationDiagnostic = ValidatePolicyValues(AllowedStatePlanes, TEXT("allowed_state_planes"),
				TEXT("authoring_memory, persisted, compiled_generated, runtime_instance, rendered_observation, registry_index"));
		}
		if (ValidationDiagnostic.IsEmpty())
			ValidationDiagnostic = ValidatePolicyValues(RequiredFacets, TEXT("required_facets"),
				TEXT("world, level, data_layer, world_partition, pie_instance, net_mode, net_role, asset_editor, viewport, selection"));
		if (ValidationDiagnostic.IsEmpty() && FString(LexToString(ResolutionPolicy)).IsEmpty())
		{
			ValidationDiagnostic = PolicyFailure(TEXT("execution_context_policy_resolution_invalid"),
				TEXT("The context resolution policy must be explicit_only or resolve_current_then_freeze."), TEXT("resolution_policy"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(ResolutionPolicy));
		}
		if (ValidationDiagnostic.IsEmpty() && FString(LexToString(TransientContextPolicy)).IsEmpty())
		{
			ValidationDiagnostic = PolicyFailure(TEXT("execution_context_policy_transient_access_invalid"),
				TEXT("The transient access policy must be read_only or temporary_mutation_allowed."), TEXT("transient_access_policy"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(TransientContextPolicy));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentExecutionContextPolicy::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}
}
