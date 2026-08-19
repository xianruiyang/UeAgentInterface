#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::AssetSimple
{
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

	FUeAgentCommandResult SubmitProductionAssetSimpleOperation(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		TSharedPtr<IUeAgentCommandHandler> Handler;
		FString Error;
		if (!OperationHost::TryResolveHandler(
				Request.GetCommandId(), Handler, Error)
			|| !Handler.IsValid())
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					Error.StartsWith(TEXT("simple_authoring_handler_missing:"))
						? FString(TEXT("asset_simple_command_not_bound"))
						: FString(TEXT("asset_simple_runtime_unavailable")),
					Request.GetCommandId(),
					Error));
		}
		return Handler->Submit(Request, CallerContext, Cancellation);
	}
}
