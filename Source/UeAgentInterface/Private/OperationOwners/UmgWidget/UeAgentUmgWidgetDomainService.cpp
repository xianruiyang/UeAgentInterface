#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"
#include "Components/Widget.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Misc/PackageName.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::OperationOwners::UmgWidget
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::UmgWidget;

	namespace
	{
		const FString ProviderId = TEXT("umg-widget-domain-provider");
		const FString SchemaId = TEXT("uai.umg.edit/1");
		const FString WidgetBlueprintType = TEXT("/Script/UMGEditor.WidgetBlueprint");
		const FString CandidateCommandId = TEXT("umg.widget.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget
				&& Type == WidgetBlueprintType)
			{
				return TEXT("uai.init.umg.widget_blueprint/1");
			}
			if (Role != EInitializationNodeRole::ContainerItem)
			{
				return FString();
			}
			if (Type == TEXT("uai.umg.widget")) return TEXT("uai.init.umg.widget/1");
			if (Type == TEXT("uai.umg.slot")) return TEXT("uai.init.umg.slot/1");
			if (Type == TEXT("uai.umg.binding")) return TEXT("uai.init.umg.binding/1");
			if (Type == TEXT("uai.umg.animation")) return TEXT("uai.init.umg.animation/1");
			return FString();
		}

		class FUmgWidgetDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return ProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request) const override
			{
				if (InitializationContract(
					Request.GetRootTypeIdentity(), Request.GetRole()).IsEmpty())
				{
					return false;
				}
				return Request.GetCommandId() != TEXT("content_graph.import_workspace")
					|| MatchesDomainImportSchema(Request, SchemaId);
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutError.Reset();
				OutInitializationContractId = Matches(Request)
					? InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole())
					: FString();
				return !OutInitializationContractId.IsEmpty();
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (Request.GetRole() != EInitializationNodeRole::RootTarget
					|| InitializationContractId != InitializationContract(
						Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_domain_contract_not_executable"),
						Request.GetRole() != EInitializationNodeRole::RootTarget
						? TEXT("This domain executor requires a root-target initialization request.")
						: TEXT("The initialization contract does not match the requested root type."), TEXT("domain_execute"));
					Detail.TargetPath = Request.GetTargetIdentity();
					Detail.SourceKind = TEXT("native_result");
					Detail.InternalContext = InitializationContractId;
					const bool bRootTarget = Request.GetRole() == EInitializationNodeRole::RootTarget;
					Detail.PropertyPath = bRootTarget ? TEXT("initialization_contract_matches") : TEXT("root_target_role");
					Detail.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Detail.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					if (bRootTarget) Detail.PublicMessage += FString::Printf(
						TEXT(" Requested root type: '%s'."), *Request.GetRootTypeIdentity());
					OutError.SetFailure(MoveTemp(Detail));
					return false;
				}
				return CommandHandlers::UmgWidget::TryHandleDomainRequest(
					Request, OutResponse, OutError);
			}
		};

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}


		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		class FUmgWidgetCandidateCommandService final
			: public IUeAgentUmgWidgetCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUmgWidgetCandidateCommandRequest& Request,
				FUmgWidgetCandidateCommandResponse& OutResponse,
				FUmgWidgetCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_cancelled"), Request.CommandId,
						TEXT("The UMG widget candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_command_not_supported"), Request.CommandId,
						FString::Printf(TEXT("This service accepts only '%s'."), *CandidateCommandId));
					Diagnostic.PropertyPath = TEXT("command_supported");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					if (!JsonError.IsEmpty()) return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(
						TEXT("umg_widget_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_params_invalid"), Request.CommandId,
						TEXT("The UMG widget candidate parameters must be a JSON object."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/");
					Diagnostic.PropertyPath = TEXT("params");
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("umg_widget_candidate_target_required"), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("non-empty asset reference")), Params->TryGetField(TEXT("target")),
						TEXT("candidate_query"), TEXT("UMG widget candidate discovery requires a non-empty target asset reference."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				UWidgetBlueprint* const TargetObject = LoadObject<UWidgetBlueprint>(
					nullptr, *ObjectPath(Target));
				if (!TargetObject
					|| TargetObject->GetClass() != UWidgetBlueprint::StaticClass())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_target_unresolved"), Target,
						TargetObject ? TEXT("UMG candidate discovery requires the WidgetBlueprint target class.")
						: TEXT("The UMG candidate target could not be loaded as a WidgetBlueprint."));
					Diagnostic.TargetPath = Target.TrimStartAndEnd();
					Diagnostic.PropertyPath = TargetObject ? TEXT("target_class") : TEXT("target");
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(
						TargetObject ? TargetObject->GetClass()->GetPathName() : Target);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UWidgetBlueprint::StaticClass()->GetPathName());
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
				}

				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				if (Params->HasField(TEXT("describe")))
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					CandidateProperties::FRequest DescriptionRequest;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
					{
						auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
							TEXT("umg_widget_candidate_description_invalid"), TEXT("/describe"),
							MakeShared<FJsonValueString>(TEXT("object")), Params->TryGetField(TEXT("describe")),
							TEXT("candidate_query"), TEXT("The candidate description request must be an object."));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
					}
					if (!CandidateProperties::ReadRequest(*Description, DescriptionRequest, JsonError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.SourceKind.IsEmpty()) JsonError.SourceKind = TEXT("command_input");
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, MoveTemp(JsonError), Target);
					}
					FUmgWidgetCandidate Candidate;
					FUmgWidgetCandidateQueryError ResolveError;
					if (!TrySelectUmgWidgetCandidate(*TargetObject, DescriptionRequest.Name, Candidate, ResolveError, DescriptionRequest.Select))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(ResolveError, TEXT("/describe"));
						if (ResolveError.SourceKind.IsEmpty()) ResolveError.SourceKind = TEXT("command_input");
						if (ResolveError.TargetPath.IsEmpty()) ResolveError.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, ResolveError, ResolveError.SubjectId);
					}
					TSharedPtr<FJsonObject> Described;
					if (Candidate.bSupported)
					{
						UClass* Class = StaticLoadClass(UWidget::StaticClass(), nullptr, *Candidate.ClassPath);
						if (!Class)
						{
							auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_class_unavailable"), DescriptionRequest.Name,
								TEXT("The selected widget candidate could not be loaded as a Widget class."));
							Diagnostic.TargetPath = Target.TrimStartAndEnd();
							Diagnostic.CandidateName = DescriptionRequest.Name;
							Diagnostic.PropertyPath = TEXT("candidate_class");
							Diagnostic.ActualValue = MakeShared<FJsonValueString>(Candidate.ClassPath);
							Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable Widget-derived class"));
							return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
						}
						UeAgentAuthoringState::FEditablePropertyReadOptions Policy;
						Policy.ExcludedProperties = UeAgentUmgAuthoring::WidgetStructuralProperties();
						Policy.Limits.bUseInstanceEditability = true;
						if (!CandidateProperties::DescribeObject(*Class->GetDefaultObject(), DescriptionRequest, MoveTemp(Policy), Described, JsonError))
						{
							auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("umg_widget_candidate_description_failed"),
								JsonError, TEXT("candidate_query"));
							if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TrimStartAndEnd();
							if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = DescriptionRequest.Name;
							return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
						}
					}
					else
					{
						Described = MakeShared<FJsonObject>();
						Described->SetStringField(TEXT("name"), DescriptionRequest.Name);
						Described->SetStringField(TEXT("basis"), TEXT("native_menu_action"));
					}
					CandidateProperties::SetCreationSupport(*Described, Candidate.bSupported,
						Candidate.SupportDiagnostic.IsEmpty() ? Candidate.SupportReason : Candidate.SupportDiagnostic.PublicMessage);
					if (!CandidateProperties::BuildPayload(DescriptionRequest, Described, OutResponse.PayloadJson, JsonError))
					{
						auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("umg_widget_candidate_description_failed"),
							JsonError, TEXT("candidate_query"));
						if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TrimStartAndEnd();
						if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = DescriptionRequest.Name;
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
					}
					if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}

				FUmgWidgetCandidateQueryResult QueryResult;
				FUmgWidgetCandidateQueryError QueryError;
				if (!TryQueryUmgWidgetCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError))
				{
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("umg_widget_candidate_query_failed"), Target,
							TEXT("UMG widget candidate discovery failed without providing a diagnostic."));
						QueryError.PropertyPath = TEXT("query_succeeded");
						QueryError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						QueryError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target.TrimStartAndEnd();
					return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
				}
				const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
					QueryResult.TotalMatches, QueryResult.Candidates.Num(), QueryResult.CatalogDiagnostic};
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(Completeness, JsonError))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}

				TArray<ContentDiscovery::FNameSelectionEntry> Names;
				for (const FUmgWidgetCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Id, Candidate.SelectionFields});
				}
				if (!ContentDiscovery::TryBuildCandidateNamesOnlyPayload(
					Names, ContentDiscovery::ENameComparison::IgnoreCase,
					OutResponse.PayloadJson, JsonError, QueryResult.CatalogDiagnostic))
				{
					return ForwardDiagnostic(OutError, JsonError, FString());
				}
				if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
				OutResponse.bReadOnly = true;
				return true;
			}
		};

	}

	bool TryCreateUmgWidgetDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!RegisterGraphTreeTargetCompositionContracts(RegistrationError))
		{
			OutProvider.Reset();
			OutError.SetFailure(RegistrationError);
			return false;
		}
		OutProvider = MakeShared<FUmgWidgetDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionUmgWidgetCandidateCommandService(
		TSharedPtr<IUeAgentUmgWidgetCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FUmgWidgetCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
