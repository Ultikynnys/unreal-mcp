#include "Commands/UnrealMCPCommonUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "Kismet/GameplayStatics.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// JSON Utilities
TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    
    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }
    
    return ResponseObject;
}

void FUnrealMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FUnrealMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

FVector2D FUnrealMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }
    
    return Result;
}

FVector FUnrealMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

FRotator FUnrealMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

// Blueprint Utilities
UBlueprint* FUnrealMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FUnrealMCPCommonUtils::FindBlueprintByName(const FString& InBlueprintName)
{
    if (InBlueprintName.IsEmpty())
    {
        return nullptr;
    }

    FString BlueprintName = InBlueprintName.TrimStartAndEnd();

    // Clean up any duplicate slashes right away
    while (BlueprintName.Contains(TEXT("//")))
    {
        BlueprintName = BlueprintName.Replace(TEXT("//"), TEXT("/"));
    }

    // 1. If caller passed a package path or asset path (starts with "/")
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        // Try UEditorAssetLibrary::LoadAsset first as it loads unloaded packages into memory properly
        UObject* LoadedObj = UEditorAssetLibrary::LoadAsset(BlueprintName);
        if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
        {
            return BP;
        }

        // Try LoadObject with full dot path
        FString FullAssetPath = BlueprintName;
        if (!FullAssetPath.Contains(TEXT(".")))
        {
            FullAssetPath = FullAssetPath + TEXT(".") + FPaths::GetBaseFilename(FullAssetPath);
        }
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FullAssetPath))
        {
            return BP;
        }

        // If it starts with "/" and direct load failed, do NOT prepend /Game/Blueprints/!
        // Instead, search Asset Registry by base asset name:
        FString BaseName = FPaths::GetBaseFilename(BlueprintName);
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;

        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssets(Filter, AssetDataList);

        for (const FAssetData& AssetData : AssetDataList)
        {
            if (AssetData.AssetName.ToString().Equals(BaseName, ESearchCase::IgnoreCase))
            {
                return Cast<UBlueprint>(AssetData.GetAsset());
            }
        }

        return nullptr;
    }

    // 2. Simple name passed (no leading slash):
    // First try standard /Game/Blueprints/<Name>
    FString DefaultPath = TEXT("/Game/Blueprints/") + BlueprintName;
    UObject* LoadedObj = UEditorAssetLibrary::LoadAsset(DefaultPath);
    if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
    {
        return BP;
    }

    FString FullDefaultPath = DefaultPath + TEXT(".") + BlueprintName;
    if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FullDefaultPath))
    {
        return BP;
    }

    // 3. Fall back to searching across the entire Asset Registry for this name anywhere under /Game
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);

    for (const FAssetData& AssetData : AssetDataList)
    {
        if (AssetData.AssetName.ToString().Equals(BlueprintName, ESearchCase::IgnoreCase))
        {
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
    }

    return nullptr;
}

UEdGraph* FUnrealMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Try to find the event graph
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph->GetName().Contains(TEXT("EventGraph")))
        {
            return Graph;
        }
    }
    
    // Create a new event graph if none exists
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    return NewGraph;
}

UEdGraph* FUnrealMCPCommonUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        return FindOrCreateEventGraph(Blueprint);
    }

    if (GraphName.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase) ||
        GraphName.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
        GraphName.Equals(TEXT("Construction"), ESearchCase::IgnoreCase))
    {
        UEdGraph* ConstructionGraph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint);
        if (ConstructionGraph)
        {
            return ConstructionGraph;
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName().Contains(TEXT("Construction")))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    // Exact match first
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
        {
            return Graph;
        }
    }

    // Substring match next
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph && Graph->GetName().Contains(GraphName))
        {
            return Graph;
        }
    }

    return nullptr;
}

