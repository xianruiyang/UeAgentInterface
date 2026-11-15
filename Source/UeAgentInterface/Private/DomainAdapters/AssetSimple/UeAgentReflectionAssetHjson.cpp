#include "DomainAdapters/AssetSimple/UeAgentReflectionAssetHjson.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/HjsonImport/UeAgentReflectionRootPropertyImport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AssetSimple
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	namespace
	{
		class FProvider final : public IImportInitializationDomainProvider
		{
		public:
			explicit FProvider(FReflectionAssetHjsonConfig InConfig) : Config(MoveTemp(InConfig)), ProviderId(Config.ProviderName + TEXT("-domain-provider")), RootType(Config.RootType), ContractId(TEXT("uai.init.") + Config.ProfileId + TEXT(".root/1")) {}
			const FString& GetProviderId() const override { return ProviderId; }
			bool IsFallbackProvider() const override
			{
				const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(Config.ProfileId);
				return Profile && Profile->bFallbackDomain;
			}
			bool SupportsNativeOwnedCreationContext(const FString& OwnerType, const FString& PropertyPath) const override
			{
				return OwnerType == RootType && Config.OwnedProperties.Contains(PropertyPath);
			}
			bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				if (Request.GetRole() != EInitializationNodeRole::RootTarget) return false;
				const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(Config.ProfileId);
				if (Profile && Profile->AcceptsConcreteClass)
					return Profile->AcceptsConcreteClass(FindObject<UClass>(nullptr, *Request.GetRootTypeIdentity()));
				return Request.GetRootTypeIdentity() == RootType;
			}
			bool TryResolveInitializationContract(const FImportInitializationDomainRequest& Request,
				FString& OutId, FWorkspaceError& Error) const override
			{
				OutId.Reset(); Error.Reset();
				if (!Matches(Request)) return false;
				OutId = ContractId; return true;
			}
			bool TryExecute(const FImportInitializationDomainRequest& Request,
				const FString& SelectedContract, FImportInitializationDomainResponse& Response,
				FWorkspaceError& Error) const override
			{
				Response = {}; Error.Reset();
				if (!Matches(Request) || SelectedContract != ContractId)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Config.ProfileId + TEXT("_hjson_contract_not_executable"),
						!Matches(Request) ? TEXT("The request target type or initialization role is not supported by this reflection asset provider.")
							: TEXT("The selected initialization contract does not match this reflection asset provider."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.InternalContext = SelectedContract;
					Detail.SourceKind = TEXT("native_result");
					Error.SetFailure(MoveTemp(Detail));
					return false;
				}
				const FString& Target = Request.GetTargetIdentity();
				UE::UAI::Contract::FDiagnostic RegistrationError;
				if (!UeAgentReflectionAuthoring::RegisterAuthoring(RegistrationError))
				{
					Error.SetFailure(MoveTemp(RegistrationError), Target);
					return false;
				}
				const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(Config.ProfileId);
				if (!Profile)
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Config.ProfileId + TEXT("_profile_missing"),
						TEXT("The reflection asset authoring profile is not registered."), TEXT("domain_execute"));
					Detail.TargetPath = Target;
					Detail.InternalContext = Config.ProfileId;
					Detail.SourceKind = TEXT("native_result");
					Error.SetFailure(MoveTemp(Detail));
					return false;
				}
				FString Payload;
				if (Request.GetCommandId() == TEXT("content_graph.export_workspace"))
				{
					TSharedPtr<FJsonObject> Params; UE::UAI::Contract::FDiagnostic ParseError; FString Path;
					if (!CommandContract::Json::TryParseObject(Request.GetCanonicalParamsJson(), Params, ParseError)
						|| !Params || !Params->TryGetStringField(TEXT("workspace_path"), Path) || Path.IsEmpty())
					{ Error.SetFailure(ParseError.IsValid() ? MoveTemp(ParseError) : UE::UAI::Contract::FDiagnostic::Failure(Config.ProfileId + TEXT("_export_path_required"), TEXT("workspace_path must be a non-empty string."), TEXT("workspace_export")), Target); return false; }
					if (!Composition::HjsonImport::TryExportReflectionRootWorkspace(*Profile, Target, Path, Payload, Error,
						Request.GetWorkspaceExportPreparation().Get())) return false;
					Response = FImportInitializationDomainResponse(MoveTemp(Payload), true);
					return Response.IsValid();
				}
				if (Request.GetCommandId() != TEXT("content_graph.import_workspace"))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Config.ProfileId + TEXT("_hjson_command_not_supported"),
						TEXT("This reflection asset provider supports only workspace export and import."), TEXT("domain_execute"));
					Detail.CandidateName = Request.GetCommandId();
					Detail.TargetPath = Target;
					Error.SetFailure(MoveTemp(Detail));
					return false;
				}
				FResolvedDomainHjsonDocument Document;
				if (!TryDecodeResolvedDocument(Target, Request.GetCanonicalParamsJson(),
					FDomainHjsonDocumentContract(Config.ProfileId, TEXT("uai.") + Config.ProfileId + TEXT(".edit/1"), Request.GetRootTypeIdentity()),
					Document, Error)) return false;
				FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, Error);
				if (!Document.bSemanticWorkspaceDirectory || !Document.SemanticWorkspace.IsValid())
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Config.ProfileId + TEXT("_semantic_workspace_required"),
						TEXT("Reflection asset import requires a resolved exported semantic workspace directory."), TEXT("document_restore"));
					Detail.TargetPath = Target;
					Detail.SourceKind = TEXT("command_input");
					Detail.Source.JsonPath = TEXT("/source");
					Error.SetFailure(MoveTemp(Detail));
					return false;
				}
				if (!Composition::HjsonImport::TryImportReflectionWorkspace(*Profile,
					Document.SemanticWorkspace.ToSharedRef(),
					{ProviderId, Target, ObjectPathFromPackageIdentity(Target), Config.ProviderName + TEXT("-semantic"),
					 (Config.ProfileId + TEXT("_prepare_failed")), (Config.ProfileId + TEXT("_apply_failed")), (Config.ProfileId + TEXT("_readback_invalid")),
					 (Config.ProfileId + TEXT("_compile_not_settled"))},
					[](TArray<TSharedPtr<FJsonValue>>&) {}, Payload, Error)) return false;
				Response = FImportInitializationDomainResponse(MoveTemp(Payload), false);
				return Response.IsValid();
			}
		private:
			FReflectionAssetHjsonConfig Config;
			FString ProviderId, RootType, ContractId;
		};
	}
	TSharedRef<IImportInitializationDomainProvider> CreateReflectionAssetHjsonProvider(FReflectionAssetHjsonConfig Config)
	{
		return MakeShared<FProvider>(MoveTemp(Config));
	}
}
