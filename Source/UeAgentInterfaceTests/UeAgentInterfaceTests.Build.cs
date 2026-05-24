// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class UeAgentInterfaceTests : ModuleRules
{
	public UeAgentInterfaceTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(ModuleDirectory, "..", "UeAgentInterface", "Private"),
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UeAgentInterface",
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"ApplicationCore",
				"HTTPServer",
				"HTTP",
				"Json",
				"JsonUtilities",
				"AssetRegistry",
				"AssetTools",
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"BlueprintEditorLibrary",
				"AnimationBlueprintLibrary",
				"AnimationEditor",
				"AnimGraph",
				"Persona",
				"StaticMeshEditor",
				"UMG",
				"UMGEditor",
				"EnhancedInput",
				"InputBlueprintNodes",
				"InputCore",
				"GameplayTags",
				"AIModule",
				"GameplayTasks",
				"NavigationSystem",
				"StateTreeModule",
				"StateTreeEditorModule",
				"GameplayStateTreeModule",
				"SmartObjectsModule",
				"WorldConditions",
				"PropertyBindingUtils",
				"PhysicsCore",
				"Landscape",
				"Foliage",
				"MaterialEditor",
				"LevelSequence",
				"MovieScene",
				"MovieSceneTools",
				"MovieSceneTracks",
				"SequencerScriptingEditor",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"IKRig",
				"IKRigEditor",
				"RigVM",
				"RigVMDeveloper",
				"Niagara",
				"NiagaraEditor",
				"PCG",
				"PCGEditor",
				"GeometryCache",
				"ComputeFramework",
				"OptimusCore",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"GeometryCore",
				"MeshDescription",
				"StaticMeshDescription",
				"MediaAssets",
				"Paper2D",
				"TextureGraph",
				"TextureGraphEngine",
				"AudioEditor",
				"AudioMixer",
				"MetasoundEngine",
				"MetasoundEditor",
				"MetasoundFrontend",
				"DataLayerEditor",
				"WorldPartitionEditor",
				"TargetPlatform",
			}
		);
	}
}
