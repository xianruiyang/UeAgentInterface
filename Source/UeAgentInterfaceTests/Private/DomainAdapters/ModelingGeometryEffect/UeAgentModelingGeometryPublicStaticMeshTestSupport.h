#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FAutomationTestBase;
class UStaticMesh;
class UWorld;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace UE::UAI::CommandEdge
{
	class FUeAgentCommandResult;
	class FUeAgentExactCommandDispatch;
}

namespace UE::UAI::CommandRegistry
{
	class FUeAgentImmutableCommandRegistry;
}

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh
{
	using FBuildParams = TFunction<TSharedRef<FJsonObject>(
		const FString&,
		const TSharedRef<FJsonObject>&)>;
	using FBuildOperandParams = TFunction<TSharedRef<FJsonObject>(
		const FString&,
		const TSharedRef<FJsonObject>&,
		const FString&,
		const TSharedRef<FJsonObject>&)>;
	using FCarrierStep = TFunction<bool(UStaticMesh&)>;
	using FPublicProductBody = TFunction<bool(
		const TSharedPtr<const
			UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>&,
		const TSharedPtr<const
			UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>&,
		UWorld&)>;

	class FScopedPublicStaticMesh final
	{
	public:
		explicit FScopedPublicStaticMesh(const FString& Scope);
		~FScopedPublicStaticMesh();

		UStaticMesh* Create();
		bool Save(UStaticMesh& StaticMesh) const;
		UStaticMesh* ReloadFromDisk(bool& bOutOriginalCollected);
		UStaticMesh* FindLoaded() const;
		FString FileHash() const;
		void Cleanup();

		FString AssetName;
		FString PackageName;
		FString ObjectPath;
		FString HeaderPath;
	};

	TSharedPtr<FJsonObject> ReadRevisionJson(
		const FString& TargetRef,
		UObject& Target);

	FString ReadCommandErrorCode(
		const UE::UAI::CommandEdge::FUeAgentCommandResult& Result);

	TSharedRef<FJsonObject> BuildTargetLocalSpaceContract();
	TSharedRef<FJsonObject> BuildTargetLocalPlane(
		const FVector3d& Origin,
		const FVector3d& Normal);
	bool ReplaceStaticMeshGeometry(
		UStaticMesh& StaticMesh,
		const UE::Geometry::FDynamicMesh3& Mesh);

	bool IsVerifiedIsolatedPackageOutcome(const FString& PayloadJson);

	bool RunCurrentStaticMeshProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const
			UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const
			UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FCarrierStep& PrepareCarrier,
		const FBuildParams& BuildParams,
		const FCarrierStep& VerifyBefore,
		const FCarrierStep& VerifyAfter);

	bool RunCurrentStaticMeshRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const
			UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const
			UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FString& ExpectedErrorCode,
		const FCarrierStep& PrepareCarrier,
		const FBuildParams& BuildParams,
		const FCarrierStep& VerifyBefore,
		const FCarrierStep& VerifyUnchanged);

	bool RunCurrentStaticMeshOperandProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const
			UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const
			UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FCarrierStep& PrepareTarget,
		const FCarrierStep& PrepareOperand,
		const FBuildOperandParams& BuildParams,
		const FCarrierStep& VerifyTargetBefore,
		const FCarrierStep& VerifyOperandBefore,
		const FCarrierStep& VerifyTargetAfter,
		const FCarrierStep& VerifyOperandAfter);

	bool RunCurrentStaticMeshOperandRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const
			UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const
			UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FString& ExpectedErrorCode,
		const FCarrierStep& PrepareTarget,
		const FCarrierStep& PrepareOperand,
		const FBuildOperandParams& BuildParams,
		const FCarrierStep& VerifyTargetBefore,
		const FCarrierStep& VerifyOperandBefore,
		const FCarrierStep& VerifyTargetUnchanged,
		const FCarrierStep& VerifyOperandUnchanged);

	bool RunWithPublicStaticMeshEnvironment(
		FAutomationTestBase& Test,
		const FPublicProductBody& Body);
}
