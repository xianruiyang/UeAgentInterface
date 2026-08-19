#include "Misc/AutomationTest.h"

#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"


#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::ReferenceCrud;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericRead;
	using namespace UE::UAI::DomainAdapters::ReferenceAsset;
	using namespace UE::UAI::StateSemantic;

	FUeAgentDescriptorIdentity OperationDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.operation.reference.crud"),
			1,
			TEXT(
				"sha256:fbde900ec387e61915a21e97ae5575a85337b3866d4299be961f75a5fc888491"));
	}

	TSharedRef<const FUeAgentDescriptorRegistry>
		OperationDescriptorRegistry()
	{
		static const TSharedRef<const FUeAgentDescriptorRegistry>
			Registry = []()
			{
				TSharedPtr<const FUeAgentDescriptorRegistry>
					Result;
				FString Error;
				const bool bCreated =
					FUeAgentDescriptorRegistry::TryCreate(
						{
							FUeAgentOperationDescriptor(
								OperationDescriptor(),
								TEXT(
									"reference.asset_apply"),
								TEXT("reference"),
								EUeAgentDescriptorOperationKind::
									Operation,
								FUeAgentExecutionContextPolicy(
									{
										EUeAgentStatePlane::
											Persisted,
									},
									{},
									EUeAgentContextResolutionPolicy::
										ExplicitOnly,
									EUeAgentTransientContextPolicy::
										ReadOnly),
								FUeAgentContractValue(
									TEXT(
										"{\"type\":\"object\"}")),
								FUeAgentContractValue(
									TEXT(
										"{\"type\":\"object\"}"))),
						},
						Result,
						Error);
				checkf(
					bCreated && Result.IsValid(),
					TEXT(
						"Reference fixture descriptor "
						"registry invalid: %s"),
					*Error);
				return Result.ToSharedRef();
			}();
		return Registry;
	}

	FUeAgentReferenceCrudApplicationConfig Config()
	{
		FUeAgentReferenceCrudApplicationConfig Result;
		Result.OperationDescriptor = OperationDescriptor();
		Result.OperationDescriptorRegistry =
			OperationDescriptorRegistry();
		Result.ExecutionContext.ProjectInstanceId =
			TEXT("p4-013-project");
		Result.ExecutionContext.EditorInstanceId =
			TEXT("p4-013-editor");
		Result.ExecutionContext.StatePlane =
			EUeAgentStatePlane::Persisted;
		Result.ExecutionContext.ContextRevision =
			TEXT("p4-013-context-revision");
		Result.ExecutionContext.bResolvedFromCurrent = false;
		Result.ServerSessionId = TEXT("p4-013-application-session");
		Result.RegistryEpoch = 1;
		Result.TimeoutSeconds = 30.0;
		Result.MaxFrameBytes = 1024 * 1024;
		Result.JournalRoot =
			FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/P4_013/ReferenceCrud");
		return Result;
	}

	FUeAgentOperationSessionIdentity Session()
	{
		const FUeAgentReferenceCrudApplicationConfig Value = Config();
		return FUeAgentOperationSessionIdentity(
			Value.ServerSessionId,
			Value.RegistryEpoch,
			Value.OperationDescriptorRegistry->GetRegistryHash());
	}

	FUeAgentCallerContext Caller()
	{
		return FUeAgentCallerContext(
			TEXT("p4-013-owned-worker"),
			{TEXT("uai:query"), TEXT("uai:reference:write")},
			TEXT("p4-013-owned-worker-correlation"));
	}

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		MakeTracker()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16777216,
				},
			}));
	}

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	FString JsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<
			TJsonWriter<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString PackageFilename(const FString& ObjectPath)
	{
		FString PackageName;
		FString NormalizedObjectPath;
		FString ErrorCode;
		if (!TryNormalizeReferenceAssetPath(
			ObjectPath,
			PackageName,
			NormalizedObjectPath,
			ErrorCode))
		{
			return FString();
		}
		FString Filename;
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			Filename,
			FPackageName::GetAssetPackageExtension());
		return FPaths::ConvertRelativePathToFull(Filename);
	}

	void CleanupOwnedFixture(const FString& ObjectPath)
	{
		const FString Filename = PackageFilename(ObjectPath);
		if (Filename.IsEmpty())
		{
			return;
		}
		IFileManager::Get().Delete(*Filename, false, true, true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("uexp")),
			false,
			true,
			true);
		IFileManager::Get().Delete(
			*FPaths::ChangeExtension(Filename, TEXT("ubulk")),
			false,
			true,
			true);
	}

	bool TryPrepareTarget(
		FUeAgentReferenceAssetCatalogSource& Source,
		IUeAgentStateReadPort& ReadPort,
		const FUeAgentOperationTicketService& TicketService,
		const FString& ObjectPath,
		TOptional<FUeAgentCandidateBinding>& OutBinding,
		TOptional<FUeAgentOperationTicket>& OutTicket,
		FString& OutError)
	{
		OutBinding.Reset();
		OutTicket.Reset();
		OutError.Reset();
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources = MakeTracker();
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
		FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentDiscoveryError DiscoveryError;
		if (!Source.TrySearch(
				FUeAgentDiscoveryQuery(
					GetReferenceAssetCatalogKind(),
					ObjectPath,
					EUeAgentDiscoveryMatchMode::Exact,
					EUeAgentMaterializationPolicy::IndexOnly),
				*Resources,
				Deadline,
				Cancellation.GetToken(),
				Catalog,
				DiscoveryError)
			|| !Catalog.IsValid()
			|| Catalog->GetCandidates().Num() != 1)
		{
			OutError = DiscoveryError.Code + TEXT(":")
				+ DiscoveryError.Detail;
			return false;
		}
		TSharedPtr<const FUeAgentCandidateBinding> Binding;
		if (!Source.TryResolveExact(
				Catalog->GetCandidates()[0]
					.GetIdentity()
					.GetCanonicalKey(),
				Catalog->GetRevision(),
				*Resources,
				Deadline,
				Cancellation.GetToken(),
				Binding,
				DiscoveryError)
			|| !Binding.IsValid())
		{
			OutError = DiscoveryError.Code + TEXT(":")
				+ DiscoveryError.Detail;
			return false;
		}
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		if (!ReadPort.TryRead(
				FUeAgentStateReadRequest(
					*Binding,
					EUeAgentStatePlane::Persisted),
				*Resources,
				Deadline,
				Cancellation.GetToken(),
				Snapshot,
				DiscoveryError)
			|| !Snapshot.IsValid())
		{
			OutError = DiscoveryError.Code + TEXT(":")
				+ DiscoveryError.Detail;
			return false;
		}
		FUeAgentOperationTicketError TicketError;
		if (!TicketService.TryIssue(
				*Binding,
				OperationDescriptor(),
				Session(),
				Caller(),
				Snapshot->GetRevision(),
				OutTicket,
				TicketError)
			|| !OutTicket.IsSet())
		{
			OutError = TicketError.Code + TEXT(":")
				+ TicketError.Detail;
			return false;
		}
		OutBinding.Emplace(*Binding);
		return true;
	}

	bool TrySubmit(
		FUeAgentReferenceCrudApplication& Application,
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentOperationTicket& Ticket,
		const FString& Intent,
		const FString& ProposedText,
		const FString& RequestId,
		FString& OutResult)
	{
		TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(
			TEXT("ticket"),
			FUeAgentBindingTicketJsonCodec::TicketToJson(Ticket));
		Target->SetObjectField(
			TEXT("binding"),
			FUeAgentBindingTicketJsonCodec::BindingToJson(Binding));
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(
			TEXT("session"),
			FUeAgentBindingTicketJsonCodec::SessionToJson(Session()));
		Root->SetObjectField(TEXT("target"), Target);
		Root->SetStringField(TEXT("intent"), Intent);
		if (Intent == TEXT("delete"))
		{
			Root->SetField(
				TEXT("proposed_after"),
				MakeShared<FJsonValueNull>());
		}
		else
		{
			TSharedRef<FJsonObject> Proposed = MakeShared<FJsonObject>();
			Proposed->SetStringField(TEXT("kind"), TEXT("string"));
			Proposed->SetStringField(TEXT("text"), ProposedText);
			Root->SetObjectField(TEXT("proposed_after"), Proposed);
		}
		Root->SetObjectField(
			TEXT("projection"),
			MakeShared<FJsonObject>());
		const FUeAgentOperationRequest Request(
			OperationDescriptor(),
			RequestId,
			RequestId + TEXT("-idempotency"),
			FUeAgentContractValue(JsonText(Root)));
		const FUeAgentOperationResult Result =
			Application.Submit(
				Request,
				Caller(),
				MakeShared<FNeverCancelled>());
		OutResult = Result.GetValue().GetJsonValue();
		return Result.IsValid()
			&& !OutResult.Contains(TEXT("\"operation_error\""));
	}

	bool TryReadPersisted(
		IUeAgentStateReadPort& ReadPort,
		const FUeAgentCandidateBinding& Binding,
		bool& bOutExists,
		FString& OutText,
		FString& OutError)
	{
		bOutExists = false;
		OutText.Reset();
		OutError.Reset();
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources = MakeTracker();
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
		FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentDiscoveryError Error;
		if (!ReadPort.TryRead(
				FUeAgentStateReadRequest(
					Binding,
					EUeAgentStatePlane::Persisted),
				*Resources,
				Deadline,
				Cancellation.GetToken(),
				Snapshot,
				Error)
			|| !Snapshot.IsValid())
		{
			OutError = Error.Code + TEXT(":") + Error.Detail;
			return false;
		}
		if (Snapshot->GetEntries().IsEmpty())
		{
			return true;
		}
		if (Snapshot->GetEntries().Num() != 1
			|| Snapshot->GetEntries()[0].GetValue()->GetKind()
				!= EUeAgentValueKind::String)
		{
			OutError = TEXT("reference_readback_shape_invalid");
			return false;
		}
		bOutExists = true;
		OutText =
			Snapshot->GetEntries()[0].GetValue()->GetText();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReferenceCrudApplicationCompositionTest,
	"UeAgentInterface.Composition.ReferenceCrud.ApplicationGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReferenceCrudApplicationCompositionTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("shared ticket owner composes"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("p4-013-application-ticket-key"),
			30.0,
			[]() { return FPlatformTime::Seconds(); },
			[]()
			{
				return FGuid::NewGuid().ToString(
					EGuidFormats::Digits);
			},
			TicketService,
			TicketError));
	const TSharedRef<FUeAgentReferenceAssetReadHook> ReadHook =
		MakeShared<FUeAgentReferenceAssetReadHook>(
			TEXT("p4-013-application-session"));
	const TSharedRef<FUeAgentGenericReadAdapter> ReadPort =
		MakeShared<FUeAgentGenericReadAdapter>(
			GetReferenceAssetReadPortId(),
			ReadHook);
	TSharedPtr<FUeAgentReferenceCrudApplication> Application;
	FUeAgentReferenceCrudApplicationError Error;
	TestTrue(
		TEXT("C3O-to-C9 reference application graph composes"),
		FUeAgentReferenceCrudApplication::TryCreate(
			Config(),
			TicketService.ToSharedRef(),
			ReadPort,
			Application,
			Error));
	TestTrue(
		TEXT("the graph exposes the one Protocol operation port"),
		Application.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReferenceCrudOwnedFixtureTest,
	"UeAgentInterface.Composition.ReferenceCrud.OwnedFixtureE3E4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReferenceCrudOwnedFixtureTest::RunTest(
	const FString& Parameters)
{
	const FString FixtureName =
		TEXT("P4_013_E3E4_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath =
		TEXT("/Game/UAI/Reference/")
		+ FixtureName
		+ TEXT(".")
		+ FixtureName;
	CleanupOwnedFixture(ObjectPath);

	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	if (!FUeAgentOperationTicketService::TryCreate(
			TEXT("p4-013-owned-worker-ticket-key"),
			30.0,
			[]() { return FPlatformTime::Seconds(); },
			[]()
			{
				return FGuid::NewGuid().ToString(
					EGuidFormats::Digits);
			},
			TicketService,
			TicketError)
		|| !TicketService.IsValid())
	{
		AddError(
			TEXT("ticket service: ")
			+ TicketError.Code
			+ TEXT(":")
			+ TicketError.Detail);
		return false;
	}

	const TSharedRef<FUeAgentReferenceAssetReadHook> ApplicationReadHook =
		MakeShared<FUeAgentReferenceAssetReadHook>(
			Config().ServerSessionId);
	const TSharedRef<FUeAgentGenericReadAdapter> ApplicationReadPort =
		MakeShared<FUeAgentGenericReadAdapter>(
			GetReferenceAssetReadPortId(),
			ApplicationReadHook);
	const TSharedRef<FUeAgentReferenceAssetReadHook> OracleReadHook =
		MakeShared<FUeAgentReferenceAssetReadHook>(
			Config().ServerSessionId);
	const TSharedRef<FUeAgentGenericReadAdapter> OracleReadPort =
		MakeShared<FUeAgentGenericReadAdapter>(
			GetReferenceAssetReadPortId(),
			OracleReadHook);
	const TSharedRef<FUeAgentReferenceAssetCatalogSource> Source =
		MakeShared<FUeAgentReferenceAssetCatalogSource>(
			OperationDescriptor(),
			Config().ServerSessionId);
	TSharedPtr<FUeAgentReferenceCrudApplication> Application;
	FUeAgentReferenceCrudApplicationError ApplicationError;
	if (!FUeAgentReferenceCrudApplication::TryCreate(
			Config(),
			TicketService.ToSharedRef(),
			ApplicationReadPort,
			Application,
			ApplicationError)
		|| !Application.IsValid())
	{
		AddError(
			TEXT("application composition: ")
			+ ApplicationError.Code
			+ TEXT(":")
			+ ApplicationError.Detail);
		return false;
	}

	const TArray<TPair<FString, FString>> Writes = {
		{TEXT("create"), TEXT("p4-013-created")},
		{TEXT("update"), TEXT("p4-013-updated")},
	};
	FString Error;
	FString ResultJson;
	TOptional<FUeAgentCandidateBinding> Binding;
	TOptional<FUeAgentOperationTicket> Ticket;
	for (int32 Index = 0; Index < Writes.Num(); ++Index)
	{
		const FString RequestId =
			TEXT("p4-013-")
			+ Writes[Index].Key
			+ TEXT("-")
			+ FString::FromInt(Index);
		if (!TryPrepareTarget(
				*Source,
				*ApplicationReadPort,
				*TicketService,
				ObjectPath,
				Binding,
				Ticket,
				Error)
			|| !Binding.IsSet()
			|| !Ticket.IsSet())
		{
			AddError(TEXT("prepare target: ") + Error);
			CleanupOwnedFixture(ObjectPath);
			return false;
		}
		if (!TrySubmit(
				*Application,
				Binding.GetValue(),
				Ticket.GetValue(),
				Writes[Index].Key,
				Writes[Index].Value,
				RequestId,
				ResultJson))
		{
			AddError(
				TEXT("submit ")
				+ Writes[Index].Key
				+ TEXT(": ")
				+ ResultJson);
			CleanupOwnedFixture(ObjectPath);
			return false;
		}
		bool bExists = false;
		FString PersistedText;
		if (!TryReadPersisted(
				*OracleReadPort,
				Binding.GetValue(),
				bExists,
				PersistedText,
				Error))
		{
			AddError(
				TEXT("independent persisted readback: ")
				+ Error);
			CleanupOwnedFixture(ObjectPath);
			return false;
		}
		TestTrue(
			TEXT("independent persisted oracle observes the asset"),
			bExists);
		TestEqual(
			TEXT("independent persisted oracle observes the requested text"),
			PersistedText,
			Writes[Index].Value);
		TestTrue(
			TEXT("C8D published the package artifact"),
			IFileManager::Get().FileExists(
				*PackageFilename(ObjectPath)));
	}

	if (!TryPrepareTarget(
			*Source,
			*ApplicationReadPort,
			*TicketService,
			ObjectPath,
			Binding,
			Ticket,
			Error)
		|| !Binding.IsSet()
		|| !Ticket.IsSet())
	{
		AddError(TEXT("prepare delete target: ") + Error);
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	if (!TrySubmit(
			*Application,
			Binding.GetValue(),
			Ticket.GetValue(),
			TEXT("delete"),
			FString(),
			TEXT("p4-013-delete"),
			ResultJson))
	{
		AddError(TEXT("submit delete: ") + ResultJson);
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	bool bExists = true;
	FString PersistedText;
	if (!TryReadPersisted(
			*OracleReadPort,
			Binding.GetValue(),
			bExists,
			PersistedText,
			Error))
	{
		AddError(
			TEXT("independent absence readback: ") + Error);
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	TestFalse(
		TEXT("independent persisted oracle observes deletion"),
		bExists);
	TestFalse(
		TEXT("C8D removed the package artifact"),
		IFileManager::Get().FileExists(
			*PackageFilename(ObjectPath)));
	FString DeletedPackageName;
	FString DeletedObjectPath;
	FString DeletedPathError;
	if (!TryNormalizeReferenceAssetPath(
			ObjectPath,
			DeletedPackageName,
			DeletedObjectPath,
			DeletedPathError))
	{
		AddError(
			TEXT("normalize deleted package: ")
			+ DeletedPathError);
		CleanupOwnedFixture(ObjectPath);
		return false;
	}
	UPackage* const DeletedPackage =
		FindPackage(nullptr, *DeletedPackageName);
	TestTrue(
		TEXT("persisted delete does not leave a dirty package"),
		!DeletedPackage || !DeletedPackage->IsDirty());
	CleanupOwnedFixture(ObjectPath);
	return true;
}

