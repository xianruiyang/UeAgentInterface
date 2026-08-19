#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/ExternalImport/Factory/UeAgentFactoryExternalImportProvider.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::ExternalImport::Factory
{
	struct UEAGENTINTERFACE_API FUeAgentFactoryExternalImportCommandRoute final
	{
		FString CommandId;
		FString PythonFunction;
		DomainAdapters::ExternalImport::Factory::
			EFactoryExternalImportOperation Operation =
				DomainAdapters::ExternalImport::Factory::
					EFactoryExternalImportOperation::Import;
		DomainAdapters::ExternalImport::Factory::EFactoryExternalImportKind Kind =
			DomainAdapters::ExternalImport::Factory::
				EFactoryExternalImportKind::Texture;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentFactoryExternalImportCommandRoute>&
	GetFactoryExternalImportCommandRoutes();

	struct UEAGENTINTERFACE_API FUeAgentFactoryExternalImportCommandRequest
	{
		FString CommandId;
		FString RequestId;
		FString CanonicalParamsJson;
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		bool bCancellationRequested = false;
	};

	struct UEAGENTINTERFACE_API FUeAgentFactoryExternalImportCommandResponse
	{
		FString PayloadJson;
	};

	struct UEAGENTINTERFACE_API FUeAgentFactoryExternalImportCommandError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
		FString TerminalStatus = TEXT("rejected");
		bool bMutationStarted = false;
		bool bRetrySafe = true;
	};

	class UEAGENTINTERFACE_API IUeAgentFactoryExternalImportCommandService
	{
	public:
		virtual ~IUeAgentFactoryExternalImportCommandService() = default;

		virtual bool Execute(
			const FUeAgentFactoryExternalImportCommandRequest& Request,
			FUeAgentFactoryExternalImportCommandResponse& OutResponse,
			FUeAgentFactoryExternalImportCommandError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateProductionFactoryExternalImportCommandService(
		TSharedPtr<IUeAgentFactoryExternalImportCommandService>& OutService,
		FString& OutError);

	namespace TestHooks
	{
		class UEAGENTINTERFACE_API FScopedFactoryImportFailureAtStage final
		{
		public:
			explicit FScopedFactoryImportFailureAtStage(FString Stage);
			~FScopedFactoryImportFailureAtStage();

			FScopedFactoryImportFailureAtStage(
				const FScopedFactoryImportFailureAtStage&) = delete;
			FScopedFactoryImportFailureAtStage& operator=(
				const FScopedFactoryImportFailureAtStage&) = delete;

		private:
			FString Previous;
		};
	}
}
