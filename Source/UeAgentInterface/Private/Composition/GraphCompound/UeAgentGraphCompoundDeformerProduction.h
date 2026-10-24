#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UPackage;
class UOptimusDeformer;
namespace UE::UAI::HjsonWorkspace { struct FGraphAuthoringConnectionContext; }

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	class FAnimationRigGraphNativeHook;
}

namespace UE::UAI::Composition::GraphCompound
{
	struct FDeformerSemanticNodeEdit
	{
		FString Identity;
		FName ExistingName;
		UClass* CreateClass = nullptr;
		FVector2D Position = FVector2D::ZeroVector;
		bool bSetPosition = false;
		bool bRemove = false;
		FString InputPointer;
	};

	// Resolved identities belong to this request. Native node names are captured,
	// never decoded from the identity text or supplied as candidate names.
	struct FDeformerSemanticGraphEdit
	{
		TArray<FDeformerSemanticNodeEdit> Nodes;
		struct FConnection
		{
			FString FromNode, FromPin, ToNode, ToPin;
			FString InputPointer;
		};
		TArray<FConnection> Connections;
		TArray<FConnection> RemovedConnections;
		TSharedPtr<const HjsonWorkspace::FGraphAuthoringConnectionContext> ConnectionContext;
		FString TargetPath;
		FString SourceUnit;
		TFunction<void(Contract::FDiagnostic&)> LocateDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryApplyDeformerSemanticGraph(UOptimusDeformer& Asset,
		const FDeformerSemanticGraphEdit& Edit, TMap<FString, FName>& OutNodeBindings,
		FGraphCompoundRuntimeError& OutError);
	UEAGENTINTERFACE_API bool TryApplyDeformerSemanticConnections(UOptimusDeformer& Asset,
		const FDeformerSemanticGraphEdit& Edit, const TMap<FString, FName>& NodeBindings,
		FGraphCompoundRuntimeError& OutError);

	enum class EGraphCompoundDeformerReadbackKind : uint8
	{
		DeformerGraphCreate,
		DeformerGraphTopology,
		MeshDeformerCollectionCreate,
		MeshDeformerCollectionEntries,
	};

	struct FGraphCompoundDeformerEntrySpec final
	{
		FString EntryKind;
		FString EntryRef;
		int32 RequestedOrderIndex = INDEX_NONE;
		EChangeIntent Intent = EChangeIntent::Update;
		FString InputPointer;
	};

	struct FGraphCompoundDeformerLinkSpec final
	{
		FString FromNode;
		FString FromPin;
		FString ToNode;
		FString ToPin;
		EChangeIntent Intent = EChangeIntent::Create;
		FString InputPointer;
	};

	struct FGraphCompoundDeformerReadbackSpec final
	{
		EGraphCompoundDeformerReadbackKind Kind =
			EGraphCompoundDeformerReadbackKind::DeformerGraphCreate;
		FString TargetKind;
		FString NativeClassPath;
		FString SkeletalMeshRef;
		FName CommentNodeName;
		TArray<FGraphCompoundDeformerEntrySpec> Entries;
		TArray<FGraphCompoundDeformerLinkSpec> Links;
		FString SkeletalMeshInputPointer;
		FString CommentNodeInputPointer;
	};

	struct FGraphCompoundDeformerCapturedValue final
	{
		FGraphCompoundDeformerCapturedValue(
			FString InSemanticId,
			TSharedRef<FJsonObject> InValue)
			: SemanticId(MoveTemp(InSemanticId))
			, Value(MoveTemp(InValue))
		{
		}

		FString SemanticId;
		TSharedRef<FJsonObject> Value;
	};

	bool IsDeformerProductionProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsDeformerCreateProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsDeformerMutationProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsUnavailableConcreteDeformerProfile(
		const FPcgAnimationGraphRouteProfile& Profile);

	bool TrySetUnavailableConcreteDeformerError(
		const FPcgAnimationGraphRouteProfile& Profile,
		FGraphCompoundRuntimeError& OutError);

	bool TryParseDeformerReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FGraphCompoundDeformerReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError);

	bool TryCaptureDeformerValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FGraphCompoundDeformerCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError,
		bool bAuthoring = false,
		const FString& DiagnosticTargetPath = FString());

	TSharedRef<FJsonObject> DeformerExpectedJson(
		const FGraphCompoundDeformerReadbackSpec& Spec);

	bool TryBuildDeformerActualJson(
		const FString& TargetRef,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		UE::UAI::Contract::FDiagnostic& OutError);

	void AddDeformerDependencyAnchors(
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors);

	UObject* TryCreateDeformerAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError);

	bool TryApplyDeformerMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		DomainAdapters::AnimationRigGraph::FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError);
}
