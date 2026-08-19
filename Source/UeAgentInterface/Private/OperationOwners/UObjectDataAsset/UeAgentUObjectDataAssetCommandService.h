#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::UObjectDataAsset
{
	enum class EUeAgentUObjectDataAssetRouteKind : uint8
	{
		Query,
		Export,
		Import,
	};

	struct UEAGENTINTERFACE_API FUeAgentUObjectDataAssetCommandRoute final
	{
		FString CommandId;
		FString PythonFunction;
		FString MechanicsCommandId;
		EUeAgentUObjectDataAssetRouteKind Kind =
			EUeAgentUObjectDataAssetRouteKind::Query;
		bool bRequiresTarget = true;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentUObjectDataAssetCommandRoute>&
	GetUObjectDataAssetCommandRoutes();

	struct UEAGENTINTERFACE_API FUeAgentUObjectDataAssetCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentUObjectDataAssetCommandResponse
	{
		FString PayloadJson;
		bool bReadOnly = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentUObjectDataAssetCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentUObjectDataAssetCommandService
	{
	public:
		virtual ~IUeAgentUObjectDataAssetCommandService() = default;

		virtual bool Execute(
			const FUeAgentUObjectDataAssetCommandRequest& Request,
			FUeAgentUObjectDataAssetCommandResponse& OutResponse,
			FUeAgentUObjectDataAssetCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateProductionUObjectDataAssetCommandService(
		TSharedPtr<IUeAgentUObjectDataAssetCommandService>& OutService,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateUObjectDataAssetDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>&
			OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);

	namespace TestHooks
	{
		class UEAGENTINTERFACE_API FScopedFailureAfterNode final
		{
		public:
			explicit FScopedFailureAfterNode(FString NodeId);
			~FScopedFailureAfterNode();

			FScopedFailureAfterNode(const FScopedFailureAfterNode&) = delete;
			FScopedFailureAfterNode& operator=(
				const FScopedFailureAfterNode&) = delete;

		private:
			FString Previous;
		};
	}
}
