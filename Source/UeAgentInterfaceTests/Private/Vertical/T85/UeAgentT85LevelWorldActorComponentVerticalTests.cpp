#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Components/AudioComponent.h"
#include "Curves/CurveFloat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const TCHAR* OperationTag,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FString CorrelationId =
			FString::Printf(TEXT("corr-t85-level-%s"), OperationTag);
		const FString RequestId =
			FString::Printf(TEXT("req-t85-level-%s"), OperationTag);
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				CorrelationId, RequestId, CommandId,
				FString(), TEXT("level_public_import_contract_missing"),
				TEXT("the exact public import contract is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"%s\",")
			TEXT("\"request_id\":\"%s\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CorrelationId, *RequestId, *CommandId,
			*Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				CorrelationId, RequestId, CommandId,
				Contract->GetContractHash(), TEXT("level_public_import_request_invalid"),
				TEXT("the exact public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-level"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				CorrelationId),
			MakeShared<FNeverCancelled>());
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool TryBuildLevelImportParams(
		const FString& Target,
		const FString& ManagedId,
		const FString& ActorLabel,
		const FString& ComponentName,
		const FString& WorldOperation,
		const FString& ActorOperation,
		const FString& ComponentOperation,
		const FVector& ActorLocation,
		FString& OutParams,
		FString& OutError,
		const FString& ComponentClass = TEXT("/Script/Engine.RotatingMovementComponent"))
	{
		const TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
		Component->SetStringField(TEXT("component_name"), ComponentName);
		Component->SetStringField(
			TEXT("type"), ComponentClass);
		Component->SetStringField(TEXT("operation"), ComponentOperation);

		const TSharedRef<FJsonObject> Actor = MakeShared<FJsonObject>();
		Actor->SetStringField(TEXT("managed_id"), ManagedId);
		Actor->SetStringField(
			TEXT("type"), TEXT("/Script/Engine.StaticMeshActor"));
		Actor->SetStringField(TEXT("label"), ActorLabel);
		Actor->SetStringField(TEXT("operation"), ActorOperation);
		Actor->SetArrayField(
			TEXT("components"), {MakeShared<FJsonValueObject>(Component)});
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
		Location->SetNumberField(TEXT("x"), ActorLocation.X);
		Location->SetNumberField(TEXT("y"), ActorLocation.Y);
		Location->SetNumberField(TEXT("z"), ActorLocation.Z);
		Transform->SetObjectField(TEXT("location"), Location);
		const TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
		Rotation->SetNumberField(TEXT("pitch"), 10.0);
		Rotation->SetNumberField(TEXT("yaw"), 20.0);
		Rotation->SetNumberField(TEXT("roll"), 30.0);
		Transform->SetObjectField(TEXT("rotation"), Rotation);
		const TSharedRef<FJsonObject> Scale = MakeShared<FJsonObject>();
		Scale->SetNumberField(TEXT("x"), 1.0);
		Scale->SetNumberField(TEXT("y"), 1.0);
		Scale->SetNumberField(TEXT("z"), 1.0);
		Transform->SetObjectField(TEXT("scale"), Scale);
		Actor->SetObjectField(TEXT("transform"), Transform);

		const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetNumberField(TEXT("dimension"), 3);
		Frame->SetStringField(TEXT("semantic_frame"), TEXT("ue_world"));
		Frame->SetStringField(TEXT("write_frame"), TEXT("ue_world"));
		Frame->SetStringField(TEXT("read_frame"), TEXT("ue_world"));
		Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Extension->SetObjectField(TEXT("frame"), Frame);
		Extension->SetArrayField(
			TEXT("actors"), {MakeShared<FJsonValueObject>(Actor)});
		const TSharedRef<FJsonObject> DocumentBody = MakeShared<FJsonObject>();
		DocumentBody->SetStringField(TEXT("operation"), WorldOperation);
		const TSharedRef<FJsonObject> DocumentTarget = MakeShared<FJsonObject>();
		DocumentTarget->SetStringField(TEXT("asset_path"), Target);
		DocumentBody->SetObjectField(TEXT("target"), DocumentTarget);
		DocumentBody->SetObjectField(TEXT("provider_extension"), Extension);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("/Script/Engine.World"));
		Root->SetObjectField(TEXT("document"), DocumentBody);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.world.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		if (!CommandContract::Json::TryCanonicalize(
				Document, DocumentText, OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), DocumentText);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildDirectoryImportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), WorkspacePath);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildExportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("workspace_path"), WorkspacePath);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryReadLocation(
		const TSharedPtr<FJsonObject>& Transform,
		FVector& OutLocation,
		FString* OutFieldName = nullptr)
	{
		if (!Transform.IsValid())
		{
			return false;
		}
		for (const TCHAR* FieldName : {TEXT("translation"), TEXT("location")})
		{
			const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
			const TSharedPtr<FJsonObject>* LocationObject = nullptr;
			if (Transform->TryGetArrayField(FieldName, LocationArray)
				&& LocationArray != nullptr && LocationArray->Num() == 3)
			{
				OutLocation = FVector(
					(*LocationArray)[0]->AsNumber(),
					(*LocationArray)[1]->AsNumber(),
					(*LocationArray)[2]->AsNumber());
				if (OutFieldName != nullptr) *OutFieldName = FieldName;
				return true;
			}
			if (Transform->TryGetObjectField(FieldName, LocationObject)
				&& LocationObject != nullptr && LocationObject->IsValid())
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if ((*LocationObject)->TryGetNumberField(TEXT("x"), X)
					&& (*LocationObject)->TryGetNumberField(TEXT("y"), Y)
					&& (*LocationObject)->TryGetNumberField(TEXT("z"), Z))
				{
					OutLocation = FVector(X, Y, Z);
					if (OutFieldName != nullptr) *OutFieldName = FieldName;
					return true;
				}
			}
		}
		return false;
	}

	bool TryWriteLocation(
		const TSharedPtr<FJsonObject>& Transform,
		const FVector& Location)
	{
		FVector CurrentLocation;
		FString FieldName;
		if (!TryReadLocation(Transform, CurrentLocation, &FieldName))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
		const TSharedPtr<FJsonObject>* LocationObject = nullptr;
		if (Transform->TryGetArrayField(FieldName, LocationArray)
			&& LocationArray != nullptr)
		{
			Transform->SetArrayField(
				FieldName,
				{
					MakeShared<FJsonValueNumber>(Location.X),
					MakeShared<FJsonValueNumber>(Location.Y),
					MakeShared<FJsonValueNumber>(Location.Z),
				});
			return true;
		}
		if (Transform->TryGetObjectField(FieldName, LocationObject)
			&& LocationObject != nullptr && LocationObject->IsValid())
		{
			(*LocationObject)->SetNumberField(TEXT("x"), Location.X);
			(*LocationObject)->SetNumberField(TEXT("y"), Location.Y);
			(*LocationObject)->SetNumberField(TEXT("z"), Location.Z);
			return true;
		}
		return false;
	}

	bool TryFindTransformValue(
		const TSharedPtr<FJsonObject>& Object,
		const FVector& ExpectedLocation,
		TSharedPtr<FJsonObject>& OutTransform)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (!Field.Value.IsValid())
			{
				continue;
			}
			if (Field.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Child = Field.Value->AsObject();
				if (Field.Key == TEXT("level_actor"))
				{
					const TSharedPtr<FJsonObject>* Value = nullptr;
					const TSharedPtr<FJsonObject>* Transform = nullptr;
					FVector CurrentLocation;
					if (Child.IsValid()
						&& Child->TryGetObjectField(TEXT("value"), Value)
						&& Value != nullptr
						&& (*Value)->TryGetObjectField(
							TEXT("transform"), Transform)
						&& Transform != nullptr
						&& TryReadLocation(*Transform, CurrentLocation)
						&& CurrentLocation.Equals(ExpectedLocation, 0.01f))
					{
						OutTransform = *Transform;
						return true;
					}
				}
				if (TryFindTransformValue(
						Child, ExpectedLocation, OutTransform))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool TryUpdateVisibleActorLocation(
		const FString& WorkspacePath,
		const FVector& ExpectedLocation,
		const FVector& NewLocation,
		FString& OutError)
	{
		TArray<FString> PropertyFiles;
		IFileManager::Get().FindFilesRecursive(
			PropertyFiles, *WorkspacePath, TEXT("properties.hjson"), true, false);
		FString MatchedFile;
		TSharedPtr<FJsonObject> MatchedDocument;
		TSharedPtr<FJsonObject> MatchedTransform;
		for (const FString& PropertyFile : PropertyFiles)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			TSharedPtr<FJsonObject> Transform;
			if (!FFileHelper::LoadFileToString(Text, *PropertyFile)
				|| !FUeAgentStructuredHjsonParser::ParseObject(
					Text, PropertyFile, Document, OutError)
				|| !Document.IsValid())
			{
				continue;
			}
			if (TryFindTransformValue(
					Document, ExpectedLocation, Transform))
			{
				if (!MatchedFile.IsEmpty())
				{
					OutError = TEXT("visible_actor_transform_ambiguous");
					return false;
				}
				MatchedFile = PropertyFile;
				MatchedDocument = Document;
				MatchedTransform = Transform;
			}
		}
		if (MatchedFile.IsEmpty()
			|| !MatchedDocument.IsValid()
			|| !MatchedTransform.IsValid())
		{
			OutError = TEXT("visible_actor_transform_not_found");
			return false;
		}
		if (!TryWriteLocation(MatchedTransform, NewLocation))
		{
			OutError = TEXT("visible_actor_transform_write_shape_invalid");
			return false;
		}
		FString UpdatedText;
		if (!CommandContract::Json::TryCanonicalize(
				MatchedDocument, UpdatedText, OutError))
		{
			return false;
		}
		if (!FFileHelper::SaveStringToFile(
				UpdatedText,
				*MatchedFile,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("visible_actor_hjson_write_failed");
			return false;
		}
		OutError.Reset();
		return true;
	}

	AActor* FindActorByManagedId(
		UWorld* World,
		const FString& ManagedId)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor
				&& UeAgentLevelAuthoring::GetManagedActorId(*Actor)
					== ManagedId)
			{
				return Actor;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelPublicInitializationSourceTest,
	"UAI.Root.T85.Level.PublicInitializationSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelPublicInitializationSourceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85LevelWorldActorComponent/L_Source_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85") / FPackageName::GetLongPackageAssetName(PackageName));
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("owned map removed by UE"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	const FVector Location(7.25, 11.5, 13.75);
	if (!TryBuildLevelImportParams(ObjectPath, TEXT("SourceActor"), TEXT("SourceActor"), TEXT("SourceMovement"),
		TEXT("create"), TEXT("create"), TEXT("create"), Location, Params, Error)) return false;
	const auto Created = SubmitPublicCommand(Registry, Dispatch, TEXT("source-create"), TEXT("content_graph.import_workspace"), Params);
	if (!TestTrue(TEXT("public inline initializes world, actor and component"), Created.IsOk()))
	{ AddError(Created.GetErrorJson()); return false; }
	UWorld* World = LoadObject<UWorld>(nullptr, *ObjectPath);
	AActor* Actor = FindActorByManagedId(World, TEXT("SourceActor"));
	if (!TestNotNull(TEXT("native created actor resolves"), Actor)) return false;
	TestTrue(TEXT("native location preserves requested fractions"), Actor->GetActorLocation().Equals(Location, 0.001));
	TestTrue(TEXT("native rotation preserves requested fields"), Actor->GetActorRotation().Equals(FRotator(10,20,30), 0.001));
	TArray<UActorComponent*> Components; Actor->GetComponents(Components);
	TestTrue(TEXT("native component identity and class match"), Components.ContainsByPredicate([](const UActorComponent* Component) {
		return Component && Component->GetName() == TEXT("SourceMovement")
			&& Component->GetClass()->GetPathName() == TEXT("/Script/Engine.RotatingMovementComponent");
	}));
	TestTrue(TEXT("initialization persisted clean native world"), !World->GetPackage()->IsDirty() && IFileManager::Get().FileExists(*Filename));
	{
		// Read-only defaults must not change the archetype used for delta serialization.
		AActor* Defaults = Actor->GetClass()->GetDefaultObject<AActor>();
		const FString OriginalLabel = Defaults->GetActorLabel(false);
		ON_SCOPE_EXIT {
			if (OriginalLabel.IsEmpty()) Defaults->ClearActorLabel();
			else Defaults->SetActorLabel(OriginalLabel, false);
		};
		Defaults->ClearActorLabel();
		UeAgentAuthoringNativeDefaults::FRequest DefaultRequest;
		DefaultRequest.Operation = UeAgentAuthoringNativeDefaults::EOperation::InspectDefault;
		DefaultRequest.ProviderIds = {TEXT("level.native")};
		if (!UeAgentLevelAuthoring::BuildActorIdentity(*Actor, DefaultRequest.TargetIdentity, Error)) return false;
		UeAgentAuthoringNativeDefaults::FResult DefaultResult;
		TestTrue(TEXT("Level native defaults resolve"), UeAgentAuthoringNativeDefaults::Resolve(DefaultRequest, DefaultResult));
		TestTrue(TEXT("Level native defaults provide a resolved value"), DefaultResult.IsResolved());
		TestTrue(TEXT("default probe does not synthesize an ActorLabel on the CDO"), Defaults->GetActorLabel(false).IsEmpty());
	}
	if (!TryBuildExportParams(ObjectPath, Workspace, Params, Error)) return false;
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("source-export"), TEXT("content_graph.export_workspace"), Params);
	if (!TestTrue(TEXT("initialized native world exports ordinary workspace"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("properties.hjson"), true, false);
	int32 Matches = 0;
	for (const FString& File : Files) {
		FString Text; TSharedPtr<FJsonObject> Document, Transform;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		if (TryFindTransformValue(Document, Location, Transform)) ++Matches;
	}
	TestEqual(TEXT("ordinary export independently contains the created actor transform"), Matches, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85LevelWorldActorComponentVerticalTest,
	"UAI.Root.T85.Level.WorldActorComponentVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelWorldActorComponentVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85LevelWorldActorComponent/L_WorldActorComponent_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetMapPackageExtension());
	const FString ManagedId = TEXT("T85LevelManagedActor");
	const FString ActorLabel = TEXT("T85LevelVisibleActor");
	const FString ComponentName = TEXT("T85RotatingMovementComponent");
	const FVector InitialLocation(7.0, 11.0, 13.0);
	const FVector UpdatedLocation(125.0, 25.0, 50.0);
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString RegistrationError;
	if (!TestTrue(
			TEXT("the production public import service composes"),
			OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(
				Service, RegistrationError))
		|| !Service.IsValid()
		|| !TestTrue(
			TEXT("production registration creates exact public dispatch"),
			Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(
				Service.ToSharedRef(), Registry, Dispatch, RegistrationError)))
	{
		AddError(RegistrationError);
		return false;
	}

	auto SubmitAndCheckStatus = [this, &Registry, &Dispatch](
		const FString& ParamsJson, const TCHAR* OperationTag,
		const FString& CommandId, const TCHAR* Label,
		FString& OutPayloadJson) -> bool
	{
		const CommandEdge::FUeAgentCommandResult Result =
			SubmitPublicCommand(Registry, Dispatch, OperationTag,
				CommandId, ParamsJson);
		if (!TestTrue(Label, Result.IsOk()))
		{
			AddError(Result.GetErrorJson());
			return false;
		}
		OutPayloadJson = Result.GetPayloadJson();
		TSharedPtr<FJsonObject> Payload;
		FString ParseError;
		if (!TestTrue(TEXT("the public Level response is canonical JSON"),
			CommandContract::Json::TryParseObject(OutPayloadJson, Payload, ParseError)))
		{
			AddError(ParseError);
			return false;
		}
		const bool bCompact =
			Payload.IsValid()
			&& Payload->Values.Num() == 1
			&& Payload->GetStringField(TEXT("status")) == TEXT("succeeded");
		TestTrue(TEXT("the public response contains only compact status"), bCompact);
		FTCHARToUTF8 Encoded(*OutPayloadJson);
		AddInfo(FString::Printf(TEXT("level_public_response bytes=%d fields=%d approx_tokens=%d"),
			Encoded.Length(), Payload.IsValid() ? Payload->Values.Num() : 0,
			Encoded.Length() / 4));
		return bCompact;
	};

	FString CreateParams;
	FString ParamsError;
	if (!TestTrue(TEXT("the Level create request uses only public business fields"),
		TryBuildLevelImportParams(ObjectPath, ManagedId, ActorLabel, ComponentName,
			TEXT("create"), TEXT("create"), TEXT("create"), InitialLocation,
			CreateParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	FString CreatePayloadJson;
	if (!SubmitAndCheckStatus(CreateParams, TEXT("create"),
		TEXT("content_graph.import_workspace"),
		TEXT("the public Level create dispatch persists world, actor, and component"),
		CreatePayloadJson))
	{
		return false;
	}
	UWorld* const CreatedWorld = LoadObject<UWorld>(nullptr, *ObjectPath);
	AActor* const CreatedActor = FindActorByManagedId(CreatedWorld, ManagedId);
	if (!TestNotNull(TEXT("the created Level independently resolves its Actor"), CreatedActor))
	{
		return false;
	}
	TestTrue(TEXT("the created Actor has the initial visible business transform"),
		CreatedActor->GetActorLocation().Equals(InitialLocation, 0.1f));

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
			/ (TEXT("Automation/UAI/T85/LevelExport_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
	};
	FString ExportParams;
	if (!TestTrue(TEXT("the Level export request uses public business fields"),
		TryBuildExportParams(ObjectPath, WorkspacePath, ExportParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	FString ExportPayloadJson;
	if (!SubmitAndCheckStatus(ExportParams, TEXT("export"),
		TEXT("content_graph.export_workspace"),
		TEXT("the public Level export publishes a directory"),
		ExportPayloadJson))
	{
		return false;
	}
	FString VisibleEditError;
	if (!TestTrue(TEXT("only the visible Actor transform business value is modified"),
		TryUpdateVisibleActorLocation(
			WorkspacePath, InitialLocation, UpdatedLocation, VisibleEditError)))
	{
		AddError(VisibleEditError);
		return false;
	}
	FString UpdateParams;
	if (!TestTrue(TEXT("the Level directory update request uses public business fields"),
		TryBuildDirectoryImportParams(ObjectPath, WorkspacePath,
			UpdateParams, ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	FString UpdatePayloadJson;
	if (!SubmitAndCheckStatus(UpdateParams, TEXT("update"),
		TEXT("content_graph.import_workspace"),
		TEXT("the public Level update dispatch restores its hidden baseline"),
		UpdatePayloadJson))
	{
		return false;
	}
	UWorld* const ReloadedWorld = LoadObject<UWorld>(nullptr, *ObjectPath);
	AActor* const ReloadedActor = FindActorByManagedId(ReloadedWorld, ManagedId);
	if (!TestNotNull(TEXT("the updated Level independently resolves the Actor"), ReloadedActor))
	{
		return false;
	}
	TestTrue(TEXT("the updated Actor preserves the requested business transform"),
		ReloadedActor->GetActorLocation().Equals(UpdatedLocation, 0.1f));
	TArray<UActorComponent*> ReloadedComponents;
	ReloadedActor->GetComponents(ReloadedComponents);
	TestTrue(TEXT("the updated Actor independently resolves its component"),
		ReloadedComponents.ContainsByPredicate(
			[&ComponentName](const UActorComponent* Component)
			{
				return Component && Component->GetName() == ComponentName;
			}));

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Level fixture leaves no map file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85LevelRuntimeFloatCurveTest,
	"UAI.Root.T85.Level.RuntimeFloatCurvePublic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelRuntimeFloatCurveTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString OriginalEditorPath = GEditor->GetEditorWorldContext().World()->GetPathName();
	const FString PackageName = TEXT("/Game/UAI/Transient/T85RuntimeCurve/L_RuntimeCurve_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85") / FPackageName::GetLongPackageAssetName(PackageName));
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("owned map file is deleted by UE"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	auto Submit = [&](const TCHAR* Tag, const TCHAR* Command) -> bool
	{
		const auto Result = SubmitPublicCommand(Registry, Dispatch, Tag, Command, Params);
		if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
		return true;
	};
	if (!TryBuildLevelImportParams(ObjectPath, TEXT("RuntimeCurveActor"), TEXT("RuntimeCurveActor"), TEXT("Audio"),
		TEXT("create"), TEXT("create"), TEXT("create"), FVector::ZeroVector, Params, Error, TEXT("/Script/Engine.AudioComponent"))
		|| !Submit(TEXT("curve-create"), TEXT("content_graph.import_workspace"))) return false;
	auto FindAudio = [&]() -> UAudioComponent*
	{
		AActor* Actor = FindActorByManagedId(LoadObject<UWorld>(nullptr, *ObjectPath), TEXT("RuntimeCurveActor"));
		return Actor ? Actor->FindComponentByClass<UAudioComponent>() : nullptr;
	};
	UAudioComponent* Audio = FindAudio();
	if (!TestNotNull(TEXT("real Level AudioComponent exists"), Audio)) return false;
	Audio->bOverrideAttenuation = true;
	FRichCurve& Initial = *Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurve();
	Initial.SetDefaultValue(0.25f);
	Initial.PreInfinityExtrap = RCCE_Linear;
	Initial.PostInfinityExtrap = RCCE_Constant;
	Initial.AddKey(0.f, 0.2f);
	Initial.AddKey(1.f, 0.8f);
	FRichCurve Expected = Initial;
	Expected.Keys[0].Value = 0.4f;
	const FRichCurve HighpassBefore = *Audio->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst();
	UWorld* World = Audio->GetWorld();
	World->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("native fixture baseline persists"), UPackage::SavePackage(World->GetPackage(), World, *Filename, SaveArgs))) return false;
	if (!TryBuildExportParams(ObjectPath, Workspace / TEXT("edit"), Params, Error)
		|| !Submit(TEXT("curve-export"), TEXT("content_graph.export_workspace"))) return false;

	auto FindObjectField = [](const TSharedPtr<FJsonObject>& Root, const FString& Name) -> TSharedPtr<FJsonObject>
	{
		TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)> Visit;
		Visit = [&](const TSharedPtr<FJsonObject>& Node) -> TSharedPtr<FJsonObject>
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Node->TryGetObjectField(Name, Found)) return *Found;
			for (const auto& Field : Node->Values)
				if (Field.Value.IsValid() && Field.Value->Type == EJson::Object)
					if (auto Nested = Visit(Field.Value->AsObject()); Nested.IsValid()) return Nested;
			return nullptr;
		};
		return Visit(Root);
	};
	auto InspectWorkspace = [&](const FString& Directory, bool bEdit) -> bool
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("definitions.hjson"), true, false);
		FString LowRef, HighRef;
		TMap<FString, TSharedPtr<FJsonObject>> Leaves;
		int32 RuntimeDefinitions = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Definitions = nullptr;
			if (!Document->TryGetObjectField(TEXT("definitions"), Definitions)) continue;
			for (const auto& Item : (*Definitions)->Values)
			{
				const auto Definition = Item.Value->AsObject();
				FString Ref;
				if (Definition->TryGetStringField(TEXT("ref"), Ref))
				{
					if (!TestFalse(TEXT("public definition references are unique"), Leaves.Contains(Ref))) return false;
					Leaves.Add(Ref, Definition);
					FString Name;
					if (Definition->TryGetStringField(TEXT("name"), Name) && Name == TEXT("RuntimeFloatCurve")) ++RuntimeDefinitions;
				}
				const TSharedPtr<FJsonObject>* Value = nullptr;
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (Definition->TryGetObjectField(TEXT("value_definition"), Value)
					&& (*Value)->TryGetObjectField(TEXT("fields"), Fields))
				{
					(*Fields)->TryGetStringField(TEXT("CustomLowpassAirAbsorptionCurve"), LowRef);
					(*Fields)->TryGetStringField(TEXT("CustomHighpassAirAbsorptionCurve"), HighRef);
				}
			}
		}
		if (!TestTrue(TEXT("both nested fields resolve one readable leaf definition"), !LowRef.IsEmpty()
			&& LowRef.Equals(HighRef, ESearchCase::CaseSensitive) && Leaves.Contains(LowRef)
			&& LowRef.StartsWith(TEXT("collection:")) && !LowRef.Contains(TEXT("property_definition:")))) return false;
		const auto Contract = Leaves.FindChecked(LowRef)->GetObjectField(TEXT("value_definition"));
		if (!TestEqual(TEXT("same RuntimeFloat contract is defined once in this export"), RuntimeDefinitions, 1)) return false;
		if (!TestTrue(TEXT("shared definition contains legal options and key template without table mode"),
			Contract->HasField(TEXT("interp_mode")) && Contract->HasField(TEXT("tangent_mode"))
			&& Contract->HasField(TEXT("tangent_weight_mode")) && Contract->HasField(TEXT("key_defaults"))
			&& !Contract->HasField(TEXT("table_curve_mode")))) return false;
		const auto Defaults = Contract->GetObjectField(TEXT("key_defaults"));
		for (const TCHAR* Key : {TEXT("time"), TEXT("value"), TEXT("interp_mode"), TEXT("tangent_mode"),
			TEXT("tangent_weight_mode"), TEXT("arrive_tangent"), TEXT("leave_tangent"), TEXT("arrive_tangent_weight"), TEXT("leave_tangent_weight")})
			if (!TestTrue(FString(TEXT("key template includes ")) + Key, Defaults->HasField(Key))) return false;
		if (!TestTrue(TEXT("legal Rich interpolation and weighted tangents are readable"),
			Contract->GetArrayField(TEXT("interp_mode")).ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("Cubic"); })
			&& Contract->GetArrayField(TEXT("tangent_weight_mode")).ContainsByPredicate([](const auto& V) { return V->AsString() == TEXT("WeightedBoth"); }))) return false;
		Files.Reset();
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("properties.hjson"), true, false);
		int32 FoundCurves = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const auto Curve = FindObjectField(Document, TEXT("CustomLowpassAirAbsorptionCurve"));
			if (!Curve.IsValid()) continue;
			++FoundCurves;
			if (!TestTrue(TEXT("ordinary RuntimeFloat has no diagnostic or unit assumption"),
				!Curve->HasField(TEXT("storage")) && !Curve->HasField(TEXT("carrier_cpp_type"))
				&& !Curve->HasField(TEXT("time_domain")) && !Curve->HasField(TEXT("value_definition")))) return false;
			const auto Channel = Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"));
			if (!TestFalse(TEXT("channel has no diagnostic key count"), Channel->HasField(TEXT("key_count")))) return false;
			const auto& Keys = Channel->GetArrayField(TEXT("keys"));
			if (!TestEqual(TEXT("two actual keys are visible"), Keys.Num(), 2)) return false;
			if (!TestFalse(TEXT("key has no duplicate seconds coordinate"), Keys[0]->AsObject()->HasField(TEXT("time_seconds")))) return false;
			if (!TestFalse(TEXT("key has no diagnostic index"), Keys[0]->AsObject()->HasField(TEXT("key_index")))) return false;
			if (bEdit)
			{
				Keys[0]->AsObject()->SetNumberField(TEXT("value"), 0.4);
				const auto Override = FindObjectField(Document, TEXT("bOverrideAttenuation"));
				if (!TestTrue(TEXT("override is available in the public property document"), Override.IsValid())) return false;
				Override->SetBoolField(TEXT("value"), true);
				FString Edited;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Edited, Error)
					|| !FFileHelper::SaveStringToFile(Edited, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			else if (!TestTrue(TEXT("reexport reads the edited normalized curve value"), FMath::IsNearlyEqual(Keys[0]->AsObject()->GetNumberField(TEXT("value")), 0.4, 0.00001))) return false;
		}
		return TestEqual(TEXT("exactly one target curve is found in ordinary properties"), FoundCurves, 1);
	};
	if (!TestTrue(TEXT("export restores original different editor map"), OriginalEditorPath != ObjectPath
		&& GEditor->GetEditorWorldContext().World()->GetPathName() == OriginalEditorPath)) return false;
	if (!InspectWorkspace(Workspace / TEXT("edit"), true)) { AddError(Error); return false; }
	if (!TryBuildDirectoryImportParams(ObjectPath, Workspace / TEXT("edit"), Params, Error)
		|| !Submit(TEXT("curve-import"), TEXT("content_graph.import_workspace"))) return false;
	if (!TestTrue(TEXT("successful public import restores original editor map identity"),
		GEditor->GetEditorWorldContext().World()->GetPathName() == OriginalEditorPath)) return false;
	Audio = FindAudio();
	if (!TestNotNull(TEXT("persistent readback resolves AudioComponent"), Audio)) return false;
	const FRichCurve& Actual = *Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurveConst();
	TestTrue(TEXT("complete native curve equals baseline with only requested key edit"), Actual == Expected);
	TestFalse(TEXT("published map is clean"), Audio->GetPackage()->IsDirty());
	TestTrue(TEXT("map file exists"), IFileManager::Get().FileExists(*Filename));
	TestTrue(TEXT("override is retained"), Audio->bOverrideAttenuation);
	TestEqual(TEXT("default value is retained"), Actual.GetDefaultValue(), 0.25f);
	TestTrue(TEXT("extrapolation retained"), Actual.PreInfinityExtrap == RCCE_Linear && Actual.PostInfinityExtrap == RCCE_Constant);
	TestEqual(TEXT("native key count retained"), Actual.GetNumKeys(), 2);
	if (Actual.GetNumKeys() != 2) return false;
	TestTrue(TEXT("native normalized coordinates reflect only requested edit"), Actual.Keys[0].Time == 0.f
		&& FMath::IsNearlyEqual(Actual.Keys[0].Value, 0.4f) && Actual.Keys[1].Time == 1.f && Actual.Keys[1].Value == 0.8f);
	TestTrue(TEXT("unmodified highpass curve is retained"), *Audio->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst() == HighpassBefore);
	if (!TryBuildExportParams(ObjectPath, Workspace / TEXT("readback"), Params, Error)
		|| !Submit(TEXT("curve-reexport"), TEXT("content_graph.export_workspace"))) return false;
	if (!InspectWorkspace(Workspace / TEXT("readback"), false)) { AddError(Error); return false; }
	return !HasAnyErrors();
}

namespace
{
	class FRuntimeCurveStageObserver final : public UeAgentAuthoringOperationApply::IDurablePackageStager
	{
	public:
		TFunction<bool(const TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>&,
			UeAgentAuthoringOperationApply::FOperationFailure&)> Observe;
		TSharedRef<UeAgentAuthoringOperationApply::IDurablePackageStager> Native =
			UeAgentAuthoringOperationApply::CreateDefaultDurablePackageStager();
		bool BuildArtifactInventory(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringOperationApply::FOperationFailure& Failure) override
		{ return Native->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringOperationApply::FOperationFailure& Failure) override
		{ return Native->StagePackages(Plans, Artifacts, Failure) && Observe(Artifacts, Failure); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85LevelRuntimeFloatLifecycleTest,
	"UAI.Root.T85.Level.RuntimeFloatCurveLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85LevelRuntimeFloatLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentAuthoringOperationApply;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85RuntimeCurve/L_CurveLifecycle_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85") / FPackageName::GetLongPackageAssetName(PackageName));
	UWorld* OriginalEditorWorld = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("editor context exists"), OriginalEditorWorld)
		|| !TestFalse(TEXT("fixture does not switch away from user dirty map"), OriginalEditorWorld->GetPackage()->IsDirty())) return false;
	const FString OriginalEditorPackage = OriginalEditorWorld->GetPackage()->GetName();
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("owned map is deleted by UE"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		if (GEditor->GetEditorWorldContext().World()->GetPackage()->GetName() != OriginalEditorPackage)
		{
			const FString OriginalFile = FPackageName::LongPackageNameToFilename(OriginalEditorPackage, FPackageName::GetMapPackageExtension());
			TestNotNull(TEXT("fixture restores original editor map"), UEditorLoadingAndSavingUtils::LoadMap(OriginalFile));
		}
	};
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	auto Submit = [&](const TCHAR* Tag, const TCHAR* Command) {
		return SubmitPublicCommand(Registry, Dispatch, Tag, Command, Params);
	};
	if (!TryBuildLevelImportParams(ObjectPath, TEXT("CurveLifecycleActor"), TEXT("CurveLifecycleActor"), TEXT("Audio"),
		TEXT("create"), TEXT("create"), TEXT("create"), FVector::ZeroVector, Params, Error, TEXT("/Script/Engine.AudioComponent"))) return false;
	const auto Created = Submit(TEXT("curve-lifecycle-create"), TEXT("content_graph.import_workspace"));
	if (!Created.IsOk()) { AddError(Created.GetErrorJson()); return false; }
	if (!TestNotNull(TEXT("activate owned map"), UEditorLoadingAndSavingUtils::LoadMap(Filename))) return false;
	auto FindAudio = [&]() -> UAudioComponent* {
		AActor* Actor = FindActorByManagedId(LoadObject<UWorld>(nullptr, *ObjectPath), TEXT("CurveLifecycleActor"));
		return Actor ? Actor->FindComponentByClass<UAudioComponent>() : nullptr;
	};
	UAudioComponent* Audio = FindAudio();
	if (!TestNotNull(TEXT("native audio exists"), Audio)) return false;
	Audio->bOverrideAttenuation = true;
	FRichCurve& Curve = *Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurve();
	Curve.SetDefaultValue(0.15f); Curve.PreInfinityExtrap = RCCE_Linear; Curve.PostInfinityExtrap = RCCE_Cycle;
	Curve.AddKey(0.f, 0.2f); Curve.AddKey(1.f, 0.8f);
	Curve.Keys[0].InterpMode = RCIM_Cubic; Curve.Keys[0].TangentMode = RCTM_User;
	Curve.Keys[0].LeaveTangent = 0.5f;
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("persist different disk baseline"), UPackage::SavePackage(Audio->GetPackage(), Audio->GetWorld(), *Filename, SaveArgs))) return false;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	Curve.Keys[0].Value = 0.3f; Curve.SetDefaultValue(0.35f); Audio->MarkPackageDirty();
	const FRichCurve DirtyBefore = Curve;
	const FRichCurve HighBefore = *Audio->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst();
	FRichCurve Desired = DirtyBefore; Desired.Keys[0].Value = 0.65f;
	auto CheckBefore = [&]() -> bool {
		UAudioComponent* Current = FindAudio();
		TArray<uint8> Bytes;
		return TestTrue(TEXT("original dirty native curve and override restored"), Current && Current->bOverrideAttenuation
			&& Current->GetPackage()->IsDirty() && *Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurveConst() == DirtyBefore
			&& *Current->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst() == HighBefore)
			&& TestTrue(TEXT("original independent disk bytes retained"), FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore)
			&& TestTrue(TEXT("original active target editor context retained"), GEditor->GetEditorWorldContext().World()->GetPathName() == ObjectPath);
	};
	// Build the same native property payload consumed by the existing Level apply entry.
	FSoundAttenuationSettings Proposed = FindAudio()->AttenuationOverrides;
	*Proposed.CustomLowpassAirAbsorptionCurve.GetRichCurve() = Desired;
	FProperty* Property = FindFProperty<FProperty>(UAudioComponent::StaticClass(), TEXT("AttenuationOverrides"));
	const auto Value = UeAgentPropertyValueCodec::Encode(Property, &Proposed, FindAudio(), UeAgentPropertyValueCodec::FOptions());
	if (!TestTrue(TEXT("shared codec produces desired value"), Value.IsValid())) return false;
	const auto Descriptor = MakeShared<FJsonObject>(); Descriptor->SetStringField(TEXT("property_path"), TEXT("AttenuationOverrides")); Descriptor->SetField(TEXT("value"), Value);
	const FString ApplyWorldPath = UeAgentLevelAuthoring::GetWorldPath(*FindAudio()->GetWorld());
	const auto Target = MakeShared<FJsonObject>(); Target->SetStringField(TEXT("world_path"), ApplyWorldPath);
	Target->SetStringField(TEXT("owner_managed_id"), TEXT("CurveLifecycleActor")); Target->SetStringField(TEXT("component_name"), TEXT("Audio"));
	Target->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.AudioComponent"));
	const auto Component = MakeShared<FJsonObject>(); Component->SetStringField(TEXT("operation"), TEXT("update"));
	Component->SetObjectField(TEXT("target"), Target); Component->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Descriptor)});
	const auto WorldUnit = MakeShared<FJsonObject>(); WorldUnit->SetStringField(TEXT("operation"), TEXT("update"));
	const auto WorldTarget = MakeShared<FJsonObject>(); WorldTarget->SetStringField(TEXT("world_path"), ApplyWorldPath); WorldUnit->SetObjectField(TEXT("target"), WorldTarget);
	const auto Units = MakeShared<FJsonObject>(); Units->SetObjectField(TEXT("index.hjson"), WorldUnit);
	Units->SetObjectField(TEXT("actors/items/actor_0000/components/component_0000/component.hjson"), Component);
	const auto Bundle = MakeShared<FJsonObject>(); Bundle->SetStringField(TEXT("schema"), TEXT("uai.virtual_workspace.v1")); Bundle->SetObjectField(TEXT("units"), Units);
	const auto Source = UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Bundle, TEXT("inline://runtime-curve-lifecycle"));
	bool bEarlyObserved = false;
	const auto Stager = MakeShared<FRuntimeCurveStageObserver>();
	Stager->Observe = [&](const auto& Artifacts, FOperationFailure& Failure) {
		UAudioComponent* Current = FindAudio();
		if (!Current || !(*Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurveConst() == Desired)) return true;
		int32 Staged = 0;
		for (const auto& Artifact : Artifacts) {
			TestEqual(TEXT("optional staged artifact matches actual presence"), IFileManager::Get().FileExists(*Artifact.StagingPath), Artifact.bStagedExists);
			if (Artifact.bStagedExists) ++Staged;
		}
		TArray<uint8> Bytes;
		bEarlyObserved = Staged > 0 && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore;
		TestTrue(TEXT("real stage follows native change but precedes disk publication"), bEarlyObserved);
		Failure.Diagnostic.Code = TEXT("test_curve_early_rejection"); return false;
	};
	UeAgentLevelAuthoring::FWorkspaceApplyOptions Options; Options.bPersistentReadback = true; Options.PackageStager = Stager;
	UeAgentLevelAuthoring::FWorkspaceApplyResult Result;
	const bool bEarlyApplied = UeAgentLevelAuthoring::ApplyWorkspaceSource(*FindAudio()->GetWorld(), Source, Options, Result);
	AddInfo(FString::Printf(TEXT("early phase=%s code=%s detail=%s rollback=%d/%d observed=%d"),
		*Result.Phase, *Result.Diagnostic.Code, *Result.Diagnostic.InternalContext, Result.bRollbackAttempted, Result.bRollbackSucceeded, bEarlyObserved));
	if (!TestFalse(TEXT("stage rejection fails apply"), bEarlyApplied)
		|| !TestTrue(TEXT("early restore completed"), bEarlyObserved && Result.bRollbackAttempted && Result.bRollbackSucceeded) || !CheckBefore()) return false;
	Options.PackageStager.Reset();
	bool bLateObserved = false;
	Options.FinalReadback = [&](FOperationFailure& Failure) {
		UAudioComponent* Current = FindAudio(); TArray<uint8> Bytes;
		bLateObserved = Current && !Current->GetPackage()->IsDirty()
			&& *Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurveConst() == Desired
			&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore;
		TestTrue(TEXT("late rejection observes clean reloaded native and different published disk"), bLateObserved);
		Failure.Diagnostic.Code = TEXT("test_curve_late_rejection"); return false;
	};
	const bool bLateApplied = UeAgentLevelAuthoring::ApplyWorkspaceSource(*FindAudio()->GetWorld(), Source, Options, Result);
	AddInfo(FString::Printf(TEXT("late phase=%s code=%s detail=%s rollback=%d/%d observed=%d"),
		*Result.Phase, *Result.Diagnostic.Code, *Result.Diagnostic.InternalContext, Result.bRollbackAttempted, Result.bRollbackSucceeded, bLateObserved));
	if (!TestFalse(TEXT("late rejection fails apply"), bLateApplied)
		|| !TestTrue(TEXT("late restore completed"), bLateObserved && Result.bRollbackAttempted && Result.bRollbackSucceeded) || !CheckBefore()) return false;
	return !HasAnyErrors();
}

