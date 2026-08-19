#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"

#include "CommandHandlers/AnimationRigGraph/UeAgentAnimationRigGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::OperationOwners::AnimationRigGraph
{
	using namespace CommandContract;
	using namespace HjsonWorkspace;
	using namespace QueryOwners::AnimationRigGraph;

	namespace
	{
		const FString ProviderId = TEXT("animation-rig-graph-domain-provider");
		const FString SchemaId = TEXT("uai.animation_rig_graph.edit/1");
		const FString ControlRigType =
			TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
		const FString IkRigType = TEXT("/Script/IKRig.IKRigDefinition");
		const FString IkRetargeterType = TEXT("/Script/IKRig.IKRetargeter");
		const FString SkeletonType = TEXT("/Script/Engine.Skeleton");
		const FString ControlType = TEXT("uai.rig.control");
		const FString BoneType = TEXT("uai.rig.bone");
		const FString ConstraintType = TEXT("uai.rig.constraint");
		const FString SpaceType = TEXT("uai.rig.space");
		const FString CandidateCommandId =
			TEXT("animation_rig_graph.candidates");

		FString InitializationContract(
			const FString& Type,
			const EInitializationNodeRole Role)
		{
			if (Role == EInitializationNodeRole::RootTarget)
			{
				if (Type == ControlRigType) return TEXT("uai.init.rig.control_rig.root/1");
				if (Type == IkRigType) return TEXT("uai.init.rig.ik_rig.root/1");
				if (Type == IkRetargeterType) return TEXT("uai.init.rig.ik_retargeter.root/1");
				if (Type == SkeletonType) return TEXT("uai.init.rig.skeleton.root/1");
			}
			if (Role == EInitializationNodeRole::ContainerItem)
			{
				if (Type == ControlType) return TEXT("uai.init.rig.control/1");
				if (Type == BoneType) return TEXT("uai.init.rig.bone/1");
				if (Type == ConstraintType) return TEXT("uai.init.rig.constraint/1");
				if (Type == SpaceType) return TEXT("uai.init.rig.space/1");
			}
			return FString();
		}

		class FAnimationRigGraphDomainProvider final : public IImportInitializationDomainProvider
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
					OutError.Set(TEXT("animation_rig_graph_domain_contract_not_executable"), InitializationContractId);
					return false;
				}
				return CommandHandlers::AnimationRigGraph::TryHandleDomainRequest(Request, OutResponse, OutError);
			}
		};

		bool Fail(
			FAnimationRigGraphCandidateCommandError& OutError,
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

		class FAnimationRigGraphCandidateCommandService final
			: public IUeAgentAnimationRigGraphCandidateCommandService
		{
		public:
			virtual bool Execute(
				const FAnimationRigGraphCandidateCommandRequest& Request,
				FAnimationRigGraphCandidateCommandResponse& OutResponse,
				FAnimationRigGraphCandidateCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("animation_rig_graph_candidate_cancelled"), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					return Fail(OutError, TEXT("animation_rig_graph_candidate_command_not_supported"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					return Fail(OutError, TEXT("animation_rig_graph_candidate_params_invalid"), Request.CommandId, JsonError);
				}
				FString Target;
				FString Query;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("animation_rig_graph_candidate_target_required"), Request.CommandId);
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
					return Fail(OutError, TEXT("animation_rig_graph_candidate_target_unresolved"), Target);
				}
				FAnimationRigGraphCandidateQueryResult QueryResult;
				FAnimationRigGraphCandidateQueryError QueryError;
				if (!TryQueryAnimationRigGraphCandidates(
						*TargetObject, Query, QueryResult, QueryError, Limit, Cursor))
				{
					return Fail(
						OutError,
						QueryError.Code.IsEmpty()
							? FString(TEXT("animation_rig_graph_candidate_query_failed"))
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
				for (const FAnimationRigGraphCandidate& Candidate : QueryResult.Candidates)
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
					return Fail(OutError, TEXT("animation_rig_graph_candidate_result_invalid"), Request.CommandId, JsonError);
				}
				OutResponse.bReadOnly = true;
				return true;
			}
		};
	}

	bool TryCreateAnimationRigGraphDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		OutProvider = MakeShared<FAnimationRigGraphDomainProvider>();
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionAnimationRigGraphCandidateCommandService(
		TSharedPtr<IUeAgentAnimationRigGraphCandidateCommandService>& OutService,
		FString& OutError)
	{
		OutService = MakeShared<FAnimationRigGraphCandidateCommandService>();
		OutError.Reset();
		return true;
	}
}
