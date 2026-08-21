#include "Misc/AutomationTest.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialHjsonCommandHandler.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Composition::HjsonImport;
	using namespace UE::UAI::Composition::TimelineSpatial;
	using namespace UE::UAI::Contract;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	bool IsCanonicalJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		FString Error;
		FString Canonical;
		return UE::UAI::Contract::Json::TryParseObject(
				Text, Object, Error)
			&& UE::UAI::Contract::Json::TryCanonicalize(
				Object, Canonical, Error)
			&& Canonical == Text;
	}

	class FPublicHjsonTokenEstimator final
		: public UE::UAI::Projection::IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const UE::UAI::Projection::FUeAgentProjectedPayload& Payload,
			const TArray<UE::UAI::Projection::FUeAgentArtifactDescriptor>&
				Artifacts,
			const TArray<UE::UAI::Projection::FUeAgentProjectionDiagnostic>&
				Diagnostics,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			const UE::UAI::ContextResource::FUeAgentOperationDeadline& Deadline,
			const UE::UAI::ContextResource::FUeAgentCancellationToken&
				Cancellation,
			UE::UAI::Projection::FUeAgentTokenEstimate& OutEstimate,
			UE::UAI::Projection::FUeAgentProjectionError& OutError) override
		{
			OutError = {};
			OutEstimate = UE::UAI::Projection::FUeAgentTokenEstimate(
				UE::UAI::Projection::
					EUeAgentTokenEstimateKind::Conservative,
				256,
				768);
			return true;
		}
	};

	class FScopedPublicTimelineAsset final
	{
	public:
		FScopedPublicTimelineAsset(
			const FString& InStem,
			UClass* InAssetClass)
			: PackageName(
				TEXT("/Game/UAI/Tests/TimelineSpatial/TS012_")
				+ InStem + TEXT("_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, AssetName(FPackageName::GetLongPackageAssetName(PackageName))
			, AssetClass(InAssetClass)
			, WorkRoot(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/TS012/PublicHjson"),
				AssetName + TEXT("_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedPublicTimelineAsset()
		{
			IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				FText UnloadError;
				UPackageTools::UnloadPackages({Package}, UnloadError, true);
				CollectGarbage(RF_NoFlags, true);
			}
			IFileManager::Get().Delete(
				*FPackageName::LongPackageNameToFilename(
					PackageName,
					FPackageName::GetAssetPackageExtension()),
				false,
				true);
		}

		template <typename TAsset>
		TAsset* Create()
		{
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return nullptr;
			}
			TAsset* Asset = NewObject<TAsset>(
				Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Asset || Asset->GetClass() != AssetClass)
			{
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			return Asset;
		}

		bool Save(UObject& Asset) const
		{
			Asset.GetOutermost()->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(
				Asset.GetOutermost(),
				&Asset,
				*FPackageName::LongPackageNameToFilename(
					PackageName,
					FPackageName::GetAssetPackageExtension()),
				Args);
		}

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

		FString ExportRoot() const
		{
			return FPaths::Combine(WorkRoot, TEXT("directory"));
		}

		FString EmbeddedPath() const
		{
			return FPaths::Combine(WorkRoot, TEXT("workspace.hjson"));
		}

		FString PackageName;
		FString AssetName;
		UClass* AssetClass = nullptr;
		FString WorkRoot;
	};

	bool ParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject)
			&& OutObject.IsValid();
	}

	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<
			TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	bool TryEditField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Field,
		const TSharedPtr<FJsonValue>& Replacement,
		const bool bRemove)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		if (Object->HasField(Field))
		{
			if (bRemove)
			{
				Object->RemoveField(Field);
			}
			else
			{
				Object->SetField(Field, Replacement);
			}
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: Object->Values)
		{
			if (Pair.Value->Type == EJson::Object
				&& TryEditField(
					Pair.Value->AsObject(),
					Field,
					Replacement,
					bRemove))
			{
				return true;
			}
			if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item
					: Pair.Value->AsArray())
				{
					if (Item->Type == EJson::Object
						&& TryEditField(
							Item->AsObject(),
							Field,
							Replacement,
							bRemove))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	bool LoadJsonFile(
		const FString& Path,
		TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path)
			&& ParseObject(Text, OutObject);
	}

	bool EditDirectoryField(
		const FString& Root,
		const FString& Field,
		const TSharedPtr<FJsonValue>& Replacement,
		const bool bRemove)
	{
		TSharedPtr<FJsonObject> Manifest;
		if (!LoadJsonFile(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				Manifest))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RowValue
			: Manifest->GetArrayField(TEXT("unit_inventory")))
		{
			const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
			if (!Row.IsValid())
			{
				return false;
			}
			const FString Path = FPaths::Combine(
				Root,
				Row->GetStringField(TEXT("logical_path")));
			TSharedPtr<FJsonObject> Document;
			if (!LoadJsonFile(Path, Document))
			{
				return false;
			}
			if (TryEditField(Document, Field, Replacement, bRemove))
			{
				return FFileHelper::SaveStringToFile(
					SerializeObject(Document.ToSharedRef()),
					*Path,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
		}
		return false;
	}

	bool BuildEmbeddedWorkspace(
		const FString& Root,
		const FString& Field,
		const TSharedPtr<FJsonValue>& Replacement,
		const bool bRemove,
		FString& OutText)
	{
		OutText.Reset();
		TSharedPtr<FJsonObject> Manifest;
		if (!LoadJsonFile(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				Manifest))
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Units;
		bool bEdited = false;
		const TArray<TSharedPtr<FJsonValue>>& Inventory =
			Manifest->GetArrayField(TEXT("unit_inventory"));
		for (int32 Index = 0; Index < Inventory.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Row = Inventory[Index]->AsObject();
			if (!Row.IsValid())
			{
				return false;
			}
			const FString Path = FPaths::Combine(
				Root,
				Row->GetStringField(TEXT("logical_path")));
			TSharedPtr<FJsonObject> Document;
			if (!LoadJsonFile(Path, Document))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Unit =
				Document->GetObjectField(TEXT("unit"));
			if (!Unit.IsValid())
			{
				return false;
			}
			if (!bEdited)
			{
				bEdited = TryEditField(
					Unit,
					Field,
					Replacement,
					bRemove);
			}
			Row->SetStringField(
				TEXT("logical_path"),
				FString::Printf(TEXT("#/units/%d"), Index));
			Units.Add(MakeShared<FJsonValueObject>(Unit));
		}
		if (!bEdited)
		{
			return false;
		}
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("manifest"), Manifest);
		Envelope->SetArrayField(TEXT("units"), MoveTemp(Units));
		OutText = SerializeObject(Envelope);
		return !OutText.IsEmpty();
	}

	TOptional<FUeAgentCommandRequest> RequestFor(
		const FString& BindingHash,
		const TSharedRef<FJsonObject>& Params,
		const FString& RequestId)
	{
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("schema"), TEXT("uai.command.v1"));
		Envelope->SetNumberField(TEXT("version"), 1);
		Envelope->SetStringField(TEXT("message_kind"), TEXT("command_request"));
		Envelope->SetStringField(TEXT("correlation_id"), TEXT("corr-ts012-hjson"));
		Envelope->SetStringField(TEXT("request_id"), RequestId);
		Envelope->SetStringField(
			TEXT("command_id"),
			TEXT("content_graph.import_workspace"));
		Envelope->SetStringField(TEXT("command_binding_hash"), BindingHash);
		Envelope->SetObjectField(TEXT("params"), Params);
		const FString Body = SerializeObject(Envelope);
		const FTCHARToUTF8 Converted(*Body);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		FUeAgentCommandProtocolCodec Codec;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<FUeAgentCommandRequest> Request;
		Codec.DecodeRequest(Bytes, Request, Diagnostic);
		return Request;
	}

	bool CreatePublicHarness(
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		FString& OutBindingHash,
		FString& OutError,
		const bool bRequirePublicContract = true)
	{
		OutHandler.Reset();
		OutBindingHash.Reset();
		if (bRequirePublicContract)
		{
			TSharedPtr<const FUeAgentCommandContractRegistry> Commands;
			if (!TryCreateTimelineSpatialProductionCommandContractRegistry(
					Commands,
					OutError)
				|| !Commands.IsValid())
			{
				return false;
			}
			const FUeAgentCommandContract* Contract = Commands->FindExact(
				TEXT("content_graph.import_workspace"));
			if (!Contract)
			{
				OutError = TEXT("public_hjson_contract_missing");
				return false;
			}
			OutBindingHash = Contract->GetBindingHash();
		}
		TSharedPtr<const UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketService> TicketService;
		UE::UAI::BindingGateway::Ticket::FUeAgentOperationTicketError
			TicketError;
		if (!UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketService::TryCreate(
				TEXT("ts012-public-hjson-ticket-integrity-key"),
				300.0,
				[]() { return FPlatformTime::Seconds(); },
				[]()
				{
					return FGuid::NewGuid().ToString(EGuidFormats::Digits);
				},
				TicketService,
				TicketError)
			|| !TicketService.IsValid())
		{
			OutError = TicketError.Code;
			return false;
		}
		TSharedPtr<const UE::UAI::Projection::FUeAgentResponseProjector>
			Projector;
		UE::UAI::Projection::FUeAgentProjectionError ProjectionError;
		if (!UE::UAI::Projection::FUeAgentResponseProjector::TryCreate(
				MakeShared<FPublicHjsonTokenEstimator>(),
				Projector,
				ProjectionError)
			|| !Projector.IsValid())
		{
			OutError = ProjectionError.Code;
			return false;
		}
		FUeAgentTimelineSpatialHjsonCommandHandlerConfig Config;
		Config.Operation.ProjectInstanceId = TEXT("ts012-project");
		Config.Operation.EditorInstanceId = TEXT("ts012-editor");
		Config.Operation.ServerSessionId = TEXT("ts012-session");
		Config.Operation.RegistryEpoch = 1;
		Config.TimeoutSeconds = 120.0;
		Config.MaxFrameBytes = 16 * 1024 * 1024;
		return TryCreateTimelineSpatialHjsonCommandHandler(
			Config,
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			OutHandler,
			OutError)
			&& OutHandler.IsValid();
	}

	TSharedRef<FJsonObject> SourceObject(
		const FString& Kind,
		const FString& Path,
		const FString& Text = FString())
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), Kind);
		if (Kind == TEXT("inline"))
		{
			Source->SetStringField(TEXT("text"), Text);
			Source->SetStringField(TEXT("source_unit"), TEXT("workspace.hjson"));
			Source->SetStringField(TEXT("source_id"), TEXT("ts012-inline"));
		}
		else
		{
			Source->SetStringField(TEXT("path"), Path);
		}
		return Source;
	}

	FUeAgentCommandResult SubmitPublicImport(
		IUeAgentCommandHandler& Handler,
		const FString& BindingHash,
		const FString& Target,
		const TSharedRef<FJsonObject>& Source,
		const FString& Intent,
		const FString& ExpectedRevision,
		const bool bDryRun,
		const FString& RequestId)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		Params->SetStringField(TEXT("intent"), Intent);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		const TOptional<FUeAgentCommandRequest> Request =
			RequestFor(BindingHash, Params, RequestId);
		if (!Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-ts012-hjson"),
				RequestId,
				TEXT("content_graph.import_workspace"),
				BindingHash,
				TEXT("test_request_decode_failed"),
				TEXT("test request could not be decoded"));
		}
		return Handler.Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("ts012-public-hjson"),
				{TEXT("uai:authoring:hjson:import")},
				TEXT("corr-ts012-hjson")),
			MakeShared<FNeverCancelled>());
	}

	bool ExportOwned(
		const FString& Kind,
		const FScopedPublicTimelineAsset& Owned,
		FUeAgentHjsonTargetExportReceipt& OutReceipt,
		FUeAgentHjsonImportTargetError& OutError)
	{
		return GetHjsonImportTargetRegistry().TryExportExact(
			FUeAgentHjsonTargetExportRequest(
				Kind,
				Owned.ObjectPath(),
				TEXT("llm_authoring"),
				Owned.ExportRoot()),
			OutReceipt,
			OutError);
	}

	ULevelSequence* CreateLevelSequence(
		FScopedPublicTimelineAsset& Owned,
		const int32 Duration)
	{
		ULevelSequence* Sequence = Owned.Create<ULevelSequence>();
		if (!Sequence)
		{
			return nullptr;
		}
		Sequence->Initialize();
		if (!Sequence->GetMovieScene())
		{
			return nullptr;
		}
		Sequence->GetMovieScene()->SetTickResolutionDirectly(FFrameRate(24000, 1));
		Sequence->GetMovieScene()->SetDisplayRate(FFrameRate(24, 1));
		Sequence->GetMovieScene()->SetPlaybackRange(0, Duration);
		return Owned.Save(*Sequence) ? Sequence : nullptr;
	}

	UAnimSequence* CreateAnimationSequence(
		FScopedPublicTimelineAsset& Owned,
		const bool bRootMotion)
	{
		USkeleton* Skeleton = Cast<USkeleton>(StaticLoadObject(
			USkeleton::StaticClass(),
			nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton")));
		if (!Skeleton)
		{
			return nullptr;
		}
		UAnimSequence* Sequence = Owned.Create<UAnimSequence>();
		if (!Sequence)
		{
			return nullptr;
		}
		Sequence->SetSkeleton(Skeleton);
		Sequence->GetController().InitializeModel();
		Sequence->GetController().SetNumberOfFrames(FFrameNumber(1), false);
		Sequence->GetController().NotifyPopulated();
		Sequence->ResetCompressionSettings();
		Sequence->RateScale = 1.0f;
		Sequence->bEnableRootMotion = bRootMotion;
		return Owned.Save(*Sequence) ? Sequence : nullptr;
	}

	int32 ReadLevelSequenceDuration(const FString& ObjectPath)
	{
		const ULevelSequence* Sequence = Cast<ULevelSequence>(StaticLoadObject(
			ULevelSequence::StaticClass(),
			nullptr,
			*ObjectPath));
		if (!Sequence || !Sequence->GetMovieScene())
		{
			return -1;
		}
		const TRange<FFrameNumber> Range =
			Sequence->GetMovieScene()->GetPlaybackRange();
		return Range.GetUpperBoundValue().Value
			- Range.GetLowerBoundValue().Value;
	}

	bool ReadRootMotion(const FString& ObjectPath, bool& bOutValue)
	{
		const UAnimSequence* Sequence = Cast<UAnimSequence>(StaticLoadObject(
			UAnimSequence::StaticClass(),
			nullptr,
			*ObjectPath));
		if (!Sequence)
		{
			return false;
		}
		bOutValue = Sequence->bEnableRootMotion;
		return true;
	}

	bool ImportReceiptIsPersisted(const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Root;
		if (!Result.IsOk() || !ParseObject(Result.GetPayloadJson(), Root))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Import =
			Root->GetObjectField(TEXT("import"));
		return Import.IsValid()
			&& Import->GetBoolField(TEXT("mutation_applied"))
			&& Import->GetBoolField(TEXT("compile_settled"))
			&& Import->GetBoolField(TEXT("package_saved"))
			&& Import->GetBoolField(TEXT("unloaded_reloaded"));
	}

	bool ImportReceiptUsesUnifiedC3O(const FUeAgentCommandResult& Result)
	{
		TSharedPtr<FJsonObject> Root;
		if (!Result.IsOk() || !ParseObject(Result.GetPayloadJson(), Root))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Import =
			Root->GetObjectField(TEXT("import"));
		FString OperationId;
		FString PlanFingerprint;
		FString VerificationStatus;
		return Import.IsValid()
			&& Import->TryGetStringField(
				TEXT("operation_id"), OperationId)
			&& !OperationId.IsEmpty()
			&& Import->TryGetStringField(
				TEXT("plan_fingerprint"), PlanFingerprint)
			&& PlanFingerprint.StartsWith(TEXT("sha256:"))
			&& Import->TryGetStringField(
				TEXT("verification_status"), VerificationStatus)
			&& VerificationStatus == TEXT("verified");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT36LegacyHjsonHandlerFailClosedTest,
	"UAI.Root.T36.LegacyHjsonHandler.FailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT36LegacyHjsonHandlerFailClosedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<IUeAgentCommandHandler> Handler;
	FString BindingHash;
	FString ErrorText;
	TestFalse(
		TEXT("the retired timeline/spatial HJSON handler cannot compose"),
		CreatePublicHarness(Handler, BindingHash, ErrorText, false));
	TestFalse(TEXT("the retired handler is not returned"), Handler.IsValid());
	TestEqual(TEXT("the retirement failure is exact"),
		ErrorText, FString(TEXT("legacy_hjson_import_retired")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialPublicHjsonSourceParityPersistedTest,
	"UeAgentInterface.Composition.TimelineSpatial.E4.PublicHjsonSourceParityPersisted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialPublicHjsonSourceParityPersistedTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<IUeAgentCommandHandler> Handler;
	FString BindingHash;
	FString ErrorText;
	TestTrue(
		TEXT("create public HJSON command harness"),
		CreatePublicHarness(Handler, BindingHash, ErrorText));
	if (!Handler.IsValid())
	{
		AddError(ErrorText);
		return false;
	}

	const TArray<FString> SourceKinds = {
		TEXT("directory"),
		TEXT("single_file"),
		TEXT("inline"),
	};
	for (int32 Index = 0; Index < SourceKinds.Num(); ++Index)
	{
		const FString& SourceKind = SourceKinds[Index];
		FScopedPublicTimelineAsset Owned(SourceKind, ULevelSequence::StaticClass());
		TestNotNull(
			TEXT("create owned LevelSequence for ") + SourceKind,
			CreateLevelSequence(Owned, 48));
		FUeAgentHjsonTargetExportReceipt ExportReceipt;
		FUeAgentHjsonImportTargetError ExportError;
		TestTrue(
			TEXT("export owned workspace for ") + SourceKind,
			ExportOwned(
				TEXT("ue.level_sequence.asset"),
				Owned,
				ExportReceipt,
				ExportError));

		TSharedRef<FJsonObject> Source =
			SourceObject(TEXT("directory"), ExportReceipt.GetWorkspaceRoot());
		if (SourceKind == TEXT("directory"))
		{
			TestTrue(
				TEXT("edit directory managed unit"),
				EditDirectoryField(
					ExportReceipt.GetWorkspaceRoot(),
					TEXT("playback_duration_ticks"),
					MakeShared<FJsonValueString>(TEXT("72")),
					false));
		}
		else
		{
			FString Embedded;
			TestTrue(
				TEXT("build embedded semantic workspace"),
				BuildEmbeddedWorkspace(
					ExportReceipt.GetWorkspaceRoot(),
					TEXT("playback_duration_ticks"),
					MakeShared<FJsonValueString>(TEXT("72")),
					false,
					Embedded));
			if (SourceKind == TEXT("single_file"))
			{
				TestTrue(
					TEXT("write single-file workspace"),
					FFileHelper::SaveStringToFile(
						Embedded,
						*Owned.EmbeddedPath(),
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
				Source = SourceObject(SourceKind, Owned.EmbeddedPath());
			}
			else
			{
				Source = SourceObject(SourceKind, FString(), Embedded);
			}
		}

		const FUeAgentCommandResult Result = SubmitPublicImport(
			*Handler,
			BindingHash,
			Owned.ObjectPath(),
			Source,
			TEXT("update"),
			ExportReceipt.GetBaseRevision(),
			false,
			TEXT("req-ts012-source-") + SourceKind);
		TestTrue(
			TEXT("public import succeeds for ") + SourceKind
				+ TEXT(": ") + Result.GetErrorJson(),
			Result.IsOk());
		TestTrue(
			TEXT("public receipt proves persisted lifecycle for ") + SourceKind,
			ImportReceiptIsPersisted(Result));
		TestTrue(
			TEXT("public import exposes unified C3O evidence for ")
				+ SourceKind,
			ImportReceiptUsesUnifiedC3O(Result));
		TestTrue(
			TEXT("public HJSON success payload is canonical JSON for ")
				+ SourceKind,
			IsCanonicalJson(Result.GetPayloadJson()));
		TestEqual(
			TEXT("independent persisted duration for ") + SourceKind,
			ReadLevelSequenceDuration(Owned.ObjectPath()),
			72);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialPublicHjsonIntentRevisionDryRunPersistedTest,
	"UeAgentInterface.Composition.TimelineSpatial.E4.PublicHjsonIntentRevisionDryRunPersisted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialPublicHjsonIntentRevisionDryRunPersistedTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<IUeAgentCommandHandler> Handler;
	FString BindingHash;
	FString ErrorText;
	TestTrue(
		TEXT("create public HJSON command harness"),
		CreatePublicHarness(Handler, BindingHash, ErrorText));
	if (!Handler.IsValid())
	{
		AddError(ErrorText);
		return false;
	}

	for (const TPair<FString, bool>& Case : {
		TPair<FString, bool>(TEXT("update"), true),
		TPair<FString, bool>(TEXT("replace"), false),
	})
	{
		FScopedPublicTimelineAsset Owned(Case.Key, UAnimSequence::StaticClass());
		TestNotNull(
			TEXT("create owned AnimationSequence for ") + Case.Key,
			CreateAnimationSequence(Owned, true));
		FUeAgentHjsonTargetExportReceipt ExportReceipt;
		FUeAgentHjsonImportTargetError ExportError;
		TestTrue(
			TEXT("export intent workspace for ") + Case.Key,
			ExportOwned(
				TEXT("ue.animation_sequence.asset"),
				Owned,
				ExportReceipt,
				ExportError));
		TestTrue(
			TEXT("remove optional root motion field for ") + Case.Key,
			EditDirectoryField(
				ExportReceipt.GetWorkspaceRoot(),
				TEXT("enable_root_motion"),
				nullptr,
				true));
		const FUeAgentCommandResult Result = SubmitPublicImport(
			*Handler,
			BindingHash,
			Owned.ObjectPath(),
			SourceObject(TEXT("directory"), ExportReceipt.GetWorkspaceRoot()),
			Case.Key,
			ExportReceipt.GetBaseRevision(),
			false,
			TEXT("req-ts012-intent-") + Case.Key);
		TestTrue(
			TEXT("public intent import succeeds: ") + Result.GetErrorJson(),
			Result.IsOk());
		bool bPersistedRootMotion = false;
		TestTrue(
			TEXT("reload persisted root motion for ") + Case.Key,
			ReadRootMotion(Owned.ObjectPath(), bPersistedRootMotion));
		TestEqual(
			TEXT("intent controls omitted-field semantics for ") + Case.Key,
			bPersistedRootMotion,
			Case.Value);
	}

	FScopedPublicTimelineAsset Owned(TEXT("revision_dry_run"), ULevelSequence::StaticClass());
	TestNotNull(
		TEXT("create revision/dry-run LevelSequence"),
		CreateLevelSequence(Owned, 48));
	FUeAgentHjsonTargetExportReceipt ExportReceipt;
	FUeAgentHjsonImportTargetError ExportError;
	TestTrue(
		TEXT("export revision/dry-run workspace"),
		ExportOwned(
			TEXT("ue.level_sequence.asset"),
			Owned,
			ExportReceipt,
			ExportError));
	TestTrue(
		TEXT("edit revision/dry-run workspace"),
		EditDirectoryField(
			ExportReceipt.GetWorkspaceRoot(),
			TEXT("playback_duration_ticks"),
			MakeShared<FJsonValueString>(TEXT("96")),
			false));

	const FUeAgentCommandResult Stale = SubmitPublicImport(
		*Handler,
		BindingHash,
		Owned.ObjectPath(),
		SourceObject(TEXT("directory"), ExportReceipt.GetWorkspaceRoot()),
		TEXT("update"),
		TEXT("sha256:stale"),
		false,
		TEXT("req-ts012-stale"));
	TestFalse(TEXT("stale expected_revision is rejected"), Stale.IsOk());
	TestTrue(
		TEXT("stale failure has stable code"),
		Stale.GetErrorJson().Contains(TEXT("timeline_hjson_stale_target")));
	TestTrue(
		TEXT("public HJSON failure payload is canonical JSON"),
		IsCanonicalJson(Stale.GetErrorJson()));
	TestEqual(
		TEXT("stale rejection performs no mutation"),
		ReadLevelSequenceDuration(Owned.ObjectPath()),
		48);

	const FUeAgentCommandResult DryRun = SubmitPublicImport(
		*Handler,
		BindingHash,
		Owned.ObjectPath(),
		SourceObject(TEXT("directory"), ExportReceipt.GetWorkspaceRoot()),
		TEXT("update"),
		ExportReceipt.GetBaseRevision(),
		true,
		TEXT("req-ts012-dry-run"));
	TestTrue(
		TEXT("dry-run succeeds: ") + DryRun.GetErrorJson(),
		DryRun.IsOk());
	TestFalse(
		TEXT("dry-run reports no mutation"),
		ImportReceiptIsPersisted(DryRun));
	TestEqual(
		TEXT("dry-run leaves persisted duration unchanged"),
		ReadLevelSequenceDuration(Owned.ObjectPath()),
		48);
	return !HasAnyErrors();
}
