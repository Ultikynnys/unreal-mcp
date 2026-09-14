#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/InputSettings.h"
#include "Camera/CameraActor.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UObjectIterator.h"

// Declare the log category
DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCP, Log, All);

FUnrealMCPBlueprintNodeCommands::FUnrealMCPBlueprintNodeCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("connect_blueprint_nodes"))
    {
        return HandleConnectBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("add_blueprint_get_self_component_reference"))
    {
        return HandleAddBlueprintGetSelfComponentReference(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_node"))
    {
        return HandleAddBlueprintEvent(Params);
    }
    else if (CommandType == TEXT("add_blueprint_function_node"))
    {
        return HandleAddBlueprintFunctionCall(Params);
    }
    else if (CommandType == TEXT("add_blueprint_variable"))
    {
        return HandleAddBlueprintVariable(Params);
    }
    else if (CommandType == TEXT("add_blueprint_input_action_node"))
    {
        return HandleAddBlueprintInputActionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_self_reference"))
    {
        return HandleAddBlueprintSelfReference(Params);
    }
    else if (CommandType == TEXT("find_blueprint_nodes"))
    {
        return HandleFindBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("add_blueprint_node"))
    {
        return HandleAddBlueprintNode(Params);
    }
    else if (CommandType == TEXT("delete_blueprint_node"))
    {
        return HandleDeleteBlueprintNode(Params);
    }
    else if (CommandType == TEXT("clear_blueprint_graph"))
    {
        return HandleClearBlueprintGraph(Params);
    }
    else if (CommandType == TEXT("disconnect_blueprint_pin"))
    {
        return HandleDisconnectBlueprintPin(Params);
    }
    else if (CommandType == TEXT("get_blueprint_graphs"))
    {
        return HandleGetBlueprintGraphs(Params);
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint node command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleConnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString SourceNodeId;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_node_id' parameter"));
    }

    FString TargetNodeId;
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_node_id' parameter"));
    }

    FString SourcePinName;
    if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_pin' parameter"));
    }

    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_pin' parameter"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Resolve preferred graph if specified
    UEdGraph* PreferredGraph = nullptr;
    if (!GraphName.IsEmpty())
    {
        PreferredGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
        if (!PreferredGraph)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
        }
    }

    // Find the nodes
    UEdGraphNode* SourceNode = FUnrealMCPCommonUtils::FindNodeByGuid(Blueprint, SourceNodeId, PreferredGraph);
    UEdGraphNode* TargetNode = FUnrealMCPCommonUtils::FindNodeByGuid(Blueprint, TargetNodeId, PreferredGraph);

    if (!SourceNode || !TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source or target node not found"));
    }

    UEdGraph* GraphToConnect = SourceNode->GetGraph();
    if (!GraphToConnect)
    {
        GraphToConnect = PreferredGraph ? PreferredGraph : FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    }

    // Connect the nodes
    if (FUnrealMCPCommonUtils::ConnectGraphNodes(GraphToConnect, SourceNode, SourcePinName, TargetNode, TargetPinName))
    {
        // Mark the blueprint as modified
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("source_node_id"), SourceNodeId);
        ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
        return ResultObj;
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to connect nodes"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintGetSelfComponentReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get target graph
    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }
    
    // We'll skip component verification since the GetAllNodes API may have changed in UE5.5
    
    // Create the variable get node directly
    UK2Node_VariableGet* GetComponentNode = NewObject<UK2Node_VariableGet>(TargetGraph);
    if (!GetComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create get component node"));
    }
    
    // Set up the variable reference properly for UE5.5
    FMemberReference& VarRef = GetComponentNode->VariableReference;
    VarRef.SetSelfMember(FName(*ComponentName));
    
    // Set node position
    GetComponentNode->NodePosX = NodePosition.X;
    GetComponentNode->NodePosY = NodePosition.Y;
    
    // Add to graph
    TargetGraph->AddNode(GetComponentNode);
    GetComponentNode->CreateNewGuid();
    GetComponentNode->PostPlacedNewNode();
    GetComponentNode->AllocateDefaultPins();
    
    // Explicitly reconstruct node for UE5.5
    GetComponentNode->ReconstructNode();
    
    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), GetComponentNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEvent(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get target graph
    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    // Create the event node
    UK2Node_Event* EventNode = FUnrealMCPCommonUtils::CreateEventNode(TargetGraph, EventName, NodePosition);
    if (!EventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create event node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'function_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Check for target parameter (optional)
    FString Target;
    Params->TryGetStringField(TEXT("target"), Target);

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get target graph
    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    // Find the function
    UFunction* Function = nullptr;
    UK2Node_CallFunction* FunctionNode = nullptr;
    
    // Add extensive logging for debugging
    UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in target '%s'"), 
           *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target);
    
    // Check if we have a target class specified
    if (!Target.IsEmpty())
    {
        // Try to find the target class
        UClass* TargetClass = nullptr;
        
        // First try without a prefix
        TargetClass = FindObject<UClass>(ANY_PACKAGE, *Target);
        UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"), 
               *Target, TargetClass ? TEXT("Found") : TEXT("Not found"));
        
        // If not found, try with U prefix (common convention for UE classes)
        if (!TargetClass && !Target.StartsWith(TEXT("U")))
        {
            FString TargetWithPrefix = FString(TEXT("U")) + Target;
            TargetClass = FindObject<UClass>(ANY_PACKAGE, *TargetWithPrefix);
            UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"), 
                   *TargetWithPrefix, TargetClass ? TEXT("Found") : TEXT("Not found"));
        }
        
        // If still not found, try with common component names
        if (!TargetClass)
        {
            // Try some common component class names
            TArray<FString> PossibleClassNames;
            PossibleClassNames.Add(FString(TEXT("U")) + Target + TEXT("Component"));
            PossibleClassNames.Add(Target + TEXT("Component"));
            
            for (const FString& ClassName : PossibleClassNames)
            {
                TargetClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
                if (TargetClass)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found class using alternative name '%s'"), *ClassName);
                    break;
                }
            }
        }
        
        // Special case handling for common classes like UGameplayStatics
        if (!TargetClass && Target == TEXT("UGameplayStatics"))
        {
            // For UGameplayStatics, use a direct reference to known class
            TargetClass = FindObject<UClass>(ANY_PACKAGE, TEXT("UGameplayStatics"));
            if (!TargetClass)
            {
                // Try loading it from its known package
                TargetClass = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"));
                UE_LOG(LogTemp, Display, TEXT("Explicitly loading GameplayStatics: %s"), 
                       TargetClass ? TEXT("Success") : TEXT("Failed"));
            }
        }
        
        // If we found a target class, look for the function there
        if (TargetClass)
        {
            UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in class '%s'"), 
                   *FunctionName, *TargetClass->GetName());
                   
            // First try exact name
            Function = TargetClass->FindFunctionByName(*FunctionName);
            
            // If not found, try class hierarchy
            UClass* CurrentClass = TargetClass;
            while (!Function && CurrentClass)
            {
                UE_LOG(LogTemp, Display, TEXT("Searching in class: %s"), *CurrentClass->GetName());
                
                // Try exact match
                Function = CurrentClass->FindFunctionByName(*FunctionName);
                
                // Try case-insensitive match
                if (!Function)
                {
                    for (TFieldIterator<UFunction> FuncIt(CurrentClass); FuncIt; ++FuncIt)
                    {
                        UFunction* AvailableFunc = *FuncIt;
                        UE_LOG(LogTemp, Display, TEXT("  - Available function: %s"), *AvailableFunc->GetName());
                        
                        if (AvailableFunc->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive match: %s"), *AvailableFunc->GetName());
                            Function = AvailableFunc;
                            break;
                        }
                    }
                }
                
                // Move to parent class
                CurrentClass = CurrentClass->GetSuperClass();
            }
            
            // Special handling for known functions
            if (!Function)
            {
                if (TargetClass->GetName() == TEXT("GameplayStatics") && 
                    (FunctionName == TEXT("GetActorOfClass") || FunctionName.Equals(TEXT("GetActorOfClass"), ESearchCase::IgnoreCase)))
                {
                    UE_LOG(LogTemp, Display, TEXT("Using special case handling for GameplayStatics::GetActorOfClass"));
                    
                    // Create the function node directly
                    FunctionNode = NewObject<UK2Node_CallFunction>(TargetGraph);
                    if (FunctionNode)
                    {
                        // Direct setup for known function
                        FunctionNode->FunctionReference.SetExternalMember(
                            FName(TEXT("GetActorOfClass")), 
                            TargetClass
                        );
                        
                        FunctionNode->NodePosX = NodePosition.X;
                        FunctionNode->NodePosY = NodePosition.Y;
                        TargetGraph->AddNode(FunctionNode);
                        FunctionNode->CreateNewGuid();
                        FunctionNode->PostPlacedNewNode();
                        FunctionNode->AllocateDefaultPins();
                        
                        UE_LOG(LogTemp, Display, TEXT("Created GetActorOfClass node directly"));
                        
                        // List all pins
                        for (UEdGraphPin* Pin : FunctionNode->Pins)
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Pin: %s, Direction: %d, Category: %s"), 
                                   *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
                        }
                    }
                }
            }
        }
    }
    
    // If we still haven't found the function, try in the blueprint's class
    if (!Function && !FunctionNode)
    {
        UE_LOG(LogTemp, Display, TEXT("Trying to find function in blueprint class"));
        Function = Blueprint->GeneratedClass->FindFunctionByName(*FunctionName);
    }
    
    // Create the function call node if we found the function
    if (Function && !FunctionNode)
    {
        FunctionNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    }
    
    if (!FunctionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function not found: %s in target %s"), *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target));
    }

    // Set parameters if provided
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* ParamsObj;
        if (Params->TryGetObjectField(TEXT("params"), ParamsObj))
        {
            // Process parameters
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamsObj)->Values)
            {
                const FString& ParamName = Param.Key;
                const TSharedPtr<FJsonValue>& ParamValue = Param.Value;
                
                // Find the parameter pin
                UEdGraphPin* ParamPin = FUnrealMCPCommonUtils::FindPin(FunctionNode, ParamName, EGPD_Input);
                if (ParamPin)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found parameter pin '%s' of category '%s'"), 
                           *ParamName, *ParamPin->PinType.PinCategory.ToString());
                    UE_LOG(LogTemp, Display, TEXT("  Current default value: '%s'"), *ParamPin->DefaultValue);
                    if (ParamPin->PinType.PinSubCategoryObject.IsValid())
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Pin subcategory: '%s'"), 
                               *ParamPin->PinType.PinSubCategoryObject->GetName());
                    }
                    
                    // Set parameter based on type
                    if (ParamValue->Type == EJson::String)
                    {
                        FString StringVal = ParamValue->AsString();
                        UE_LOG(LogTemp, Display, TEXT("  Setting string parameter '%s' to: '%s'"), 
                               *ParamName, *StringVal);
                        
                        // Handle class reference parameters (e.g., ActorClass in GetActorOfClass)
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
                        {
                            // For class references, we require the exact class name with proper prefix
                            // - Actor classes must start with 'A' (e.g., ACameraActor)
                            // - Non-actor classes must start with 'U' (e.g., UObject)
                            const FString& ClassName = StringVal;
                            
                            // TODO: This likely won't work in UE5.5+, so don't rely on it.
                            UClass* Class = FindObject<UClass>(ANY_PACKAGE, *ClassName);

                            if (!Class)
                            {
                                Class = LoadObject<UClass>(nullptr, *ClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("FindObject<UClass> failed. Assuming soft path  path: %s"), *ClassName);
                            }
                            
                            // If not found, try with Engine module path
                            if (!Class)
                            {
                                FString EngineClassName = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
                                Class = LoadObject<UClass>(nullptr, *EngineClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("Trying Engine module path: %s"), *EngineClassName);
                            }
                            
                            if (!Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to find class '%s'. Make sure to use the exact class name with proper prefix (A for actors, U for non-actors)"), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to find class '%s'"), *ClassName));
                            }

                            const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
                            if (!K2Schema)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to get K2Schema"));
                                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2Schema"));
                            }

                            K2Schema->TrySetDefaultObject(*ParamPin, Class);
                            if (ParamPin->DefaultObject != Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set class reference for pin '%s'"), *ParamPin->PinName.ToString()));
                            }

                            UE_LOG(LogUnrealMCP, Log, TEXT("Successfully set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                            continue;
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                        {
                            // Ensure we're using an integer value (no decimal)
                            int32 IntValue = FMath::RoundToInt(ParamValue->AsNumber());
                            ParamPin->DefaultValue = FString::FromInt(IntValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set integer parameter '%s' to: %d (string: '%s')"), 
                                   *ParamName, IntValue, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float)
                        {
                            // For other numeric types
                            float FloatValue = ParamValue->AsNumber();
                            ParamPin->DefaultValue = FString::SanitizeFloat(FloatValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set float parameter '%s' to: %f (string: '%s')"), 
                                   *ParamName, FloatValue, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
                        {
                            bool BoolValue = ParamValue->AsBool();
                            ParamPin->DefaultValue = BoolValue ? TEXT("true") : TEXT("false");
                            UE_LOG(LogTemp, Display, TEXT("  Set boolean parameter '%s' to: %s"), 
                                   *ParamName, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && ParamPin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get())
                        {
                            // Handle array parameters - like Vector parameters
                            const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
                            if (ParamValue->TryGetArray(ArrayValue))
                            {
                                // Check if this could be a vector (array of 3 numbers)
                                if (ArrayValue->Num() == 3)
                                {
                                    // Create a proper vector string: (X=0.0,Y=0.0,Z=1000.0)
                                    float X = (*ArrayValue)[0]->AsNumber();
                                    float Y = (*ArrayValue)[1]->AsNumber();
                                    float Z = (*ArrayValue)[2]->AsNumber();
                                    
                                    FString VectorString = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), X, Y, Z);
                                    ParamPin->DefaultValue = VectorString;
                                    
                                    UE_LOG(LogTemp, Display, TEXT("  Set vector parameter '%s' to: %s"), 
                                           *ParamName, *VectorString);
                                    UE_LOG(LogTemp, Display, TEXT("  Final pin value: '%s'"), 
                                           *ParamPin->DefaultValue);
                                }
                                else
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("Array parameter type not fully supported yet"));
                                }
                            }
                        }
                    }
                    else if (ParamValue->Type == EJson::Number)
                    {
                        // Handle integer vs float parameters correctly
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                        {
                            // Ensure we're using an integer value (no decimal)
                            int32 IntValue = FMath::RoundToInt(ParamValue->AsNumber());
                            ParamPin->DefaultValue = FString::FromInt(IntValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set integer parameter '%s' to: %d (string: '%s')"), 
                                   *ParamName, IntValue, *ParamPin->DefaultValue);
                        }
                        else
                        {
                            // For other numeric types
                            float FloatValue = ParamValue->AsNumber();
                            ParamPin->DefaultValue = FString::SanitizeFloat(FloatValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set float parameter '%s' to: %f (string: '%s')"), 
                                   *ParamName, FloatValue, *ParamPin->DefaultValue);
                        }
                    }
                    else if (ParamValue->Type == EJson::Boolean)
                    {
                        bool BoolValue = ParamValue->AsBool();
                        ParamPin->DefaultValue = BoolValue ? TEXT("true") : TEXT("false");
                        UE_LOG(LogTemp, Display, TEXT("  Set boolean parameter '%s' to: %s"), 
                               *ParamName, *ParamPin->DefaultValue);
                    }
                    else if (ParamValue->Type == EJson::Array)
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Processing array parameter '%s'"), *ParamName);
                        // Handle array parameters - like Vector parameters
                        const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
                        if (ParamValue->TryGetArray(ArrayValue))
                        {
                            // Check if this could be a vector (array of 3 numbers)
                            if (ArrayValue->Num() == 3 && 
                                (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) &&
                                (ParamPin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get()))
                            {
                                // Create a proper vector string: (X=0.0,Y=0.0,Z=1000.0)
                                float X = (*ArrayValue)[0]->AsNumber();
                                float Y = (*ArrayValue)[1]->AsNumber();
                                float Z = (*ArrayValue)[2]->AsNumber();
                                
                                FString VectorString = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), X, Y, Z);
                                ParamPin->DefaultValue = VectorString;
                                
                                UE_LOG(LogTemp, Display, TEXT("  Set vector parameter '%s' to: %s"), 
                                       *ParamName, *VectorString);
                                UE_LOG(LogTemp, Display, TEXT("  Final pin value: '%s'"), 
                                       *ParamPin->DefaultValue);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Warning, TEXT("Array parameter type not fully supported yet"));
                            }
                        }
                    }
                    // Add handling for other types as needed
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("Parameter pin '%s' not found"), *ParamName);
                }
            }
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), FunctionNode->NodeGuid.ToString());

    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (UEdGraphPin* Pin : FunctionNode->Pins)
    {
        if (!Pin) { continue; }
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }
    ResultObj->SetArrayField(TEXT("pins"), PinsArray);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_type' parameter"));
    }

    // Get optional parameters
    bool IsExposed = false;
    if (Params->HasField(TEXT("is_exposed")))
    {
        IsExposed = Params->GetBoolField(TEXT("is_exposed"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Create variable based on type
    FEdGraphPinType PinType;
    
    // Set up pin type based on variable_type string
    if (VariableType == TEXT("Boolean"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (VariableType == TEXT("Integer") || VariableType == TEXT("Int"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (VariableType == TEXT("Float"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (VariableType == TEXT("String"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (VariableType == TEXT("Vector"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported variable type: %s"), *VariableType));
    }

    // Optional container type (Array / Set / Map) for the variable.
    FString Container;
    if (Params->TryGetStringField(TEXT("container"), Container))
    {
        if (Container.Equals(TEXT("Array"), ESearchCase::IgnoreCase)) { PinType.ContainerType = EPinContainerType::Array; }
        else if (Container.Equals(TEXT("Set"), ESearchCase::IgnoreCase)) { PinType.ContainerType = EPinContainerType::Set; }
        else if (Container.Equals(TEXT("Map"), ESearchCase::IgnoreCase)) { PinType.ContainerType = EPinContainerType::Map; }
    }

    // Create the variable
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType);

    // Set variable properties
    FBPVariableDescription* NewVar = nullptr;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == FName(*VariableName))
        {
            NewVar = &Variable;
            break;
        }
    }

    if (NewVar)
    {
        // Set exposure in editor
        if (IsExposed)
        {
            NewVar->PropertyFlags |= CPF_Edit;
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("variable_type"), VariableType);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get target graph
    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    // Create the input action node
    UK2Node_InputAction* InputActionNode = FUnrealMCPCommonUtils::CreateInputActionNode(TargetGraph, ActionName, NodePosition);
    if (!InputActionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create input action node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), InputActionNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSelfReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get target graph
    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    // Create the self node
    UK2Node_Self* SelfNode = FUnrealMCPCommonUtils::CreateSelfReferenceNode(TargetGraph, NodePosition);
    if (!SelfNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create self node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), SelfNode->NodeGuid.ToString());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleFindBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    // node_type is optional: an empty value means "every node in the target graph".
    FString NodeType;
    Params->TryGetStringField(TEXT("node_type"), NodeType);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get the target graph
    UEdGraph* TargetGraph = nullptr;
    if (!GraphName.IsEmpty())
    {
        TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
        if (!TargetGraph)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
        }
    }
    else
    {
        TargetGraph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
    }

    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get graph"));
    }

    // Create JSON arrays for the results
    TArray<TSharedPtr<FJsonValue>> NodeGuidArray;
    TArray<TSharedPtr<FJsonValue>> DetailedNodesArray;
    
    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        Params->TryGetStringField(TEXT("event_type"), EventName);
    }

    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (!Node) continue;

        bool bMatches = false;
        if (NodeType.IsEmpty() || NodeType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
        {
            bMatches = true;
        }
        else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
        {
            UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            if (EventNode && (EventName.IsEmpty() || EventNode->EventReference.GetMemberName() == FName(*EventName)))
            {
                bMatches = true;
            }
        }
        else if (NodeType.Equals(TEXT("Function"), ESearchCase::IgnoreCase))
        {
            if (Node->IsA(UK2Node_CallFunction::StaticClass()))
            {
                bMatches = true;
            }
        }
        else if (NodeType.Equals(TEXT("FunctionEntry"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("Entry"), ESearchCase::IgnoreCase))
        {
            if (Node->IsA(UK2Node_FunctionEntry::StaticClass()))
            {
                bMatches = true;
            }
        }
        else if (Node->GetClass()->GetName().Contains(NodeType))
        {
            bMatches = true;
        }

        if (bMatches)
        {
            NodeGuidArray.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));

            TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
            NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
            NodeObj->SetStringField(TEXT("node_name"), Node->GetName());
            NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
            NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
            NodeObj->SetBoolField(TEXT("can_delete"), Node->CanUserDeleteNode());

            TArray<TSharedPtr<FJsonValue>> PinsArray;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
                PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
                PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
                PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);

                TArray<TSharedPtr<FJsonValue>> LinksArray;
                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin && LinkedPin->GetOwningNode())
                    {
                        TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
                        LinkObj->SetStringField(TEXT("node_id"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
                        LinkObj->SetStringField(TEXT("node_name"), LinkedPin->GetOwningNode()->GetName());
                        LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
                        LinksArray.Add(MakeShared<FJsonValueObject>(LinkObj));
                    }
                }
                PinObj->SetArrayField(TEXT("linked_to"), LinksArray);
                PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
            }
            NodeObj->SetArrayField(TEXT("pins"), PinsArray);
            DetailedNodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("node_guids"), NodeGuidArray);
    ResultObj->SetArrayField(TEXT("nodes"), DetailedNodesArray);
    ResultObj->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
    return ResultObj;
}

namespace
{
    // Resolves a class by full path ("/Script/Engine.Actor") or by name, trying the common
    // A*/U* prefixes, then any loaded class of that name.
    UClass* ResolveBlueprintNodeClass(const FString& ClassName)
    {
        if (ClassName.IsEmpty()) { return nullptr; }

        if (ClassName.StartsWith(TEXT("/")))
        {
            if (UClass* C = LoadObject<UClass>(nullptr, *ClassName)) { return C; }
        }

        // UClass object names drop the C++ A/U prefix, so "ACharacter" -> "Character".
        TArray<FString> Candidates;
        Candidates.Add(ClassName);
        if (ClassName.StartsWith(TEXT("A")) || ClassName.StartsWith(TEXT("U")))
        {
            Candidates.Add(ClassName.RightChop(1));
        }
        else
        {
            Candidates.Add(TEXT("A") + ClassName);
            Candidates.Add(TEXT("U") + ClassName);
        }

        for (const FString& Candidate : Candidates)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                if (It->GetName() == Candidate) { return *It; }
            }
        }
        return nullptr;
    }

    // Resolves a UScriptStruct by path ("/Script/CoreUObject.Vector") or by name, trying the
    // common F* prefix so both "FVector"/"Vector" and "FBox"/"Box" resolve.
    UScriptStruct* ResolveScriptStruct(const FString& StructName)
    {
        if (StructName.IsEmpty()) { return nullptr; }

        if (StructName.StartsWith(TEXT("/")))
        {
            if (UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *StructName)) { return S; }
        }

        TArray<FString> Candidates;
        Candidates.Add(StructName);
        if (StructName.StartsWith(TEXT("F"))) { Candidates.Add(StructName.RightChop(1)); }
        else { Candidates.Add(TEXT("F") + StructName); }

        for (const FString& Candidate : Candidates)
        {
            if (UScriptStruct* S = FindObject<UScriptStruct>(ANY_PACKAGE, *Candidate)) { return S; }
        }
        return nullptr;
    }

    // Resolves the target struct for a break_struct/make_struct node: the sugar node types
    // imply their struct, otherwise params.struct_type names it.
    UScriptStruct* ResolveStructForNode(const FString& NodeType, const TSharedPtr<FJsonObject>& NodeParams)
    {
        if (NodeType == TEXT("break_vector") || NodeType == TEXT("make_vector")) { return ResolveScriptStruct(TEXT("Vector")); }
        if (NodeType == TEXT("break_box") || NodeType == TEXT("make_box")) { return ResolveScriptStruct(TEXT("Box")); }

        FString StructName;
        if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("struct_type"), StructName))
        {
            return ResolveScriptStruct(StructName);
        }
        return nullptr;
    }
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeType;
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_type' parameter"));
    }
    NodeType.ToLowerInline();

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    TSharedPtr<FJsonObject> NodeParams;
    const TSharedPtr<FJsonObject>* NodeParamsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("params"), NodeParamsObj) && NodeParamsObj)
    {
        NodeParams = *NodeParamsObj;
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    UEdGraphNode* Node = nullptr;

    if (NodeType == TEXT("branch") || NodeType == TEXT("if"))
    {
        Node = FUnrealMCPCommonUtils::CreateBranchNode(TargetGraph, NodePosition);
    }
    else if (NodeType == TEXT("sequence"))
    {
        int32 NumOutputs = 0;
        if (NodeParams.IsValid()) { NodeParams->TryGetNumberField(TEXT("num_outputs"), NumOutputs); }
        Node = FUnrealMCPCommonUtils::CreateSequenceNode(TargetGraph, NumOutputs, NodePosition);
    }
    else if (NodeType == TEXT("cast"))
    {
        FString TargetClassName;
        if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("target_class"), TargetClassName))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'cast' node requires params.target_class"));
        }
        UClass* TargetClass = ResolveBlueprintNodeClass(TargetClassName);
        if (!TargetClass)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Could not resolve class: %s"), *TargetClassName));
        }
        Node = FUnrealMCPCommonUtils::CreateCastNode(TargetGraph, TargetClass, NodePosition);
    }
    else if (NodeType == TEXT("custom_event"))
    {
        FString EventName;
        if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("event_name"), EventName))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'custom_event' node requires params.event_name"));
        }
        Node = FUnrealMCPCommonUtils::CreateCustomEventNode(TargetGraph, EventName, NodePosition);
    }
    else if (NodeType == TEXT("foreach") || NodeType == TEXT("for_each"))
    {
        Node = FUnrealMCPCommonUtils::CreateMacroNode(TargetGraph, TEXT("ForEachLoop"), NodePosition);
    }
    else if (NodeType == TEXT("spawn_actor"))
    {
        UClass* ActorClass = nullptr;
        FString ActorClassName;
        if (NodeParams.IsValid() && NodeParams->TryGetStringField(TEXT("actor_class"), ActorClassName))
        {
            ActorClass = ResolveBlueprintNodeClass(ActorClassName);
        }
        Node = FUnrealMCPCommonUtils::CreateSpawnActorNode(TargetGraph, ActorClass, NodePosition);
    }
    else if (NodeType == TEXT("variable_get"))
    {
        FString VariableName;
        if (!NodeParams.IsValid() || !NodeParams->TryGetStringField(TEXT("variable_name"), VariableName))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'variable_get' node requires params.variable_name"));
        }
        Node = FUnrealMCPCommonUtils::CreateVariableGetNode(TargetGraph, Blueprint, VariableName, NodePosition);
    }
    else if (NodeType == TEXT("make_transform"))
    {
        UFunction* MakeTransformFn = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("MakeTransform"));
        if (!MakeTransformFn)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Could not find KismetMathLibrary::MakeTransform"));
        }
        Node = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, MakeTransformFn, NodePosition);
    }
    else if (NodeType == TEXT("break_struct") || NodeType == TEXT("break_vector") || NodeType == TEXT("break_box"))
    {
        UScriptStruct* StructType = ResolveStructForNode(NodeType, NodeParams);
        if (!StructType)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'break_struct' node requires params.struct_type (e.g. Vector, Box)"));
        }
        Node = FUnrealMCPCommonUtils::CreateBreakStructNode(TargetGraph, StructType, NodePosition);
    }
    else if (NodeType == TEXT("make_struct") || NodeType == TEXT("make_vector") || NodeType == TEXT("make_box"))
    {
        UScriptStruct* StructType = ResolveStructForNode(NodeType, NodeParams);
        if (!StructType)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'make_struct' node requires params.struct_type (e.g. Vector, Box)"));
        }
        Node = FUnrealMCPCommonUtils::CreateMakeStructNode(TargetGraph, StructType, NodePosition);
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Unsupported node_type: %s (expected branch|sequence|cast|custom_event|foreach|spawn_actor|variable_get|make_transform|break_struct|make_struct)"), *NodeType));
    }

    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create '%s' node"), *NodeType));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    ResultObj->SetStringField(TEXT("node_type"), NodeType);

    // Report the pins so the caller can wire connections with connect_blueprint_nodes.
    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) { continue; }
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
    }
    ResultObj->SetArrayField(TEXT("pins"), PinsArray);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDeleteBlueprintNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    UEdGraph* PreferredGraph = nullptr;
    if (!GraphName.IsEmpty())
    {
        PreferredGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
        if (!PreferredGraph)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
        }
    }

    UEdGraphNode* TargetNode = FUnrealMCPCommonUtils::FindNodeByGuid(Blueprint, NodeId, PreferredGraph);
    if (!TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    if (!TargetNode->CanUserDeleteNode())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Cannot delete protected node: %s"), *TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
    }

    FString DeletedTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
    FString DeletedGuid = TargetNode->NodeGuid.ToString();

    FBlueprintEditorUtils::RemoveNode(Blueprint, TargetNode, false);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("deleted_node_id"), DeletedGuid);
    ResultObj->SetStringField(TEXT("node_title"), DeletedTitle);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleClearBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString GraphName = TEXT("UserConstructionScript");
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    bool bKeepEntryNodes = true;
    if (Params->HasField(TEXT("keep_entry_nodes")))
    {
        bKeepEntryNodes = Params->GetBoolField(TEXT("keep_entry_nodes"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    UEdGraph* TargetGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Graph not found: %s"), *GraphName));
    }

    TArray<UEdGraphNode*> NodesToDelete;
    FString EntryNodeId;

    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (!Node) continue;

        if (bKeepEntryNodes && !Node->CanUserDeleteNode())
        {
            if (TargetGraph->GetSchema())
            {
                TargetGraph->GetSchema()->BreakNodeLinks(*Node);
            }
            if (EntryNodeId.IsEmpty())
            {
                EntryNodeId = Node->NodeGuid.ToString();
            }
        }
        else
        {
            NodesToDelete.Add(Node);
        }
    }

    int32 DeletedCount = NodesToDelete.Num();
    for (UEdGraphNode* Node : NodesToDelete)
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
    ResultObj->SetNumberField(TEXT("deleted_nodes_count"), DeletedCount);
    ResultObj->SetStringField(TEXT("entry_node_id"), EntryNodeId);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleDisconnectBlueprintPin(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    FString PinName;
    Params->TryGetStringField(TEXT("pin_name"), PinName);

    FString TargetNodeId;
    Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId);

    FString TargetPinName;
    Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName);

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    UEdGraph* PreferredGraph = nullptr;
    if (!GraphName.IsEmpty())
    {
        PreferredGraph = FUnrealMCPCommonUtils::FindGraphByName(Blueprint, GraphName);
    }

    UEdGraphNode* Node = FUnrealMCPCommonUtils::FindNodeByGuid(Blueprint, NodeId, PreferredGraph);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    if (!PinName.IsEmpty())
    {
        UEdGraphPin* Pin = FUnrealMCPCommonUtils::FindPin(Node, PinName);
        if (!Pin)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Pin not found: %s on node %s"), *PinName, *NodeId));
        }

        if (!TargetNodeId.IsEmpty() && !TargetPinName.IsEmpty())
        {
            UEdGraphNode* TargetNode = FUnrealMCPCommonUtils::FindNodeByGuid(Blueprint, TargetNodeId, PreferredGraph);
            if (!TargetNode)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
            }
            UEdGraphPin* TargetPin = FUnrealMCPCommonUtils::FindPin(TargetNode, TargetPinName);
            if (!TargetPin)
            {
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target pin not found: %s on node %s"), *TargetPinName, *TargetNodeId));
            }
            Pin->BreakLinkTo(TargetPin);
        }
        else
        {
            Pin->BreakAllPinLinks();
        }
    }
    else
    {
        if (Node->GetGraph() && Node->GetGraph()->GetSchema())
        {
            Node->GetGraph()->GetSchema()->BreakNodeLinks(*Node);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("node_id"), NodeId);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleGetBlueprintGraphs(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    UEdGraph* ConstructionGraph = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint);
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

    TArray<TSharedPtr<FJsonValue>> GraphsArray;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph) continue;

        TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
        GraphObj->SetStringField(TEXT("name"), Graph->GetName());
        GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        GraphObj->SetBoolField(TEXT("is_construction_script"), Graph == ConstructionGraph);
        GraphObj->SetBoolField(TEXT("is_event_graph"), Graph == EventGraph);
        if (Graph->GetSchema())
        {
            GraphObj->SetStringField(TEXT("schema"), Graph->GetSchema()->GetClass()->GetName());
        }
        GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("graphs"), GraphsArray);
    ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
    return ResultObj;
} 