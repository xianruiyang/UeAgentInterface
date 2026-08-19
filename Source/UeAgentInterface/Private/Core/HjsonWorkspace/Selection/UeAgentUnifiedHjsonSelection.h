#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "Core/HjsonWorkspace/UeAgentEditBundleSelection.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	class FUnifiedHjsonSelectionPackage;
	using FUnifiedHjsonSelectionPackagePtr =
		TSharedPtr<const FUnifiedHjsonSelectionPackage>;
	using FUnifiedHjsonSelectionPackageRef =
		TSharedRef<const FUnifiedHjsonSelectionPackage>;

	class UEAGENTINTERFACE_API FUnifiedHjsonSelectionPackage final
	{
	public:
		const FUnifiedHjsonSemanticModelRef& GetModel() const;
		const FSelectionClosure& GetClosure() const;
		const FString& GetModelFingerprint() const;
		bool IsValid() const;

	private:
		FUnifiedHjsonSelectionPackage(
			FUnifiedHjsonSemanticModelRef InModel,
			FSelectionClosure InClosure);

		FUnifiedHjsonSemanticModelRef Model;
		FSelectionClosure Closure;
		FString ModelFingerprint;
		bool bValid = false;

		friend UEAGENTINTERFACE_API bool TryResolveUnifiedHjsonSelection(
			FUnifiedHjsonSemanticModelRef,
			const FWorkspaceSelection&,
			const FSelectionPolicy&,
			FUeAgentResourceTracker&,
			FUnifiedHjsonSelectionPackagePtr&,
			FWorkspaceError&);
	};

	UEAGENTINTERFACE_API bool TryResolveUnifiedHjsonSelection(
		FUnifiedHjsonSemanticModelRef Model,
		const FWorkspaceSelection& Selection,
		const FSelectionPolicy& Policy,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonSelectionPackagePtr& OutPackage,
		FWorkspaceError& OutError);
}
