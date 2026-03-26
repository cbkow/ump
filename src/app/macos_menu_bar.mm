#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#include "app/macos_menu_bar.h"
#include "app/application.h"
#include "utils/debug_utils.h"

#include <algorithm>

// ============================================================================
// Menu item tag enum — used to identify items for state sync and panel toggle
// ============================================================================
enum MenuTag : NSInteger {
    MenuTagFullscreen = 1000,
    MenuTagPanelProject,
    MenuTagPanelInspector,
    MenuTagPanelTimeline,
    MenuTagPanelColor,
    MenuTagPanelAnnotations,
    MenuTagPanelAnnToolbar,
    MenuTagPanelSidebar,
    MenuTagUndo,
    MenuTagRedo,
    MenuTagSaveProject,
    MenuTagResetLayout,
    MenuTagDefaultView,
    MenuTagMinimalView,
    MenuTagShowAllPanels,
};

// ============================================================================
// QCViewMenuTarget — receives all NSMenuItem actions, dispatches to Application
// ============================================================================
@interface QCViewMenuTarget : NSObject
+ (instancetype)shared;
- (void)openMedia:(id)sender;
- (void)openProject:(id)sender;
- (void)saveProject:(id)sender;
- (void)saveProjectAs:(id)sender;
- (void)openRecentFile:(id)sender;
- (void)clearRecentFiles:(id)sender;
- (void)undo:(id)sender;
- (void)redo:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)togglePanel:(id)sender;
- (void)showSettings:(id)sender;
- (void)openURL:(id)sender;
- (void)about:(id)sender;
@end

@implementation QCViewMenuTarget

+ (instancetype)shared {
    static QCViewMenuTarget* instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[QCViewMenuTarget alloc] init];
    });
    return instance;
}

- (void)openMedia:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuOpenMedia();
}

- (void)openProject:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuOpenProject();
}

- (void)saveProject:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuSaveProject();
}

- (void)saveProjectAs:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuSaveProjectAs();
}

- (void)openRecentFile:(id)sender {
    NSMenuItem* item = (NSMenuItem*)sender;
    NSString* path = [item representedObject];
    if (!path) return;

    auto* app = Application::app_instance;
    if (app) app->NativeMenuLoadRecentFile([path UTF8String]);
}

- (void)clearRecentFiles:(id)sender {
    // Clear recent files list and update menu
    Debug::Log("Native menu: Clear recent files");
    qcview::UpdateNativeRecentFiles({});
}

- (void)undo:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuUndo();
}

- (void)redo:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuRedo();
}

- (void)toggleFullscreen:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuToggleFullscreen();
}

- (void)togglePanel:(id)sender {
    NSMenuItem* item = (NSMenuItem*)sender;
    NSInteger tag = [item tag];
    auto* app = Application::app_instance;
    if (app) app->NativeMenuTogglePanel(static_cast<int>(tag));
}

- (void)showSettings:(id)sender {
    auto* app = Application::app_instance;
    if (app) app->NativeMenuShowSettings();
}

- (void)openURL:(id)sender {
    NSMenuItem* item = (NSMenuItem*)sender;
    NSString* urlStr = [item representedObject];
    if (!urlStr) return;

    NSURL* url = [NSURL URLWithString:urlStr];
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
        Debug::Log("Native menu: Opened URL: " + std::string([urlStr UTF8String]));
    }
}

- (void)about:(id)sender {
    [NSApp orderFrontStandardAboutPanel:sender];
}

@end

// ============================================================================
// Module state
// ============================================================================
namespace {
    bool g_initialized = false;
    qcview::NativeMenuState g_last_state;

    // Stored menu item references for state sync
    NSMenuItem* g_fullscreen_item = nil;
    NSMenuItem* g_panel_project_item = nil;
    NSMenuItem* g_panel_inspector_item = nil;
    NSMenuItem* g_panel_timeline_item = nil;
    NSMenuItem* g_panel_color_item = nil;
    NSMenuItem* g_panel_annotations_item = nil;
    NSMenuItem* g_panel_ann_toolbar_item = nil;
    NSMenuItem* g_panel_sidebar_item = nil;
    NSMenuItem* g_undo_item = nil;
    NSMenuItem* g_redo_item = nil;
    NSMenuItem* g_save_item = nil;
    NSMenu* g_recent_files_menu = nil;
}

