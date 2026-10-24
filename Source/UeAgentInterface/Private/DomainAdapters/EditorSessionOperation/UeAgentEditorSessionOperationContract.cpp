#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationContract.h"

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	FUeAgentEditorSessionOperationFamilyContract::
	FUeAgentEditorSessionOperationFamilyContract(
		EUeAgentEditorSessionOperationFamily InFamily,
		FString InFamilyId,
		FString InSubjectKind,
		FString InPolicyKind,
		FString InSignalTypeId)
		: Family(InFamily)
		, FamilyId(MoveTemp(InFamilyId))
		, SubjectKind(MoveTemp(InSubjectKind))
		, PolicyKind(MoveTemp(InPolicyKind))
		, SignalTypeId(MoveTemp(InSignalTypeId))
	{
	}

	EUeAgentEditorSessionOperationFamily
	FUeAgentEditorSessionOperationFamilyContract::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentEditorSessionOperationFamilyContract::GetFamilyId() const
	{
		return FamilyId;
	}

	const FString&
	FUeAgentEditorSessionOperationFamilyContract::GetSubjectKind() const
	{
		return SubjectKind;
	}

	const FString&
	FUeAgentEditorSessionOperationFamilyContract::GetPolicyKind() const
	{
		return PolicyKind;
	}

	const FString&
	FUeAgentEditorSessionOperationFamilyContract::GetSignalTypeId() const
	{
		return SignalTypeId;
	}

	const TArray<FUeAgentEditorSessionOperationFamilyContract>&
	GetEditorSessionOperationFamilyContracts()
	{
		static const TArray<
			FUeAgentEditorSessionOperationFamilyContract> Contracts = {
				{
					EUeAgentEditorSessionOperationFamily::AssetEditor,
					TEXT("asset_editor"),
					TEXT("asset_ref"),
					TEXT("asset_editor_policy_ref"),
					TEXT("uai.editor_session.asset_editor.state/v1"),
				},
				{
					EUeAgentEditorSessionOperationFamily::RuntimePreview,
					TEXT("runtime_preview"),
					TEXT("runtime_subject_ref"),
					TEXT("runtime_session_policy_ref"),
					TEXT("uai.editor_session.runtime_preview.state/v1"),
				},
				{
					EUeAgentEditorSessionOperationFamily::WorldContext,
					TEXT("world_context"),
					TEXT("world_ref"),
					TEXT("world_context_policy_ref"),
					TEXT("uai.editor_session.world_context.state/v1"),
				},
				{
					EUeAgentEditorSessionOperationFamily::ModelingTool,
					TEXT("modeling_tool"),
					TEXT("modeling_session_ref"),
					TEXT("modeling_tool_policy_ref"),
					TEXT("uai.editor_session.modeling_tool.state/v1"),
				},
				{
					EUeAgentEditorSessionOperationFamily::HostLifecycle,
					TEXT("host_lifecycle"),
					TEXT("editor_instance_ref"),
					TEXT("host_lifecycle_policy_ref"),
					TEXT("uai.editor_session.host_lifecycle.state/v1"),
				},
				{
					EUeAgentEditorSessionOperationFamily::PcgGeneration,
					TEXT("pcg_generation"),
					TEXT("pcg_component_ref"),
					TEXT("pcg_generation_policy_ref"),
					TEXT("uai.editor_session.pcg_generation.state/v1"),
				},
			};
		return Contracts;
	}

	bool IsEditorSessionCommandFamily(const EUeAgentEditorSessionOperationFamily Family)
	{
		return Family != EUeAgentEditorSessionOperationFamily::HostLifecycle
			&& FindEditorSessionOperationFamilyContract(Family) != nullptr;
	}

	const FUeAgentEditorSessionOperationFamilyContract*
	FindEditorSessionOperationFamilyContract(
		const EUeAgentEditorSessionOperationFamily Family)
	{
		return GetEditorSessionOperationFamilyContracts().FindByPredicate(
			[Family](
				const FUeAgentEditorSessionOperationFamilyContract& Contract)
			{
				return Contract.GetFamily() == Family;
			});
	}
}
