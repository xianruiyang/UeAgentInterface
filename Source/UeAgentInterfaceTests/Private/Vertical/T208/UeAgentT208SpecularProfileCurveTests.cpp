#include "Misc/AutomationTest.h"
#include "Engine/SpecularProfile.h"
#include "Curves/CurveLinearColor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindSpecularCurve(const TSharedPtr<FJsonValue>& Value, const FString& Name)
{
	if (!Value) return nullptr;
	if (Value->Type == EJson::Array)
		for (const auto& Item : Value->AsArray()) if (auto Found = FindSpecularCurve(Item, Name)) return Found;
	if (Value->Type != EJson::Object) return nullptr;
	for (const auto& Field : Value->AsObject()->Values)
	{
		if (Field.Key == Name && Field.Value->Type == EJson::Object)
		{
			const auto Object = Field.Value->AsObject();
			if (Object->HasField(TEXT("curve_kind"))) return Object;
			if (Object->HasTypedField<EJson::Object>(TEXT("value"))) return Object->GetObjectField(TEXT("value"));
		}
		if (auto Found = FindSpecularCurve(Field.Value, Name)) return Found;
	}
	return nullptr;
}
bool SameColor(const FRuntimeCurveLinearColor& A, const FRuntimeCurveLinearColor& B)
{
	if (A.ExternalCurve != B.ExternalCurve) return false;
	for (int32 I = 0; I < 4; ++I) if (!(A.ColorCurves[I] == B.ColorCurves[I])) return false;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SpecularProfileCurveTest,
	"UAI.Root.T208.SpecularProfile.RuntimeColorWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208SpecularProfileCurveTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208SpecularCurve/SP_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString ExternalPackage = TEXT("/Game/UAI/Transient/T208SpecularCurve/CC_") + Token;
	const FString ExternalPath = ExternalPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(ExternalPackage);
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-specular-profile-curve/native") / Token);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-specular"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	auto Save = [&](UObject* Object) {
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Object->GetPackage(), Object,
			*FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Args); };
	ON_SCOPE_EXIT
	{
		for (const auto& Path : {ObjectPath, ExternalPath})
		{
			if (auto* Object = FindObject<UObject>(nullptr, *Path); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("self fixture settles"), Save(Object));
			if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path))) continue;
			auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Path)}); Args->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
			TestFalse(TEXT("own package removed"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)));
		}
	};
	// Native setup supplies discriminating inactive and multi-channel data; edits below use ordinary public files.
	auto Create = MakeShared<FJsonObject>(); Create->SetStringField(TEXT("factory"), TEXT("Specular Profile")); Create->SetStringField(TEXT("target_ref"), ObjectPath);
	const auto Created = Submit(TEXT("asset.object.create"), Create);
	if (!TestTrue(TEXT("public native factory creates profile"), Created.IsOk())) { AddError(Created.GetErrorJson()); return false; }
	auto* Profile = LoadObject<USpecularProfile>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("factory initializes native Guid"), Profile && Profile->Guid.IsValid())) return false;
	auto* External = NewObject<UCurveLinearColor>(CreatePackage(*ExternalPackage), *FPackageName::GetLongPackageAssetName(ExternalPackage), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(External);
	for (int32 I = 0; I < 4; ++I)
	{
		auto& Curve = Profile->Settings.ViewColor.ColorCurves[I]; Curve.Reset(); Curve.SetDefaultValue(0.15f + I);
		Curve.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(I); Curve.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(5 - I);
		for (float X : {-0.25f, 0.5f, 1.25f})
		{
			auto& Key = Curve.GetKey(Curve.AddKey(X, X + I)); Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_User;
			Key.TangentWeightMode = RCTWM_WeightedBoth; Key.ArriveTangent = 0.2f + I; Key.LeaveTangent = 0.3f + I;
			Key.ArriveTangentWeight = 0.1f; Key.LeaveTangentWeight = 0.2f;
		}
		Profile->Settings.LightColor.ColorCurves[I] = Curve;
		External->FloatCurves[I].AddKey(0.2f, 7.f + I);
	}
	if (!Save(Profile) || !Save(External)) return false;
	const auto Light = Profile->Settings.LightColor;
	const auto Format = Profile->Settings.Format; const auto Guid = Profile->Guid;
	const FString ExternalFilename = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	// AssetCreated broadcasts the in-memory asset; register this native setup's
	// saved file before any unload/GC.
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({ExternalFilename}, true);
	TArray<uint8> ExternalBytes; FFileHelper::LoadFileToArray(ExternalBytes, *ExternalFilename);
	auto Expected = Profile->Settings.ViewColor;
	auto NonTarget = [&](USpecularProfile* Object) { return Object && Object->Guid == Guid && Object->Settings.Format == Format
		&& Object->Settings.Texture == nullptr && SameColor(Object->Settings.LightColor, Light); };
	auto Export = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Edit = [&](const FString& Stage, TFunctionRef<void(TSharedPtr<FJsonObject>)> Change) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
			if (auto Curve = FindSpecularCurve(MakeShared<FJsonValueObject>(Root), TEXT("ViewColor"))) {
				Change(Curve); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		} return TestTrue(TEXT("one ordinary ViewColor"), Count == 1); };
	auto SetRed = [](TSharedPtr<FJsonObject> Curve, double Value) {
		for (const auto& Key : Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("r"))->GetArrayField(TEXT("keys")))
			if (Key->AsObject()->GetNumberField(TEXT("time")) == 0.5) Key->AsObject()->SetNumberField(TEXT("value"), Value); };
	auto Import = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
	auto Reload = [&]() { Profile = nullptr; if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Profile = LoadObject<USpecularProfile>(nullptr, *ObjectPath); return Profile != nullptr; };
	TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
	if (!Export(TEXT("conflict")) || !Edit(TEXT("conflict"), [](auto Curve) { Curve->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Constant")); })) return false;
	TestFalse(TEXT("root versus channel extrapolation conflict rejected"), Import(TEXT("conflict")).IsOk()); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("conflict preserves native and disk"), SameColor(Profile->Settings.ViewColor, Expected) && Before == After && !Profile->GetPackage()->IsDirty());
	if (!Export(TEXT("invalid")) || !Edit(TEXT("invalid"), [](auto Curve) { Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("g"))->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Invalid")); })) return false;
	TestFalse(TEXT("invalid channel enum rejected"), Import(TEXT("invalid")).IsOk()); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("invalid leaves native/disk clean"), SameColor(Profile->Settings.ViewColor, Expected) && Before == After && !Profile->GetPackage()->IsDirty());
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), [&](auto Curve) { SetRed(Curve, 0.75); })) return false;
	auto Result = Import(TEXT("edit")); if (!TestTrue(TEXT("public color edit"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	Expected.ColorCurves[0].GetKey(Expected.ColorCurves[0].FindKey(0.5f)).Value = 0.75f;
	if (!Reload() || !TestTrue(TEXT("full RGBA and non-target persist"), SameColor(Profile->Settings.ViewColor, Expected) && NonTarget(Profile))) return false;
	TSharedPtr<FJsonObject> Embedded;
	if (!Export(TEXT("reference")) || !Edit(TEXT("reference"), [&](auto Curve) {
		Embedded = MakeShared<FJsonObject>(*Curve); Curve->Values.Reset(); Curve->SetStringField(TEXT("schema"), Embedded->GetStringField(TEXT("schema"))); Curve->SetStringField(TEXT("curve_kind"), TEXT("asset_reference"));
		auto Ref = MakeShared<FJsonObject>(); Ref->SetStringField(TEXT("asset_path"), ExternalPackage); Curve->SetObjectField(TEXT("asset_reference"), Ref);
	})) return false;
	Result = Import(TEXT("reference")); if (!TestTrue(TEXT("external switch imports"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!Reload()) return false;
	External = LoadObject<UCurveLinearColor>(nullptr, *ExternalPath);
	Expected.ExternalCurve = External;
	if (!TestTrue(TEXT("external and inactive embedded persist"), External && SameColor(Profile->Settings.ViewColor, Expected) && NonTarget(Profile))) return false;
	if (!Export(TEXT("embedded")) || !Edit(TEXT("embedded"), [&](auto Curve) { Curve->Values = Embedded->Values; })) return false;
	Result = Import(TEXT("embedded")); if (!TestTrue(TEXT("embedded switch imports"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	Expected.ExternalCurve = nullptr;
	if (!Reload() || !TestTrue(TEXT("embedded restored and non-target persist"), SameColor(Profile->Settings.ViewColor, Expected) && NonTarget(Profile))) return false;
	FFileHelper::LoadFileToArray(Before, *Filename);
	Profile->Settings.ViewColor.ColorCurves[0].GetKey(Profile->Settings.ViewColor.ColorCurves[0].FindKey(0.5f)).Value = 0.85f; Profile->MarkPackageDirty();
	const auto Dirty = Profile->Settings.ViewColor; auto PublishedExpected = Dirty; PublishedExpected.ColorCurves[0].GetKey(PublishedExpected.ColorCurves[0].FindKey(0.5f)).Value = 0.95f;
	if (!Export(TEXT("late")) || !Edit(TEXT("late"), [&](auto Curve) { SetRed(Curve, 0.95); })) return false;
	bool SawPublished = false;
	const auto Handle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object) {
		auto* Published = Cast<USpecularProfile>(Object); if (SawPublished || !Published || Published->GetPathName() != ObjectPath) return;
		TArray<uint8> Bytes; SawPublished = SameColor(Published->Settings.ViewColor, PublishedExpected) && NonTarget(Published) && !Published->GetPackage()->IsDirty()
			&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != Before;
		if (SawPublished) Published->Settings.ViewColor.ColorCurves[0].GetKey(Published->Settings.ViewColor.ColorCurves[0].FindKey(0.5f)).Value = 0.11f;
	});
	Profile = nullptr; Result = Import(TEXT("late")); FCoreUObjectDelegates::OnAssetLoaded.Remove(Handle);
	TestFalse(TEXT("late rejection"), Result.IsOk()); TestTrue(TEXT("saved reloaded candidate observed first"), SawPublished);
	Profile = LoadObject<USpecularProfile>(nullptr, *ObjectPath); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("dirty memory restored independently"), Profile && Profile->GetPackage()->IsDirty() && SameColor(Profile->Settings.ViewColor, Dirty) && NonTarget(Profile));
	TestTrue(TEXT("original disk restored independently"), !Before.IsEmpty() && Before == After);
	TArray<uint8> ExternalAfter; FFileHelper::LoadFileToArray(ExternalAfter, *ExternalFilename); TestTrue(TEXT("external source untouched"), ExternalAfter == ExternalBytes);
	return !HasAnyErrors();
}
