#include "Misc/AutomationTest.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundClass.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace
{
TSharedPtr<FJsonObject> FindElementProperty(const TSharedPtr<FJsonObject>& Object)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Key == TEXT("SoundClassEffects") && Field.Value->Type == EJson::Object) return Field.Value->AsObject();
		if (Field.Value->Type == EJson::Object) if (auto Found = FindElementProperty(Field.Value->AsObject())) return Found;
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208EmptyStructElementDefinitionTest,
	"UAI.Root.T208.SoundMix.EmptyStructElementDefinition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208EmptyStructElementDefinitionTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208EmptyStruct/") + Token;
	const FString MixPackage = Base / TEXT("Mix"), ClassPackage = Base / TEXT("Class");
	const FString MixPath = MixPackage + TEXT(".Mix"), ClassPath = ClassPackage + TEXT(".Class");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-empty-struct-definition/native") / Token);
	auto* Mix = NewObject<USoundMix>(CreatePackage(*MixPackage), TEXT("Mix"), RF_Public | RF_Standalone | RF_Transactional);
	auto* SoundClass = NewObject<USoundClass>(CreatePackage(*ClassPackage), TEXT("Class"), RF_Public | RF_Standalone | RF_Transactional);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-empty-struct"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	ON_SCOPE_EXIT
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(MixPath), MakeShared<FJsonValueString>(ClassPath)}); Args->SetBoolField(TEXT("recursive"), false);
		for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Deleted = Submit(TEXT("asset.delete"), Args); if (!Deleted.IsOk()) AddError(Deleted.GetErrorJson()); }
	};
	for (UObject* Object : {static_cast<UObject*>(Mix), static_cast<UObject*>(SoundClass)})
	{
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Object->GetPackage(), Object, *FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Args)) return false;
	}
	if (!TestTrue(TEXT("original target is empty, never seeded"), Mix->SoundClassEffects.IsEmpty())) return false;
	auto Export = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), MixPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Import = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), MixPath); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source);
		const auto Result = Submit(TEXT("content_graph.import_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	TSharedPtr<FJsonValue> OriginalTemplate;
	FString ReferenceField, NumberField;
	auto EditFromPublicDefinition = [&](const FString& Stage, bool bAppendAndReorder) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		TMap<FString, TSharedPtr<FJsonObject>> DefinitionsByRef, DefinitionsByName;
		TSharedPtr<FJsonObject> Property, PropertyDocument; FString PropertyFile;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) { AddError(Error); return false; }
			const TSharedPtr<FJsonObject>* Definitions = nullptr;
			if (Document->TryGetObjectField(TEXT("definitions"), Definitions))
				for (const auto& Entry : (*Definitions)->Values) { auto Definition = Entry.Value->AsObject(); DefinitionsByName.Add(Entry.Key, Definition); DefinitionsByRef.Add(Definition->GetStringField(TEXT("ref")), Definition); }
			if (auto Found = FindElementProperty(Document)) { Property = Found; PropertyDocument = Document; PropertyFile = File; }
		}
		if (!TestTrue(TEXT("ordinary property and definition are available"), Property && DefinitionsByName.Contains(Property->GetStringField(TEXT("definition"))))) return false;
		const auto ArrayDefinition = DefinitionsByName.FindChecked(Property->GetStringField(TEXT("definition")));
		const TSharedPtr<FJsonObject>* SchemaPtr = nullptr;
		if (!TestTrue(TEXT("empty and nonempty arrays expose element metadata"), ArrayDefinition->TryGetObjectField(TEXT("value_definition"), SchemaPtr))) return false;
		const auto Schema = *SchemaPtr;
		FString ElementRef;
		if (!TestTrue(TEXT("element definition resolves through ordinary catalog"), Schema->TryGetStringField(TEXT("element_definition"), ElementRef) && DefinitionsByRef.Contains(ElementRef))) return false;
		const auto Element = DefinitionsByRef.FindChecked(ElementRef);
		const auto Fields = Element->GetObjectField(TEXT("value_definition"))->GetObjectField(TEXT("fields"));
		const auto Template = Schema->TryGetField(TEXT("element_template"));
		if (!TestTrue(TEXT("copyable element template is independent of items"), Template && Template->Type == EJson::Object && !Template->AsObject()->HasField(TEXT("instance")))) return false;
		if (!OriginalTemplate) OriginalTemplate = Template;
		else { FString A, B; FUeAgentStructuredHjsonFormatter::TryWriteObject(OriginalTemplate->AsObject().ToSharedRef(), A, Error); FUeAgentStructuredHjsonFormatter::TryWriteObject(Template->AsObject().ToSharedRef(), B, Error); if (!TestEqual(TEXT("template is native default, not first captured item"), A, B)) return false; }
		const TSharedPtr<FJsonObject>* DefaultFields = nullptr;
		const auto Defaults = Template->AsObject()->TryGetObjectField(TEXT("fields"), DefaultFields) ? *DefaultFields : Template->AsObject();
		for (const auto& Field : Fields->Values)
		{
			const auto Definition = DefinitionsByRef.FindRef(Field.Value->AsString());
			if (!Definition) continue;
			const auto Type = Definition->GetObjectField(TEXT("type")); FString Kind, Allowed;
			Type->TryGetStringField(TEXT("kind"), Kind); Type->TryGetStringField(TEXT("allowed_class"), Allowed);
			if (Kind == TEXT("object_ref") && Allowed == TEXT("/Script/Engine.SoundClass")) ReferenceField = Field.Key;
			double DefaultNumber = 0;
			if (NumberField.IsEmpty() && Kind == TEXT("float") && Defaults->TryGetNumberField(Field.Key, DefaultNumber) && DefaultNumber == 1.0) NumberField = Field.Key;
		}
		if (!TestTrue(TEXT("legal reference and scalar fields are discoverable without source"), !ReferenceField.IsEmpty() && !NumberField.IsEmpty())) return false;
		auto Item = MakeShared<FJsonObject>(); Item->Values = Template->AsObject()->Values;
		const TSharedPtr<FJsonObject>* TemplateFields = nullptr;
		auto Members = MakeShared<FJsonObject>();
		if (Item->TryGetObjectField(TEXT("fields"), TemplateFields)) { Members->Values = (*TemplateFields)->Values; Item->SetObjectField(TEXT("fields"), Members); }
		else Members = Item;
		Members->SetStringField(ReferenceField, ClassPath); Members->SetNumberField(NumberField, bAppendAndReorder ? 0.5 : 0.25);
		TArray<TSharedPtr<FJsonValue>> Desired{MakeShared<FJsonValueObject>(Item)};
		if (bAppendAndReorder) { const auto Existing = Property->GetArrayField(TEXT("value")); if (!TestEqual(TEXT("captured item remains one before insertion"), Existing.Num(), 1)) return false; Desired.Append(Existing); }
		Property->SetArrayField(TEXT("value"), Desired);
		FString Text; return FUeAgentStructuredHjsonFormatter::TryWriteObject(PropertyDocument.ToSharedRef(), Text, Error) && FFileHelper::SaveStringToFile(Text, *PropertyFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if (!Export(TEXT("empty")) || !EditFromPublicDefinition(TEXT("empty"), false)) return false;
	TestTrue(TEXT("definition export leaves real empty target and dirty state unchanged"), Mix->SoundClassEffects.IsEmpty() && !Mix->GetPackage()->IsDirty());
	TWeakObjectPtr<USoundMix> Before(Mix);
	if (!Import(TEXT("empty"))) return false;
	Mix = LoadObject<USoundMix>(nullptr, *MixPath);
	if (!TestTrue(TEXT("first item independently saved and reloaded"), !Before.IsValid() && Mix && Mix->SoundClassEffects.Num() == 1 && Mix->SoundClassEffects[0].SoundClassObject == SoundClass && !Mix->GetPackage()->IsDirty())) return false;
	if (!Export(TEXT("nonempty")) || !EditFromPublicDefinition(TEXT("nonempty"), true) || !Import(TEXT("nonempty"))) return false;
	Mix = LoadObject<USoundMix>(nullptr, *MixPath);
	const auto* Number = FindFProperty<FFloatProperty>(FSoundClassAdjuster::StaticStruct(), *NumberField);
	TestTrue(TEXT("new item and prior item retain authored order and values"), Mix && Mix->SoundClassEffects.Num() == 2 && Number
		&& Number->GetPropertyValue_InContainer(&Mix->SoundClassEffects[0]) == 0.5f && Number->GetPropertyValue_InContainer(&Mix->SoundClassEffects[1]) == 0.25f
		&& Mix->SoundClassEffects[0].SoundClassObject == SoundClass && Mix->SoundClassEffects[1].SoundClassObject == SoundClass);
	return !HasAnyErrors();
}
