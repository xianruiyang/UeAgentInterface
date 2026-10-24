#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UE::UAI::HjsonWorkspace { class FWorkspaceError; }
namespace UE::UAI::AssetPreview { struct FLocalState; }

namespace UE::UAI::Composition::HjsonImport
{
	class FPreparedAuthoringImport;
	struct FSemanticWorkspaceChangeProjection;

	class FPreviewWorkspaceApply
	{
	public:
		bool Prepare(const FPreparedAuthoringImport& Import,
			FSemanticWorkspaceChangeProjection& Projection, HjsonWorkspace::FWorkspaceError& Error);
		bool HasChanges() const { return !Profiles.IsEmpty() || !Locals.IsEmpty(); }
		bool Apply(HjsonWorkspace::FWorkspaceError& Error);
		bool Verify(HjsonWorkspace::FWorkspaceError& Error);
		bool Rollback(HjsonWorkspace::FWorkspaceError& Error);

	private:
		struct FProfileApply
		{
			int32 Index = INDEX_NONE;
			FString Name;
			FString SourceUnit;
			FString Target;
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			const void* PreparedAddress = nullptr;
			TSet<FName> Fields;
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
		};
		TArray<FProfileApply> Profiles;
		struct FLocalApply
		{
			TSharedPtr<AssetPreview::FLocalState> State;
			FString Group;
			FString SourceUnit;
			TSet<FName> Fields;
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			TWeakObjectPtr<UObject> PreparedOwner;
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
		};
		TArray<FLocalApply> Locals;
		TFunction<void(HjsonWorkspace::FWorkspaceError&)> LocateFailure;
		bool PrepareLocal(FLocalApply& Plan, HjsonWorkspace::FWorkspaceError& Error);
		bool ValidateOwner(const FProfileApply& Profile, HjsonWorkspace::FWorkspaceError& Error,
			const FString& Stage, bool bAppend = false) const;
		bool Notify(FProfileApply& Profile, HjsonWorkspace::FWorkspaceError& Error,
			const FString& Stage, bool bAppend = false);
	};
}
