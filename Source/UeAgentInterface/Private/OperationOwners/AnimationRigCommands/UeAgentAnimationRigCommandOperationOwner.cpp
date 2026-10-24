#include "OperationOwners/AnimationRigCommands/UeAgentAnimationRigCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentInternalCommandExecution.h"
#include "DomainAdapters/AnimationRigCommands/UeAgentAnimationRigCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"

namespace UE::UAI::OperationOwners::AnimationRigCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return DomainAdapters::AnimationRigCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::AnimationRigCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}

	bool TrySubmitMechanic(
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayload.Reset();
		OutError.Reset();
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> Mechanics;
		TArray<FString> AuthorityScopes;
		if (!TryResolveMechanics(
				CommandId, Mechanics, AuthorityScopes, OutError)
			|| !Mechanics.IsValid())
		{
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_internal_mechanics_unavailable"),
					TEXT("The animation rig resolver failed or returned no executable handler without reporting a diagnostic."), TEXT("internal_command"), CommandId);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("command_handler_ready");
				OutError.SourceKind = TEXT("native_result");
			} else {
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_internal_mechanics_unavailable"),
					OutError, TEXT("internal_command"), TEXT("The animation rig mechanics could not be prepared."));
			}
			return false;
		}

		FString ParamsJson;
		if (!Contract::Json::TryCanonicalize(Params, ParamsJson, OutError))
		{
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_internal_params_invalid"),
					TEXT("The animation rig command parameters could not be encoded."), TEXT("internal_command"), CommandId);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("command_parameters_encoded");
				OutError.SourceKind = TEXT("native_result");
			} else {
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_internal_params_invalid"),
					OutError, TEXT("internal_command"), TEXT("The animation rig command parameters could not be encoded."));
			}
			return false;
		}
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
			Contracts;
		if (QueryOwners::AnimationRigCommands::IsQuery(CommandId))
		{
			if (!Composition::TimelineSpatial::
					TryCreateTimelineSpatialProductionCommandContractRegistry(
						Contracts, OutError))
			{
				return false;
			}
		}
		else
		{
			TArray<Composition::GraphCompound::
				FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!QueryOwners::AnimationRigCommands::TryGetGraphProfiles(
					Profiles, OutError)
				|| !Composition::GraphCompound::
					TryCreateGraphCompoundProductionCommandContractRegistry(
						Profiles, Contracts, OutError))
			{
				return false;
			}
		}
		const CommandContract::FUeAgentCommandContract* ResolvedContract =
			Contracts.IsValid() ? Contracts->FindExact(CommandId) : nullptr;
		if (!ResolvedContract)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_internal_contract_missing"),
				Contracts.IsValid() ? TEXT("No execution contract is registered for the required animation rig operation.")
					: TEXT("The animation rig contract factory returned no command registry."), TEXT("internal_command"), CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.PropertyPath = Contracts.IsValid() ? TEXT("command_contract_registered") : TEXT("command_registry_available");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}

		AuthorityScopes.AddUnique(TEXT("uai:query"));
		AuthorityScopes.AddUnique(TEXT("uai:operate"));
		AuthorityScopes.AddUnique(TEXT("uai:authoring:hjson:import"));
		if (!CommandContract::TryExecuteInternalCommand(
				*ResolvedContract,
				Mechanics.ToSharedRef(),
				ParamsJson,
				Contract::FUeAgentCallerContext(
				TEXT("animation-rig-graph-domain-provider"),
				AuthorityScopes,
				CorrelationId),
				TEXT("animation_rig_internal_"),
				OutPayload,
				OutError))
		{
			return false;
		}
		return true;
	}
}
