#include "OperationOwners/HjsonExternalInput/UeAgentHjsonExternalInputProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputWorkspace.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"
#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"
#include "QueryOwners/HjsonExternalInput/UeAgentHjsonExternalInputReadback.h"


namespace UE::UAI::OperationOwners::HjsonExternalInput
{
	using namespace HjsonWorkspace;
	using namespace DomainAdapters::HjsonExternalInput;
	using namespace CommandContract;

	namespace
	{
		const FString ProviderId = TEXT("hjson-external-input-domain-provider");
		const FString SchemaId = TEXT("uai.hjson.external_input/1");

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
				UE::UAI::Contract::FDiagnostic Error;
				if (!ExternalImport::Factory::
					TryCreateProductionFactoryExternalImportCommandService(
						Factory, Error)
					|| !Factory.IsValid())
				{
					OutError.SetFailure(Error);
					return false;
				}
				if (!ExternalImport::SkinWeight::
					TryCreateProductionSkinWeightImportCommandService(
						SkinWeight, Error)
					|| !SkinWeight.IsValid())
				{
					OutError.SetFailure(Error);
					return false;
				}
				if (!ExternalImport::Localization::
					TryCreateProductionLocalizationImportCommandService(
						Localization, Error)
					|| !Localization.IsValid())
				{
					OutError.SetFailure(Error);
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
				auto AcceptPayload = [&](FString&& Payload)
				{
					if (Payload.IsEmpty())
					{
						OutError.SetFailure(UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_importer_result_missing"),
							FString::Printf(TEXT("Importer '%s' reported success without a result payload."), *CommandId), TEXT("external_import")));
						return false;
					}
					OutPayloadJson = MoveTemp(Payload);
					return true;
				};
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					OutError.SetFailure(JsonError, FString());
					return false;
				}
				FString ExpectedSha256;
				FString SourceRef;
				Params->TryGetStringField(
					TEXT("_hjson_expected_source_sha256"), ExpectedSha256);
				Params->TryGetStringField(TEXT("source_ref"), SourceRef);
				Params->RemoveField(TEXT("_hjson_expected_source_sha256"));
				Params->RemoveField(TEXT("_hjson_source_format"));
				FString ActualPath, ActualSha256;
				if (!TryReadExternalInputSource(SourceRef, ActualPath, ActualSha256, OutError))
				{
					auto Failure = OutError.GetDiagnostic();
					Failure.SemanticPointer = TEXT("/root/external_input/source/identity");
					Failure.SourceKind = TEXT("related_input");
					OutError.SetFailure(MoveTemp(Failure));
					return false;
				}
				if (ActualSha256 != ExpectedSha256.ToLower())
				{
					auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_source_digest_mismatch"),
						FString::Printf(TEXT("External source '%s' changed between preparation and importer execution."), *SourceRef), TEXT("external_import"));
					Failure.SemanticPointer = TEXT("/root/external_input/source/identity");
					Failure.SourceKind = TEXT("related_input");
					OutError.SetFailure(MoveTemp(Failure));
					return false;
				}
				FString DelegatedParams;
				if (!Json::TryCanonicalize(Params, DelegatedParams, JsonError))
				{
					OutError.SetFailure(JsonError, FString());
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
						OutError.SetFailure(Error, Error.SubjectId);
						return false;
					}
					return AcceptPayload(MoveTemp(Response.PayloadJson));
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
						OutError.SetFailure(Error, Error.SubjectId);
						return false;
					}
					return AcceptPayload(MoveTemp(Response.PayloadJson));
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
						OutError.SetFailure(Error, Error.SubjectId);
						return false;
					}
					return AcceptPayload(MoveTemp(Response.PayloadJson));
				}
				auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_importer_unregistered"),
					TEXT("No external-input importer is registered for this command."), TEXT("external_import"));
				Detail.CandidateName = CommandId;
				Detail.ActualValue = MakeShared<FJsonValueString>(CommandId);
				Detail.Source.InternalJsonPath = TEXT("/root/external_input/target_policy/command_id");
				OutError.SetFailure(MoveTemp(Detail));
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

			virtual bool SupportsWorkspaceModelContract(const FString& ContractId) const override
			{ return ContractId == GetExternalInputWorkspaceContract(); }
			virtual bool SupportsWorkspacePreparation() const override { return true; }
			virtual bool SupportsExplicitCommand(const FString& CommandId) const override
			{ return CommandId == TEXT("content_graph.export_workspace"); }

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetCommandId() == TEXT("content_graph.import_workspace"))
				{
					return MatchesDomainImportSchema(Request, SchemaId)
						&& FindHjsonExternalInputFamilyContract(
							Request.GetRootTypeIdentity()) != nullptr;
				}
				return Request.GetCommandId() == TEXT("content_graph.export_workspace")
					&& Request.GetSchemaId() == SchemaId
					&& FindHjsonExternalInputFamilyContract(Request.GetRootTypeIdentity());
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
						auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_contract_not_executable"),
							TEXT("The initialization contract does not support external-input workspace export."), TEXT("domain_execute"));
						Detail.TargetPath = Request.GetTargetIdentity();
						Detail.InternalContext = InitializationContractId;
						Detail.SourceKind = TEXT("native_result");
						OutError.SetFailure(MoveTemp(Detail));
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_external_input_contract_not_executable"),
						!Contract ? TEXT("No external-input import contract supports the requested root type.")
							: TEXT("The initialization contract does not match the external-input root type."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.ActualValue = MakeShared<FJsonValueString>(Request.GetRootTypeIdentity());
					Detail.InternalContext = InitializationContractId;
					Detail.SourceKind = TEXT("native_result");
					OutError.SetFailure(MoveTemp(Detail));
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
					Delegation.LocateFailure(OutError);
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(PayloadJson), false);
				return OutResponse.IsValid();
			}

		private:
			TSharedRef<IHjsonExternalInputImporterGateway> Gateway;
		};

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
