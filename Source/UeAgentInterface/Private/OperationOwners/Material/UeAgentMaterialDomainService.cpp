#include "OperationOwners/Material/UeAgentMaterialDomainService.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/Material/UeAgentMaterialDomainCommandHandlers.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"
#include "Materials/MaterialExpression.h"
#include "Misc/PackageName.h"
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentCandidatePropertyDescription.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::OperationOwners::Material
{
	using namespace HjsonWorkspace;
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace QueryOwners::Material;

	namespace
	{
		const FString ProviderId = TEXT("material-domain-provider");
		const FString SchemaId = TEXT("uai.material.edit/1");
		const FString MaterialType = TEXT("/Script/Engine.Material");
		const FString FunctionType = TEXT("/Script/Engine.MaterialFunction");
		const FString InstanceType = TEXT("/Script/Engine.MaterialInstanceConstant");
		const FString ExpressionType = TEXT("/Script/Engine.MaterialExpression");
		const FString CandidateCommandId = TEXT("material.expression.candidates");

		FString RootContract(const FString& Type)
		{
			if (Type == MaterialType) return TEXT("uai.init.material.root/1");
			if (Type == FunctionType
				|| Type == TEXT("/Script/Engine.MaterialFunctionMaterialLayer")
				|| Type == TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlend")) return TEXT("uai.init.material_function.root/1");
			if (Type == InstanceType) return TEXT("uai.init.material_instance.root/1");
			return FString();
		}

		class FMaterialDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				const bool bRoot = Request.GetRole() == EInitializationNodeRole::RootTarget
					&& !RootContract(Request.GetRootTypeIdentity()).IsEmpty();
				const bool bOwnedExpression =
					Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& Request.GetRootTypeIdentity() == ExpressionType;
				if (!bRoot && !bOwnedExpression) return false;
				return MatchesDomainImportSchema(Request, SchemaId);
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (!Matches(Request)) return false;
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject)
				{
					OutInitializationContractId = TEXT("uai.init.material.expression/1");
				}
				else
				{
					OutInitializationContractId = RootContract(Request.GetRootTypeIdentity());
				}
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
					|| InitializationContractId != RootContract(Request.GetRootTypeIdentity()))
				{
					auto Detail = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_domain_contract_not_executable"),
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
				return CommandHandlers::Material::TryHandleDomainRequest(
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
			const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
			return AssetName.IsEmpty() ? Target : Target + TEXT(".") + AssetName;
		}

		class FMaterialCandidateCommandService final
			: public IUeAgentMaterialCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentMaterialCandidateCommandRequest& Request,
				FUeAgentMaterialCandidateCommandResponse& OutResponse,
				FUeAgentMaterialCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_cancelled"), Request.CommandId,
						TEXT("The material expression candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_command_not_supported"), Request.CommandId,
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
						TEXT("material_candidate_params_invalid"), JsonError, TEXT("candidate_query")), Request.CommandId);
					auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_params_invalid"), Request.CommandId,
						TEXT("The material expression candidate parameters must be a JSON object."));
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
						TEXT("material_candidate_target_required"), TEXT("/target"),
						MakeShared<FJsonValueString>(TEXT("non-empty asset reference")), Params->TryGetField(TEXT("target")),
						TEXT("candidate_query"), TEXT("Material expression candidate discovery requires a non-empty target asset reference."));
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
					if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
				if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError))
					return ForwardDiagnostic(OutError, TargetError, Target);
				FMaterialExpressionCandidateContext CandidateContext;
				Params->TryGetStringField(
					TEXT("graph_name"), CandidateContext.GraphName);
				Params->TryGetStringField(
					TEXT("pin_name"), CandidateContext.PinName);
				UObject* TargetObject = StaticLoadObject(
					UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_target_unresolved"), Target,
						TEXT("The material expression candidate target asset could not be loaded."));
					Diagnostic.TargetPath = Target.TrimStartAndEnd();
					Diagnostic.PropertyPath = TEXT("target");
					Diagnostic.SourceKind = TEXT("command_input");
					Diagnostic.Source.JsonPath = TEXT("/target");
					Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable asset"));
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target);
				}

				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("target"), Target.TrimStartAndEnd());
				if (!CandidateContext.GraphName.IsEmpty()) Scope->SetStringField(TEXT("graph_name"), CandidateContext.GraphName);
				if (!CandidateContext.PinName.IsEmpty()) Scope->SetStringField(TEXT("pin_name"), CandidateContext.PinName);
				if (Params->HasField(TEXT("describe")))
				{
					const TSharedPtr<FJsonObject>* Description = nullptr;
					CandidateProperties::FRequest DescriptionRequest;
					if (!Params->TryGetObjectField(TEXT("describe"), Description))
					{
						auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
							TEXT("material_candidate_description_invalid"), TEXT("/describe"),
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
					UeAgentMaterialAuthoring::FExpressionResolveRequest ResolveRequest;
					ResolveRequest.CatalogName = DescriptionRequest.Name;
					ResolveRequest.CatalogSelect = DescriptionRequest.Select;
					ResolveRequest.CatalogInputContext.JsonPath = TEXT("/describe");
					ResolveRequest.CatalogInputContext.SourceKind = TEXT("command_input");
					ResolveRequest.ContextAssetPath = TargetObject->GetPathName();
					ResolveRequest.ContextGraphName = CandidateContext.GraphName;
					ResolveRequest.ContextPinName = CandidateContext.PinName;
					UeAgentMaterialAuthoring::FExpressionResolveResult Resolved;
					if (!UeAgentMaterialAuthoring::ResolveExpression(ResolveRequest, Resolved, JsonError))
						return ForwardDiagnostic(OutError, JsonError, Target);
					const auto* Prototype = Resolved.ExpressionClass ? Resolved.ExpressionClass->GetDefaultObject<UMaterialExpression>() : nullptr;
					TSharedPtr<FJsonObject> Described;
					if (!Prototype)
					{
						auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_description_failed"), DescriptionRequest.Name,
							TEXT("The selected material expression has no class default object to describe."));
						Diagnostic.TargetPath = Target.TrimStartAndEnd();
						Diagnostic.CandidateName = DescriptionRequest.Name;
						Diagnostic.PropertyPath = TEXT("class_default_object_available");
						Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
						Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						if (Resolved.ExpressionClass) Diagnostic.PublicMessage += FString::Printf(
							TEXT(" Expression class: '%s'."), *Resolved.ExpressionClass->GetPathName());
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
					}
					if (!CandidateProperties::DescribeObject(*Prototype, DescriptionRequest,
						UeAgentMaterialAuthoring::ExpressionPropertyPolicy(*Prototype), Described, JsonError))
					{
						auto Diagnostic = Contract::FDiagnostic::WrapFailure(TEXT("material_candidate_description_failed"),
							JsonError, TEXT("candidate_query"));
						if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TrimStartAndEnd();
						if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = DescriptionRequest.Name;
						return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), DescriptionRequest.Name);
					}
					Described->SetStringField(TEXT("interface_scope"), TEXT("properties_only; pins_resolve_in_created_graph_context"));
					CandidateProperties::SetCreationSupport(*Described, true, FString());
					if (!CandidateProperties::BuildPayload(DescriptionRequest, Described, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutResponse.PayloadJson, JsonError)) return ForwardDiagnostic(OutError, JsonError, FString());
					OutResponse.bReadOnly = true;
					return true;
				}

				FMaterialExpressionCandidateQueryResult QueryResult;
				FMaterialExpressionCandidateQueryError QueryError;
				if (!TryQueryMaterialExpressionCandidates(
						*TargetObject,
						Query,
						QueryResult,
						QueryError,
						CandidateContext))
				{
					if (QueryError.IsEmpty())
					{
						QueryError = CandidateDiagnostic(TEXT("material_candidate_query_failed"), Target,
							TEXT("Material expression candidate discovery failed without providing a diagnostic."));
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
				for (const FMaterialExpressionCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name, Candidate.Id, {}});
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

	bool TryCreateMaterialDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FMaterialDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionMaterialCandidateCommandService(
		TSharedPtr<IUeAgentMaterialCandidateCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FMaterialCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