UEdGraphNode* FUnrealMCPCommonUtils::FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeGuidStr, UEdGraph* PreferredGraph)
{
    if (NodeGuidStr.IsEmpty())
    {
        return nullptr;
    }

    if (PreferredGraph)
    {
        for (UEdGraphNode* Node : PreferredGraph->Nodes)
        {
            if (Node && (Node->NodeGuid.ToString().Equals(NodeGuidStr, ESearchCase::IgnoreCase) || Node->GetName().Equals(NodeGuidStr, ESearchCase::IgnoreCase)))
            {
                return Node;
            }
        }
    }

    if (Blueprint)
    {
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        for (UEdGraph* Graph : AllGraphs)
        {
            if (Graph == PreferredGraph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && (Node->NodeGuid.ToString().Equals(NodeGuidStr, ESearchCase::IgnoreCase) || Node->GetName().Equals(NodeGuidStr, ESearchCase::IgnoreCase)))
                {
                    return Node;
                }
            }
        }
    }

    return nullptr;
}

// Blueprint node utilities
UK2Node_Event* FUnrealMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Check for existing event node with this exact name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Using existing event node with name %s (ID: %s)"), 
                *EventName, *EventNode->NodeGuid.ToString());
            return EventNode;
        }
    }

    // No existing node found, create a new one
    UK2Node_Event* EventNode = nullptr;
    
    // Find the function to create the event
    UClass* BlueprintClass = Blueprint->GeneratedClass;
    UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));
    
    if (EventFunction)
    {
        EventNode = NewObject<UK2Node_Event>(Graph);
        EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
        EventNode->NodePosX = Position.X;
        EventNode->NodePosY = Position.Y;
        Graph->AddNode(EventNode, true);
        EventNode->PostPlacedNewNode();
        EventNode->AllocateDefaultPins();
        UE_LOG(LogTemp, Display, TEXT("Created new event node with name %s (ID: %s)"), 
            *EventName, *EventNode->NodeGuid.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to find function for event name: %s"), *EventName);
    }
    
    return EventNode;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
{
    if (!Graph || !Function)
    {
        return nullptr;
    }
    
    UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
    FunctionNode->SetFromFunction(Function);
    FunctionNode->NodePosX = Position.X;
    FunctionNode->NodePosY = Position.Y;
    Graph->AddNode(FunctionNode, true);
    FunctionNode->CreateNewGuid();
    FunctionNode->PostPlacedNewNode();
    FunctionNode->AllocateDefaultPins();
    
    return FunctionNode;
}

UK2Node_BreakStruct* FUnrealMCPCommonUtils::CreateBreakStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position)
{
    if (!Graph || !StructType)
    {
        return nullptr;
    }

    UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Graph);
    BreakNode->StructType = StructType;
    BreakNode->NodePosX = Position.X;
    BreakNode->NodePosY = Position.Y;
    Graph->AddNode(BreakNode, true);
    BreakNode->CreateNewGuid();
    BreakNode->PostPlacedNewNode();
    BreakNode->AllocateDefaultPins();

    return BreakNode;
}

UK2Node_MakeStruct* FUnrealMCPCommonUtils::CreateMakeStructNode(UEdGraph* Graph, UScriptStruct* StructType, const FVector2D& Position)
{
    if (!Graph || !StructType)
    {
        return nullptr;
    }

    UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(Graph);
    MakeNode->StructType = StructType;
    MakeNode->NodePosX = Position.X;
    MakeNode->NodePosY = Position.Y;
    Graph->AddNode(MakeNode, true);
    MakeNode->CreateNewGuid();
    MakeNode->PostPlacedNewNode();
    MakeNode->AllocateDefaultPins();

    return MakeNode;
}

UK2Node_VariableGet* FUnrealMCPCommonUtils::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableGetNode->VariableReference.SetSelfMember(VarName);
        VariableGetNode->NodePosX = Position.X;
        VariableGetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableGetNode, true);
        VariableGetNode->CreateNewGuid();
        VariableGetNode->PostPlacedNewNode();
        VariableGetNode->AllocateDefaultPins();
        
        return VariableGetNode;
    }
    
    return nullptr;
}

