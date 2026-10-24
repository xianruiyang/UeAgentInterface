#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::DomainAdapters::WorldDestructive
{
	struct FWorldDestructivePackage final
	{
		FString PackageName;
		AssetDurable::EPackageHeaderKind HeaderKind = AssetDurable::EPackageHeaderKind::EvidenceRequired;
		AssetDurable::EAssetDurableIntent Intent = AssetDurable::EAssetDurableIntent::Save;
		/** Map owning an external package; empty for the primary map. */
		FString WorldPackageName;
	};
	struct UEAGENTINTERFACE_API FUeAgentWorldDestructiveRequest final
	{
		FString RequestId;
		FString CommandId;
		FString TargetRef;
		FString OperandRef;
		FString ManagedId;
		FString ComponentName;
		FString Space;
		FString CallerId;
		FVector BoundsMin = FVector::ZeroVector;
		FVector BoundsMax = FVector::ZeroVector;
		int32 Index = INDEX_NONE;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldDestructiveResult final
	{
		FString CommandId;
		FString TargetRef;
		FString ResultRef;
		FString TerminalState = TEXT("rolled_back");
		FString PersistenceScope;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString JournalId;
		FString VerificationId;
		bool bChanged = false;
		bool bPersisted = false;
		bool bReadbackVerified = false;

		bool IsSuccess() const
		{
			return TerminalState == TEXT("committed")
				&& Diagnostic.IsEmpty()
				&& bReadbackVerified;
		}
	};

	/**
	 * Native mutation mechanics supplied by the domain adapter to the
	 * composition-owned durable runner. Persistence, fencing, journals and
	 * independent persisted readback do not belong to this interface.
	 */
	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveDurableMutation
	{
	public:
		virtual ~IUeAgentWorldDestructiveDurableMutation() = default;

		/** Primary Map package required to reconstruct an isolated world. */
		virtual const FString& GetPackageName() const = 0;
		/** Complete native package closure, including removed external actor packages. */
		virtual const TArray<FWorldDestructivePackage>& GetMutationPackages() const = 0;
		virtual const FString& GetPersistenceScope() const = 0;
		virtual bool Apply(UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool Rollback(UE::UAI::Contract::FDiagnostic& OutError) = 0;
		/** Destruction that cannot be undone uses the shared package checkpoint lifecycle. */
		virtual bool RequiresPackageCheckpoint() const { return false; }
		virtual bool VerifyRestored(UWorld& World, const AssetDurable::FPersistedWorldReferenceMap& References,
			UE::UAI::Contract::FDiagnostic& OutError) const { return false; }
		virtual bool VerifyPersisted(
			UWorld& PersistedWorld,
			const AssetDurable::FPersistedWorldReferenceMap& References,
			UE::UAI::Contract::FDiagnostic& OutError) const = 0;
	};

	/**
	 * Composition port that owns the durable lifecycle around one native
	 * WorldDestructive mutation.
	 */
	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveDurableRunner
	{
	public:
		virtual ~IUeAgentWorldDestructiveDurableRunner() = default;

		virtual FUeAgentWorldDestructiveResult Execute(
			UWorld& ExplicitWorld,
			const FUeAgentWorldDestructiveRequest& Request,
			TSharedRef<IUeAgentWorldDestructiveDurableMutation> Mutation,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveOwner
	{
	public:
		virtual ~IUeAgentWorldDestructiveOwner() = default;

		virtual FUeAgentWorldDestructiveResult Execute(
			const FUeAgentWorldDestructiveRequest& Request,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateWorldDestructiveOwner(
		UWorld& ExplicitWorld,
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedRef<IUeAgentWorldDestructiveDurableRunner> DurableRunner,
		TSharedPtr<IUeAgentWorldDestructiveOwner>& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError);
}
