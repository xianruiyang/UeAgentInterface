#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProvider.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	struct UEAGENTINTERFACE_API FUeAgentContentGraphSemanticIdentity
	{
		FString SemanticId;
		TMap<FString, FString> Selectors;
		FString ObjectRole;

		bool IsExplicit() const
		{
			return !SemanticId.IsEmpty()
				&& !Selectors.IsEmpty()
				&& !ObjectRole.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphContainer
	{
		FString ContainerId;
		FString ContainerKind;
		FString OwnerId;
		FString ParentContainerId;
		int32 Order = 0;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr State;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphItem
	{
		FString ItemId;
		FString ItemKind;
		FString OwnerId;
		FString ClassPath;
		FString ContainerId;
		int32 Order = 0;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr State;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphEndpoint
	{
		FString Identity;
		FString PublicName;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphRelation
	{
		FString RelationId;
		FString RelationKind;
		FString SourceItemId;
		FString TargetItemId;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr State;
		// Optional precise endpoints; item identities continue to own graph closure.
		FUeAgentContentGraphEndpoint SourceEndpoint;
		FUeAgentContentGraphEndpoint TargetEndpoint;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphFieldState
	{
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr AuthoringDescriptor;
		TOptional<UE::UAI::StateSemantic::FUeAgentStateDetails> AuthoringDetails;
		TOptional<
			UE::UAI::StateSemantic::FUeAgentCanonicalType>
			Type;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Value;
		TOptional<
			UE::UAI::StateSemantic::FUeAgentDefaultValue>
			Default;
		UE::UAI::StateSemantic::EUeAgentAuthoredPresence
			AuthoredPresence =
				UE::UAI::StateSemantic::
					EUeAgentAuthoredPresence::Absent;
		UE::UAI::StateSemantic::EUeAgentOverrideState OverrideState =
			UE::UAI::StateSemantic::EUeAgentOverrideState::None;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphProperty
	{
		FString OwnerId;
		FString FieldId;
		FUeAgentContentGraphFieldState State;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphParameter
	{
		FString ParameterId;
		FString OwnerId;
		FUeAgentContentGraphFieldState State;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphDiagnostic
	{
		FString DiagnosticId;
		FString OwnerId;
		FString Severity;
		FString Code;
		FString Message;
		bool bResolved = false;
		FUeAgentContentGraphSemanticIdentity SemanticIdentity;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr State;
	};

	struct UEAGENTINTERFACE_API FUeAgentContentGraphCapture
	{
		UObject* Owner = nullptr;
		FString DomainId;
		FString Revision;
		TOptional<UE::UAI::StateSemantic::FUeAgentStateRevision>
			CanonicalRevision;
		TArray<FUeAgentContentGraphContainer> Containers;
		TArray<FUeAgentContentGraphItem> Items;
		TArray<FUeAgentContentGraphRelation> Relations;
		TArray<FUeAgentContentGraphProperty> Properties;
		TArray<FUeAgentContentGraphParameter> Parameters;
		TArray<FUeAgentContentGraphDiagnostic> Diagnostics;
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr DiagnosticCoverage;
	};

	class UEAGENTINTERFACE_API
		IUeAgentContentGraphReadEnvironment
	{
	public:
		virtual ~IUeAgentContentGraphReadEnvironment() = default;

		virtual bool TryCapture(
			const UE::UAI::Discovery::
				FUeAgentCandidateBinding& Target,
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError) = 0;

		// Requested separately so ordinary authoring reads do not run native diagnostics.
		virtual bool TryCaptureDiagnostics(
			FUeAgentContentGraphCapture& Capture,
			FUeAgentContentGraphReadError& OutError)
		{
			return true;
		}
	};

	class UEAGENTINTERFACE_API
		FUeAgentContentGraphReadNativeHookFactory final
	{
	public:
		static bool TryCreate(
			const FString& ProviderBinding,
			TSharedRef<IUeAgentContentGraphReadEnvironment>
				Environment,
			TSharedPtr<IUeAgentContentGraphReadHook>& OutHook,
			FUeAgentContentGraphReadError& OutError);
	};

	UEAGENTINTERFACE_API bool TryBuildContentGraphCompoundSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>&
			OutSnapshot,
		FUeAgentContentGraphReadError& OutError);

	UEAGENTINTERFACE_API bool TryBuildContentGraphAuthoringSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>&
			OutSnapshot,
		FUeAgentContentGraphReadError& OutError);

	UEAGENTINTERFACE_API bool TryBuildContentGraphAuthoringSnapshot(
		const FUeAgentContentGraphCapture& Capture,
		const UE::UAI::ContextResource::FUeAgentResourceTrackerOwner& Resources,
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>&
			OutSnapshot,
		FUeAgentContentGraphReadError& OutError);
}