UK2Node_VariableSet* FUnrealMCPCommonUtils::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableSetNode->VariableReference.SetSelfMember(VarName);
        VariableSetNode->NodePosX = Position.X;
        VariableSetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableSetNode, true);
        VariableSetNode->CreateNewGuid();
        VariableSetNode->PostPlacedNewNode();
        VariableSetNode->AllocateDefaultPins();
        
        return VariableSetNode;
    }
    
    return nullptr;
}

UK2Node_InputAction* FUnrealMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
    InputActionNode->InputActionName = FName(*ActionName);
    InputActionNode->NodePosX = Position.X;
    InputActionNode->NodePosY = Position.Y;
    Graph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();
    
    return InputActionNode;
}

UK2Node_Self* FUnrealMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
    SelfNode->NodePosX = Position.X;
    SelfNode->NodePosY = Position.Y;
    Graph->AddNode(SelfNode, true);
    SelfNode->CreateNewGuid();
    SelfNode->PostPlacedNewNode();
    SelfNode->AllocateDefaultPins();
    
    return SelfNode;
}

UK2Node_IfThenElse* FUnrealMCPCommonUtils::CreateBranchNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(Graph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_ExecutionSequence* FUnrealMCPCommonUtils::CreateSequenceNode(UEdGraph* Graph, int32 NumOutputs, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    UK2Node_ExecutionSequence* Node = NewObject<UK2Node_ExecutionSequence>(Graph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    // Count existing 'then_' execution outputs, then add pins until NumOutputs is reached.
    int32 Existing = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().StartsWith(TEXT("then_")))
        {
            ++Existing;
        }
    }
    while (NumOutputs > 0 && Existing < NumOutputs)
    {
        Node->AddInputPin();
        ++Existing;
    }

    return Node;
}

