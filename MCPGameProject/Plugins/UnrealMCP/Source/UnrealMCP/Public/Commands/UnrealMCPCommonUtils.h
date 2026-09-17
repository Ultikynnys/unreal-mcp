#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Forward declarations
class AActor;
class UWorld;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node_Event;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_InputAction;
class UK2Node_Self;
class UK2Node_IfThenElse;
class UK2Node_ExecutionSequence;
class UK2Node_DynamicCast;
class UK2Node_CustomEvent;
class UK2Node_MacroInstance;
class UK2Node_BreakStruct;
class UK2Node_MakeStruct;
class UK2Node_Knot;
class UScriptStruct;
class UClass;
class UFunction;

/**
 * Common utilities for UnrealMCP commands
 */
class UNREALMCP_API FUnrealMCPCommonUtils
{
public:
    // JSON utilities
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& Message);
    static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data = nullptr);
    static void GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray);
    static void GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray);
    static FVector2D GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FVector GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FRotator GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    
    // Actor utilities
    // The persistent editor world (never GWorld, which becomes the PIE world during a
    // Play-In-Editor session), so handlers act on the level the user is editing.
    static UWorld* GetEditorWorld();
    // Find an actor in the editor world by object path, then label, then display name -
    // the same identifiers get_actor_details accepts.
    static AActor* ResolveActor(const FString& Identifier);
    static TSharedPtr<FJsonValue> ActorToJson(AActor* Actor);
    static TSharedPtr<FJsonObject> ActorToJsonObject(AActor* Actor, bool bDetailed = false);
    
    // Blueprint utilities
    static UBlueprint* FindBlueprint(const FString& BlueprintName);
    static UBlueprint* FindBlueprintByName(const FString& BlueprintName);
    static UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);
    static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName = TEXT(""));
    static UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidStr, UEdGraph* PreferredGraph = nullptr);
    
    // Blueprint node utilities
    static UK2Node_Event* CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position);
    static UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position);
    static UK2Node_VariableGet* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_VariableSet* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_InputAction* CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position);
    static UK2Node_Self* CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position);
    static UK2Node_Knot* CreateKnotNode(UEdGraph* Graph, const FVector2D& Position);
    static UK2Node_IfThenElse* CreateBranchNode(UEdGraph* Graph, const FVector2D& Position);
    static UK2Node_ExecutionSequence* CreateSequenceNode(UEdGraph* Graph, int32 NumOutputs, const FVector2D& Position);
    static UK2Node_DynamicCast* CreateCastNode(UEdGraph* Graph, UClass* TargetClass, const FVector2D& Position);
    static UK2Node_CustomEvent* CreateCustomEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position);
    static UK2Node_MacroInstance* CreateMacroNode(UEdGraph* Graph, const FString& MacroName, const FVector2D& Position);
    static UK2Node_CallFunction* CreateSpawnActorNode(UEdGraph* Graph, UClass* ActorClass, const FVector2D& Position);
    static UK2Node_BreakStruct* CreateBreakStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position);
    static UK2Node_MakeStruct* CreateMakeStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position);
    static bool ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                UEdGraphNode* TargetNode, const FString& TargetPinName);
    // Wires Src -> (optional chain of reroute knots at KnotPath) -> Dst using low-level
    // connects only. No design-rule rejection: callers pass an already-validated layout.
    static bool ConnectWithKnots(UEdGraph* Graph, UEdGraphNode* Src, const FString& SrcPin,
                                 UEdGraphNode* Dst, const FString& DstPin, const TArray<FVector2D>& KnotPath);
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static UEdGraphPin* FindPinOrSplitMember(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static bool SetNodePinDefault(UEdGraphNode* Node, const FString& PinName, const FString& Value);
    static UK2Node_Event* FindExistingEventNode(UEdGraph* Graph, const FString& EventName);

    // Node layout / placement validation (headless; no graph editor required)
    static FVector2D EstimateNodeSize(const UEdGraphNode* Node);
    static float NodeGap(const UEdGraphNode* A, const UEdGraphNode* B);
    static bool ValidatePlacement(UEdGraph* Graph, UEdGraphNode* NewNode, FString& OutErrorMessage);
    // Area sanity check: reject a node placed absurdly far (beyond +/-100000 on either
    // axis) or more than 20000 units beyond the existing graph's bounding box.
    static bool ValidateNodeBounds(UEdGraph* Graph, UEdGraphNode* NewNode, FString& OutErrorMessage);
    // Axis-aligned bounding box over the graph's nodes (origins + estimated sizes).
    static void ComputeGraphBounds(UEdGraph* Graph, FVector2D& OutMin, FVector2D& OutMax, int32& OutCount, bool bIncludeStructural = true);
    static bool FinalizePlacedNode(UEdGraph* Graph, UEdGraphNode* Node, FString& OutErrorMessage);
    static UEdGraphNode* FindWireOverlap(UEdGraphNode* Source, UEdGraphNode* Target, FString& OutErrorMessage);
    static bool NodesOverlap(const UEdGraphNode* A, const UEdGraphNode* B);
    static void CollectGraphEdges(UEdGraph* Graph, TArray<TPair<UEdGraphNode*, UEdGraphNode*>>& OutEdges);
    static bool IsStructuralNode(const UEdGraphNode* Node);

    // --- Graph auto-layout (layered, left -> right) ---

    // Abstract topology: pure indices + box sizes, so the layout maths carry no
    // UObject dependency and can run before nodes exist (plan build) or over an
    // existing graph (repair).
    struct FLayoutInput
    {
        int32 NodeCount = 0;
        TArray<FVector2D> Sizes;            // per node (index -> box size)
        TArray<TPair<int32, int32>> Edges;  // src index -> dst index
    };
    struct FLayoutOutput
    {
        TArray<FVector2D> Positions;            // per node
        TArray<TArray<FVector2D>> EdgeKnots;    // per edge: intermediate knot positions (in order)
    };
    // Ranks nodes by longest-path depth, orders each column to reduce crossings, and
    // returns column coordinates. Edges spanning >1 column get an interpolated knot
    // chain so no single wire is long. Origin is the top-left of the layout.
    static void LayeredLayout(const FLayoutInput& In, float ColGap, float RowGap, const FVector2D& Origin, FLayoutOutput& Out);

    // A directed edge that keeps pin names; CollectLogicEdges follows reroute knots so
    // they are invisible to the layout (an edge through n knots becomes one edge).
    struct FGraphEdge
    {
        UEdGraphNode* Src = nullptr;
        FString SrcPin;
        UEdGraphNode* Dst = nullptr;
        FString DstPin;
    };
    static void CollectLogicEdges(UEdGraph* Graph, TArray<FGraphEdge>& Out);
    // Removes every reroute (knot) node in the graph; used before rebuilding routing.
    static void RemoveStructuralKnots(UEdGraph* Graph, UBlueprint* Blueprint);

    // Engine state utilities
    // Returns true only when it is safe to perform object/asset lookups (LoadObject,
    // LoadAsset, StaticFindObject chains). The engine fatal-asserts those calls while a
    // package is being saved (GIsSavingPackage) or while the game thread is garbage
    // collecting, which crashes the editor. When false, OutReason explains why.
    static bool IsObjectLookupSafe(FString& OutReason);

    // Property utilities
    static bool SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);
}; 