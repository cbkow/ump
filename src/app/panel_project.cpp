// ============================================================================
// Project panel wrapper
// ============================================================================

#include "app/application.h"
#include "project/project_manager.h"

    void Application::CreateProjectPanel() {
        project_manager->CreateProjectPanel(&show_project_panel);
    }