UK2Node_DynamicCast* FUnrealMCPCommonUtils::CreateCastNode(UEdGraph* Graph, UClass* TargetClass, const FVector2D& Position)
{
    if (!Graph || !TargetClass) { return nullptr; }

    UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(Graph);
    Node->TargetType = TargetClass;
    Node->SetPurity(false); // impure => exec pins, so the cast can gate success/failure
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_CustomEvent* FUnrealMCPCommonUtils::CreateCustomEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    // UK2Node_CustomEvent::CreateFromFunction requires a non-null UFunction, so build the
    // node directly instead (a brand new custom event has no backing function).
    UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
    Node->CustomFunctionName = FName(*EventName);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_MacroInstance* FUnrealMCPCommonUtils::CreateMacroNode(UEdGraph* Graph, const FString& MacroName, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    const FString MacroPath = FString::Printf(
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:%s"), *MacroName);
    UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *MacroPath);
    if (!MacroGraph)
    {
        UE_LOG(LogTemp, Error, TEXT("Could not load standard macro graph: %s"), *MacroPath);
        return nullptr;
    }

    UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(Graph);
    Node->SetMacroGraph(MacroGraph);
    Node->NodePosX = Position.X;
    Node->NodePosY = Position.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();

    return Node;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateSpawnActorNode(UEdGraph* Graph, UClass* ActorClass, const FVector2D& Position)
{
    if (!Graph) { return nullptr; }

    // The dedicated UK2Node_SpawnActorFromClass is a composite node that asserts when built
    // headless, so expose spawning through the underlying gameplay-statics function instead.
    UFunction* SpawnFunction = UGameplayStatics::StaticClass()->FindFunctionByName(TEXT("BeginDeferredActorSpawnFromClass"));
    if (!SpawnFunction) { return nullptr; }

    UK2Node_CallFunction* Node = CreateFunctionCallNode(Graph, SpawnFunction, Position);
    if (Node && ActorClass)
    {
        if (UEdGraphPin* ClassPin = FindPin(Node, TEXT("ActorClass"), EGPD_Input))
        {
            ClassPin->DefaultObject = ActorClass;
        }
    }

    return Node;
}

bool FUnrealMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph || !SourceNode || !TargetNode)
    {
        return false;
    }
    
    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    if (!SourcePin)
    {
        // Not a top-level pin: maybe a split struct member ("X", "Min", or "ReturnValue_X").
        SourcePin = FindPinOrSplitMember(SourceNode, SourcePinName, EGPD_Output);
    }
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    if (!TargetPin)
    {
        TargetPin = FindPinOrSplitMember(TargetNode, TargetPinName, EGPD_Input);
    }
    
    if (SourcePin && TargetPin)
    {
        // Route through the graph schema so wildcard pins (Cast.Object, ForEach.Array, ...)
        // propagate their type and the link survives recompiles. A raw MakeLinkTo skips the
        // schema's connection notifications and leaves those pins untyped ("undetermined").
        const UEdGraphSchema* Schema = Graph->GetSchema();
        if (Schema && Schema->TryCreateConnection(SourcePin, TargetPin))
        {
            return true;
        }
        return false;
    }
    
    return false;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    
    // Log all pins for debugging
    UE_LOG(LogTemp, Display, TEXT("FindPin: Looking for pin '%s' (Direction: %d) in node '%s'"), 
           *PinName, (int32)Direction, *Node->GetName());
    
    for (UEdGraphPin* Pin : Node->Pins)
    {
        UE_LOG(LogTemp, Display, TEXT("  - Available pin: '%s', Direction: %d, Category: %s"), 
               *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
    }
    
    // First try exact match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found exact matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If no exact match and we're looking for a component reference, try case-insensitive match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) && 
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If we're looking for a component output and didn't find it by name, try to find the first data output pin
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                UE_LOG(LogTemp, Display, TEXT("  - Found fallback data output pin: '%s'"), *Pin->PinName.ToString());
                return Pin;
            }
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("  - No matching pin found for '%s'"), *PinName);
    return nullptr;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPinOrSplitMember(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }

    // A caller may pass "Parent.Member" to disambiguate; a bare name has no explicit parent.
    FString ParentPart;
    FString MemberPart = PinName;
    if (PinName.Contains(TEXT(".")))
    {
        PinName.Split(TEXT("."), &ParentPart, &MemberPart);
    }

    // 1) Already-split struct pins keep their children in <ParentPin>->SubPins (NOT Node->Pins),
    //    named "<ParentPinName>_<Member>". Match by full name or by the trailing member segment.
    for (UEdGraphPin* ParentPin : Node->Pins)
    {
        if (!ParentPin || (Direction != EGPD_MAX && ParentPin->Direction != Direction) || ParentPin->SubPins.Num() == 0)
        {
            continue;
        }
        if (!ParentPart.IsEmpty() && !ParentPin->PinName.ToString().Equals(ParentPart, ESearchCase::IgnoreCase))
        {
            continue;
        }
        for (UEdGraphPin* SubPin : ParentPin->SubPins)
        {
            if (!SubPin)
            {
                continue;
            }
            const FString SubName = SubPin->PinName.ToString();
            if (SubName.Equals(PinName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
            FString SubParent, SubMember;
            if (SubName.Split(TEXT("_"), &SubParent, &SubMember, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
                && SubMember.Equals(MemberPart, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
        }
    }

    // 2) Not split yet: find a struct pin whose struct defines the requested member, split it
    //    through the schema, then resolve the freshly created child pin from its SubPins.
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || (Direction != EGPD_MAX && Pin->Direction != Direction) || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
        {
            continue;
        }
        if (!ParentPart.IsEmpty() && !Pin->PinName.ToString().Equals(ParentPart, ESearchCase::IgnoreCase))
        {
            continue;
        }
        UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
        if (!Struct)
        {
            continue;
        }

        // The requested pin may be a bare member ("Max") or the full split name the schema
        // generates ("ReturnValue_Max" = "<ParentPinName>_<Member>").
        const FString ParentPinName = Pin->PinName.ToString();
        FString MemberName = MemberPart;
        if (PinName.StartsWith(ParentPinName + TEXT("_")))
        {
            MemberName = PinName.RightChop(ParentPinName.Len() + 1);
        }
        if (!Struct->FindPropertyByName(FName(*MemberName)))
        {
            continue;
        }

        const UEdGraphSchema* Schema = Node->GetSchema();
        if (!Schema)
        {
            continue;
        }
        Schema->SplitPin(Pin);
        for (UEdGraphPin* SubPin : Pin->SubPins)
        {
            if (!SubPin)
            {
                continue;
            }
            const FString SubName = SubPin->PinName.ToString();
            if (SubName.Equals(PinName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
            FString SubParent, SubMember;
            if (SubName.Split(TEXT("_"), &SubParent, &SubMember, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
                && SubMember.Equals(MemberName, ESearchCase::IgnoreCase))
            {
                return SubPin;
            }
        }
    }

    return nullptr;
}

bool FUnrealMCPCommonUtils::SetNodePinDefault(UEdGraphNode* Node, const FString& PinName, const FString& Value)
{
    if (!Node)
    {
        return false;
    }

    // Exact top-level pin first; otherwise a split struct member (accepts "X" or "Vector.X").
    UEdGraphPin* Pin = FindPin(Node, PinName, EGPD_MAX);
    if (!Pin)
    {
        Pin = FindPinOrSplitMember(Node, PinName, EGPD_MAX);
    }
    if (!Pin)
    {
        return false;
    }

    const UEdGraphSchema* Schema = Node->GetSchema();
    if (!Schema)
    {
        return false;
    }

    // TrySetDefaultValue validates/converts per pin type and marks the graph modified.
    Schema->TrySetDefaultValue(*Pin, Value);
    return true;
}

// Actor utilities
TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ActorToJsonObject(AActor* Actor, bool bDetailed)
{
    if (!Actor)
    {
        return nullptr;
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return ActorObject;
}

UK2Node_Event* FUnrealMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    // Look for existing event nodes
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Found existing event node with name: %s"), *EventName);
            return EventNode;
        }
    }

    return nullptr;
}

bool FUnrealMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);
    
    // Handle different property types
    if (Property->IsA<FBoolProperty>())
    {
        ((FBoolProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsBool());
        return true;
    }
    else if (Property->IsA<FIntProperty>())
    {
        int32 IntValue = static_cast<int32>(Value->AsNumber());
        FIntProperty* IntProperty = CastField<FIntProperty>(Property);
        if (IntProperty)
        {
            IntProperty->SetPropertyValue_InContainer(Object, IntValue);
            return true;
        }
    }
    else if (Property->IsA<FFloatProperty>())
    {
        ((FFloatProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsNumber());
        return true;
    }
    else if (Property->IsA<FStrProperty>())
    {
        ((FStrProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsString());
        return true;
    }
    else if (Property->IsA<FByteProperty>())
    {
        FByteProperty* ByteProp = CastField<FByteProperty>(Property);
        UEnum* EnumDef = ByteProp ? ByteProp->GetIntPropertyEnum() : nullptr;
        
        // If this is a TEnumAsByte property (has associated enum)
        if (EnumDef)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
                ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %d"), 
                      *PropertyName, ByteValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    uint8 ByteValue = FCString::Atoi(*EnumValueName);
                    ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %d"), 
                          *PropertyName, *EnumValueName, ByteValue);
                    return true;
                }
                
                // Handle qualified enum names (e.g., "Player0" or "EAutoReceiveInput::Player0")
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
        else
        {
            // Regular byte property
            uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
            ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
            return true;
        }
    }
    else if (Property->IsA<FEnumProperty>())
    {
        FEnumProperty* EnumProp = CastField<FEnumProperty>(Property);
        UEnum* EnumDef = EnumProp ? EnumProp->GetEnum() : nullptr;
        FNumericProperty* UnderlyingNumericProp = EnumProp ? EnumProp->GetUnderlyingProperty() : nullptr;
        
        if (EnumDef && UnderlyingNumericProp)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                int64 EnumValue = static_cast<int64>(Value->AsNumber());
                UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %lld"), 
                      *PropertyName, EnumValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    int64 EnumValue = FCString::Atoi64(*EnumValueName);
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                
                // Handle qualified enum names
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
    }
    
    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"), 
                                    *Property->GetClass()->GetName(), *PropertyName);
    return false;
} 