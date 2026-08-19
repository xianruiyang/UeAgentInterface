#include "OperationOwners/HjsonExternalInput/UeAgentHjsonExternalInputProvider.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"
#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"
#include "QueryOwners/HjsonExternalInput/UeAgentHjsonExternalInputReadback.h"

#include <openssl/sha.h>

namespace UE::UAI::OperationOwners::HjsonExternalInput
{
	using namespace HjsonWorkspace;
	using namespace DomainAdapters::HjsonExternalInput;
	using namespace CommandContract;

	namespace
	{
		const FString ProviderId = TEXT("hjson-external-input-domain-provider");
		const FString SchemaId = TEXT("uai.hjson.external_input/1");

		FString HashFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				return FString();
			}
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Bytes.IsEmpty() ? &Empty : Bytes.GetData();
			if (!SHA256(Data, static_cast<size_t>(Bytes.Num()), Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		class FProductionImporterGateway final
			: public IHjsonExternalInputImporterGateway
		{
		public:
			static bool TryCreate(
				TSharedPtr<IHjsonExternalInputImporterGateway>& OutGateway,
				FWorkspaceError& OutError)
			{
				OutGateway.Reset();
				OutError.Reset();
				TSharedPtr<ExternalImport::Factory::
					IUeAgentFactoryExternalImportCommandService> Factory;
				TSharedPtr<ExternalImport::SkinWeight::
					IUeAgentSkinWeightImportCommandService> SkinWeight;
				TSharedPtr<ExternalImport::Localization::
					IUeAgentLocalizationImportCommandService> Localization;
				FString Error;
				if (!ExternalImport::Factory::
					TryCreateProductionFactoryExternalImportCommandService(
						Factory, Error)
					|| !Factory.IsValid())
				{
					OutError.Set(TEXT("hjson_external_input_factory_gateway_unavailable"), Error);
					return false;
				}
				if (!ExternalImport::SkinWeight::
					TryCreateProductionSkinWeightImportCommandService(
						SkinWeight, Error)
					|| !SkinWeight.IsValid())
				{
					OutError.Set(TEXT("hjson_external_input_skin_weight_gateway_unavailable"), Error);
					return false;
				}
				if (!ExternalImport::Localization::
					TryCreateProductionLocalizationImportCommandService(
						Localization, Error)
					|| !Localization.IsValid())
				{
					OutError.Set(TEXT("hjson_external_input_localization_gateway_unavailable"), Error);
					return false;
				}
				OutGateway = MakeShared<FProductionImporterGateway>(
					Factory.ToSharedRef(),
					SkinWeight.ToSharedRef(),
					Localization.ToSharedRef());
				return true;
			}

			FProductionImporterGateway(
				TSharedRef<ExternalImport::Factory::
					IUeAgentFactoryExternalImportCommandService> InFactory,
				TSharedRef<ExternalImport::SkinWeight::
					IUeAgentSkinWeightImportCommandService> InSkinWeight,
				TSharedRef<ExternalImport::Localization::
					IUeAgentLocalizationImportCommandService> InLocalization)
				: Factory(MoveTemp(InFactory))
				, SkinWeight(MoveTemp(InSkinWeight))
				, Localization(MoveTemp(InLocalization))
			{
			}

			virtual bool Execute(
				const FString& CommandId,
				const FString& CanonicalParamsJson,
				FString& OutPayloadJson,
				FWorkspaceError& OutError) override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					OutError.Set(TEXT("hjson_external_input_delegation_invalid"), JsonError);
					return false;
				}
				FString ExpectedSha256;
				FString SourceRef;
				Params->TryGetStringField(
					TEXT("_hjson_expected_source_sha256"), ExpectedSha256);
				Params->TryGetStringField(TEXT("source_ref"), SourceRef);
				Params->RemoveField(TEXT("_hjson_expected_source_sha256"));
				Params->RemoveField(TEXT("_hjson_source_format"));
				const FString ActualSha256 = HashFileSha256(SourceRef);
				if (ActualSha256.IsEmpty())
				{
					OutError.Set(
						TEXT("hjson_external_input_source_unavailable"),
						SourceRef,
						TEXT("/root/external_input/source/identity"));
					return false;
				}
				if (ActualSha256 != ExpectedSha256.ToLower())
				{
					OutError.Set(
						TEXT("hjson_external_input_source_digest_mismatch"),
						SourceRef,
						TEXT("/root/external_input/source/content_sha256"));
					return false;
				}
				FString DelegatedParams;
				if (!Json::TryCanonicalize(Params, DelegatedParams, JsonError))
				{
					OutError.Set(TEXT("hjson_external_input_delegation_invalid"), JsonError);
					return false;
				}
				const FString RequestId = Json::Sha256Utf8(
					CommandId + TEXT("\n") + DelegatedParams);

				if (ExternalImport::Factory::GetFactoryExternalImportCommandRoutes()
					.ContainsByPredicate([&CommandId](const auto& Route)
					{
						return Route.CommandId == CommandId;
					}))
				{
					ExternalImport::Factory::FUeAgentFactoryExternalImportCommandRequest Request;
					Request.CommandId = CommandId;
					Request.RequestId = RequestId;
					Request.CanonicalParamsJson = DelegatedParams;
					Request.CallerId = ProviderId;
					Request.AuthorityScopes = {TEXT("uai:operate")};
					Request.CorrelationId = RequestId;
					ExternalImport::Factory::FUeAgentFactoryExternalImportCommandResponse Response;
					ExternalImport::Factory::FUeAgentFactoryExternalImportCommandError Error;
					if (!Factory->Execute(Request, Response, Error))
					{
						OutError.Set(Error.Code, Error.SubjectId, Error.Detail);
						return false;
					}
					OutPayloadJson = MoveTemp(Response.PayloadJson);
					return !OutPayloadJson.IsEmpty();
				}
				if (CommandId == TEXT("external.skin_weight_profile.import"))
				{
					ExternalImport::SkinWeight::FUeAgentSkinWeightImportCommandRequest Request;
					Request.CommandId = CommandId;
					Request.RequestId = RequestId;
					Request.CanonicalParamsJson = DelegatedParams;
					Request.CallerId = ProviderId;
					Request.AuthorityScopes = {TEXT("uai:operate")};
					Request.CorrelationId = RequestId;
					ExternalImport::SkinWeight::FUeAgentSkinWeightImportCommandResponse Response;
					ExternalImport::SkinWeight::FUeAgentSkinWeightImportCommandError Error;
					if (!SkinWeight->Execute(Request, Response, Error))
					{
						OutError.Set(Error.Code, Error.SubjectId, Error.Detail);
						return false;
					}
					OutPayloadJson = MoveTemp(Response.PayloadJson);
					return !OutPayloadJson.IsEmpty();
				}
				if (CommandId == TEXT("external.localization.po.import"))
				{
					ExternalImport::Localization::FUeAgentLocalizationImportCommandRequest Request;
					Request.CommandId = CommandId;
					Request.RequestId = RequestId;
					Request.CanonicalParamsJson = DelegatedParams;
					Request.CallerId = ProviderId;
					Request.AuthorityScopes = {TEXT("uai:operate")};
					Request.CorrelationId = RequestId;
					ExternalImport::Localization::FUeAgentLocalizationImportCommandResponse Response;
					ExternalImport::Localization::FUeAgentLocalizationImportCommandError Error;
					if (!Localization->Execute(Request, Response, Error))
					{
						OutError.Set(Error.Code, Error.SubjectId, Error.Detail);
						return false;
					}
					OutPayloadJson = MoveTemp(Response.PayloadJson);
					return !OutPayloadJson.IsEmpty();
				}
				OutError.Set(TEXT("hjson_external_input_importer_unregistered"), CommandId);
				return false;
			}

		private:
			TSharedRef<ExternalImport::Factory::
				IUeAgentFactoryExternalImportCommandService> Factory;
			TSharedRef<ExternalImport::SkinWeight::
				IUeAgentSkinWeightImportCommandService> SkinWeight;
			TSharedRef<ExternalImport::Localization::
				IUeAgentLocalizationImportCommandService> Localization;
		};

