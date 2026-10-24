#include "Misc/AutomationTest.h"
#include "AnimGraphNode_PoseDriver.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "AnimationGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

// This explicit setup entry leaves one named disposable fixture for the public
// SDK consumer. The SDK owns its ordinary preview/confirm deletion afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208PoseDriverCurveFixtureTest,
	"UAI.Root.T208.AnimationBlueprint.PoseDriverCurveFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208PoseDriverCurveFixtureTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208PoseDriverCurve/ABP_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Evidence = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-pose-driver-curve");
	FString Error;
	auto* Reference = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
	if (!TestNotNull(TEXT("existing skeleton source is read only"), Reference)) return false;
	auto* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->ParentClass = UAnimInstance::StaticClass(); Factory->TargetSkeleton = Reference->GetSkeleton();
	auto* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), CreatePackage(*PackageName),
		FName(*FPackageName::GetLongPackageAssetName(PackageName)), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!TestNotNull(TEXT("native AnimBlueprint setup"), Blueprint)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
	UEdGraph** Graph = Graphs.FindByPredicate([](UEdGraph* Item) { return Item && Item->IsA<UAnimationGraph>(); });
	if (!TestTrue(TEXT("native AnimGraph exists"), Graph != nullptr)) return false;
	FGraphNodeCreator<UAnimGraphNode_PoseDriver> Creator(**Graph);
	auto* Driver = Creator.CreateNode(); Creator.Finalize();
	Driver->NodePosX = 256; Driver->NodePosY = 128;
	auto& Target = Driver->Node.PoseTargets.AddDefaulted_GetRef();
	Target.bApplyCustomCurve = true; Target.DrivenName = TEXT("FixtureWeight");
	Target.BoneTransforms.AddDefaulted();
	Target.CustomCurve.PreInfinityExtrap = RCCE_Linear;
	Target.CustomCurve.PostInfinityExtrap = RCCE_CycleWithOffset;
	for (const auto& Point : {FVector2D(-0.5, -0.2), FVector2D(0.5, 0.4), FVector2D(1.5, 1.2)})
	{
		const auto Handle = Target.CustomCurve.AddKey(Point.X, Point.Y);
		auto& Key = Target.CustomCurve.GetKey(Handle);
		Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_User;
		Key.TangentWeightMode = RCTWM_WeightedBoth;
		Key.ArriveTangent = 0.25f; Key.LeaveTangent = 0.5f;
		Key.ArriveTangentWeight = 0.1f; Key.LeaveTangentWeight = 0.2f;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("fixture compiles"), Blueprint->Status != BS_Error)) return false;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Blueprint->GetPackage(), Blueprint,
		*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Save)) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
	Args->SetStringField(TEXT("workspace_path"), Evidence / TEXT("baseline") / Token);
	Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args,
		TEXT("t208-pose-driver"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!TestTrue(TEXT("public ordinary PoseDriver export"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	auto Receipt = MakeShared<FJsonObject>(); Receipt->SetStringField(TEXT("target"), ObjectPath);
	Receipt->SetStringField(TEXT("workspace"), Evidence / TEXT("baseline") / Token);
	Receipt->SetStringField(TEXT("graph"), (*Graph)->GetName()); Receipt->SetStringField(TEXT("node_guid"), Driver->NodeGuid.ToString());
	FString Text;
	return FUeAgentStructuredHjsonFormatter::TryWriteObject(Receipt, Text, Error)
		&& FFileHelper::SaveStringToFile(Text, *(Evidence / TEXT("fixture.hjson")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

namespace
{
UAnimGraphNode_PoseDriver* FindDriver(UAnimBlueprint* Blueprint)
{
	if (!Blueprint) return nullptr;
	TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
	for (auto* Graph : Graphs) for (const auto& Node : Graph->Nodes)
		if (auto* Driver = Cast<UAnimGraphNode_PoseDriver>(Node)) return Driver;
	return nullptr;
}
TMap<FString, FString> PoseDriverNonTarget(UAnimBlueprint& Blueprint)
{
	TMap<FString, FString> State;
	State.Add(TEXT("skeleton"), GetPathNameSafe(Blueprint.TargetSkeleton));
	State.Add(TEXT("parent"), GetPathNameSafe(Blueprint.ParentClass));
	TArray<UEdGraph*> Graphs; Blueprint.GetAllGraphs(Graphs);
	for (auto* Graph : Graphs)
	{
		const FString GraphKey = Graph->GraphGuid.ToString(); State.Add(GraphKey, Graph->GetName() + GetPathNameSafe(Graph->GetSchema()));
		for (const auto& NodePointer : Graph->Nodes)
		{
			UEdGraphNode* Node = NodePointer.Get();
			const FString Key = GraphKey + TEXT("/") + Node->NodeGuid.ToString();
			State.Add(Key, Node->GetClass()->GetPathName() + FString::Printf(TEXT(":%d:%d"), Node->NodePosX, Node->NodePosY));
			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit)) continue;
				FString Value;
				if (auto* Driver = Cast<UAnimGraphNode_PoseDriver>(Node); Driver && It->GetName() == TEXT("Node"))
				{
					const auto* StructProperty = CastFieldChecked<FStructProperty>(*It);
					FStructOnScope Copy(StructProperty->Struct);
					StructProperty->Struct->CopyScriptStruct(Copy.GetStructMemory(), &Driver->Node);
					auto* CopiedNode = reinterpret_cast<FAnimNode_PoseDriver*>(Copy.GetStructMemory());
					if (!CopiedNode->PoseTargets.IsEmpty()) CopiedNode->PoseTargets[0].CustomCurve = FRichCurve();
					It->ExportTextItem_Direct(Value, Copy.GetStructMemory(), nullptr, Node, PPF_None);
				}
				else It->ExportText_InContainer(0, Value, Node, Node, Node, PPF_None);
				State.Add(Key + TEXT("/") + It->GetName(), Value);
			}
			for (auto* Pin : Node->Pins)
			{
				FString Links; for (auto* Link : Pin->LinkedTo) Links += Link->PinId.ToString();
				State.Add(Key + TEXT("/pin/") + Pin->PinId.ToString(), Pin->PinName.ToString() + TEXT(":") + Pin->DefaultValue + Links);
			}
		}
	}
	return State;
}
TSharedPtr<FJsonObject> FindCustomCurve(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value) return nullptr;
	if (Value->Type == EJson::Array) { for (const auto& Item : Value->AsArray()) if (auto Curve = FindCustomCurve(Item)) return Curve; }
	if (Value->Type != EJson::Object) return nullptr;
	for (const auto& Field : Value->AsObject()->Values)
	{
		if (Field.Key == TEXT("CustomCurve") && Field.Value->Type == EJson::Object)
		{
			auto Object = Field.Value->AsObject();
			if (Object->HasField(TEXT("channels"))) return Object;
			if (auto Curve = FindCustomCurve(Field.Value)) return Curve;
		}
		if (Field.Key == TEXT("value") && Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("channels"))) return Field.Value->AsObject();
		if (auto Curve = FindCustomCurve(Field.Value)) return Curve;
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208PoseDriverCurveWorkspaceTest,
	"UAI.Root.T208.AnimationBlueprint.PoseDriverCurveWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208PoseDriverCurveWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Evidence = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-pose-driver-curve");
	FString Text, Error; TSharedPtr<FJsonObject> Fixture;
	if (!FFileHelper::LoadFileToString(Text, *(Evidence / TEXT("fixture.hjson")))
		|| !FUeAgentStructuredHjsonParser::ParseObject(Text, TEXT("fixture"), Fixture, Error)) { AddError(Error); return false; }
	const FString ObjectPath = Fixture->GetStringField(TEXT("target"));
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	auto* Driver = FindDriver(Blueprint);
	if (!TestTrue(TEXT("retained native setup exists"), Driver && Driver->Node.PoseTargets.Num() == 1)) return false;
	const auto NonTarget = PoseDriverNonTarget(*Blueprint);
	FRichCurve Expected = Driver->Node.PoseTargets[0].CustomCurve;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-pose-driver-edit"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	const FString Workspace = FPaths::ConvertRelativePathToFull(Evidence / TEXT("native") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	auto Export = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Workspace / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Edit = [&](const FString& Stage, double Value, bool bInvalid) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Workspace / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
			auto Curve = FindCustomCurve(MakeShared<FJsonValueObject>(Root)); if (!Curve) continue;
			const auto Channel = Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"));
			TestTrue(TEXT("ordinary curve has no storage carrier seconds or counts"), !Curve->HasField(TEXT("storage"))
				&& !Curve->HasField(TEXT("carrier_cpp_type")) && !Curve->HasField(TEXT("time_domain")) && !Channel->HasField(TEXT("key_count")));
			for (const auto& KeyValue : Channel->GetArrayField(TEXT("keys"))) {
				auto Key = KeyValue->AsObject();
				TestTrue(TEXT("ordinary curve key has no ordinal or seconds alias"), !Key->HasField(TEXT("key_index")) && !Key->HasField(TEXT("time_seconds")));
				if (Key->GetNumberField(TEXT("time")) == 0.5) {
					++Count; if (bInvalid) Key->SetStringField(TEXT("interp_mode"), TEXT("InvalidInterpolation")); else Key->SetNumberField(TEXT("value"), Value);
				}
			}
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		return TestTrue(TEXT("one curve key is addressed by its coordinate"), Count == 1);
	};
	auto Import = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Workspace / Stage); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args); };
	TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
	if (!Export(TEXT("invalid")) || !Edit(TEXT("invalid"), 0.65, true)) return false;
	auto Result = Import(TEXT("invalid")); TestFalse(TEXT("invalid curve enum rejects before writes"), Result.IsOk());
	FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("invalid enum preserves exact native curve and disk"), Driver->Node.PoseTargets[0].CustomCurve == Expected && Before == After && !Blueprint->GetPackage()->IsDirty());
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), 0.65, false)) return false;
	Driver = nullptr; Blueprint = nullptr; Result = Import(TEXT("edit"));
	if (!TestTrue(TEXT("existing AnimGraph property imports publicly"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath); Driver = FindDriver(Blueprint);
	Expected.GetKey(Expected.FindKey(0.5f)).Value = 0.65f;
	if (!TestTrue(TEXT("full curve keys tangents extrapolation and graph identity persist"), Driver && Driver->Node.PoseTargets[0].CustomCurve == Expected
		&& PoseDriverNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget) && !Blueprint->GetPackage()->IsDirty())) return false;
	Before.Reset(); FFileHelper::LoadFileToArray(Before, *Filename);
	Driver->Modify(); Driver->Node.PoseTargets[0].CustomCurve.GetKey(Driver->Node.PoseTargets[0].CustomCurve.FindKey(0.5f)).Value = 0.85f; Blueprint->MarkPackageDirty();
	const FRichCurve Dirty = Driver->Node.PoseTargets[0].CustomCurve;
	if (!Export(TEXT("late")) || !Edit(TEXT("late"), 0.95, false)) return false;
	FRichCurve PublishedExpected = Dirty; PublishedExpected.GetKey(PublishedExpected.FindKey(0.5f)).Value = 0.95f;
	bool SawPublished = false;
	const auto Handle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object) {
		auto* Published = Cast<UAnimBlueprint>(Object); if (SawPublished || !Published || Published->GetPathName() != ObjectPath) return;
		auto* Node = FindDriver(Published); TArray<uint8> Bytes;
		SawPublished = Node && Published->GeneratedClass && Published->Status != BS_Error
			&& Node->Node.PoseTargets[0].CustomCurve == PublishedExpected && !Published->GetPackage()->IsDirty()
			&& PoseDriverNonTarget(*Published).OrderIndependentCompareEqual(NonTarget) && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != Before;
		if (SawPublished) Node->Node.PoseTargets[0].CustomCurve.GetKey(Node->Node.PoseTargets[0].CustomCurve.FindKey(0.5f)).Value = 0.11f;
	});
	Driver = nullptr; Blueprint = nullptr; Result = Import(TEXT("late")); FCoreUObjectDelegates::OnAssetLoaded.Remove(Handle);
	TestFalse(TEXT("late reloaded curve mismatch rejects"), Result.IsOk()); TestTrue(TEXT("new saved reloaded curve observed before rejection"), SawPublished);
	Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath); Driver = FindDriver(Blueprint);
	TestTrue(TEXT("dirty native curve independently restored"), Driver && Driver->Node.PoseTargets[0].CustomCurve == Dirty
		&& Blueprint->GetPackage()->IsDirty() && PoseDriverNonTarget(*Blueprint).OrderIndependentCompareEqual(NonTarget));
	After.Reset(); FFileHelper::LoadFileToArray(After, *Filename); TestTrue(TEXT("original disk bytes independently restored"), !Before.IsEmpty() && Before == After);
	// Recovery assertions are complete; settle this retained SDK fixture only.
	if (Blueprint && Blueprint->GetPackage()->IsDirty()) { FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
		TestTrue(TEXT("self fixture settles for SDK continuation"), UPackage::SavePackage(Blueprint->GetPackage(), Blueprint, *Filename, Save)); }
	return !HasAnyErrors();
}
