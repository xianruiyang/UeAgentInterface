#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UPhysicsAsset;
class FProperty;
class UPrimitiveComponent;

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
    /** Native primitive authoring supports changing sphere, box and capsule counts. */
    UEAGENTINTERFACE_API bool AuthorizesSimpleCollisionShapeResize(const FProperty* Property);
	/** Rebuild native asset caches and refresh existing component physics state. */
	UEAGENTINTERFACE_API void RefreshPhysicsAsset(UPhysicsAsset& Asset);
	/** Non-interactive PhAT body removal, including native constraint reconnection. */
	UEAGENTINTERFACE_API bool DeletePhysicsAssetBody(UPhysicsAsset& Asset, int32 BodyIndex, UE::UAI::Contract::FDiagnostic& Error,
		TMap<FName, FName>* ReconnectedNames = nullptr);
	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsBodyBinding
	{
		FString Identity;
		FString BoneIdentity;
		FName BoneName;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsShapeBinding
	{
		FString Identity;
		FString BodyIdentity;
		FName NativeName;
		FString ShapeKind;
		FVector Dimensions = FVector::ZeroVector;
		FTransform LocalTransform = FTransform::Identity;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsConstraintBinding
	{
		FString Identity;
		FName NativeName;
		FString BodyAIdentity;
		FString BodyBIdentity;
		FString NativeFrameAId;
		FString NativeFrameBId;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsAssetBinding
	{
		FString Identity;
		TWeakObjectPtr<UPhysicsAsset> PhysicsAsset;
		TArray<FUeAgentRigSimulationSpatialPhysicsBodyBinding> BodyBindings;
		TArray<FUeAgentRigSimulationSpatialPhysicsShapeBinding> ShapeBindings;
		TArray<FUeAgentRigSimulationSpatialPhysicsConstraintBinding> ConstraintBindings;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsSubjectBinding
	{
		FString Identity;
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FString NativeSimulationFrameId;
		FString NativeSimulationClockId;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsProbeSubject
	{
		FString SubjectRef;
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FString NativeSimulationFrameId;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsProbeVector
	{
		FUeAgentRigSimulationSpatialPhysicsProbeVector()
			: Time(TEXT(""), TEXT("")) {}
		FUeAgentRigSimulationSpatialPhysicsProbeVector(
			FString InSubjectRef,
			UPrimitiveComponent* InComponent,
			UE::UAI::StateSemantic::FUeAgentTemporalPoint InTime,
			FString InNativeFrameId,
			FVector InValue);

		FString SubjectRef;
		TWeakObjectPtr<UPrimitiveComponent> Component;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint Time;
		FString NativeFrameId;
		FVector Value = FVector::ZeroVector;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsProbeSample
	{
		FUeAgentRigSimulationSpatialPhysicsProbeSample()
			: Time(TEXT(""), TEXT("")) {}

		FString SubjectRef;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint Time;
		FString FrameId;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FString EvidenceId;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest
	{
		FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest()
			: SimulationRange(TEXT(""), TEXT(""),
				UE::UAI::StateSemantic::FUeAgentTemporalBound::Open(),
				UE::UAI::StateSemantic::FUeAgentTemporalBound::Open()) {}

		FRigSimulationSpatialSimulationSession Session;
		TArray<FUeAgentRigSimulationSpatialPhysicsProbeSubject> Subjects;
		UE::UAI::StateSemantic::FUeAgentTemporalRange SimulationRange;
		TArray<FUeAgentRigSimulationSpatialPhysicsProbeVector> Impulses;
		TArray<FUeAgentRigSimulationSpatialPhysicsProbeVector> Velocities;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPhysicsProbeSessionResult
	{
		FUeAgentRigSimulationSpatialPhysicsProbeSessionResult() = default;
		FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
			FRigSimulationSpatialSimulationSession InSession,
			bool bInCompleted,
			bool bInRestored,
			TArray<FUeAgentRigSimulationSpatialPhysicsProbeSample> InSamples,
			FString InRestorationEvidenceId,
			UObject* InOwnerObject);

		FRigSimulationSpatialSimulationSession Session;
		bool bCompleted = false;
		bool bRestored = false;
		TArray<FUeAgentRigSimulationSpatialPhysicsProbeSample> Samples;
		FString RestorationEvidenceId;
		TWeakObjectPtr<UObject> OwnerObject;
		Contract::FDiagnostic RestorationDiagnostic;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialPhysicsSessionPort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialPhysicsSessionPort() = default;
		virtual bool TryReadPhysicsProbe(
			const FRigSimulationSpatialSimulationSession& Session,
			const TArray<FString>& SubjectRefs,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
		virtual bool TryRunPhysicsProbe(
			const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialPhysicsNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> PhysicsSessionPort,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}
