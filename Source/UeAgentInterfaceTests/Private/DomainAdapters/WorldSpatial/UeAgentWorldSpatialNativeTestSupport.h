#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	class FUeAgentWorldSpatialProductionRuntime;
}

namespace UE::UAI::Tests::WorldSpatial
{
	struct FOwnedScenarioContext
	{
		FString WorldIdentity;
		FString AssetEditorIdentity;
		FString ViewportIdentity;
		TArray<FString> SelectionIdentities;
	};

	bool RequireNativeExecution(
		FAutomationTestBase& Test,
		const FString& ScenarioId);

	class FProductionScenarioBindings
	{
	public:
		TMap<FString, FString> ExplicitRefs;
		FString ViewportIdentity;
	};

	using FPrepareCapabilityInput = TFunction<bool(
		FAutomationTestBase&,
		const FString&,
		const TSharedPtr<FJsonObject>&)>;

	class FOwnedNativeEnvironmentBase
		: public UE::UAI::DomainAdapters::WorldSpatial::
			IUeAgentWorldSpatialNativeEnvironment,
		  public UE::UAI::DomainAdapters::WorldSpatial::
			IUeAgentWorldSpatialSpaceCompiler,
		  public UE::UAI::DomainAdapters::WorldSpatial::
			IUeAgentWorldSpatialRevisionSource
	{
	public:
		bool TryResolveObject(
			const FString& ExplicitRef,
			UObject*& OutObject,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutObject = nullptr;
			OutError.Code = TEXT("owned_fixture_object_unresolved");
			OutError.SubjectId = ExplicitRef;
			return false;
		}

		bool TryResolveGraphEditor(
			const FString& AssetEditorIdentity,
			TSharedPtr<SGraphEditor>& OutGraphEditor,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutGraphEditor.Reset();
			OutError.Code = TEXT("owned_fixture_graph_editor_unresolved");
			OutError.SubjectId = AssetEditorIdentity;
			return false;
		}

		bool TryResolveViewport(
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutClient = nullptr;
			OutViewport = nullptr;
			OutError.Code = TEXT("owned_fixture_viewport_unresolved");
			OutError.SubjectId = ViewportIdentity;
			return false;
		}

		bool TryGetExplicitObjectRef(
			const UObject* Object,
			FString& OutExplicitRef,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutExplicitRef.Reset();
			OutError.Code = TEXT("owned_fixture_reverse_object_unresolved");
			OutError.SubjectId = Object ? TEXT("unmapped_object") : TEXT("null");
			return false;
		}

		bool TryPersistObject(
			UObject& Object,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutError = {};
			return IsValid(&Object);
		}

		bool TryResolveMeshSelectionSession(
			const FString& ViewportIdentity,
			const TArray<FString>& SelectionIdentities,
			UGeometrySelectionManager*& OutManager,
			TArray<UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialMeshSelectionTarget>& OutTargets,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutManager = nullptr;
			OutTargets.Reset();
			OutError.Code = TEXT("owned_fixture_mesh_selection_unresolved");
			OutError.SubjectId = ViewportIdentity;
			return false;
		}

		void GetExplicitViewportIdentities(
			TArray<FString>& OutViewportIdentities) const override
		{
			OutViewportIdentities.Reset();
		}

		bool TryConvertToNative(
			const UE::UAI::DomainAdapters::WorldSpatial::FWorldSpatialValue& Value,
			const FString& NativeFrameId,
			const FString& NativeUnit,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialCompiledValue& OutValue,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override;

		bool TryConvertFromNative(
			const UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialCompiledValue& Value,
			const UE::UAI::DomainAdapters::WorldSpatial::FWorldSpatialFrame&
				SemanticFrame,
			UE::UAI::DomainAdapters::WorldSpatial::FWorldSpatialValue& OutValue,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override;

		bool TryReadRevision(
			const FString& ExplicitRef,
			UE::UAI::DomainAdapters::WorldSpatial::FWorldSpatialRevision&
				OutRevision,
			UE::UAI::DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutRevision = {};
			OutError.Code = TEXT("owned_fixture_revision_unresolved");
			OutError.SubjectId = ExplicitRef;
			return false;
		}
	};

	using FAfterCapability =
		TFunction<void(FAutomationTestBase&, const FString&)>;
	using FAfterQueryReadback = TFunction<void(
		FAutomationTestBase&,
		const FString&,
		const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot&)>;
	using FBeforeCapability =
		TFunction<void(const FString&)>;

	bool RunOwnedScenarioThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TSharedRef<FOwnedNativeEnvironmentBase>& Environment,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FAfterCapability& AfterCapability = {},
		const FBeforeCapability& BeforeCapability = {},
		const TMap<FString, FString>& ExpectedOperationFailures = {},
		const FAfterQueryReadback& AfterQueryReadback = {});

	bool TryCreateProductionRuntimeForTest(
		FAutomationTestBase& Test,
		UWorld& ExplicitWorld,
		TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime>& OutRuntime);
	bool TryCreateProductionRuntimeForTest(
		FAutomationTestBase& Test,
		UWorld& ExplicitWorld,
		const FString& ViewportIdentity,
		FEditorViewportClient& ViewportClient,
		FViewport& Viewport,
		UGeometrySelectionManager* SelectionManager,
		const TArray<UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialMeshSelectionTarget>& SelectionTargets,
		TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime>& OutRuntime);

	bool PrepareProductionCapabilityInput(
		FAutomationTestBase& Test,
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& RawInput,
		const UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime& Runtime,
		const FProductionScenarioBindings& Bindings);

	bool RunProductionScenarioThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TSharedRef<UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime>& Runtime,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FPrepareCapabilityInput& PrepareCapabilityInput,
		const FAfterCapability& AfterCapability = {},
		const FBeforeCapability& BeforeCapability = {},
		const TMap<FString, FString>& ExpectedOperationFailures = {},
		const FAfterQueryReadback& AfterQueryReadback = {});

	bool RunProductionCapabilitiesThroughC0C3(
		FAutomationTestBase& Test,
		const FString& ScenarioId,
		const TArray<FString>& Capabilities,
		const TSharedPtr<FJsonObject>& TypedInputs,
		const TSharedRef<UE::UAI::DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime>& Runtime,
		const FOwnedScenarioContext& ScenarioContext,
		const TMap<FString, FString>& QueryTargetRefs,
		const FPrepareCapabilityInput& PrepareCapabilityInput,
		const FAfterCapability& AfterCapability = {},
		const FBeforeCapability& BeforeCapability = {},
		const TMap<FString, FString>& ExpectedOperationFailures = {},
		const FAfterQueryReadback& AfterQueryReadback = {});
}