		class FHjsonExternalInputDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			explicit FHjsonExternalInputDomainProvider(
				TSharedRef<IHjsonExternalInputImporterGateway> InGateway)
				: Gateway(MoveTemp(InGateway))
			{
			}

			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetCommandId() == TEXT("content_graph.import_workspace"))
				{
					return Request.GetSchemaId() == SchemaId
						&& FindHjsonExternalInputFamilyContract(
							Request.GetRootTypeIdentity()) != nullptr;
				}
				return Request.GetCommandId() == TEXT("content_graph.export_workspace")
					&& QueryOwners::HjsonExternalInput::
						HasHjsonExternalInputProvenance(Request.GetTargetIdentity());
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)
					|| Request.GetRole() != EInitializationNodeRole::RootTarget)
				{
					return false;
				}
				if (Request.GetCommandId() == TEXT("content_graph.export_workspace"))
				{
					OutInitializationContractId =
						TEXT("uai.export.hjson_external_input.factory/1");
					return true;
				}
				const FHjsonExternalInputFamilyContract* Contract =
					FindHjsonExternalInputFamilyContract(Request.GetRootTypeIdentity());
				if (!Contract)
				{
					return false;
				}
				OutInitializationContractId = Contract->InitializationContractId;
				return true;
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (Request.GetCommandId() == TEXT("content_graph.export_workspace"))
				{
					if (InitializationContractId
						!= TEXT("uai.export.hjson_external_input.factory/1"))
					{
						OutError.Set(
							TEXT("hjson_external_input_contract_not_executable"),
							InitializationContractId);
						return false;
					}
					FString PayloadJson;
					if (!QueryOwners::HjsonExternalInput::
							TryExportHjsonExternalInputWorkspace(
								Request.GetTargetIdentity(),
								Request.GetCanonicalParamsJson(),
								PayloadJson,
								OutError))
					{
						return false;
					}
					OutResponse = FImportInitializationDomainResponse(
						MoveTemp(PayloadJson), true);
					return OutResponse.IsValid();
				}
				const FHjsonExternalInputFamilyContract* Contract =
					FindHjsonExternalInputFamilyContract(Request.GetRootTypeIdentity());
				if (!Contract || Contract->InitializationContractId != InitializationContractId)
				{
					OutError.Set(
						TEXT("hjson_external_input_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FHjsonExternalInputDelegation Delegation;
				if (!TryBuildHjsonExternalInputDelegation(
						Request.GetCanonicalParamsJson(),
						*Contract,
						Delegation,
						OutError))
				{
					return false;
				}
				FString PayloadJson;
				if (!Gateway->Execute(
						Delegation.CommandId,
						Delegation.CanonicalParamsJson,
						PayloadJson,
						OutError))
				{
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(PayloadJson), false);
				return OutResponse.IsValid();
			}

		private:
			TSharedRef<IHjsonExternalInputImporterGateway> Gateway;
		};

		bool TryCreateRegisteredProvider(
			TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
			FWorkspaceError& OutError)
		{
			return TryCreateProductionHjsonExternalInputDomainProvider(
				OutProvider, OutError);
		}

		const FImportInitializationDomainProviderFactoryRegistration
			HjsonExternalInputDomainProviderRegistration(
				TEXT("hjson-external-input-domain-provider"),
				TEXT("hjson-external-input-domain-provider"),
				&TryCreateRegisteredProvider);
	}

	bool TryCreateHjsonExternalInputDomainProvider(
		TSharedRef<IHjsonExternalInputImporterGateway> Gateway,
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutError.Reset();
		OutProvider = MakeShared<FHjsonExternalInputDomainProvider>(MoveTemp(Gateway));
		return OutProvider.IsValid();
	}

	bool TryCreateProductionHjsonExternalInputDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		TSharedPtr<IHjsonExternalInputImporterGateway> Gateway;
		if (!FProductionImporterGateway::TryCreate(Gateway, OutError)
			|| !Gateway.IsValid())
		{
			OutProvider.Reset();
			return false;
		}
		return TryCreateHjsonExternalInputDomainProvider(
			Gateway.ToSharedRef(), OutProvider, OutError);
	}
}
