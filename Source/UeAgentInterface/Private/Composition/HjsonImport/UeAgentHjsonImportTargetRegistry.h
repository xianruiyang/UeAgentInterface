#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

#define UAI_HJSON_IMPORT_TARGET_REGISTRY_V1 1
#define UAI_HJSON_IMPORT_SINGLE_RESOLUTION_V1 1
#define UAI_HJSON_IMPORT_UNIFIED_OPERATION_BINDING_V1 1
#define UAI_HJSON_IMPORT_OPERATION_FACTORY_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::Composition::HjsonImport
{
	struct UEAGENTINTERFACE_API FUeAgentHjsonImportTargetError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const { return !Code.IsEmpty(); }
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonTargetExportRequest final
	{
	public:
		FUeAgentHjsonTargetExportRequest(
			FString InCompositionKind,
			FString InTargetAssetPath,
			FString InProfileId,
			FString InWorkspaceRoot);

		const FString& GetCompositionKind() const;
		const FString& GetTargetAssetPath() const;
		const FString& GetProfileId() const;
		const FString& GetWorkspaceRoot() const;
		bool IsValid() const;

	private:
		FString CompositionKind;
		FString TargetAssetPath;
		FString ProfileId;
		FString WorkspaceRoot;
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonTargetImportRequest final
	{
	public:
		FUeAgentHjsonTargetImportRequest(
			FString InCompositionKind,
			FString InTargetAssetPath,
			HjsonWorkspace::FResolvedSemanticWorkspaceRef
				InWorkspace,
			PlanningPreflight::EChangeIntent InIntent,
			FString InExpectedBaseRevision,
			bool bInDryRun);

		const FString& GetCompositionKind() const;
		const FString& GetTargetAssetPath() const;
		const HjsonWorkspace::FResolvedSemanticWorkspacePtr&
			GetWorkspace() const;
		PlanningPreflight::EChangeIntent GetIntent() const;
		const FString& GetExpectedBaseRevision() const;
		bool IsDryRun() const;
		bool IsValid() const;

	private:
		FString CompositionKind;
		FString TargetAssetPath;
		HjsonWorkspace::FResolvedSemanticWorkspacePtr Workspace;
		PlanningPreflight::EChangeIntent Intent =
			PlanningPreflight::EChangeIntent::Update;
		FString ExpectedBaseRevision;
		bool bDryRun = false;
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonTargetExportReceipt final
	{
	public:
		FUeAgentHjsonTargetExportReceipt() = default;
		FUeAgentHjsonTargetExportReceipt(
			FString InCompositionKind,
			FString InTargetAssetPath,
			FString InWorkspaceRoot,
			FString InBaseRevision,
			FString InWorkspaceFingerprint);

		const FString& GetCompositionKind() const;
		const FString& GetTargetAssetPath() const;
		const FString& GetWorkspaceRoot() const;
		const FString& GetBaseRevision() const;
		const FString& GetWorkspaceFingerprint() const;
		bool IsValid() const;

	private:
		FString CompositionKind;
		FString TargetAssetPath;
		FString WorkspaceRoot;
		FString BaseRevision;
		FString WorkspaceFingerprint;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonTargetImportReceipt final
	{
	public:
		FUeAgentHjsonTargetImportReceipt() = default;
		FUeAgentHjsonTargetImportReceipt(
			FString InCompositionKind,
			FString InTargetAssetPath,
			FString InWorkspaceFingerprint,
			FString InPreRevision,
			FString InPersistedRevision,
			bool bInMutationApplied,
			bool bInCompileSettled,
			bool bInPackageSaved,
			bool bInUnloadedReloaded,
			TMap<FString, FString> InPersistedValues);

		const FString& GetWorkspaceFingerprint() const;
		const FString& GetPreRevision() const;
		const FString& GetPersistedRevision() const;
		bool WasMutationApplied() const;
		bool WasCompileSettled() const;
		bool WasPackageSaved() const;
		bool WasUnloadedReloaded() const;
		FString GetPersistedValue(const FString& FieldId) const;
		bool IsValid() const;

	private:
		FString CompositionKind;
		FString TargetAssetPath;
		FString WorkspaceFingerprint;
		FString PreRevision;
		FString PersistedRevision;
		bool bMutationApplied = false;
		bool bCompileSettled = false;
		bool bPackageSaved = false;
		bool bUnloadedReloaded = false;
		TMap<FString, FString> PersistedValues;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentHjsonImportOperation
		: public BindingGateway::Operation::IUeAgentOperationRequestPreparer
		, public BindingGateway::Operation::IUeAgentOperationPlanner
		, public BindingGateway::Operation::IUeAgentOperationExecutor
		, public BindingGateway::Operation::IUeAgentOperationVerifier
	{
	public:
		virtual ~IUeAgentHjsonImportOperation() = default;

		virtual const FUeAgentHjsonTargetImportReceipt&
			GetImportReceipt() const = 0;
		virtual bool UsedOneOperationContext() const = 0;
	};

	using FUeAgentHjsonImportOperationFactory = bool (*)(
		FUeAgentHjsonTargetImportRequest,
		TSharedPtr<IUeAgentHjsonImportOperation>&,
		FUeAgentHjsonImportTargetError&);

	using FUeAgentHjsonTargetExportFactory = bool (*)(
		const FUeAgentHjsonTargetExportRequest&,
		FUeAgentHjsonTargetExportReceipt&,
		FUeAgentHjsonImportTargetError&);

	struct UEAGENTINTERFACE_API FUeAgentHjsonImportTargetBinding
	{
		FString CompositionKind;
		Contract::FUeAgentDescriptorIdentity ImportDescriptorIdentity{
			FString(), 0, FString()};
		FString TypedOperationId;
		FString ProviderBinding;
		FString CompileSettlePolicy;
		FString DurableKind;
		FString OracleKind;
		TSet<FString> AllowedCapabilities;
		FUeAgentHjsonImportOperationFactory OperationFactory = nullptr;
		FUeAgentHjsonTargetExportFactory ExportFactory = nullptr;

		bool IsValid() const;
		bool HasOperationFactory() const;
		bool HasExportFactory() const;
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonImportTargetRegistry final
	{
	public:
		const TArray<FUeAgentHjsonImportTargetBinding>& GetBindings() const;
		const FUeAgentHjsonImportTargetBinding* FindExact(
			const FString& CompositionKind) const;
		bool TryExportExact(
			const FUeAgentHjsonTargetExportRequest& Request,
			FUeAgentHjsonTargetExportReceipt& OutReceipt,
			FUeAgentHjsonImportTargetError& OutError) const;
		bool TryCreateOperationExact(
			FUeAgentHjsonTargetImportRequest Request,
			TSharedPtr<IUeAgentHjsonImportOperation>& OutOperation,
			FUeAgentHjsonImportTargetError& OutError) const;
	private:
		FUeAgentHjsonImportTargetRegistry();
		TArray<FUeAgentHjsonImportTargetBinding> Bindings;

		friend UEAGENTINTERFACE_API const FUeAgentHjsonImportTargetRegistry&
		GetHjsonImportTargetRegistry();
	};

	UEAGENTINTERFACE_API const FUeAgentHjsonImportTargetRegistry&
	GetHjsonImportTargetRegistry();

	UEAGENTINTERFACE_API Contract::FUeAgentDescriptorIdentity
	GetHjsonImportDescriptorIdentity(const FString& CompositionKind);

	UEAGENTINTERFACE_API bool TryCreateHjsonImportDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);
}
