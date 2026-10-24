#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	enum class EUnifiedHjsonCompositionKind : uint8
	{
		Target,
		PropertySet,
		MemberSet,
		Structure,
		Item,
		Endpoint,
		Relation,
		Order,
		Layout,
		TemporalContract,
		SpatialContract,
		ExternalInput,
	};

	enum class EUnifiedHjsonAuthoringIntent : uint8
	{
		FullState,
		MinimalCreate,
		LocalEdit,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EUnifiedHjsonCompositionKind Kind);

	UEAGENTINTERFACE_API FString LexToString(
		EUnifiedHjsonAuthoringIntent Intent);

	UEAGENTINTERFACE_API TArray<EUnifiedHjsonCompositionKind>
		GetAllUnifiedHjsonCompositionKinds();

	class FUnifiedHjsonSemanticModel;
	using FUnifiedHjsonSemanticModelPtr =
		TSharedPtr<const FUnifiedHjsonSemanticModel>;
	using FUnifiedHjsonSemanticModelRef =
		TSharedRef<const FUnifiedHjsonSemanticModel>;

	class UEAGENTINTERFACE_API FUnifiedHjsonSemanticModel final
	{
	public:
		const FAuthoringModelRef& GetCoreModel() const;
		EUnifiedHjsonAuthoringIntent GetAuthoringIntent() const;
		const TMap<EUnifiedHjsonCompositionKind, TArray<FString>>&
			GetCompositions() const;
		const TArray<FString>* FindCompositionUnitIds(
			EUnifiedHjsonCompositionKind Kind) const;
		int32 GetBoundUnitCount() const;
		const FString& GetModelFingerprint() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUnifiedHjsonSemanticModel(
			FAuthoringModelRef InCoreModel,
			EUnifiedHjsonAuthoringIntent InAuthoringIntent,
			TMap<EUnifiedHjsonCompositionKind, TArray<FString>>
				InCompositions,
			int32 InBoundUnitCount,
			FString InModelFingerprint);

		FAuthoringModelRef CoreModel;
		EUnifiedHjsonAuthoringIntent AuthoringIntent =
			EUnifiedHjsonAuthoringIntent::FullState;
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions;
		int32 BoundUnitCount = 0;
		FString ModelFingerprint;
		Contract::FDiagnostic ValidationDiagnostic;

		friend UEAGENTINTERFACE_API bool
			TryCreateUnifiedHjsonSemanticModel(
				FAuthoringModelRef,
				EUnifiedHjsonAuthoringIntent,
				TMap<EUnifiedHjsonCompositionKind, TArray<FString>>,
				FUeAgentResourceTracker&,
				FUnifiedHjsonSemanticModelPtr&,
				FWorkspaceError&);
	};

	UEAGENTINTERFACE_API bool TryCreateUnifiedHjsonSemanticModel(
		FAuthoringModelRef CoreModel,
		EUnifiedHjsonAuthoringIntent AuthoringIntent,
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonSemanticModelPtr& OutModel,
		FWorkspaceError& OutError);
}