namespace
{
	class FDeclaredPackageState final : public UeAgentAuthoringOperationApply::IPreparedOperationState
	{
	public:
		UeAgentAuthoringOperationApply::FPackageDurablePlan Plan;
		explicit FDeclaredPackageState(const UeAgentAuthoringOperationApply::FPackageDurablePlan& InPlan) : Plan(InPlan) {}
		FString GetTargetKey() const override { return Plan.PlanId; }
		UeAgentApplyInputContract::EOperation GetResolvedOperation() const override { return UeAgentApplyInputContract::EOperation::Update; }
		bool BuildDurablePackagePlans(TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
			UeAgentAuthoringOperationApply::FOperationFailure&) const override { Plans = {Plan}; return true; }
		bool CaptureRollback(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool ApplyProposed(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool ApplyLifecycle(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool Compile(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool Save(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool Rollback(UeAgentAuthoringOperationApply::FOperationFailure&) override { return false; }
		bool BuildExpectedReadRequests(TArray<UeAgentAuthoringState::FReadRequest>&,
			UeAgentAuthoringOperationApply::FOperationFailure&) const override { return false; }
		bool BuildActualReadRequests(TArray<UeAgentAuthoringState::FReadRequest>&,
			UeAgentAuthoringOperationApply::FOperationFailure&) const override { return false; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85PackagePlanAggregationTest,
	"UAI.Root.T85.Level.PackagePlanAggregation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PackagePlanAggregationTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringOperationApply;
	using namespace UeAgentAuthoringWorkspaceTransaction;
	FPackageDurablePlan Plan;
	Plan.PlanId = TEXT("level-plan"); Plan.PackageName = TEXT("/Game/UAI/Transient/T85PlanInventory");
	Plan.BeforePresence = EPackageBeforePresence::Absent;
	FBackendOptions Options; Options.bUseEditorTransaction = false;
	FOperationTransactionBackend Backend(Options);
	FOperationFailure Failure;
	UeAgentAuthoringWorkspaceRouter::FWorkspace Workspace;
	UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
	if (!TestTrue(TEXT("open original backend"), Backend.BeginWorkspace(Workspace, Graph, Failure))) return false;
	ON_SCOPE_EXIT { FOperationFailure EndFailure; Backend.EndWorkspace(false, EndFailure); };
	auto Inventory = [&](const FPackageDurablePlan& Second, TArray<FDurableArtifact>& Artifacts) {
		FPreparedOperationMutation First(TEXT("first"), {}, {}, MakeUnique<FDeclaredPackageState>(Plan));
		FPreparedOperationMutation Other(TEXT("second"), {}, {}, MakeUnique<FDeclaredPackageState>(Second));
		return Backend.BuildDurableArtifactInventory({&First, &Other}, Artifacts, Failure);
	};
	TArray<FDurableArtifact> Artifacts;
	if (!TestTrue(TEXT("identical declarations share one inventory"), Inventory(Plan, Artifacts))
		|| !TestEqual(TEXT("one header and optional payload for one package"), Artifacts.Num(), 2)) return false;
	for (int32 Difference = 0; Difference < 6; ++Difference) {
		FPackageDurablePlan Other = Plan;
		switch (Difference) {
		case 0: Other.PlanId = TEXT("other-plan"); break;
		case 1: Other.Intent = EPackageDurableIntent::Delete; break;
		case 2: Other.Package = GetTransientPackage(); break;
		case 3: Other.TopLevelAsset = GetTransientPackage(); break;
		case 4: Other.ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage; break;
		case 5: Other.BeforePresence = EPackageBeforePresence::Present; break;
		}
		if (!TestFalse(TEXT("conflicting declaration rejected before staging"), Inventory(Other, Artifacts))
			|| !TestTrue(TEXT("failed inventory publishes no partial artifacts"), Artifacts.IsEmpty())
			|| !TestEqual(TEXT("specific conflict family preserved"), Failure.Diagnostic.Code,
				FString(Difference >= 4 ? TEXT("durable_package_readback_policy_conflict") : TEXT("durable_package_plan_conflict")))) return false;
	}
	const auto Stager = CreateDefaultDurablePackageStager();
	TestFalse(TEXT("stager still rejects duplicate physical inventory outside aggregation"), Stager->BuildArtifactInventory({Plan, Plan}, Artifacts, Failure));
	TestEqual(TEXT("stager duplicate protection retained"), Failure.Diagnostic.Code, FString(TEXT("durable_package_plan_duplicate")));
	TestTrue(TEXT("stager duplicate rejection has no partial output"), Artifacts.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85RuntimeFloatReferenceTest,
	"UAI.Root.T85.Level.RuntimeFloatCurveReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85RuntimeFloatReferenceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85RuntimeCurve/L_Reference_") + Suffix;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString CurvePackage = TEXT("/Game/UAI/Transient/T85RuntimeCurve/C_Reference_") + Suffix;
	const FString CurvePath = CurvePackage + TEXT(".") + FPackageName::GetLongPackageAssetName(CurvePackage);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	const FString CurveFile = FPackageName::LongPackageNameToFilename(CurvePackage, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85") / Suffix);
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath); DeleteAssetIfPresent(CurvePath);
		TestFalse(TEXT("owned map removed by UE"), IFileManager::Get().FileExists(*Filename));
		TestFalse(TEXT("owned reference removed by UE"), IFileManager::Get().FileExists(*CurveFile));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const TCHAR* Tag, const TCHAR* Command) {
		const auto Result = SubmitPublicCommand(Registry, Dispatch, Tag, Command, Params);
		if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; } return true;
	};
	if (!TryBuildLevelImportParams(ObjectPath, TEXT("ReferenceActor"), TEXT("ReferenceActor"), TEXT("Audio"),
		TEXT("create"), TEXT("create"), TEXT("create"), FVector::ZeroVector, Params, Error, TEXT("/Script/Engine.AudioComponent"))
		|| !Submit(TEXT("reference-create"), TEXT("content_graph.import_workspace"))) return false;
	auto FindAudio = [&]() -> UAudioComponent* {
		AActor* Actor = FindActorByManagedId(LoadObject<UWorld>(nullptr, *ObjectPath), TEXT("ReferenceActor"));
		return Actor ? Actor->FindComponentByClass<UAudioComponent>() : nullptr;
	};
	UCurveFloat* External = NewObject<UCurveFloat>(CreatePackage(*CurvePackage), FName(*FPackageName::GetLongPackageAssetName(CurvePackage)), RF_Public | RF_Standalone);
	External->FloatCurve.AddKey(0.f, 0.9f); External->FloatCurve.AddKey(1.f, 0.1f);
	const FRichCurve ExternalBefore = External->FloatCurve;
	FAssetRegistryModule::AssetCreated(External);
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("owned UCurveFloat persists"), UPackage::SavePackage(External->GetPackage(), External, *CurveFile, SaveArgs))) return false;
	TArray<uint8> CurveDisk; if (!FFileHelper::LoadFileToArray(CurveDisk, *CurveFile)) return false;
	UAudioComponent* Audio = FindAudio(); if (!TestNotNull(TEXT("audio fixture exists"), Audio)) return false;
	Audio->bOverrideAttenuation = true;
	FRuntimeFloatCurve& Runtime = Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve;
	Runtime.EditorCurveData.SetDefaultValue(0.12f);
	Runtime.EditorCurveData.AddKey(0.f, 0.11f); Runtime.EditorCurveData.AddKey(1.f, 0.82f);
	const FRichCurve LatentBefore = Runtime.EditorCurveData;
	Runtime.ExternalCurve = External;
	Audio->MarkPackageDirty();
	if (!TestTrue(TEXT("external runtime and latent data persist"), UPackage::SavePackage(Audio->GetPackage(), Audio->GetWorld(), *Filename, SaveArgs))) return false;
	FRichCurve EmbeddedExpected; EmbeddedExpected.SetDefaultValue(0.3f);
	EmbeddedExpected.PreInfinityExtrap = RCCE_Cycle; EmbeddedExpected.PostInfinityExtrap = RCCE_Linear;
	EmbeddedExpected.AddKey(0.f, 0.35f); EmbeddedExpected.AddKey(1.f, 0.75f);
	auto Export = [&](const FString& Directory, const TCHAR* Tag) {
		return TryBuildExportParams(ObjectPath, Directory, Params, Error) && Submit(Tag, TEXT("content_graph.export_workspace"));
	};
	// Read only ordinary definitions/properties; template values are supplied by the original curve owner.
	auto ReadContract = [&](const FString& Directory) -> TSharedPtr<FJsonObject> {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("definitions.hjson"), true, false);
		TSharedPtr<FJsonObject> Contract; int32 Count = 0; FString LowRef, HighRef, LeafRef;
		for (const FString& File : Files) {
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return nullptr;
			const TSharedPtr<FJsonObject>* Definitions = nullptr; if (!Doc->TryGetObjectField(TEXT("definitions"), Definitions)) continue;
			for (const auto& Pair : (*Definitions)->Values) {
				const auto D = Pair.Value->AsObject(); FString Name;
				if (D->TryGetStringField(TEXT("name"), Name) && Name == TEXT("RuntimeFloatCurve")) {
					++Count; Contract = D->GetObjectField(TEXT("value_definition")); LeafRef = D->GetStringField(TEXT("ref"));
				}
				const TSharedPtr<FJsonObject>* Value = nullptr; const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (D->TryGetObjectField(TEXT("value_definition"), Value) && (*Value)->TryGetObjectField(TEXT("fields"), Fields)) {
					(*Fields)->TryGetStringField(TEXT("CustomLowpassAirAbsorptionCurve"), LowRef);
					(*Fields)->TryGetStringField(TEXT("CustomHighpassAirAbsorptionCurve"), HighRef);
				}
			}
		}
		if (!TestTrue(TEXT("external and embedded fields share a single readable definition"), Count == 1 && Contract.IsValid()
			&& !LeafRef.IsEmpty() && LowRef == LeafRef && HighRef == LeafRef && !LeafRef.Contains(TEXT("property_definition:")))) return nullptr;
		const auto Templates = Contract->GetObjectField(TEXT("value_templates"));
		if (!TestTrue(TEXT("both state-independent value templates are exported"), Templates->HasField(TEXT("embedded")) && Templates->HasField(TEXT("asset_reference")))
			|| !TestEqual(TEXT("reference class is declared once in definition"), Contract->GetObjectField(TEXT("asset_reference"))->GetStringField(TEXT("asset_class")), FString(TEXT("/Script/Engine.CurveFloat")))
			|| !TestTrue(TEXT("path requires nonempty string"), Contract->GetObjectField(TEXT("asset_reference"))->GetObjectField(TEXT("asset_path"))->GetBoolField(TEXT("non_empty")))) return nullptr;
		return Contract;
	};
	auto VisitValue = [&](const FString& Directory, TFunction<bool(const TSharedPtr<FJsonObject>&)> Visit, bool bWrite) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("properties.hjson"), true, false);
		int32 Count = 0;
		for (const FString& File : Files) {
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&)> Find;
			Find = [&](const auto& Node) -> TSharedPtr<FJsonObject> {
				const TSharedPtr<FJsonObject>* Found = nullptr;
				if (Node->TryGetObjectField(TEXT("CustomLowpassAirAbsorptionCurve"), Found)) return *Found;
				for (const auto& Field : Node->Values) if (Field.Value->Type == EJson::Object)
					if (auto Child = Find(Field.Value->AsObject()); Child.IsValid()) return Child;
				return nullptr;
			};
			const auto Value = Find(Doc); if (!Value.IsValid()) continue;
			++Count; if (!Visit(Value)) return false;
			if (bWrite && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		return TestEqual(TEXT("one ordinary runtime value located"), Count, 1);
	};
	auto CheckReferenceValue = [&](const TSharedPtr<FJsonObject>& Value) {
		return TestTrue(TEXT("reference value has only schema kind and path"), Value->Values.Num() == 3
			&& Value->GetStringField(TEXT("curve_kind")) == TEXT("asset_reference")
			&& Value->GetObjectField(TEXT("asset_reference"))->Values.Num() == 1
			&& Value->GetObjectField(TEXT("asset_reference"))->GetStringField(TEXT("asset_path")) == CurvePackage);
	};
	if (!Export(Workspace / TEXT("external"), TEXT("reference-export"))) return false;
	const auto ExternalContract = ReadContract(Workspace / TEXT("external")); if (!ExternalContract.IsValid()) return false;
	Audio = FindAudio();
	if (!TestTrue(TEXT("external export preserves latent native data"), Audio && Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.EditorCurveData == LatentBefore)
		|| !VisitValue(Workspace / TEXT("external"), CheckReferenceValue, false)) return false;
	if (!VisitValue(Workspace / TEXT("external"), [&](const auto& Value) {
		Value->Values = ExternalContract->GetObjectField(TEXT("value_templates"))->GetObjectField(TEXT("embedded"))->Values;
		Value->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Cycle")); Value->SetStringField(TEXT("post_infinity_extrap"), TEXT("Linear"));
		const auto Channel = Value->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"));
		const auto Default = Channel->GetObjectField(TEXT("default_value")); Default->SetBoolField(TEXT("enabled"), true); Default->SetNumberField(TEXT("value"), 0.3);
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (int32 Index = 0; Index < 2; ++Index) {
			const auto Key = MakeShared<FJsonObject>(); Key->Values = ExternalContract->GetObjectField(TEXT("key_defaults"))->Values;
			Key->SetNumberField(TEXT("time"), Index); Key->SetNumberField(TEXT("value"), Index == 0 ? 0.35 : 0.75); Keys.Add(MakeShared<FJsonValueObject>(Key));
		}
		Channel->SetArrayField(TEXT("keys"), Keys); return true;
	}, true)) return false;
	if (!TryBuildDirectoryImportParams(ObjectPath, Workspace / TEXT("external"), Params, Error)
		|| !Submit(TEXT("reference-to-embedded"), TEXT("content_graph.import_workspace"))) return false;
	Audio = FindAudio();
	if (!TestTrue(TEXT("embedded import clears reference and persists complete curve"), Audio && !Audio->GetPackage()->IsDirty()
		&& !Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve
		&& Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.EditorCurveData == EmbeddedExpected)) return false;
	if (!Export(Workspace / TEXT("embedded"), TEXT("embedded-export"))) return false;
	const auto EmbeddedContract = ReadContract(Workspace / TEXT("embedded")); if (!EmbeddedContract.IsValid()) return false;
	if (!VisitValue(Workspace / TEXT("embedded"), [&](const auto& Value) {
		const auto& Keys = Value->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"))->GetArrayField(TEXT("keys"));
		if (!TestTrue(TEXT("persisted embedded reexport has requested curve"), Keys.Num() == 2 && FMath::IsNearlyEqual(Keys[0]->AsObject()->GetNumberField(TEXT("value")), 0.35, 0.00001))) return false;
		Value->Values = EmbeddedContract->GetObjectField(TEXT("value_templates"))->GetObjectField(TEXT("asset_reference"))->Values;
		Value->GetObjectField(TEXT("asset_reference"))->SetStringField(TEXT("asset_path"), CurvePackage); return true;
	}, true)) return false;
	if (!TryBuildDirectoryImportParams(ObjectPath, Workspace / TEXT("embedded"), Params, Error)
		|| !Submit(TEXT("embedded-to-reference"), TEXT("content_graph.import_workspace"))) return false;
	Audio = FindAudio();
	if (!TestTrue(TEXT("reference restored with latent edited curve retained"), Audio && !Audio->GetPackage()->IsDirty()
		&& Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve
		&& Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve->GetPathName() == CurvePath
		&& Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.EditorCurveData == EmbeddedExpected)) return false;
	TArray<uint8> ExternalDisk;
	External = LoadObject<UCurveFloat>(nullptr, *CurvePath);
	if (!TestTrue(TEXT("referenced asset native and disk never edited"), External && External->FloatCurve == ExternalBefore
		&& FFileHelper::LoadFileToArray(ExternalDisk, *CurveFile) && ExternalDisk == CurveDisk)) return false;
	if (!Export(Workspace / TEXT("readback"), TEXT("reference-readback")) || !ReadContract(Workspace / TEXT("readback")).IsValid()
		|| !VisitValue(Workspace / TEXT("readback"), CheckReferenceValue, false)) return false;
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85RuntimeFloatReferenceLifecycleTest,
	"UAI.Root.T85.Level.RuntimeFloatCurveReferenceLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85RuntimeFloatReferenceLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentAuthoringOperationApply;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85RuntimeCurve/L_ReferenceLifecycle_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString CurvePackage = PackageName + TEXT("_Reference");
	const FString CurvePath = CurvePackage + TEXT(".") + FPackageName::GetLongPackageAssetName(CurvePackage);
	const FString CurveFile = FPackageName::LongPackageNameToFilename(CurvePackage, FPackageName::GetAssetPackageExtension());
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85") / FPackageName::GetLongPackageAssetName(PackageName));
	UWorld* OriginalEditorWorld = GEditor->GetEditorWorldContext().World();
	if (!TestNotNull(TEXT("editor context exists"), OriginalEditorWorld)
		|| !TestFalse(TEXT("fixture does not switch away from user dirty map"), OriginalEditorWorld->GetPackage()->IsDirty())) return false;
	const FString OriginalEditorPackage = OriginalEditorWorld->GetPackage()->GetName();
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		DeleteAssetIfPresent(CurvePath);
		TestFalse(TEXT("owned curve removed by UE"), IFileManager::Get().FileExists(*CurveFile));
		TestFalse(TEXT("owned map is deleted by UE"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		if (GEditor->GetEditorWorldContext().World()->GetPackage()->GetName() != OriginalEditorPackage)
		{
			const FString OriginalFile = FPackageName::LongPackageNameToFilename(OriginalEditorPackage, FPackageName::GetMapPackageExtension());
			TestNotNull(TEXT("fixture restores original editor map"), UEditorLoadingAndSavingUtils::LoadMap(OriginalFile));
		}
	};
	TSharedPtr<OperationOwners::UObjectDataAsset::IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!OperationOwners::UObjectDataAsset::TryCreateProductionUObjectDataAssetCommandService(Service, Error)
		|| !Module::CommandRegistration::UObjectDataAsset::TryCreateUObjectDataAssetRegistration(Service.ToSharedRef(), Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	auto Submit = [&](const TCHAR* Tag, const TCHAR* Command) {
		return SubmitPublicCommand(Registry, Dispatch, Tag, Command, Params);
	};
	if (!TryBuildLevelImportParams(ObjectPath, TEXT("CurveLifecycleActor"), TEXT("CurveLifecycleActor"), TEXT("Audio"),
		TEXT("create"), TEXT("create"), TEXT("create"), FVector::ZeroVector, Params, Error, TEXT("/Script/Engine.AudioComponent"))) return false;
	const auto Created = Submit(TEXT("curve-lifecycle-create"), TEXT("content_graph.import_workspace"));
	if (!Created.IsOk()) { AddError(Created.GetErrorJson()); return false; }
	if (!TestNotNull(TEXT("activate owned map"), UEditorLoadingAndSavingUtils::LoadMap(Filename))) return false;
	auto FindAudio = [&]() -> UAudioComponent* {
		AActor* Actor = FindActorByManagedId(LoadObject<UWorld>(nullptr, *ObjectPath), TEXT("CurveLifecycleActor"));
		return Actor ? Actor->FindComponentByClass<UAudioComponent>() : nullptr;
	};
	UAudioComponent* Audio = FindAudio();
	if (!TestNotNull(TEXT("native audio exists"), Audio)) return false;
	Audio->bOverrideAttenuation = true;
	FRichCurve& Curve = *Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.GetRichCurve();
	Curve.SetDefaultValue(0.15f); Curve.PreInfinityExtrap = RCCE_Linear; Curve.PostInfinityExtrap = RCCE_Cycle;
	Curve.AddKey(0.f, 0.2f); Curve.AddKey(1.f, 0.8f);
	Curve.Keys[0].InterpMode = RCIM_Cubic; Curve.Keys[0].TangentMode = RCTM_User;
	Curve.Keys[0].LeaveTangent = 0.5f;
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
	UCurveFloat* External = NewObject<UCurveFloat>(CreatePackage(*CurvePackage), FName(*FPackageName::GetLongPackageAssetName(CurvePackage)), RF_Public | RF_Standalone);
	External->FloatCurve.AddKey(0.f, 0.9f); External->FloatCurve.AddKey(1.f, 0.1f);
	const FRichCurve ExternalBefore = External->FloatCurve;
	FAssetRegistryModule::AssetCreated(External);
	if (!UPackage::SavePackage(External->GetPackage(), External, *CurveFile, SaveArgs)) return false;
	TArray<uint8> CurveDisk; if (!FFileHelper::LoadFileToArray(CurveDisk, *CurveFile)) return false;
	Audio->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve = External;
	if (!TestTrue(TEXT("persist different disk baseline"), UPackage::SavePackage(Audio->GetPackage(), Audio->GetWorld(), *Filename, SaveArgs))) return false;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	Curve.Keys[0].Value = 0.3f; Curve.SetDefaultValue(0.35f); Audio->MarkPackageDirty();
	const FRichCurve DirtyBefore = Curve;
	const FRichCurve HighBefore = *Audio->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst();
	FRichCurve Desired = DirtyBefore; Desired.Keys[0].Value = 0.65f;
	auto CheckBefore = [&]() -> bool {
		UAudioComponent* Current = FindAudio();
		TArray<uint8> Bytes;
		return TestTrue(TEXT("original dirty native curve and override restored"), Current && Current->bOverrideAttenuation
			&& Current->GetPackage()->IsDirty() && Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve
			&& Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve->GetPathName() == CurvePath
			&& Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.EditorCurveData == DirtyBefore
			&& *Current->AttenuationOverrides.CustomHighpassAirAbsorptionCurve.GetRichCurveConst() == HighBefore)
			&& TestTrue(TEXT("original independent disk bytes retained"), FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore)
			&& TestTrue(TEXT("external asset content and disk remain unchanged"), LoadObject<UCurveFloat>(nullptr, *CurvePath)
				&& LoadObject<UCurveFloat>(nullptr, *CurvePath)->FloatCurve == ExternalBefore
				&& FFileHelper::LoadFileToArray(Bytes, *CurveFile) && Bytes == CurveDisk)
			&& TestTrue(TEXT("original active target editor context retained"), GEditor->GetEditorWorldContext().World()->GetPathName() == ObjectPath);
	};
	if (!TryBuildExportParams(ObjectPath, Workspace, Params, Error)) return false;
	const auto Exported = Submit(TEXT("curve-lifecycle-export"), TEXT("content_graph.export_workspace"));
	if (!Exported.IsOk()) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("properties.hjson"), true, false);
	bool bEdited = false;
	for (const FString& File : Files) {
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&, const FString&)> Find;
		Find = [&](const TSharedPtr<FJsonObject>& Node, const FString& Name) -> TSharedPtr<FJsonObject> {
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Node->TryGetObjectField(Name, Found)) return *Found;
			for (const auto& Field : Node->Values) if (Field.Value->Type == EJson::Object)
				if (auto Child = Find(Field.Value->AsObject(), Name); Child.IsValid()) return Child;
			return nullptr;
		};
		const auto Low = Find(Document, TEXT("CustomLowpassAirAbsorptionCurve"));
		if (!Low.IsValid()) continue;
		Low->GetObjectField(TEXT("asset_reference"))->SetStringField(TEXT("asset_path"), ObjectPath);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEdited = true;
	}
	if (!TestTrue(TEXT("ordinary source was edited"), bEdited) || !TryBuildDirectoryImportParams(ObjectPath, Workspace, Params, Error)) return false;
	const auto Rejected = Submit(TEXT("reference-class-invalid"), TEXT("content_graph.import_workspace"));
	TSharedPtr<FJsonObject> Rejection;
	if (!TestFalse(TEXT("wrong external class rejects public change"), Rejected.IsOk())
		|| !FUeAgentStructuredHjsonParser::ParseObject(Rejected.GetErrorJson(), TEXT("condition-result"), Rejection, Error)
		|| !TestEqual(TEXT("public nested curve rejection is preserved"), Rejection->GetStringField(TEXT("code")), FString(TEXT("CustomLowpassAirAbsorptionCurve:invalid_curve_value")))) return false;
	if (!CheckBefore()) return false;
	// Build the same native property payload consumed by the existing Level apply entry.
	FSoundAttenuationSettings Proposed = FindAudio()->AttenuationOverrides;
	Proposed.CustomLowpassAirAbsorptionCurve.ExternalCurve = nullptr;
	Proposed.CustomLowpassAirAbsorptionCurve.EditorCurveData = Desired;
	FProperty* Property = FindFProperty<FProperty>(UAudioComponent::StaticClass(), TEXT("AttenuationOverrides"));
	const auto Value = UeAgentPropertyValueCodec::Encode(Property, &Proposed, FindAudio(), UeAgentPropertyValueCodec::FOptions());
	if (!TestTrue(TEXT("shared codec produces desired value"), Value.IsValid())) return false;
	const auto Descriptor = MakeShared<FJsonObject>(); Descriptor->SetStringField(TEXT("property_path"), TEXT("AttenuationOverrides")); Descriptor->SetField(TEXT("value"), Value);
	const FString ApplyWorldPath = UeAgentLevelAuthoring::GetWorldPath(*FindAudio()->GetWorld());
	const auto Target = MakeShared<FJsonObject>(); Target->SetStringField(TEXT("world_path"), ApplyWorldPath);
	Target->SetStringField(TEXT("owner_managed_id"), TEXT("CurveLifecycleActor")); Target->SetStringField(TEXT("component_name"), TEXT("Audio"));
	Target->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.AudioComponent"));
	const auto Component = MakeShared<FJsonObject>(); Component->SetStringField(TEXT("operation"), TEXT("update"));
	Component->SetObjectField(TEXT("target"), Target); Component->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Descriptor)});
	const auto WorldUnit = MakeShared<FJsonObject>(); WorldUnit->SetStringField(TEXT("operation"), TEXT("update"));
	const auto WorldTarget = MakeShared<FJsonObject>(); WorldTarget->SetStringField(TEXT("world_path"), ApplyWorldPath); WorldUnit->SetObjectField(TEXT("target"), WorldTarget);
	const auto Units = MakeShared<FJsonObject>(); Units->SetObjectField(TEXT("index.hjson"), WorldUnit);
	Units->SetObjectField(TEXT("actors/items/actor_0000/components/component_0000/component.hjson"), Component);
	const auto Bundle = MakeShared<FJsonObject>(); Bundle->SetStringField(TEXT("schema"), TEXT("uai.virtual_workspace.v1")); Bundle->SetObjectField(TEXT("units"), Units);
	const auto Source = UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Bundle, TEXT("inline://runtime-curve-lifecycle"));
	UeAgentLevelAuthoring::FWorkspaceApplyOptions Options; Options.bPersistentReadback = true;
	UeAgentLevelAuthoring::FWorkspaceApplyResult Result;
	bool bLateObserved = false;
	Options.FinalReadback = [&](FOperationFailure& Failure) {
		UAudioComponent* Current = FindAudio(); TArray<uint8> Bytes;
		bLateObserved = Current && !Current->GetPackage()->IsDirty()
			&& !Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.ExternalCurve
			&& Current->AttenuationOverrides.CustomLowpassAirAbsorptionCurve.EditorCurveData == Desired
			&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore;
		TestTrue(TEXT("late rejection observes clean reloaded native and different published disk"), bLateObserved);
		Failure.Diagnostic.Code = TEXT("test_reference_late_rejection"); return false;
	};
	const bool bLateApplied = UeAgentLevelAuthoring::ApplyWorkspaceSource(*FindAudio()->GetWorld(), Source, Options, Result);
	AddInfo(FString::Printf(TEXT("late phase=%s code=%s detail=%s rollback=%d/%d observed=%d"),
		*Result.Phase, *Result.Diagnostic.Code, *Result.Diagnostic.InternalContext, Result.bRollbackAttempted, Result.bRollbackSucceeded, bLateObserved));
	if (!TestFalse(TEXT("late rejection fails apply"), bLateApplied)
		|| !TestTrue(TEXT("late restore completed"), bLateObserved && Result.bRollbackAttempted && Result.bRollbackSucceeded) || !CheckBefore()) return false;
	return !HasAnyErrors();
}
