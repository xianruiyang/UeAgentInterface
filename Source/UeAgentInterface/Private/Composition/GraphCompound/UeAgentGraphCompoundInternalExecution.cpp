#include "Composition/GraphCompound/UeAgentGraphCompoundInternalExecution.h"

#include "Core/Contract/UeAgentInternalCommandExecution.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace CommandContract;
	using namespace CommandEdge;

	namespace
	{
		FString ErrorCode(
			const FString& Prefix,
			const TCHAR* Suffix)
		{
			return Prefix + Suffix;
		}
	}

	bool TryExecuteGraphCompoundInternal(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		const TMap<FString, TSharedRef<IUeAgentCommandHandler>>& Mechanics,
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		const FString& ErrorPrefix,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();

		TSharedPtr<const FUeAgentCommandContractRegistry> Contracts;
		if (!TryCreateGraphCompoundProductionCommandContractRegistry(
				Profiles, Contracts, OutError))
		{
			return false;
		}
		const FUeAgentCommandContract* Contract = Contracts->FindExact(CommandId);
		if (!Contract)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(
				ErrorPrefix + TEXT("internal_contract_missing"), TEXT("No internal command contract is registered for the requested command."), TEXT("internal_command"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = CommandId;
			OutError.PropertyPath = TEXT("contract_present");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}

		const TSharedRef<IUeAgentCommandHandler>* Handler = Mechanics.Find(CommandId);
		if (!Handler)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(
				ErrorPrefix + TEXT("internal_mechanics_missing"), TEXT("No internal command handler is registered for the requested command."), TEXT("internal_command"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = CommandId;
			OutError.PropertyPath = TEXT("handler_present");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		return TryExecuteInternalCommand(
			*Contract, *Handler, ParamsJson, Caller, ErrorPrefix,
			OutPayloadJson, OutError);
	}
}
