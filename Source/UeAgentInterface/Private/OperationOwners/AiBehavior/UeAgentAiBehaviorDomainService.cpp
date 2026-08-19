#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"

#include "CommandHandlers/AiBehavior/UeAgentAiBehaviorDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AiBehavior
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AiBehavior;

	namespace
	{
		const FString ProviderId = TEXT("ai-behavior-domain-provider");
		const FString SchemaId = TEXT("uai.ai_behavior.edit/1");
		const FString BehaviorTreeType = TEXT("/Script/AIModule.BehaviorTree");
		const FString BlackboardType = TEXT("/Script/AIModule.BlackboardData");
		const FString StateTreeType = TEXT("/Script/StateTreeModule.StateTree");
		const FString BehaviorNodeType = TEXT("/Script/AIModule.BTNode");
		const FString BehaviorServiceType = TEXT("/Script/AIModule.BTService");
		const FString BehaviorDecoratorType = TEXT("/Script/AIModule.BTDecorator");
		const FString StateTreeNodeType =
			TEXT("/Script/StateTreeModule.StateTreeNodeBlueprintBase");
		const FString BlackboardEntryType = TEXT("uai.ai.blackboard_entry");
		const FString ConnectionType = TEXT("uai.ai.connection");
		const FString CandidateCommandId = TEXT("ai.behavior.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == BehaviorTreeType) return TEXT("uai.init.ai.behavior_tree.root/1");
				if (Type == BlackboardType) return TEXT("uai.init.ai.blackboard.root/1");
				if (Type == StateTreeType) return TEXT("uai.init.ai.state_tree.root/1");
			}
			if (Role == EInitializationNodeRole::OwnedObject)
			{
				if (Type == BehaviorNodeType) return TEXT("uai.init.ai.behavior_node/1");
				if (Type == BehaviorServiceType) return TEXT("uai.init.ai.behavior_service/1");
				if (Type == BehaviorDecoratorType) return TEXT("uai.init.ai.behavior_decorator/1");
				if (Type == StateTreeNodeType) return TEXT("uai.init.ai.state_tree_node/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == BlackboardEntryType) return TEXT("uai.init.ai.blackboard_entry/1");
				if (Type == ConnectionType) return TEXT("uai.init.ai.connection/1");
			}
			return FString();
		}

		class FAiBehaviorDomainProvider final : public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override { return ProviderId; }

			virtual bool Matches(const FImportInitializationDomainRequest& Request) const override
			{
				if (InitializationContract(Request.GetRootTypeIdentity(), Request.GetRole()).IsEmpty())
				{
					return false;
				}
				return Request.GetCommandId() != TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == SchemaId;
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
					OutError.Set(TEXT("ai_behavior_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::AiBehavior::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FAiBehaviorCandidateCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT("."))) return Target;
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		class FAiBehaviorCandidateCommandService final
			: public IUeAgentAiBehaviorCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAiBehaviorCandidateCommandRequest& Request,
				FAiBehaviorCandidateCommandResponse& OutResponse,
				FAiBehaviorCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_command_not_supported"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_params_invalid"), Request.CommandId, JsonError);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_target_required"), Request.CommandId);
				}
				Params->TryGetStringField(TEXT("query"), Query);
				int32 Limit = 64;
				int32 Cursor = 0;
				double Number = 0.0;
				if (Params->TryGetNumberField(TEXT("limit"), Number)) Limit = static_cast<int32>(Number);
				if (Params->TryGetNumberField(TEXT("cursor"), Number)) Cursor = static_cast<int32>(Number);
				UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target));
				if (!TargetObject)
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_target_unresolved"), Target);
				}
				FAiBehaviorCandidateQueryResult QueryResult;
				FAiBehaviorCandidateQueryError QueryError;
				if (!TryQueryAiBehaviorCandidates(
						*TargetObject, Query, QueryResult, QueryError, Limit, Cursor))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT("ai_behavior_candidate_query_failed"))
							: QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CandidateCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("target"), Target);
				Payload->SetStringField(TEXT("target_revision"), QueryResult.TargetRevision);
				Payload->SetNumberField(TEXT("total_matches"), QueryResult.TotalMatches);
				Payload->SetNumberField(TEXT("next_cursor"), QueryResult.NextCursor);
				Payload->SetBoolField(TEXT("complete"), QueryResult.bComplete);
				Payload->SetBoolField(TEXT("has_more"), QueryResult.bHasMore);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FAiBehaviorCandidate& Candidate : QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("name"), Candidate.Name);
					Value->SetStringField(TEXT("label"), Candidate.Label);
					Value->SetStringField(TEXT("kind"), Candidate.Kind);
					Value->SetArrayField(TEXT("supported_operations"), StringArray(Candidate.SupportedOperations));
					Value->SetStringField(TEXT("input_contract"), Candidate.InputContract);
					Value->SetStringField(TEXT("placement_contract"), Candidate.PlacementContract);
					Value->SetStringField(TEXT("required_context"), Candidate.RequiredContext);
					Value->SetStringField(TEXT("binding_revision"), Candidate.BindingRevision);
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					return Fail(OutError, TEXT("ai_behavior_candidate_result_invalid"), Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};
	}

	bool TryCreateAiBehaviorDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAiBehaviorDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAiBehaviorCandidateCommandService(
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FAiBehaviorCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
