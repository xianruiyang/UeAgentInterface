#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::OperationOwners::AnimationBlueprint
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AnimationBlueprint;

	namespace
	{
		const FString ProviderId = TEXT("animation-blueprint-domain-provider");
		const FString SchemaId = TEXT("uai.animation_blueprint.edit/1");
		const FString RootType = TEXT("/Script/Engine.AnimBlueprint");
		const FString CandidateCommandId =
			TEXT("animation.state_machine.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget && Type == RootType)
			{
				return TEXT("uai.init.animation_blueprint.asset/1");
			}
			if (Role != EInitializationNodeRole::ContainerItem)
			{
				return FString();
			}
			if (Type == TEXT("uai.animation_blueprint.graph")) return TEXT("uai.init.animation_blueprint.graph/1");
			if (Type == TEXT("uai.animation_blueprint.state_machine")) return TEXT("uai.init.animation_blueprint.state_machine/1");
			if (Type == TEXT("uai.animation_blueprint.state")) return TEXT("uai.init.animation_blueprint.state/1");
			if (Type == TEXT("uai.animation_blueprint.transition")) return TEXT("uai.init.animation_blueprint.transition/1");
			if (Type == TEXT("uai.animation_blueprint.class_settings")) return TEXT("uai.init.animation_blueprint.class_settings/1");
			if (Type == TEXT("uai.blueprint.interface")) return TEXT("uai.init.blueprint.interface/1");
			if (Type == TEXT("uai.blueprint.member")) return TEXT("uai.init.blueprint.member/1");
			if (Type == TEXT("uai.blueprint.component")) return TEXT("uai.init.blueprint.component/1");
			if (Type == TEXT("uai.blueprint.graph")) return TEXT("uai.init.blueprint.graph/1");
			if (Type == TEXT("uai.blueprint.node")) return TEXT("uai.init.blueprint.node/1");
			if (Type == TEXT("uai.blueprint.pin")) return TEXT("uai.init.blueprint.pin/1");
			if (Type == TEXT("uai.blueprint.edge")) return TEXT("uai.init.blueprint.edge/1");
			return FString();
		}

		class FAnimationBlueprintDomainProvider final
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
				return Request.GetCommandId()
						!= TEXT("content_graph.import_workspace")
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
					|| InitializationContractId
						!= InitializationContract(
							Request.GetRootTypeIdentity(), Request.GetRole()))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_domain_contract_not_executable"),
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
				return CommandHandlers::AnimationBlueprint::TryHandleDomainRequest(
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


		class FAnimationBlueprintCandidateCommandService final
			: public IUeAgentAnimationBlueprintCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAnimationBlueprintCandidateCommandRequest& Request,
				FAnimationBlueprintCandidateCommandResponse& OutResponse,
				FAnimationBlueprintCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_cancelled"), Request.CommandId,
						TEXT("The Animation Blueprint candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_command_not_supported"), Request.CommandId,
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
						TEXT("animation_blueprint_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("animation_blueprint_candidate_params_invalid"), Request.CommandId,
						TEXT("The Animation Blueprint candidate parameters must be a JSON object."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/");
					Diagnostic.PropertyPath = TEXT("params");
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				FString Target;
				FString Surface;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("animation_blueprint_candidate_target_required"),
						TEXT("Animation candidate discovery requires a non-empty target asset reference."), TEXT("candidate_query"));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.ActualValue = Params->TryGetField(TEXT("target"));
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty asset reference"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (!Params->TryGetStringField(TEXT("surface"), Surface)
					|| Surface.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("animation_blueprint_candidate_surface_required"),
						TEXT("Animation candidate discovery requires a non-empty surface name."), TEXT("candidate_query"));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/surface");
					Diagnostic.PropertyPath = TEXT("surface");
					Diagnostic.TargetPath = Target;
					Diagnostic.ActualValue = Params->TryGetField(TEXT("surface"));
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("animation_graph or state_machine_graph"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				const TSharedPtr<FJsonObject>* ClassSettingsField = nullptr;
				TSharedPtr<FJsonObject> ClassSettings = MakeShared<FJsonObject>();
				if (Params->TryGetObjectField(
						TEXT("class_settings"), ClassSettingsField)
					&& ClassSettingsField && ClassSettingsField->IsValid())
				{
					ClassSettings = *ClassSettingsField;
				}
				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				Scope->SetStringField(TEXT("surface"), Surface.TrimStartAndEnd());
				if (!ClassSettings->Values.IsEmpty()) Scope->SetObjectField(TEXT("class_settings"), ClassSettings);
				CandidateProperties::FRequest DescriptionRequest;
				const bool bDescribe = Params->HasField(TEXT("describe"));
				if (bDescribe)
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
					{
						auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("animation_blueprint_candidate_description_invalid"),
							TEXT("The candidate description request must be an object."), TEXT("candidate_query"));
						Diagnostic.SourceKind = TEXT("command_input");
						Diagnostic.Source.JsonPath = TEXT("/describe");
						Diagnostic.TargetPath = Target;
						Diagnostic.ActualValue = Params->TryGetField(TEXT("describe"));
						Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
						Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
					}
					if (!CandidateProperties::ReadRequest(*Description, DescriptionRequest, JsonError))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, MoveTemp(JsonError));
					}
					Query = DescriptionRequest.Name;
				}
				FAnimationBlueprintCandidateQueryResult QueryResult;
				FAnimationBlueprintCandidateQueryError QueryError;
				if (!TryQueryAnimationBlueprintCandidates(
						Target,
						Surface,
						ClassSettings,
						Query,
						QueryResult,
						QueryError))
				{
					if (QueryError.IsEmpty())
					{
						QueryError = Contract::FDiagnostic::Failure(TEXT("animation_blueprint_candidate_query_failed"),
							TEXT("Animation candidate discovery failed without providing a diagnostic."), TEXT("candidate_query"));
						QueryError.SourceKind = TEXT("native_result");
						QueryError.PropertyPath = TEXT("query_succeeded");
						QueryError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						QueryError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target;
					return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
				}
				if (bDescribe)
				{
					TArray<ContentDiscovery::FNameSelectionEntry> Choices;
					for (const FAnimationBlueprintCandidate& Candidate : QueryResult.Candidates)
						Choices.Add({Candidate.Name, Candidate.Identity.CanonicalKey, Candidate.SelectionFields});
					int32 SelectedIndex = INDEX_NONE;
					if (!ContentDiscovery::TryResolveNameSelection(Choices, DescriptionRequest.Name,
						DescriptionRequest.Select, SelectedIndex, JsonError, ContentDiscovery::ENameComparison::IgnoreCase))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(JsonError, TEXT("/describe"));
						if (JsonError.SourceKind.IsEmpty()) JsonError.SourceKind = TEXT("command_input");
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, JsonError, DescriptionRequest.Name);
					}
					const FAnimationBlueprintCandidate* Selected = &QueryResult.Candidates[SelectedIndex];
					if (!CandidateProperties::BuildCreationSupportPayload(DescriptionRequest,
						Selected->bSupported, Selected->SupportDiagnostic.IsEmpty() ? Selected->SupportReason : Selected->SupportDiagnostic.PublicMessage, TEXT("native_animation_graph_action"),
						TEXT("creation_support_only; editable_fields_resolve_after_creation"),
						OutResponse.PayloadJson, JsonError))
					{
						if (JsonError.TargetPath.IsEmpty()) JsonError.TargetPath = Target;
						return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(TEXT("animation_blueprint_candidate_description_failed"), JsonError), DescriptionRequest.Name);
					}
					if (!TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}
				const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
					QueryResult.TotalMatches, QueryResult.Candidates.Num(), QueryResult.CatalogDiagnostic};
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(Completeness, JsonError))
					return ForwardDiagnostic(OutError, JsonError, FString());
				TArray<ContentDiscovery::FNameSelectionEntry> Names;
				for (const FAnimationBlueprintCandidate& Candidate
					: QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Identity.CanonicalKey, Candidate.SelectionFields});
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

	bool TryCreateAnimationBlueprintDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationBlueprintDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAnimationBlueprintCandidateCommandService(
		TSharedPtr<IUeAgentAnimationBlueprintCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FAnimationBlueprintCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
