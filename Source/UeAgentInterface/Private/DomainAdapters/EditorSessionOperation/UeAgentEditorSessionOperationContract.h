#pragma once

#include "CoreMinimal.h"

#define UAI_EDITOR_SESSION_OPERATION_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	enum class EUeAgentEditorSessionOperationFamily : uint8
	{
		AssetEditor,
		RuntimePreview,
		WorldContext,
		ModelingTool,
		HostLifecycle,
		PcgGeneration,
	};

	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionOperationFamilyContract final
	{
	public:
		FUeAgentEditorSessionOperationFamilyContract(
			EUeAgentEditorSessionOperationFamily InFamily,
			FString InFamilyId,
			FString InSubjectKind,
			FString InPolicyKind,
			FString InSignalTypeId);

		EUeAgentEditorSessionOperationFamily GetFamily() const;
		const FString& GetFamilyId() const;
		const FString& GetSubjectKind() const;
		const FString& GetPolicyKind() const;
		const FString& GetSignalTypeId() const;

	private:
		EUeAgentEditorSessionOperationFamily Family;
		FString FamilyId;
		FString SubjectKind;
		FString PolicyKind;
		FString SignalTypeId;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentEditorSessionOperationFamilyContract>&
	GetEditorSessionOperationFamilyContracts();

	UEAGENTINTERFACE_API const
	FUeAgentEditorSessionOperationFamilyContract*
	FindEditorSessionOperationFamilyContract(
		EUeAgentEditorSessionOperationFamily Family);

	// Host lifecycle is bound by editor.safe_exit, never by editor.session.
	UEAGENTINTERFACE_API bool IsEditorSessionCommandFamily(
		EUeAgentEditorSessionOperationFamily Family);
}
