#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	struct UEAGENTINTERFACE_API FUnifiedHjsonWorkspaceIndex
	{
		TArray<FString> UnitIds;
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>>
			UnitsByComposition;
		TMap<FString, FString> OwnerByUnitId;
		TMap<FString, FString> FieldOwnerByFieldSetId;
		TMap<FString, TArray<FString>> DirectRelationsByUnitId;
	};

	class FUnifiedHjsonWorkspace;
	using FUnifiedHjsonWorkspacePtr =
		TSharedPtr<const FUnifiedHjsonWorkspace>;
	using FUnifiedHjsonWorkspaceRef =
		TSharedRef<const FUnifiedHjsonWorkspace>;

	class UEAGENTINTERFACE_API FUnifiedHjsonWorkspace final
	{
	public:
		const FUnifiedHjsonSemanticModelRef& GetModel() const;
		const TSharedRef<const FWorkspaceLayoutPlan>& GetLayout() const;
		EWorkspaceSourceMode GetSourceMode() const;
		const FString& GetSemanticFingerprint() const;
		const FUnifiedHjsonWorkspaceIndex& GetIndex() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUnifiedHjsonWorkspace(
			FUnifiedHjsonSemanticModelRef InModel,
			TSharedRef<const FWorkspaceLayoutPlan> InLayout,
			FUnifiedHjsonWorkspaceIndex InIndex);

		FUnifiedHjsonSemanticModelRef Model;
		TSharedRef<const FWorkspaceLayoutPlan> Layout;
		FString SemanticFingerprint;
		FUnifiedHjsonWorkspaceIndex Index;
		Contract::FDiagnostic ValidationDiagnostic;

		friend UEAGENTINTERFACE_API bool TryCreateUnifiedHjsonWorkspace(
			FUnifiedHjsonSemanticModelRef,
			TSharedRef<const FWorkspaceLayoutPlan>,
			FUeAgentResourceTracker&,
			FUnifiedHjsonWorkspacePtr&,
			FWorkspaceError&);
	};

	UEAGENTINTERFACE_API bool TryCreateUnifiedHjsonWorkspace(
		FUnifiedHjsonSemanticModelRef Model,
		TSharedRef<const FWorkspaceLayoutPlan> Layout,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);
}
