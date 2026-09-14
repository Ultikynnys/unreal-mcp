#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Forward declarations
class AActor;
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
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static UEdGraphPin* FindPinOrSplitMember(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static bool SetNodePinDefault(UEdGraphNode* Node, const FString& PinName, const FString& Value);
    static UK2Node_Event* FindExistingEventNode(UEdGraph* Graph, const FString& EventName);

    // Property utilities
    static bool SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);
}; 