#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentAuthoringAssetRelations
{
// Prepared parent/child relationship state consumed by the existing operation
// transaction. Domains validate topology and supply their native parent setter.
class FPlan
{
public:
	FPlan();
	~FPlan();
	bool Prepare(UObject& Root, const TArray<UObject*>& Children, FName ParentProperty, FName ChildrenProperty,
		TFunction<void(UObject&, UObject*)> SetParent, UE::UAI::Contract::FDiagnostic& OutError);
	void ApplyPreview(UObject& Root) const;
	bool Apply(UE::UAI::Contract::FDiagnostic& OutError);
	bool Rollback(UE::UAI::Contract::FDiagnostic& OutError);
	void Release();
	bool Rebind(UE::UAI::Contract::FDiagnostic& OutError);
	void CollectRollbackObjects(TArray<UObject*>& OutObjects) const;
	bool AppendPackagePlans(TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& OutPlans,
		UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const;
	bool AppendReadRequests(bool bExpected, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const;
private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
}
