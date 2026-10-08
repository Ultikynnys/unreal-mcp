#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Handles UMG (Widget Blueprint) commands: create and modify Widget Blueprints, add widget
// components, manage widget instances in the viewport.
class UNREALMCP_API FUnrealMCPUMGCommands
{
public:
    FUnrealMCPUMGCommands();

    // Handle UMG-related commands and return a JSON response or error.
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Create a new UMG Widget Blueprint ("name").
    TSharedPtr<FJsonObject> HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);

    // Add a Text Block to a Widget Blueprint: "blueprint_name", "widget_name", and optionally
    // "text" and "position".
    TSharedPtr<FJsonObject> HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params);

    // Add a widget instance to the viewport: "blueprint_name", optional "z_order".
    TSharedPtr<FJsonObject> HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params);

    // Add a Button to a Widget Blueprint: "blueprint_name", "widget_name", "text", "position".
    TSharedPtr<FJsonObject> HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params);

    // Bind an event to a widget: "blueprint_name", "widget_name", "event_name".
    TSharedPtr<FJsonObject> HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params);

    // Bind TextBlock.Text to <BindingName>: "blueprint_name", "widget_name", "binding_name".
    TSharedPtr<FJsonObject> HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params);
}; 