// ============================================================================
// Helper to create NSMenuItem
// ============================================================================
static NSMenuItem* MakeItem(NSString* title, SEL action, NSString* key,
                            NSUInteger modifiers = 0, NSInteger tag = 0) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key];
    [item setTarget:[QCViewMenuTarget shared]];
    if (modifiers != 0) {
        [item setKeyEquivalentModifierMask:modifiers];
    }
    if (tag != 0) {
        [item setTag:tag];
    }
    return item;
}

static NSMenuItem* MakeURLItem(NSString* title, NSString* urlStr) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:@selector(openURL:)
                                           keyEquivalent:@""];
    [item setTarget:[QCViewMenuTarget shared]];
    [item setRepresentedObject:urlStr];
    return item;
}

// ============================================================================
// Build the menu hierarchy
// ============================================================================
static void BuildAppMenu(NSMenu* mainMenu) {
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"QCView"];

    NSMenuItem* aboutItem = [[NSMenuItem alloc] initWithTitle:@"About QCView"
                                                       action:@selector(about:)
                                                keyEquivalent:@""];
    [aboutItem setTarget:[QCViewMenuTarget shared]];
    [appMenu addItem:aboutItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItem:MakeItem(@"Settings...", @selector(showSettings:), @",",
                              NSEventModifierFlagCommand)];

    [appMenu addItem:[NSMenuItem separatorItem]];

    // Standard app menu items — target NSApp's responder chain
    NSMenuItem* hideItem = [[NSMenuItem alloc] initWithTitle:@"Hide QCView"
                                                      action:@selector(hide:)
                                               keyEquivalent:@"h"];
    [appMenu addItem:hideItem];

    NSMenuItem* hideOthersItem = [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                                            action:@selector(hideOtherApplications:)
                                                     keyEquivalent:@"h"];
    [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [appMenu addItem:hideOthersItem];

    NSMenuItem* showAllItem = [[NSMenuItem alloc] initWithTitle:@"Show All"
                                                         action:@selector(unhideAllApplications:)
                                                  keyEquivalent:@""];
    [appMenu addItem:showAllItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit QCView"
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];

    [appMenuItem setSubmenu:appMenu];
}

static void BuildFileMenu(NSMenu* mainMenu) {
    NSMenuItem* fileMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:fileMenuItem];

    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];

    [fileMenu addItem:MakeItem(@"Open Media...", @selector(openMedia:), @"o",
                               NSEventModifierFlagCommand)];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    // Recent Files submenu
    NSMenuItem* recentMenuItem = [[NSMenuItem alloc] init];
    [recentMenuItem setTitle:@"Recent Files"];
    g_recent_files_menu = [[NSMenu alloc] initWithTitle:@"Recent Files"];

    NSMenuItem* placeholder = [[NSMenuItem alloc] initWithTitle:@"No Recent Files"
                                                         action:nil
                                                  keyEquivalent:@""];
    [placeholder setEnabled:NO];
    [g_recent_files_menu addItem:placeholder];

    [recentMenuItem setSubmenu:g_recent_files_menu];
    [fileMenu addItem:recentMenuItem];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    // Project management
    [fileMenu addItem:MakeItem(@"Open Project...", @selector(openProject:), @"o",
                               NSEventModifierFlagCommand | NSEventModifierFlagShift)];

    g_save_item = MakeItem(@"Save Project", @selector(saveProject:), @"s",
                           NSEventModifierFlagCommand,
                           MenuTagSaveProject);
    [fileMenu addItem:g_save_item];

    [fileMenu addItem:MakeItem(@"Save Project As...", @selector(saveProjectAs:), @"s",
                               NSEventModifierFlagCommand | NSEventModifierFlagShift)];

    [fileMenuItem setSubmenu:fileMenu];
}

static void BuildEditMenu(NSMenu* mainMenu) {
    NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:editMenuItem];

    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];

    g_undo_item = MakeItem(@"Undo", @selector(undo:), @"z",
                           NSEventModifierFlagCommand, MenuTagUndo);
    [editMenu addItem:g_undo_item];

    g_redo_item = MakeItem(@"Redo", @selector(redo:), @"z",
                           NSEventModifierFlagCommand | NSEventModifierFlagShift, MenuTagRedo);
    [editMenu addItem:g_redo_item];

    [editMenuItem setSubmenu:editMenu];
}

