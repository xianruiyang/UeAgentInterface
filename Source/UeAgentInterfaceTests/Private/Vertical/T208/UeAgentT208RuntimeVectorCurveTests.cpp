#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Curves/CurveVector.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
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
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
FRuntimeVectorCurve* FindVectorDefault(UBlueprint* Blueprint)
{
	const auto* Property = Blueprint && Blueprint->GeneratedClass ? FindFProperty<FStructProperty>(Blueprint->GeneratedClass, TEXT("VectorResponse")) : nullptr;
	return Property && Property->Struct == FRuntimeVectorCurve::StaticStruct()
		? Property->ContainerPtrToValuePtr<FRuntimeVectorCurve>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
}
bool SameVector(const FRuntimeVectorCurve& A, const FRuntimeVectorCurve& B)
{
	if (A.ExternalCurve != B.ExternalCurve) return false;
	for (int32 I = 0; I < 3; ++I) if (!(A.VectorCurves[I] == B.VectorCurves[I])) return false;
	return true;
}
TMap<FString, FString> VectorNonTarget(UBlueprint& Blueprint)
{
	TMap<FString, FString> State;
	for (const auto& Member : Blueprint.NewVariables) State.Add(TEXT("member/") + Member.VarName.ToString(), Member.VarGuid.ToString());
	const auto* Keep = FindFProperty<FIntProperty>(Blueprint.GeneratedClass, TEXT("KeepCount"));
	State.Add(TEXT("KeepCount"), Keep ? LexToString(Keep->GetPropertyValue_InContainer(Blueprint.GeneratedClass->GetDefaultObject())) : TEXT("missing"));
	State.Add(TEXT("parent"), GetPathNameSafe(Blueprint.ParentClass));
	TArray<UEdGraph*> Graphs; Blueprint.GetAllGraphs(Graphs);
	for (const auto* Graph : Graphs)
	{
		State.Add(TEXT("graph/") + Graph->GetName(), Graph->GraphGuid.ToString());
		for (const auto& Node : Graph->Nodes)
		{
			const FString Key = Graph->GetName() + TEXT("/") + Node->NodeGuid.ToString();
			State.Add(Key, Node->GetClass()->GetPathName() + FString::Printf(TEXT(":%d:%d"), Node->NodePosX, Node->NodePosY));
			for (const auto* Pin : Node->Pins)
			{
				FString Links; for (const auto* Link : Pin->LinkedTo) Links += Link->PinId.ToString();
				State.Add(Key + TEXT("/") + Pin->PinId.ToString(), Pin->PinName.ToString() + Pin->DefaultValue + Links);
			}
		}
	}
	return State;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208RuntimeVectorCurveTest,
	"UAI.Root.T208.Blueprint.RuntimeVectorCurveWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208RuntimeVectorCurveTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208RuntimeVector/BP_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString ExternalPackage = TEXT("/Game/UAI/Transient/T208RuntimeVector/CV_") + Token;
	const FString ExternalPath = ExternalPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(ExternalPackage);
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-runtime-vector-curve/native") / Token);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString ExternalFilename = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-runtime-vector"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
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
	const FString Seed = FString::Printf(TEXT(R"JSON({"schema":"uai.blueprint.edit/1","root":{"type":"/Script/Engine.Blueprint","document":{
"operation":"create","target":{"asset_path":"%s","asset_kind":"actor_blueprint"},"provider_extension":{"parent_class":"/Script/Engine.Actor",
"members":{"variables":{"KeepCount":{"type":{"pin_category":"int"},"default_property":{"property_path":"KeepCount","value":23}}}},
"graphs":{"event_graph:EventGraph":{"operation":"update","graph":{"name":"EventGraph","graph_kind":"event_graph"},"nodes":{"custom_event:PreservedEvent":{"operation":"create","node_type":"custom_event","event_name":"PreservedEvent","pos":{"x":0,"y":320}}}}}}}}})JSON"), *ObjectPath);
	auto Create = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("inline")); Source->SetStringField(TEXT("document"), Seed);
	Create->SetStringField(TEXT("target"), ObjectPath); Create->SetObjectField(TEXT("source"), Source);
	auto Result = Submit(TEXT("content_graph.import_workspace"), Create);
	if (!TestTrue(TEXT("public Blueprint seed"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	auto Export = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Exported = Submit(TEXT("content_graph.export_workspace"), Args); if (!Exported.IsOk()) AddError(Exported.GetErrorJson()); return Exported.IsOk(); };
	auto Edit = [&](const FString& Stage, const FString& FieldName, TFunctionRef<void(TSharedPtr<FJsonObject>)> Change) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Count = 0;
		TFunction<void(const TSharedPtr<FJsonValue>&)> Visit;
		Visit = [&](const TSharedPtr<FJsonValue>& Value) {
			if (Value->Type == EJson::Array) { for (const auto& Child : Value->AsArray()) Visit(Child); return; }
			if (Value->Type != EJson::Object) return;
			const auto Object = Value->AsObject();
			if (FieldName == TEXT("variables") && Object->HasTypedField<EJson::Object>(FieldName)) { Change(Object->GetObjectField(FieldName)); ++Count; return; }
			if (Object->HasTypedField<EJson::Object>(FieldName)) {
				const auto Row = Object->GetObjectField(FieldName);
				if (Row->HasTypedField<EJson::Object>(TEXT("value")) && Row->GetObjectField(TEXT("value"))->HasField(TEXT("curve_kind"))) { Change(Row->GetObjectField(TEXT("value"))); ++Count; return; }
			}
			for (const auto& Field : Object->Values) Visit(Field.Value);
		};
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
			const int32 Before = Count; Visit(MakeShared<FJsonValueObject>(Root));
			if (Count != Before && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		} return TestTrue(TEXT("one ordinary edit owner"), Count == 1); };
	auto Import = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("kind"), TEXT("directory")); Input->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Input); return Submit(TEXT("content_graph.import_workspace"), Args); };
	auto Reload = [&]() { Blueprint = nullptr; if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); return Blueprint && FindVectorDefault(Blueprint); };
	if (!Export(TEXT("member")) || !Edit(TEXT("member"), TEXT("variables"), [](auto Variables) {
		auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("/Script/Engine.RuntimeVectorCurve"));
		auto Variable = MakeShared<FJsonObject>(); Variable->SetObjectField(TEXT("type"), Type); Variable->SetBoolField(TEXT("instance_editable"), true);
		Variables->SetObjectField(TEXT("VectorResponse"), Variable);
	})) return false;
	Result = Import(TEXT("member")); if (!TestTrue(TEXT("ordinary RuntimeVector member creates"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!Reload()) return false;
	const auto NonTarget = VectorNonTarget(*Blueprint);
	FRuntimeVectorCurve Expected = *FindVectorDefault(Blueprint);
	if (!Export(TEXT("initialize")) || !Edit(TEXT("initialize"), TEXT("VectorResponse"), [&](auto Curve) {
		int32 I = 0;
		for (const FString Name : {TEXT("x"), TEXT("y"), TEXT("z")}) {
			auto Channel = Curve->GetObjectField(TEXT("channels"))->GetObjectField(Name); auto Default = MakeShared<FJsonObject>(); Default->SetBoolField(TEXT("enabled"), true); Default->SetNumberField(TEXT("value"), I + 0.15); Channel->SetObjectField(TEXT("default_value"), Default);
			Channel->SetStringField(TEXT("pre_infinity_extrap"), I == 0 ? TEXT("Cycle") : I == 1 ? TEXT("CycleWithOffset") : TEXT("Oscillate"));
			Channel->SetStringField(TEXT("post_infinity_extrap"), I == 0 ? TEXT("None") : I == 1 ? TEXT("Constant") : TEXT("Linear"));
			auto& Native = Expected.VectorCurves[I]; Native.Reset(); Native.SetDefaultValue(I + 0.15f); Native.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(I); Native.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(5 - I);
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (float X : {-0.25f, 0.5f, 1.25f}) {
				auto Key = MakeShared<FJsonObject>(); Key->SetNumberField(TEXT("time"), X); Key->SetNumberField(TEXT("value"), X + I); Key->SetStringField(TEXT("interp_mode"), TEXT("Cubic")); Key->SetStringField(TEXT("tangent_mode"), TEXT("User")); Key->SetStringField(TEXT("tangent_weight_mode"), TEXT("WeightedBoth"));
				Key->SetNumberField(TEXT("arrive_tangent"), I + 0.2); Key->SetNumberField(TEXT("leave_tangent"), I + 0.3); Key->SetNumberField(TEXT("arrive_tangent_weight"), 0.1); Key->SetNumberField(TEXT("leave_tangent_weight"), 0.2); Keys.Add(MakeShared<FJsonValueObject>(Key));
				auto& NativeKey = Native.GetKey(Native.AddKey(X, X + I)); NativeKey.InterpMode = RCIM_Cubic; NativeKey.TangentMode = RCTM_User; NativeKey.TangentWeightMode = RCTWM_WeightedBoth;
				NativeKey.ArriveTangent = I + 0.2f; NativeKey.LeaveTangent = I + 0.3f; NativeKey.ArriveTangentWeight = 0.1f; NativeKey.LeaveTangentWeight = 0.2f;
			} Channel->SetArrayField(TEXT("keys"), Keys); ++I;
		}
	})) return false;
	Result = Import(TEXT("initialize")); if (!TestTrue(TEXT("ordinary CDO initializes XYZ"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!Reload() || !TestTrue(TEXT("native XYZ and other members/graphs persist"), SameVector(*FindVectorDefault(Blueprint), Expected) && VectorNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget))) return false;
	auto SetX = [](auto Curve, double Value) { for (const auto& Key : Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("x"))->GetArrayField(TEXT("keys"))) if (Key->AsObject()->GetNumberField(TEXT("time")) == 0.5) Key->AsObject()->SetNumberField(TEXT("value"), Value); };
	TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
	if (!Export(TEXT("invalid")) || !Edit(TEXT("invalid"), TEXT("VectorResponse"), [](auto Curve) { Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("y"))->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Invalid")); })) return false;
	TestFalse(TEXT("invalid enum rejects before write"), Import(TEXT("invalid")).IsOk()); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("invalid preserves native/disk"), SameVector(*FindVectorDefault(Blueprint), Expected) && Before == After && !Blueprint->GetPackage()->IsDirty());
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("VectorResponse"), [&](auto Curve) { SetX(Curve, 0.75); })) return false;
	Result = Import(TEXT("edit")); if (!TestTrue(TEXT("local CDO curve edit"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	Expected.VectorCurves[0].GetKey(Expected.VectorCurves[0].FindKey(0.5f)).Value = 0.75f;
	if (!Reload() || !TestTrue(TEXT("local edit preserves full native data"), SameVector(*FindVectorDefault(Blueprint), Expected) && VectorNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget))) return false;
	// Only the external source uses native setup; the Blueprint variable and defaults above are public edits.
	auto* External = NewObject<UCurveVector>(CreatePackage(*ExternalPackage), *FPackageName::GetLongPackageAssetName(ExternalPackage), RF_Public | RF_Standalone);
	for (int32 I = 0; I < 3; ++I) External->FloatCurves[I].AddKey(0.2f, 7.f + I);
	FAssetRegistryModule::AssetCreated(External); if (!Save(External)) return false;
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({ExternalFilename}, true);
	TArray<uint8> ExternalBytes; FFileHelper::LoadFileToArray(ExternalBytes, *ExternalFilename);
	TSharedPtr<FJsonObject> Embedded;
	if (!Export(TEXT("reference")) || !Edit(TEXT("reference"), TEXT("VectorResponse"), [&](auto Curve) {
		Embedded = MakeShared<FJsonObject>(*Curve); Curve->Values.Reset(); Curve->SetStringField(TEXT("schema"), Embedded->GetStringField(TEXT("schema"))); Curve->SetStringField(TEXT("curve_kind"), TEXT("asset_reference"));
		auto Ref = MakeShared<FJsonObject>(); Ref->SetStringField(TEXT("asset_path"), ExternalPackage); Curve->SetObjectField(TEXT("asset_reference"), Ref);
	})) return false;
	Result = Import(TEXT("reference")); if (!TestTrue(TEXT("external switch"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!Reload()) return false; Expected.ExternalCurve = LoadObject<UCurveVector>(nullptr, *ExternalPath);
	if (!TestTrue(TEXT("external reference and inactive XYZ persist"), Expected.ExternalCurve && SameVector(*FindVectorDefault(Blueprint), Expected) && VectorNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget))) return false;
	if (!Export(TEXT("embedded")) || !Edit(TEXT("embedded"), TEXT("VectorResponse"), [&](auto Curve) { Curve->Values = Embedded->Values; })) return false;
	Result = Import(TEXT("embedded")); if (!TestTrue(TEXT("embedded switch"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	Expected.ExternalCurve = nullptr;
	if (!Reload() || !TestTrue(TEXT("embedded data retained"), SameVector(*FindVectorDefault(Blueprint), Expected))) return false;
	FFileHelper::LoadFileToArray(Before, *Filename);
	FindVectorDefault(Blueprint)->VectorCurves[0].GetKey(FindVectorDefault(Blueprint)->VectorCurves[0].FindKey(0.5f)).Value = 0.85f; Blueprint->MarkPackageDirty();
	const auto Dirty = *FindVectorDefault(Blueprint); auto PublishedExpected = Dirty; PublishedExpected.VectorCurves[0].GetKey(PublishedExpected.VectorCurves[0].FindKey(0.5f)).Value = 0.95f;
	TSharedPtr<FJsonObject> LateCurve;
	if (!Export(TEXT("late")) || !Edit(TEXT("late"), TEXT("VectorResponse"), [&](auto Curve) { SetX(Curve, 0.95); LateCurve = Curve; })) return false;
	// Public success/rejection paths above exercise the adapter. Its same ApplyAsset
	// final-readback boundary provides a deterministic post-persistence rejection.
	UeAgentBlueprintAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.AssetPath = PackageName; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetBoolField(TEXT("compile"), true); LateRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
	auto Default = MakeShared<FJsonObject>(); Default->SetStringField(TEXT("property_path"), TEXT("VectorResponse")); Default->SetObjectField(TEXT("value"), LateCurve);
	LateRequest.ProviderExtension->SetArrayField(TEXT("class_defaults"), {MakeShared<FJsonValueObject>(Default)});
	const TWeakObjectPtr<UBlueprint> OldBlueprint = Blueprint;
	bool SawPublished = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Published = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		auto* Curve = FindVectorDefault(Published); TArray<uint8> Bytes;
		SawPublished = !OldBlueprint.IsValid() && Curve && SameVector(*Curve, PublishedExpected) && VectorNonTarget(*Published).OrderIndependentCompareEqual(NonTarget)
			&& Published->Status != BS_Error && !Published->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != Before;
		Failure.Diagnostic.Code = TEXT("t208_runtime_vector_late_rejected"); Failure.Diagnostic.PublicMessage = TEXT("published RuntimeVector CDO observed before rejection"); return false;
	};
	Blueprint = nullptr; UeAgentBlueprintAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("domain late rejection"), UeAgentBlueprintAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("runtime-vector-late-") + Token));
	TestTrue(TEXT("new saved reloaded vector observed before rejection"), SawPublished);
	TestTrue(TEXT("requested rejection and checkpoint restore"), LateResult.Diagnostic.Code == TEXT("t208_runtime_vector_late_rejected") && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded);
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("dirty memory independently restored"), FindVectorDefault(Blueprint) && SameVector(*FindVectorDefault(Blueprint), Dirty) && Blueprint->GetPackage()->IsDirty() && VectorNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget));
	TestTrue(TEXT("original disk independently restored"), !Before.IsEmpty() && Before == After);
	TArray<uint8> ExternalAfter; FFileHelper::LoadFileToArray(ExternalAfter, *ExternalFilename); TestTrue(TEXT("external source disk unchanged"), ExternalBytes == ExternalAfter);
	return !HasAnyErrors();
}
