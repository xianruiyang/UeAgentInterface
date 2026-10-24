// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringRegistration.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringExport.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStackApplyProvider.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDomainRegistry.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		class FNiagaraApplyProvider final : public UeAgentAuthoringOperationApply::IApplyProvider
		{
		public:
			FNiagaraApplyProvider()
				: AssetProvider(MakeAssetApplyProvider())
				, StackProvider(MakeStackApplyProvider())
			{
			}

			virtual FString GetProviderId() const override { return ApplyProviderId; }

			virtual bool PrepareOperation(
				const UeAgentAuthoringOperationApply::FPrepareRequest& Request,
				TUniquePtr<UeAgentAuthoringOperationApply::IPreparedOperationState>& OutState,
				UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const override
			{
				if (!Request.Contract)
				{
					OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_apply_contract_missing"),
						TEXT("The Niagara apply provider requires an operation contract before dispatch."), TEXT("registration"));
					OutFailure.Diagnostic.SourceKind = TEXT("native_result");
					OutFailure.Diagnostic.PropertyPath = TEXT("contract_available");
					OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				if (Request.Contract->ContractId == AssetInputContractId)
				{
					return AssetProvider->PrepareOperation(Request, OutState, OutFailure);
				}
				if (Request.Contract->ContractId == ModuleInputContractId
					|| Request.Contract->ContractId == DynamicInputInputContractId)
				{
					return StackProvider->PrepareOperation(Request, OutState, OutFailure);
				}
				OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_apply_contract_unsupported"),
					TEXT("The Niagara apply provider supports asset, module and dynamic-input contracts."), TEXT("registration"));
				OutFailure.Diagnostic.SourceKind = TEXT("native_result");
				OutFailure.Diagnostic.PropertyPath = TEXT("contract_id");
				OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.Contract->ContractId);
				OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(AssetInputContractId), MakeShared<FJsonValueString>(ModuleInputContractId),
					MakeShared<FJsonValueString>(DynamicInputInputContractId)});
				return false;
			}

		private:
			TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> AssetProvider;
			TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> StackProvider;
		};

		bool RegisterApplyContractsAndProvider(UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!RegisterAssetApplyContracts(OutError) || !RegisterStackApplyContracts(OutError)) return false;
			static const TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider> Provider = MakeShared<FNiagaraApplyProvider>();
			UeAgentAuthoringOperationApply::ERegistrationResult Result;
			return UeAgentAuthoringOperationApply::FProviderRegistry::Get().RegisterProvider(Provider, Result, OutError);
		}
	}

	bool RegisterAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!RegisterDescriptors(OutError)
			|| !RegisterStateAccessors(OutError)
			|| !RegisterNativeDefaultProviders(OutError)
			|| !RegisterApplyContractsAndProvider(OutError)
			|| !RegisterExportAuthoring(OutError))
		{
			return false;
		}
		return UeAgentAuthoringDomainRegistry::RegisterGeneratedDomain(DomainId, OutError);
	}
}
