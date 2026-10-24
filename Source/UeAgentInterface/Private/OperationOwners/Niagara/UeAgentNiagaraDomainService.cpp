#include "OperationOwners/Niagara/UeAgentNiagaraDomainService.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/Niagara/UeAgentNiagaraDomainCommandHandlers.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::Niagara
{
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::Niagara;

	namespace
	{
		const FString ProviderId = TEXT("niagara-domain-provider");
		const FString SchemaId = TEXT("uai.niagara.edit/1");
		const FString SystemType = TEXT("/Script/Niagara.NiagaraSystem");
		const FString EmitterType = TEXT("/Script/Niagara.NiagaraEmitter");
		const FString ScriptType = TEXT("/Script/Niagara.NiagaraScript");
		const FString StackType = TEXT("uai.niagara.stack");
		const FString ModuleType = TEXT("/Script/NiagaraEditor.NiagaraNodeFunctionCall");
		const FString InputType = TEXT("uai.niagara.input");
		const FString ParameterType = TEXT("uai.niagara.parameter");
		const FString CandidateCommandId = TEXT("niagara.module.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == SystemType) return TEXT("uai.init.niagara_system.root/1");
				if (Type == EmitterType) return TEXT("uai.init.niagara_emitter.root/1");
				if (Type == ScriptType) return TEXT("uai.init.niagara_script.root/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == StackType) return TEXT("uai.init.niagara.stack/1");
				if (Type == InputType) return TEXT("uai.init.niagara.input/1");
				if (Type == ParameterType) return TEXT("uai.init.niagara.parameter/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject && Type == ModuleType)
			{
				return TEXT("uai.init.niagara.module/1");
			}
			return FString();
		}

		class FNiagaraDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }
			virtual bool SupportsNativeOwnedCreationContext(const FString& OwnerType, const FString& PropertyPath) const override
			{
				return OwnerType == ScriptType && PropertyPath == TEXT("ValidationRules");
			}

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
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
					? InitializationContract(Request.GetRootTypeIdentity(), Request.GetRole())
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
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_domain_contract_not_executable"),
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
				return CommandHandlers::Niagara::TryHandleDomainRequest(
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
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}


		class FNiagaraCandidateCommandService final
			: public IUeAgentNiagaraCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentNiagaraCandidateCommandRequest& Request,
				FUeAgentNiagaraCandidateCommandResponse& OutResponse,
				FUeAgentNiagaraCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_cancelled"), Request.CommandId,
						TEXT("The Niagara module candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_command_not_supported"), Request.CommandId,
						FString::Printf(TEXT("This service accepts only '%s'."), *CandidateCommandId));
					Diagnostic.PropertyPath = TEXT("command_supported");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					if (!JsonError.IsEmpty()) return ForwardDiagnostic(OutError, Contract::FDiagnostic::WrapFailure(
						TEXT("niagara_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_params_invalid"), Request.CommandId,
						TEXT("The Niagara module candidate parameters must be a JSON object."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/");
					Diagnostic.PropertyPath = TEXT("params");
					Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				FString Target;
				FString TargetUsage;
				FString EmitterName;
				FString StageName;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("niagara_candidate_target_required"), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("non-empty asset reference")), Params->TryGetField(TEXT("target")),
						TEXT("candidate_query"), TEXT("Niagara module candidate discovery requires a non-empty target asset reference."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (!Params->TryGetStringField(TEXT("target_usage"), TargetUsage)
					|| TargetUsage.TrimStartAndEnd().IsEmpty())
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("niagara_candidate_target_usage_required"), TEXT("/target_usage"),
						MakeShared<FJsonValueString>(TEXT("non-empty target usage")), Params->TryGetField(TEXT("target_usage")),
						TEXT("candidate_query"), TEXT("Niagara candidate discovery requires a non-empty target usage."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.TargetPath = Target.TrimStartAndEnd();
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("emitter_name"), EmitterName);
				Params->TryGetStringField(TEXT("stage_name"), StageName);
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("niagara_candidate_target_unresolved"), Target,
						TEXT("The Niagara module candidate target asset could not be loaded."));
					Diagnostic.TargetPath = Target.TrimStartAndEnd();
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable asset"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
				}

				FNiagaraModuleCandidateQueryResult QueryResult;
				FNiagaraModuleCandidateQueryError QueryError;
				FNiagaraModuleCandidateContext Context;
				Context.TargetUsage = TargetUsage;
				Context.EmitterName = EmitterName;
				Context.StageName = StageName;
				Context.SourceKind = TEXT("command_input");
				Context.Source.JsonPath = TEXT("/");
				Context.TargetUsageSource.JsonPath = TEXT("/target_usage");
				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				Scope->SetStringField(TEXT("target_usage"), TargetUsage.TrimStartAndEnd());
				if (!EmitterName.IsEmpty()) Scope->SetStringField(TEXT("emitter_name"), EmitterName);
				if (!StageName.IsEmpty()) Scope->SetStringField(TEXT("stage_name"), StageName);
				if (Params->HasField(TEXT("describe")))
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					CandidateProperties::FRequest DescriptionRequest;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
					{
						auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
							TEXT("niagara_candidate_description_invalid"), TEXT("/describe"),
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
					FNiagaraModuleCandidate Candidate;
					FNiagaraModuleCandidateContext ResolvedContext;
					if (!TrySelectNiagaraModuleCandidate(*TargetObject, DescriptionRequest.Name, Context, Candidate, ResolvedContext, QueryError, nullptr, DescriptionRequest.Select))
					{
						if (QueryError.SourceKind.IsEmpty())
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(QueryError, TEXT("/describe"));
							QueryError.SourceKind = TEXT("command_input");
						}
						if (QueryError.TargetPath.IsEmpty()) QueryError.TargetPath = Target.TrimStartAndEnd();
						return ForwardDiagnostic(OutError, QueryError, QueryError.SubjectId);
					}
					if (!CandidateProperties::BuildCreationSupportPayload(DescriptionRequest,
						Candidate.bSupported, Candidate.SupportReason, TEXT("native_stack_action"),
						TEXT("creation_support_only; module_inputs_resolve_in_created_stack_context"),
						OutResponse.PayloadJson, JsonError))
					{
						auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("niagara_candidate_description_failed"),
							JsonError, TEXT("candidate_query"));
						if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TrimStartAndEnd();
						if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = DescriptionRequest.Name;
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
					}
					if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}
				if (!TryQueryNiagaraModuleCandidates(
						*TargetObject,
						Query,
						Context,
						QueryResult,
						QueryError))
				{
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("niagara_candidate_query_failed"), Target,
							TEXT("Niagara module candidate discovery failed without providing a diagnostic."));
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
				for (const FNiagaraModuleCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.NativeIdentity, Candidate.SelectionFields});
				}
				if (!TryBuildCandidateNamesOnlyPayload(
						Names, ENameComparison::IgnoreCase,
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

	bool TryCreateNiagaraDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FNiagaraDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionNiagaraCandidateCommandService(
		TSharedPtr<IUeAgentNiagaraCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FNiagaraCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
