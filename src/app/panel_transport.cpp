// ============================================================================
// Timeline & Transport panel wrapper
// ============================================================================

#include "app/application.h"
#include <imgui.h>

    void Application::CreateTimelineTransportPanel() {
        if (!show_timeline_panel) return;

        // Style the window with border and rounding (matching annotation style)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

        if (ImGui::Begin("Timeline & Transport", &show_timeline_panel,
            ImGuiWindowFlags_NoCollapse)) {

            RenderTimelineContent();
        }
        ImGui::End();

        ImGui::PopStyleVar(3); // WindowRounding, WindowBorderSize, WindowPadding
        ImGui::PopStyleColor(); // Border
    }
