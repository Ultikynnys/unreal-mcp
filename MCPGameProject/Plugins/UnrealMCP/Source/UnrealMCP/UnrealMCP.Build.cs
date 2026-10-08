// Copyright Epic Games, Inc. All Rights Reserved.

using System.Diagnostics;
using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
	// The commit this plugin is BUILT from, baked in as a define so a stale DLL reports its own
	// revision instead of the checkout's. MCP_REVISION env wins, so a plugin copied into another
	// project can pin the revision of the repo it came from rather than the hosting HEAD.
	// Careful: UBT caches rule evaluation, so a bare rebuild after a commit says "up to date" and
	// keeps the old revision. Touch this file (or a clean build) to refresh it.
	private string BuildRevision()
	{
		string FromEnv = System.Environment.GetEnvironmentVariable("MCP_REVISION");
		if (!string.IsNullOrEmpty(FromEnv)) { return FromEnv; }
		string Output = RunGit("rev-parse HEAD");
		return string.IsNullOrEmpty(Output) ? "unknown" : Output;
	}

	private bool BuiltDirty()
	{
		// Only this module's own tree counts: an unrelated dirty file must not look like plugin drift.
		return System.Environment.GetEnvironmentVariable("MCP_REVISION_DIRTY") == "1"
			|| RunGit("status --porcelain -- .").Length > 0;
	}

	private string RunGit(string Arguments)
	{
		try
		{
			ProcessStartInfo StartInfo = new ProcessStartInfo("git", Arguments)
			{
				WorkingDirectory = ModuleDirectory,
				RedirectStandardOutput = true,
				RedirectStandardError = true,
				UseShellExecute = false,
				CreateNoWindow = true,
			};
			using (Process Git = Process.Start(StartInfo))
			{
				string Text = Git.StandardOutput.ReadToEnd().Trim();
				Git.WaitForExit(5000);
				return Git.ExitCode == 0 ? Text : string.Empty;
			}
		}
		catch
		{
			return string.Empty; // no git: the revision is "unknown", and the handshake refuses it
		}
	}

	public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// Use IWYUSupport instead of the deprecated bEnforceIWYU in UE5.5
		IWYUSupport = IWYUSupport.Full;

		string Revision = BuildRevision();
		PrivateDefinitions.Add("MCP_REVISION=\"" + Revision + "\"");
		PrivateDefinitions.Add("MCP_REVISION_DIRTY=" + (BuiltDirty() ? "1" : "0"));

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
		);
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Networking",
				"Sockets",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings"
			}
		);
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"EditorScriptingUtilities",
				"EditorSubsystem",
				"Slate",
				"SlateCore",
				"UMG",
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"Projects",
				"AssetRegistry",
				"AssetTools",
				"LevelEditor",
				"RenderCore",
				"PythonScriptPlugin"
			}
		);
		
		if (Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"PropertyEditor",      // For widget property editing
					"ToolMenus",           // For editor UI
					"BlueprintEditorLibrary", // For Blueprint utilities
					"UMGEditor"           // For WidgetBlueprint.h and other UMG editor functionality
				}
			);
		}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
} 