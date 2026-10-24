#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::AssetSimple
{
	using namespace BindingGateway::Operation;
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		UE::UAI::Contract::FDiagnostic CommandFailure(FString Code, const FString& Subject, FString Reason = FString(), FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("command"), MoveTemp(InternalContext), Subject);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = CommandFailure(Code, Subject, TEXT("The operation runtime could not be prepared."));
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_projection"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
		}
	}

	const TArray<FUeAgentSimpleAuthoringCommandRoute>&
	GetAssetSimpleOperationRoutes()
	{
		static const TArray<FUeAgentSimpleAuthoringCommandRoute> Routes = []()
		{
			TArray<FUeAgentSimpleAuthoringCommandRoute> Result;
			for (const FUeAgentSimpleAuthoringCommandRoute& Route
				: GetSimpleAuthoringCommandRoutes())
			{
				if (DomainAdapters::AssetSimple::
					IsAssetSimpleLifecycleCommand(Route.CommandId))
				{
					Result.Add(Route);
				}
			}
			if (Result.Num() != DomainAdapters::AssetSimple::
				GetAssetSimpleLifecycleCommandIds().Num())
			{
				Result.Reset();
			}
			return Result;
		}();
		return Routes;
	}

	const TArray<FUeAgentSimpleAuthoringCommandRoute>&
	GetAssetSimplePublicOperationRoutes()
	{
		static const TArray<FUeAgentSimpleAuthoringCommandRoute> Routes = []
		{
			TArray<DomainAdapters::AssetDataSimpleEffect::
				FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
			UE::UAI::Contract::FDiagnostic ProfileError;
			if (!DomainAdapters::AssetDataSimpleEffect::
					TryGetAssetDataSimpleEffectProductionProfiles(
						Profiles, ProfileError))
			{
				return TArray<FUeAgentSimpleAuthoringCommandRoute>();
			}
			TSet<FString> GenericCreateSuccessors;
			for (const DomainAdapters::AssetDataSimpleEffect::
				FUeAgentAssetDataSimpleEffectProductionProfile& Profile : Profiles)
			{
				if (DomainAdapters::AssetDataSimpleEffect::
						IsGenericAssetObjectCreateSuccessor(Profile))
				{
					GenericCreateSuccessors.Add(Profile.CapabilityId);
				}
			}
			TArray<FUeAgentSimpleAuthoringCommandRoute> Result;
			for (const FUeAgentSimpleAuthoringCommandRoute& Route
				: GetAssetSimpleOperationRoutes())
			{
				if (DomainAdapters::AssetSimple::
						IsAssetSimpleMechanicsCommand(Route.CommandId)
					&& !GenericCreateSuccessors.Contains(Route.CommandId))
				{
					Result.Add(Route);
				}
			}
			return Result;
		}();
		return Routes;
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		// EditorProject and destructive-content owners also submit through this
		// shared lifecycle. Public AssetSimple registration keeps its own inventory;
		// execution resolves the exact command from the common runtime routes.
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			GetSimpleAuthoringCommandRoutes();
		if (!Routes.ContainsByPredicate(
				[&OperationId](const FUeAgentSimpleAuthoringCommandRoute& Route)
				{
					return Route.CommandId == OperationId;
				}))
		{
			return nullptr;
		}
		return OperationHost::GetOperationInvocationFactory(OperationId);
	}

	FUeAgentCommandResult SubmitProductionAssetSimpleOperation(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const FString RequiredPlugin = DomainAdapters::AssetSimple::
			GetAssetSimpleRequiredOfficialPlugin(Request.GetCommandId());
		if (!RequiredPlugin.IsEmpty())
		{
			const DomainCapability::OfficialPluginAvailability::
				FOfficialPluginAvailabilityObservation Availability =
					DomainCapability::OfficialPluginAvailability::Inspect(
						RequiredPlugin);
			if (!Availability.IsAvailable())
			{
				return FUeAgentCommandResult::Failure(
					Request,
					TEXT("{}"),
					CommandFailure(
						TEXT("asset_simple_official_plugin_unavailable"),
						RequiredPlugin,
						Availability.GetReason()));
			}
		}
		const TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				FindOperationInvocationFactory(Request.GetCommandId());
		if (!Factory.IsValid())
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				CommandFailure(TEXT("asset_simple_operation_factory_missing"), Request.GetCommandId(), TEXT("the exact AssetSimple operation has no lifecycle factory")));
		}

		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request,
				CallerContext,
				Cancellation,
				Invocation,
				InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				InvocationFailure(TEXT("asset_simple_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}

		FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			auto Diagnostic = Receipt.GetDiagnostic();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}

		FString Payload;
		UE::UAI::Contract::FDiagnostic PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("asset_simple_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
