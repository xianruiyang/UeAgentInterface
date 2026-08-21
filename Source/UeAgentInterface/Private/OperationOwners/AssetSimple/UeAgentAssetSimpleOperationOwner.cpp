#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"
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
		constexpr int32 ExpectedRouteCount = 52;

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), SubjectId);
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Object->SetBoolField(TEXT("retry_safe"), true);
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
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
					IsAssetSimpleMechanicsCommand(Route.CommandId))
				{
					Result.Add(Route);
				}
			}
			if (Result.Num() != ExpectedRouteCount)
			{
				Result.Reset();
			}
			return Result;
		}();
		return Routes;
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			GetAssetSimpleOperationRoutes();
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
					ErrorJson(
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
				ErrorJson(
					TEXT("asset_simple_operation_factory_missing"),
					Request.GetCommandId(),
					TEXT("the exact AssetSimple operation has no lifecycle factory")));
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
				ErrorJson(
					TEXT("asset_simple_runtime_unavailable"),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}

		FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			FString Detail = Invocation.GetContext()->GetFailureDetail();
			if (Detail.IsEmpty())
			{
				Detail = TEXT("phase_history:")
					+ FString::Join(Receipt.GetPhaseHistory(), TEXT(","));
			}
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Receipt.GetErrorCode());
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(
				TEXT("terminal_status"), LexToString(Receipt.GetStatus()));
			Error->SetBoolField(TEXT("retry_safe"), Receipt.IsRetrySafe());
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(Text));
		}

		FString Payload;
		FString PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("asset_simple_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
