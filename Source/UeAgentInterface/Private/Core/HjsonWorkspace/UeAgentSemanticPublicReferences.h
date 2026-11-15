#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

namespace UE::UAI::HjsonWorkspace
{
	class FAuthoringModel;
	class FSemanticUnit;
	class FWorkspaceError;

	/** Borrowed, already accepted bindings for this export. Implementations keep
	 * the original unit identity beside its name; a name is never an identity
	 * selector. The projection consumes this view synchronously and retains no
	 * source workspace or additional per-unit hint cache. */
	class FSemanticProjectionBindings
	{
	public:
		virtual ~FSemanticProjectionBindings() = default;
		virtual const FAuthoringModel& GetModel() const = 0;
		virtual bool Visit(TFunctionRef<bool(const FSemanticUnit&, const FString&, const FString&, const FString&)> Visitor) const = 0;
		/** Namespace exclusions outside a formatter slice need names, not values
		 * or declarations. Complete-model bindings already carry their members. */
		virtual bool IsReservedHandle(const FString& Handle, bool& OutReserved, FWorkspaceError& Error) const { OutReserved = false; return true; }
		/** Published handles read for references in actual output values. These
		 * are not formatter members and require no alias or value projection. */
		virtual bool VisitReferencedPublicReferences(TFunctionRef<bool(const FString&, const FString&)> Visitor, FWorkspaceError& Error) const { return true; }
	};

	/** Actual allocator output, reused by the next layout membership consumer. */
	struct FSemanticReferenceNamespace
	{
		TMap<FString, TSharedPtr<const TSet<FString>>> AliasesByScope;
		TSharedPtr<const TSet<FString>> Handles;
		TMap<FString, FString> AliasExceptions;
		// A cold accepted directory has exact public handles/paths. The
		// original alias grammar is read only for a scope a new name consumes.
		bool bAliasesFromPublishedRows = false;
		// Actual allocator input/output delta. The weak predecessor retains no
		// old namespace generation; consumers use this only beside that owner.
		TWeakPtr<const FSemanticReferenceNamespace> Previous;
		TArray<TSharedRef<const FString, ESPMode::ThreadSafe>> ChangedNames;
	};

	struct FSemanticReferenceProjection
	{
		TMap<FString, FString> ReadableAliases;
		TMap<FString, FString> Handles;
		TMap<FString, FString> PublicRefs;
		// Current immutable model positions, sealed by the accepted identity
		// comparison above; never old-workspace ordinals or retained hints.
		TBitArray<> AcceptedIdentityUnits;
		TSharedPtr<const FSemanticReferenceNamespace> Namespace;
		int32 ReusedNames = 0;
		int32 AllocatedNames = 0;
		// Layout can borrow accepted names for path lookups. The standalone
		// public projection still produces its complete legacy maps.
		bool bBorrowedAcceptedNames = false;
	};

	// Shared by workspace layout and read-only consumers of model-visible references.
	UEAGENTINTERFACE_API bool TryBuildSemanticReferenceProjection(
		const FAuthoringModel& Model, FSemanticReferenceProjection& OutProjection,
		FWorkspaceError& OutError, const FSemanticProjectionBindings* Bindings = nullptr);
}