static void BuildViewMenu(NSMenu* mainMenu) {
    NSMenuItem* viewMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewMenuItem];

    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];

    // Layout controls
    NSMenuItem* resetLayoutItem = MakeItem(@"Reset Layout", @selector(togglePanel:), @"r",
                                           NSEventModifierFlagCommand, MenuTagResetLayout);
    [viewMenu addItem:resetLayoutItem];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // View modes
    NSMenuItem* defaultViewItem = MakeItem(@"Default View", @selector(togglePanel:), @"0",
                                           NSEventModifierFlagCommand, MenuTagDefaultView);
    [viewMenu addItem:defaultViewItem];

    NSMenuItem* minimalViewItem = MakeItem(@"Minimal View", @selector(togglePanel:), @"-",
                                           NSEventModifierFlagCommand, MenuTagMinimalView);
    [viewMenu addItem:minimalViewItem];

    // Fullscreen — no key equivalent (ImGui handles 'F' key directly)
    g_fullscreen_item = [[NSMenuItem alloc] initWithTitle:@"Fullscreen"
                                                   action:@selector(toggleFullscreen:)
                                            keyEquivalent:@""];
    [g_fullscreen_item setTarget:[QCViewMenuTarget shared]];
    [g_fullscreen_item setTag:MenuTagFullscreen];
    [viewMenu addItem:g_fullscreen_item];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // Show All Panels
    NSMenuItem* showAllItem = MakeItem(@"Show All Panels", @selector(togglePanel:), @"9",
                                       NSEventModifierFlagCommand, MenuTagShowAllPanels);
    [viewMenu addItem:showAllItem];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // Panel toggles (Cmd+1 through Cmd+7)
    g_panel_project_item = MakeItem(@"Project Panel", @selector(togglePanel:), @"1",
                                    NSEventModifierFlagCommand, MenuTagPanelProject);
    [viewMenu addItem:g_panel_project_item];

    g_panel_inspector_item = MakeItem(@"Inspector Panel", @selector(togglePanel:), @"2",
                                      NSEventModifierFlagCommand, MenuTagPanelInspector);
    [viewMenu addItem:g_panel_inspector_item];

    g_panel_timeline_item = MakeItem(@"Timeline Panel", @selector(togglePanel:), @"3",
                                     NSEventModifierFlagCommand, MenuTagPanelTimeline);
    [viewMenu addItem:g_panel_timeline_item];

    g_panel_color_item = MakeItem(@"Color Panels", @selector(togglePanel:), @"4",
                                  NSEventModifierFlagCommand, MenuTagPanelColor);
    [viewMenu addItem:g_panel_color_item];

    g_panel_annotations_item = MakeItem(@"Annotations", @selector(togglePanel:), @"5",
                                        NSEventModifierFlagCommand, MenuTagPanelAnnotations);
    [viewMenu addItem:g_panel_annotations_item];

    g_panel_ann_toolbar_item = MakeItem(@"Annotation Toolbar", @selector(togglePanel:), @"6",
                                        NSEventModifierFlagCommand, MenuTagPanelAnnToolbar);
    [viewMenu addItem:g_panel_ann_toolbar_item];

    [viewMenuItem setSubmenu:viewMenu];
}

static void BuildWindowMenu(NSMenu* mainMenu) {
    NSMenuItem* windowMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:windowMenuItem];

    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];

    NSMenuItem* minimizeItem = [[NSMenuItem alloc] initWithTitle:@"Minimize"
                                                          action:@selector(performMiniaturize:)
                                                   keyEquivalent:@"m"];
    [windowMenu addItem:minimizeItem];

    NSMenuItem* zoomItem = [[NSMenuItem alloc] initWithTitle:@"Zoom"
                                                      action:@selector(performZoom:)
                                               keyEquivalent:@""];
    [windowMenu addItem:zoomItem];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* bringAllItem = [[NSMenuItem alloc] initWithTitle:@"Bring All to Front"
                                                          action:@selector(arrangeInFront:)
                                                   keyEquivalent:@""];
    [windowMenu addItem:bringAllItem];

    [windowMenuItem setSubmenu:windowMenu];
    [NSApp setWindowsMenu:windowMenu];
}

static void BuildHelpMenu(NSMenu* mainMenu) {
    NSMenuItem* helpMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:helpMenuItem];

    NSMenu* helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];

    [helpMenu addItem:MakeURLItem(@"Manual", @"https://qcview.app/")];
    [helpMenu addItem:MakeURLItem(@"License", @"https://github.com/cbkow/QCView-Player/blob/main/LICENSE")];
    [helpMenu addItem:MakeURLItem(@"Check for Updates", @"https://github.com/cbkow/QCView-Player/releases")];

    [helpMenuItem setSubmenu:helpMenu];
    [NSApp setHelpMenu:helpMenu];
}

