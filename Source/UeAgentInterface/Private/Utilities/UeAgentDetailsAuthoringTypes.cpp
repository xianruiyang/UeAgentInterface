#include "Utilities/UeAgentDetailsAuthoring.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

namespace
{
	FString NormalizeDetailsIndexFile(const FString& IndexFile)
	{
		FString Normalized = IndexFile;
		Normalized.TrimStartAndEndInline();
		if (Normalized.IsEmpty())
		{
			Normalized = TEXT("details/index.hjson");
		}
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.StartsWith(TEXT("/")))
		{
			Normalized.RightChopInline(1, EAllowShrinking::No);
		}
		return Normalized;
	}
}

namespace UeAgentDetailsAuthoring
{
	FOwnerDetailsExportRequest::FOwnerDetailsExportRequest()
		: Object(nullptr)
		, IndexFile(TEXT("details/index.hjson"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsApplyRequest::FOwnerDetailsApplyRequest()
		: Object(nullptr)
		, IndexFile(TEXT("details/index.hjson"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsValidateRequest::FOwnerDetailsValidateRequest()
		: IndexFile(TEXT("details/index.hjson"))
		, RefFieldName(TEXT("details"))
	{
	}

	FOwnerDetailsDiffRequest::FOwnerDetailsDiffRequest()
		: OldIndexFile(TEXT("details/index.hjson"))
		, NewIndexFile(TEXT("details/index.hjson"))
		, RefFieldName(TEXT("details"))
	{
	}

	FDiagnosticsIsolationRequest::FDiagnosticsIsolationRequest()
		: AllowedTarget(TEXT("details/index.hjson"))
	{
	}

	TSharedPtr<FJsonObject> BuildOwnerRef(const FOwnerInfo& OwnerInfo)
	{
		TSharedPtr<FJsonObject> OwnerRef = MakeShared<FJsonObject>();
		TMap<FString, FString> Selectors;
		Selectors.Add(TEXT("owner_id"), OwnerInfo.OwnerId);
		Selectors.Add(TEXT("owner_kind"), OwnerInfo.OwnerKind);
		Selectors.Add(TEXT("name"), OwnerInfo.OwnerName);
		Selectors.Add(TEXT("object_path"), OwnerInfo.OwnerPath);
		Selectors.Add(TEXT("class_path"), OwnerInfo.OwnerClassPath);
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		UE::UAI::Contract::FDiagnostic IdentityError;
		if (UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			TEXT("owner_identity"), TEXT("owner"), Selectors, Identity, IdentityError))
		{
			UeAgentAuthoringIdentity::WriteSelectorsFlat(Identity, OwnerRef.ToSharedRef());
		}
		for (const TPair<FString, FString>& Pair : Selectors)
		{
			if (!OwnerRef->HasField(Pair.Key)) OwnerRef->SetStringField(Pair.Key, Pair.Value);
		}
		if (!OwnerInfo.SourceCommand.IsEmpty())
		{
			OwnerRef->SetStringField(TEXT("source_command"), OwnerInfo.SourceCommand);
		}
		return OwnerRef;
	}

	FOwnerInfo MakeOwnerInfoForObject(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		const FString& OwnerName)
	{
		FOwnerInfo OwnerInfo;
		OwnerInfo.OwnerKind = OwnerKind;
		OwnerInfo.OwnerId = OwnerId;
		OwnerInfo.OwnerName = OwnerName;
		OwnerInfo.SourceCommand = SourceCommand;
		if (Object)
		{
			OwnerInfo.OwnerName = OwnerInfo.OwnerName.IsEmpty() ? Object->GetName() : OwnerInfo.OwnerName;
			OwnerInfo.OwnerPath = Object->GetPathName();
			OwnerInfo.OwnerClassPath = Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
		}
		return OwnerInfo;
	}

	FOwnerDetailsExportRequest MakeOwnerDetailsExportRequest(
		UObject* Object,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& DetailsRoot,
		const FString& SourceCommand,
		const FString& IndexFile,
		const FString& RefFieldName)
	{
		FOwnerDetailsExportRequest Request;
		Request.Object = Object;
		Request.OwnerInfo = MakeOwnerInfoForObject(Object, OwnerKind, OwnerId, SourceCommand);
		Request.DetailsRoot = DetailsRoot;
		Request.IndexFile = NormalizeDetailsIndexFile(IndexFile);
		Request.RefFieldName = RefFieldName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("details")) : RefFieldName.TrimStartAndEnd();
		return Request;
	}
}