// ============================================================================
// Public API
// ============================================================================
namespace qcview {

void InitNativeMenuBar() {
    @autoreleasepool {
        Debug::Log("InitNativeMenuBar: Building native macOS menu bar");

        NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"MainMenu"];

        BuildAppMenu(mainMenu);
        BuildFileMenu(mainMenu);
        BuildEditMenu(mainMenu);
        BuildViewMenu(mainMenu);
        BuildWindowMenu(mainMenu);
        BuildHelpMenu(mainMenu);

        [NSApp setMainMenu:mainMenu];

        g_initialized = true;
        g_last_state = NativeMenuState{};

        Debug::Log("InitNativeMenuBar: Native menu bar installed");
    }
}

void ShutdownNativeMenuBar() {
    @autoreleasepool {
        g_initialized = false;
        g_fullscreen_item = nil;
        g_panel_project_item = nil;
        g_panel_inspector_item = nil;
        g_panel_timeline_item = nil;
        g_panel_color_item = nil;
        g_panel_annotations_item = nil;
        g_panel_ann_toolbar_item = nil;
        g_panel_sidebar_item = nil;
        g_undo_item = nil;
        g_redo_item = nil;
        g_save_item = nil;
        g_recent_files_menu = nil;
        Debug::Log("ShutdownNativeMenuBar: Cleaned up");
    }
}

void SyncNativeMenuState(const NativeMenuState& state) {
    if (!g_initialized) return;

    // Early exit if nothing changed
    if (memcmp(&state, &g_last_state, sizeof(NativeMenuState)) == 0) return;
    g_last_state = state;

    @autoreleasepool {
        [g_fullscreen_item setState:state.fullscreen ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_project_item setState:state.show_project ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_inspector_item setState:state.show_inspector ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_timeline_item setState:state.show_timeline ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_color_item setState:state.show_color ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_annotations_item setState:state.show_annotations ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_ann_toolbar_item setState:state.show_ann_toolbar ? NSControlStateValueOn : NSControlStateValueOff];
        [g_panel_sidebar_item setState:state.show_sidebar ? NSControlStateValueOn : NSControlStateValueOff];

        [g_undo_item setEnabled:state.can_undo ? YES : NO];
        [g_redo_item setEnabled:state.can_redo ? YES : NO];
    }
}

void UpdateNativeRecentFiles(const std::vector<std::string>& files) {
    if (!g_initialized || !g_recent_files_menu) return;

    @autoreleasepool {
        [g_recent_files_menu removeAllItems];

        if (files.empty()) {
            NSMenuItem* placeholder = [[NSMenuItem alloc] initWithTitle:@"No Recent Files"
                                                                 action:nil
                                                          keyEquivalent:@""];
            [placeholder setEnabled:NO];
            [g_recent_files_menu addItem:placeholder];
            return;
        }

        for (const auto& file : files) {
            std::string filename = file;
            size_t pos = file.find_last_of("/\\");
            if (pos != std::string::npos) {
                filename = file.substr(pos + 1);
            }

            NSString* title = [NSString stringWithUTF8String:filename.c_str()];
            NSString* path = [NSString stringWithUTF8String:file.c_str()];

            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                          action:@selector(openRecentFile:)
                                                   keyEquivalent:@""];
            [item setTarget:[QCViewMenuTarget shared]];
            [item setRepresentedObject:path];
            [item setToolTip:path];
            [g_recent_files_menu addItem:item];
        }

        [g_recent_files_menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* clearItem = [[NSMenuItem alloc] initWithTitle:@"Clear Recent Files"
                                                           action:@selector(clearRecentFiles:)
                                                    keyEquivalent:@""];
        [clearItem setTarget:[QCViewMenuTarget shared]];
        [g_recent_files_menu addItem:clearItem];
    }
}

bool IsNativeMenuBarActive() {
    return g_initialized;
}

} // namespace qcview

// ============================================================================
// Application NativeMenu* dispatch methods
// ============================================================================
#include "project/project_manager.h"
#include "timeline/timeline_commands.h"
#include "annotations/annotation_toolbar.h"

extern std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;
extern bool show_cache_settings;

void Application::NativeMenuOpenMedia() {
    OpenFileDialog();
}

void Application::NativeMenuOpenProject() {
    if (project_manager) {
        project_manager->LoadProject();
        show_project_panel = true;
        show_inspector_panel = true;
    }
}

void Application::NativeMenuSaveProject() {
    if (project_manager) {
        project_manager->SaveProject();
    }
}

void Application::NativeMenuSaveProjectAs() {
    if (project_manager) {
        project_manager->SaveProjectAs();
    }
}

void Application::NativeMenuLoadRecentFile(const std::string& path) {
    Debug::Log("Native menu: Loading recent file: " + path);

    // Route project files through LoadProject
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "qcvproj" || ext == "uproj") {
        if (project_manager) {
            project_manager->LoadProject(path);
        }
        return;
    }

    if (project_manager) {
        project_manager->LoadSingleFileFromDrop(path);
    } else {
        current_file_path = path;
        if (video_player) {
            video_player->LoadFile(current_file_path);
            video_player->Seek(0.0);
        }
    }
}

void Application::NativeMenuUndo() {
    if (timeline_command_manager) {
        timeline_command_manager->Undo();
    }
}

void Application::NativeMenuRedo() {
    if (timeline_command_manager) {
        timeline_command_manager->Redo();
    }
}

void Application::NativeMenuToggleFullscreen() {
    pending_fullscreen_toggle = true;
}

void Application::NativeMenuTogglePanel(int panel_tag) {
    switch (panel_tag) {
        case MenuTagPanelProject:
            show_project_panel = !show_project_panel;
            if (show_project_panel) minimal_view_mode = false;
            break;
        case MenuTagPanelInspector:
            show_inspector_panel = !show_inspector_panel;
            if (show_inspector_panel) minimal_view_mode = false;
            break;
        case MenuTagPanelTimeline:
            show_timeline_panel = !show_timeline_panel;
            if (show_timeline_panel) minimal_view_mode = false;
            first_time_setup = true;
            break;
        case MenuTagPanelColor:
            show_color_panels = !show_color_panels;
            first_time_setup = true;
            break;
        case MenuTagPanelAnnotations:
            show_annotation_panel = !show_annotation_panel;
            if (show_annotation_panel) minimal_view_mode = false;
            break;
        case MenuTagPanelAnnToolbar:
            show_annotation_toolbar = !show_annotation_toolbar;
            if (annotation_toolbar) annotation_toolbar->SetVisible(show_annotation_toolbar);
            break;
        case MenuTagPanelSidebar:
            show_sidebar_panel = !show_sidebar_panel;
            first_time_setup = true;
            break;
        case MenuTagResetLayout:
            first_time_setup = true;
            Debug::Log("Layout reset requested");
            break;
        case MenuTagDefaultView:
            minimal_view_mode = false;
            SetDefaultView();
            break;
        case MenuTagMinimalView:
            minimal_view_mode = !minimal_view_mode;
            if (minimal_view_mode) {
                saved_show_project_panel = show_project_panel;
                saved_show_inspector_panel = show_inspector_panel;
                saved_show_color_panels = show_color_panels;
                saved_show_annotation_panel = show_annotation_panel;
                saved_show_annotation_toolbar = show_annotation_toolbar;
                show_project_panel = false;
                show_inspector_panel = false;
                show_timeline_panel = true;
                show_color_panels = false;
                show_annotation_panel = false;
                show_annotation_toolbar = false;
                if (annotation_toolbar) annotation_toolbar->SetVisible(false);
                first_time_setup = true;
            } else {
                show_project_panel = saved_show_project_panel;
                show_inspector_panel = saved_show_inspector_panel;
                show_timeline_panel = true;
                show_color_panels = saved_show_color_panels;
                show_annotation_panel = saved_show_annotation_panel;
                show_annotation_toolbar = saved_show_annotation_toolbar;
                if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);
                first_time_setup = true;
            }
            break;
        case MenuTagShowAllPanels:
            minimal_view_mode = false;
            ShowAllPanels();
            break;
    }
}

void Application::NativeMenuShowSettings() {
    show_cache_settings = true;
}

void Application::NativeMenuGetState(bool& fullscreen, bool& proj, bool& inspector,
                                      bool& timeline, bool& color, bool& annotations,
                                      bool& ann_toolbar, bool& sidebar,
                                      bool& can_undo, bool& can_redo, bool& has_proj) {
    fullscreen = is_fullscreen;
    proj = show_project_panel;
    inspector = show_inspector_panel;
    timeline = show_timeline_panel;
    color = show_color_panels;
    annotations = show_annotation_panel;
    ann_toolbar = show_annotation_toolbar;
    sidebar = show_sidebar_panel;
    can_undo = timeline_command_manager ? timeline_command_manager->CanUndo() : false;
    can_redo = timeline_command_manager ? timeline_command_manager->CanRedo() : false;
    has_proj = project_manager && !project_manager->GetProjectPath().empty();
}

#endif // __APPLE__
