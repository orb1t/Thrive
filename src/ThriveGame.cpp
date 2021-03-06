// ------------------------------------ //
#include "ThriveGame.h"

#include "engine/player_data.h"
#include "general/global_keypresses.h"

#include "generated/cell_stage_world.h"
#include "generated/microbe_editor_world.h"
#include "main_menu_keypresses.h"
#include "microbe_stage/simulation_parameters.h"
#include "scripting/script_initializer.h"
#include "thrive_net_handler.h"
#include "thrive_version.h"
#include "thrive_world_factory.h"


#include <Addons/GameModuleLoader.h>
#include <GUI/GuiView.h>
#include <Handlers/ObjectLoader.h>
#include <Networking/NetworkHandler.h>
#include <Physics/PhysicsMaterialManager.h>
#include <Rendering/GeometryHelpers.h>
#include <Script/Bindings/BindHelpers.h>
#include <Script/Bindings/StandardWorldBindHelper.h>
#include <Script/ScriptExecutor.h>
#include <Window.h>

#include <OgreManualObject.h>
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSubMesh2.h>


using namespace thrive;

// ------------------------------------ //
//! Contains properties that would need unnecessary large includes in the header
class ThriveGame::Implementation {
public:
    Implementation(ThriveGame& game) :
        m_game(game), m_playerData("player"),
        m_menuKeyPresses(std::make_shared<MainMenuKeyPressListener>()),
        m_globalKeyPresses(std::make_shared<GlobalUtilityKeyHandler>(
            *game.ApplicationConfiguration->GetKeyConfiguration())),
        m_cellStageKeys(std::make_shared<PlayerMicrobeControl>(
            *game.ApplicationConfiguration->GetKeyConfiguration()))
    {}

    //! Releases Ogre things. Needs to be called before shutdown
    void
        releaseOgreResources()
    {
        destroyBackgroundItem();

        if(m_microbeBackgroundMesh) {

            Ogre::MeshManager::getSingleton().remove(m_microbeBackgroundMesh);
            m_microbeBackgroundMesh.reset();
            m_microbeBackgroundSubMesh = nullptr;
        }

        if(m_microbeEditorBackgroundMesh) {

            Ogre::MeshManager::getSingleton().remove(
                m_microbeEditorBackgroundMesh);
            m_microbeEditorBackgroundMesh.reset();
            m_microbeEditorBackgroundSubMesh = nullptr;
        }
    }

    void
        destroyBackgroundItem()
    {
        if(m_microbeBackgroundItem) {

            m_cellStage->GetScene()->destroyItem(m_microbeBackgroundItem);
            m_microbeBackgroundItem = nullptr;
        }

        if(m_microbeEditorBackgroundItem) {
            m_microbeEditor->GetScene()->destroyItem(
                m_microbeEditorBackgroundItem);
            m_microbeEditorBackgroundItem = nullptr;
        }
    }

    void
        createBackgroundItem()
    {
        destroyBackgroundItem();

        m_microbeBackgroundItem = m_cellStage->GetScene()->createItem(
            m_microbeBackgroundMesh, Ogre::SCENE_STATIC);
        m_microbeBackgroundItem->setCastShadows(false);

        // Need to edit the render queue and add it to an early one
        m_microbeBackgroundItem->setRenderQueueGroup(1);

        // Editor version
        // We only checked the first background item before we did this. Not the
        // editor one.
        if(m_microbeEditor) {
            if(!m_microbeEditorBackgroundItem) {
                m_microbeEditorBackgroundItem =
                    m_microbeEditor->GetScene()->createItem(
                        m_microbeEditorBackgroundMesh, Ogre::SCENE_STATIC);
                m_microbeEditorBackgroundItem->setCastShadows(false);

                // Need to edit the render queue and add it to an early one
                m_microbeEditorBackgroundItem->setRenderQueueGroup(1);
            }
        }

        // Re-attach if the nodes exist
        // Add it
        if(m_backgroundRenderNode)
            m_backgroundRenderNode->attachObject(m_microbeBackgroundItem);

        if(m_editorBackgroundRenderNode)
            m_editorBackgroundRenderNode->attachObject(
                m_microbeEditorBackgroundItem);
    }

    ThriveGame& m_game;

    PlayerData m_playerData;

    std::shared_ptr<CellStageWorld> m_cellStage;
    std::shared_ptr<MicrobeEditorWorld> m_microbeEditor;

    // This contains all the microbe_stage AngelScript code
    Leviathan::GameModule::pointer m_microbeScripts;

    // This is "temporarily" merged with the microbe scripts as this needs to
    // share some types
    // // This contains all the microbe_editor AngelScript code
    // Leviathan::GameModule::pointer m_MicrobeEditorScripts;

    //! This is the background object of the cell stage
    Ogre::MeshPtr m_microbeBackgroundMesh;
    Ogre::SubMesh* m_microbeBackgroundSubMesh;
    Ogre::MeshPtr m_microbeEditorBackgroundMesh;
    Ogre::SubMesh* m_microbeEditorBackgroundSubMesh;
    Ogre::Item* m_microbeBackgroundItem = nullptr;
    Ogre::SceneNode* m_backgroundRenderNode = nullptr;

    Ogre::Item* m_microbeEditorBackgroundItem = nullptr;
    Ogre::SceneNode* m_editorBackgroundRenderNode = nullptr;

    std::shared_ptr<MainMenuKeyPressListener> m_menuKeyPresses;
    std::shared_ptr<GlobalUtilityKeyHandler> m_globalKeyPresses;
    std::shared_ptr<PlayerMicrobeControl> m_cellStageKeys;
};

// ------------------------------------ //
ThriveGame::ThriveGame()
{
    StaticGame = this;
}

ThriveGame::~ThriveGame()
{
    StaticGame = nullptr;
}

std::string
    ThriveGame::GenerateWindowTitle()
{
    return "Thrive " GAME_VERSIONS;
}

ThriveGame*
    ThriveGame::Get()
{
    return StaticGame;
}

ThriveGame*
    ThriveGame::instance()
{
    return StaticGame;
}

ThriveGame* ThriveGame::StaticGame = nullptr;

Leviathan::NetworkInterface*
    ThriveGame::_GetApplicationPacketHandler()
{
    if(!Network)
        Network = std::make_unique<ThriveNetHandler>();
    return Network.get();
}

void
    ThriveGame::_ShutdownApplicationPacketHandler()
{
    Network.reset();
}
// ------------------------------------ //
void
    ThriveGame::startNewGame()
{
    // Used to stop it from clearing entities when rwe restart so species can
    // spawn.
    bool restarted = false;
    // To work with instant start, we need to invoke this if we have no cell
    // stage world
    if(!m_postLoadRan) {

        Engine::Get()->Invoke([=]() { startNewGame(); });
        return;
    }

    Leviathan::Engine* engine = Engine::GetEngine();

    LOG_INFO("New game started");

    Leviathan::Window* window1 = engine->GetWindowEntity();

    // Create world if not already created //
    if(!m_impl->m_cellStage) {
        LOG_INFO("ThriveGame: startNewGame: Creating new cellstage world");
        m_impl->m_cellStage =
            std::dynamic_pointer_cast<CellStageWorld>(engine->CreateWorld(
                window1, static_cast<int>(THRIVE_WORLD_TYPE::CELL_STAGE),
                createPhysicsMaterials(),
                Leviathan::WorldNetworkSettings::GetSettingsForClient()));
    } else {
        restarted = true;
        m_impl->m_cellStage->ClearEntities();
        // We also need to somehow force stop all the music here.
        LOG_INFO("ThriveGame: startNewGame called after already created once: "
                 "Creating new cellstage world");
    }

    LEVIATHAN_ASSERT(m_impl->m_cellStage, "Cell stage world creation failed");

    window1->LinkObjects(m_impl->m_cellStage);

    // Set the right input handlers active //
    m_impl->m_menuKeyPresses->setEnabled(false);
    m_impl->m_cellStageKeys->setEnabled(true);

    // And switch the GUI mode to allow key presses through
    auto layer = window1->GetGui()->GetLayerByIndex(0);

    // Allow running without GUI
    if(layer)
        layer->SetInputMode(Leviathan::GUI::INPUT_MODE::Gameplay);


    // Clear world //
    if(restarted == false) {
        m_impl->m_cellStage->ClearEntities();
    }

    // TODO: unpause, if it was paused

    // Main camera that will be attached to the player
    m_cellCamera = Leviathan::ObjectLoader::LoadCamera(*m_impl->m_cellStage,
        Float3(0, 15, 0),
        Ogre::Quaternion(Ogre::Degree(-90), Ogre::Vector3::UNIT_X));

    // Link the camera to the camera control system
    m_impl->m_cellStage->GetMicrobeCameraSystem().setCameraEntity(m_cellCamera);

    // TODO: attach a ligth to the camera
    // -- Light
    //     local light = OgreLightComponent.new()
    //     light:setRange(200)
    //     entity:addComponent(light)

    m_impl->m_cellStage->SetCamera(m_cellCamera);

    // Setup compound clouds //

    // This is needed for the compound clouds to work in generale
    const auto compoundCount = SimulationParameters::compoundRegistry.getSize();

    LEVIATHAN_ASSERT(SimulationParameters::compoundRegistry.getSize() > 0,
        "compound registry is empty when creating cloud entities for them");
    std::unordered_map<Leviathan::ObjectID, thrive::CompoundCloudComponent> u =
        {};

    std::vector<Compound> clouds;

    for(size_t i = 0; i < compoundCount; ++i) {

        const auto& data =
            SimulationParameters::compoundRegistry.getTypeData(i);

        if(!data.isCloud)
            continue;

        clouds.push_back(data);
    }

    m_impl->m_cellStage->GetCompoundCloudSystem().registerCloudTypes(
        *m_impl->m_cellStage, clouds);

    // Let the script do setup //
    // This registers all the script defined systems to run and be
    // available from the world
    LEVIATHAN_ASSERT(m_impl->m_microbeScripts, "microbe scripts not loaded");

    LOG_INFO("Calling world setup script setupScriptsForWorld");

    ScriptRunningSetup setup;
    setup.SetEntrypoint("setupScriptsForWorld");

    auto result = m_impl->m_microbeScripts->ExecuteOnModule<void>(
        setup, false, m_impl->m_cellStage.get());

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR(
            "Failed to run script setup function: " + setup.Entryfunction);
        MarkAsClosing();
        return;
    }

    LOG_INFO("Finished calling setupScriptsForWorld");

    // Set background plane //
    // This is needed to be created here for biome.as to work correctly
    // Also this is a manual object and with infinite extent as this isn't
    // perspective projected in the shader
    m_impl->m_backgroundRenderNode =
        m_impl->m_cellStage->GetScene()->createSceneNode(Ogre::SCENE_STATIC);

    // This needs to be manually destroyed later
    if(!m_impl->m_microbeBackgroundMesh) {
        m_impl->m_microbeBackgroundMesh =
            Leviathan::GeometryHelpers::CreateScreenSpaceQuad(
                "CellStage_background", -1, -1, 2, 2);

        m_impl->m_microbeBackgroundSubMesh =
            m_impl->m_microbeBackgroundMesh->getSubMesh(0);

        m_impl->m_microbeBackgroundSubMesh->setMaterialName("Background");
    }
    // This also needs to be manually destroyed later.
    if(!m_impl->m_microbeEditorBackgroundMesh) {
        m_impl->m_microbeEditorBackgroundMesh =
            Leviathan::GeometryHelpers::CreateScreenSpaceQuad(
                "Editor_background", -1, -1, 2, 2);

        m_impl->m_microbeEditorBackgroundSubMesh =
            m_impl->m_microbeEditorBackgroundMesh->getSubMesh(0);

        m_impl->m_microbeEditorBackgroundSubMesh->setMaterialName("Background");
    }
    // Setup render queue for it
    m_impl->m_cellStage->GetScene()->getRenderQueue()->setRenderQueueMode(
        1, Ogre::RenderQueue::FAST);

    // This now attaches the item as well (as long as the scene node is created)
    // This makes it easier to manage the multiple backgrounds and reattaching
    // them
    if(!m_impl->m_microbeBackgroundItem) {
        m_impl->createBackgroundItem();
    }

    // Gen species if this is a restart//
    if(restarted) {
        ScriptRunningSetup setup("resetWorld");
        auto returned =
            ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<void>(
                setup, false, m_impl->m_cellStage.get());

        if(returned.Result != SCRIPT_RUN_RESULT::Success)
            LOG_ERROR("Failed to run script side resetWorld");
    }
    // Spawn player //
    setup = ScriptRunningSetup("setupPlayer");

    result = m_impl->m_microbeScripts->ExecuteOnModule<void>(
        setup, false, m_impl->m_cellStage.get());

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR("Failed to spawn player!");
        return;
    }
}

void
    ThriveGame::loadSaveGame(const std::string& saveFile)
{
    // i hate the very idea of writing the same code twice, so i want to call
    // startNewGame first
    LOG_INFO("saved game being loaded");
    ThriveGame::startNewGame();
    // start a new game, then run a loading script
    //(the script will disable the tutorial etc)
}

void
    ThriveGame::saveGame(const std::string& saveFile)
{
    // i hate the very idea of writing the same code twice, so i want to call
    // startNewGame first
    LOG_INFO("game being saved");
    // start a new game, then run a loading script
    //(the script will disable the tutorial etc)
}
// ------------------------------------ //
bool
    ThriveGame::scriptSetup()
{
    LOG_INFO("Calling global setup script setupProcesses");

    ScriptRunningSetup setup("setupProcesses");

    auto result = m_impl->m_microbeScripts->ExecuteOnModule<void>(setup, false);

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR(
            "Failed to run script setup function: " + setup.Entryfunction);
        return false;
    }

    LOG_INFO("Finished calling the above setup script");

    LOG_INFO("Calling global setup script setupOrganelles");

    setup = ScriptRunningSetup("setupOrganelles");

    result = m_impl->m_microbeScripts->ExecuteOnModule<void>(setup, false);

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR(
            "Failed to run script setup function: " + setup.Entryfunction);
        return false;
    }

    LOG_INFO("Finished calling the above setup script");


    LOG_INFO("Finished calling script setup");
    return true;
}
// ------------------------------------ //
CellStageWorld*
    ThriveGame::getCellStage()
{
    return m_impl->m_cellStage.get();
}

PlayerData&
    ThriveGame::playerData()
{
    return m_impl->m_playerData;
}

PlayerMicrobeControl*
    ThriveGame::getPlayerInput()
{
    return m_impl->m_cellStageKeys.get();
}

Leviathan::GameModule*
    ThriveGame::getMicrobeScripts()
{
    return m_impl->m_microbeScripts.get();
}
// ------------------------------------ //
void
    ThriveGame::killPlayerCellClicked()
{
    LOG_INFO("Calling killPlayerCellClicked");

    ScriptRunningSetup setup = ScriptRunningSetup("killPlayerCellClicked");

    auto result = m_impl->m_microbeScripts->ExecuteOnModule<void>(
        setup, false, m_impl->m_cellStage.get());

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR(
            "Failed to run killPlayerCellClicked: " + setup.Entryfunction);
    }
}

void
    ThriveGame::editorButtonClicked()
{
    LOG_INFO("Editor button pressed");

    // Fire an event to switch over the GUI
    Engine::Get()->GetEventHandler()->CallEvent(
        new Leviathan::GenericEvent("MicrobeEditorEntered"));

    Leviathan::Engine* engine = Engine::GetEngine();
    Leviathan::Window* window1 = engine->GetWindowEntity();

    // Create an editor world
    LOG_INFO("Entering MicrobeEditor");

    // Create world if not already created //
    if(!m_impl->m_microbeEditor) {

        LOG_INFO("ThriveGame: editorButtonClicked: Creating new microbe editor "
                 "world");
        m_impl->m_microbeEditor =
            std::dynamic_pointer_cast<MicrobeEditorWorld>(engine->CreateWorld(
                window1, static_cast<int>(THRIVE_WORLD_TYPE::MICROBE_EDITOR),
                createPhysicsMaterials(),
                Leviathan::WorldNetworkSettings::GetSettingsForClient()));
    }

    LEVIATHAN_ASSERT(
        m_impl->m_microbeEditor, "Microbe editor world creation failed");

    // Link the new world to the window (this will automatically make
    // the old one go to the background)
    window1->LinkObjects(m_impl->m_microbeEditor);

    // Set the right input handlers active //
    m_impl->m_menuKeyPresses->setEnabled(false);
    m_impl->m_cellStageKeys->setEnabled(false);
    // // TODO: editor hotkeys. Maybe they should be in the GUI

    // // So using this
    // // // And switch the GUI mode to allow key presses through
    // Leviathan::GUI::View* view = window1->GetGui()->GetLayerByIndex(0);

    // // Allow running without GUI
    // if(view)
    //     view->SetInputMode(Leviathan::GUI::INPUT_MODE::Menu);


    // Clear world //
    m_impl->m_microbeEditor->ClearEntities();

    // Main camera that will be attached to the player
    auto camera = Leviathan::ObjectLoader::LoadCamera(*m_impl->m_microbeEditor,
        Float3(0, 15, 0),
        Ogre::Quaternion(Ogre::Degree(-90), Ogre::Vector3::UNIT_X));

    // TODO: attach a ligth to the camera
    // -- Light
    //     local light = OgreLightComponent.new()
    //     light:setRange(200)
    //     entity:addComponent(light)

    m_impl->m_microbeEditor->SetCamera(camera);

    // Background
    // Ill add an if statement here and see what happens
    if(!m_impl->m_editorBackgroundRenderNode) {
        m_impl->m_editorBackgroundRenderNode =
            m_impl->m_microbeEditor->GetScene()->createSceneNode(
                Ogre::SCENE_STATIC);
        // Setup render queue for it
        m_impl->m_microbeEditor->GetScene()
            ->getRenderQueue()
            ->setRenderQueueMode(1, Ogre::RenderQueue::FAST);
        // This also attaches it
        m_impl->createBackgroundItem();
    }

    // Let the script do setup //
    // This registers all the script defined systems to run and be
    // available from the world
    // LEVIATHAN_ASSERT(
    //     m_impl->m_MicrobeEditorScripts, "microbe editor scripts not loaded");

    LOG_INFO("Calling editor setup script onEditorEntry");

    ScriptRunningSetup setup("onEditorEntry");

    auto result = m_impl->m_microbeScripts->ExecuteOnModule<void>(
        setup, false, m_impl->m_microbeEditor.get());

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR(
            "Failed to run editor setup function: " + setup.Entryfunction);
        return;
    }
}

void
    ThriveGame::finishEditingClicked()
{
    LOG_INFO("Finish editing pressed");

    // Fire an event to switch over the GUI
    // And make the Editor apply current changes to the player Species, the
    // microbe stage code will apply it to the player cell in onReturnFromEditor
    Engine::Get()->GetEventHandler()->CallEvent(
        new Leviathan::GenericEvent("MicrobeEditorExited"));

    Leviathan::Engine* engine = Engine::GetEngine();
    Leviathan::Window* window1 = engine->GetWindowEntity();

    // Make the cell world to be back in the foreground
    LEVIATHAN_ASSERT(m_impl->m_cellStage,
        "Cell stage world not created before exiting the editor");

    // This will automatically background the editor world
    window1->LinkObjects(m_impl->m_cellStage);

    // Set the right input handlers active //
    m_impl->m_menuKeyPresses->setEnabled(false);
    m_impl->m_cellStageKeys->setEnabled(true);
    // // TODO: editor hotkeys. Maybe they should be in the GUI

    // // So using this
    // // // And switch the GUI mode to allow key presses through
    // Leviathan::GUI::View* view = window1->GetGui()->GetLayerByIndex(0);

    // // Allow running without GUI
    // if(view)
    //     view->SetInputMode(Leviathan::GUI::INPUT_MODE::Gameplay);


    // Run the post editing script

    // Let the script do setup //
    // This registers all the script defined systems to run and be
    // available from the world
    LEVIATHAN_ASSERT(
        m_impl->m_microbeScripts, "microbe stage scripts not loaded");

    LOG_INFO("Calling return from editor script, onReturnFromEditor");

    ScriptRunningSetup setup("onReturnFromEditor");

    auto result = m_impl->m_microbeScripts->ExecuteOnModule<void>(
        setup, false, m_impl->m_cellStage.get());

    if(result.Result != SCRIPT_RUN_RESULT::Success) {

        LOG_ERROR("Failed to run return from editor function: " +
                  setup.Entryfunction);
        return;
    }
}

void
    ThriveGame::exitToMenuClicked()
{
    // Unlink window
    Leviathan::Window* window1 = Engine::GetEngine()->GetWindowEntity();
    window1->LinkObjects(nullptr);

    // Clear the world
    m_impl->m_cellStage->ClearEntities();

    // Get proper keys setup
    m_impl->m_menuKeyPresses->setEnabled(true);
    m_impl->m_cellStageKeys->setEnabled(false);

    // And switch the GUI mode to allow key presses through
    auto layer = window1->GetGui()->GetLayerByIndex(0);

    // Allow running without GUI
    if(layer)
        layer->SetInputMode(Leviathan::GUI::INPUT_MODE::Menu);

    // Fire an event to switch over the GUI
    Engine::Get()->GetEventHandler()->CallEvent(
        new Leviathan::GenericEvent("ExitedToMenu"));
}

// ------------------------------------ //
void
    ThriveGame::onZoomChange(float amount)
{
    if(m_impl->m_cellStage)
        m_impl->m_cellStage->GetMicrobeCameraSystem().changeCameraOffset(
            amount);
}

// ------------------------------------ //
void
    ThriveGame::setBackgroundMaterial(const std::string& material)
{
    LOG_INFO("Setting microbe background to: " + material);
    m_impl->m_microbeBackgroundSubMesh->setMaterialName(material);

    // If the banana background re-appears after this change, we will make it
    // just use the default blue background. SO just comment this out then.
    m_impl->m_microbeEditorBackgroundSubMesh->setMaterialName(material);
    m_impl->createBackgroundItem();
}

// ------------------------------------ //
void
    ThriveGame::Tick(int mspassed)
{}

void
    ThriveGame::CustomizeEnginePostLoad()
{
    Engine* engine = Engine::Get();

    try {
        m_impl = std::make_unique<Implementation>(*this);
    } catch(const Leviathan::InvalidArgument& e) {

        LOG_ERROR("ThriveGame: loading configuration data failed: ");
        e.PrintToLog();
        MarkAsClosing();
        return;
    }

    // Load json data //
    SimulationParameters::init();

    // Load scripts
    LOG_INFO("ThriveGame: loading main scripts");

    // TODO: should these load failures be fatal errors (process would exit
    // immediately)

    try {
        m_impl->m_microbeScripts =
            engine->GetGameModuleLoader()->Load("microbe_stage", "ThriveGame");
    } catch(const Leviathan::Exception& e) {

        LOG_ERROR(
            "ThriveGame: microbe_stage module failed to load, exception:");
        e.PrintToLog();
        MarkAsClosing();
        return;
    }

    LOG_INFO("ThriveGame: script loading succeeded");

    if(!scriptSetup()) {

        LOG_ERROR("ThriveGame: failed to run setup script functions");
        MarkAsClosing();
        return;
    }

    // This is fine to set here to avoid putting this behind the next no gui
    // check //
    m_postLoadRan = true;

    // Load GUI documents (but only if graphics are enabled) //
    if(engine->GetNoGui()) {

        // Skip the graphical objects when not in graphical mode //
        return;
    }

    Leviathan::Window* window1 = Engine::GetEngine()->GetWindowEntity();

    // Register utility key presses
    window1->GetInputController()->LinkReceiver(m_impl->m_globalKeyPresses);

    // Register keypresses that will be used in the menu
    window1->GetInputController()->LinkReceiver(m_impl->m_menuKeyPresses);

    // Register the player input listener
    window1->GetInputController()->LinkReceiver(m_impl->m_cellStageKeys);

    Leviathan::GUI::GuiManager* GuiManagerAccess = window1->GetGui();

    if(!GuiManagerAccess->LoadGUIFile("Data/Scripts/gui/thrive_gui.html")) {

        LOG_ERROR("Thrive: failed to load the main menu gui, quitting");
        StartRelease();
        return;
    }
}
// ------------------------------------ //
//! \note This isn't actually called
void
    cellHitAgent(Leviathan::PhysicalWorld& physicalWorld,
        Leviathan::PhysicsBody& first,
        Leviathan::PhysicsBody& second)
{
    // GameWorld* gameWorld = physicalWorld.GetGameWorld();

    // ScriptRunningSetup setup("cellHitAgent");

    // auto result =
    // ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<void>(
    //     setup, false, gameWorld, first.GetOwningEntity(),
    //     second.GetOwningEntity());

    // if(result.Result != SCRIPT_RUN_RESULT::Success)
    //     LOG_ERROR("Failed to run script side cellHitAgent");
}

void
    cellHitFloatingOrganelle(Leviathan::PhysicalWorld& physicalWorld,
        Leviathan::PhysicsBody& first,
        Leviathan::PhysicsBody& second)
{
    GameWorld* gameWorld = physicalWorld.GetGameWorld();

    ScriptRunningSetup setup("cellHitFloatingOrganelle");

    auto result = ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<void>(
        setup, false, gameWorld, first.GetOwningEntity(),
        second.GetOwningEntity());

    if(result.Result != SCRIPT_RUN_RESULT::Success)
        LOG_ERROR("Failed to run script side cellHitFloatingOrganelle");
}

//! \todo This should return false when either cell is engulfing and apply the
//! damaging effect
bool
    cellOnCellAABBHitCallback(Leviathan::PhysicalWorld& physicalWorld,
        Leviathan::PhysicsBody& first,
        Leviathan::PhysicsBody& second)
{
    GameWorld* gameWorld = physicalWorld.GetGameWorld();

    ScriptRunningSetup setup("beingEngulfed");

    auto returned =
        ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<bool>(setup,
            false, gameWorld, first.GetOwningEntity(),
            second.GetOwningEntity());

    if(returned.Result != SCRIPT_RUN_RESULT::Success) {
        LOG_ERROR("Failed to run script side beingEngulfed");
        return true;
    }

    return returned.Value;
}


bool
    agentCallback(Leviathan::PhysicalWorld& physicalWorld,
        Leviathan::PhysicsBody& first,
        Leviathan::PhysicsBody& second)
{
    GameWorld* gameWorld = physicalWorld.GetGameWorld();

    // Now we can do more interetsing things with agents
    ScriptRunningSetup setup("hitAgent");

    auto returned =
        ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<bool>(setup,
            false, gameWorld, first.GetOwningEntity(),
            second.GetOwningEntity());

    if(returned.Result != SCRIPT_RUN_RESULT::Success) {
        LOG_ERROR("Failed to run script side hitAgent");
        return true;
    }

    return returned.Value;
}


void
    cellOnCellActualContact(Leviathan::PhysicalWorld& physicalWorld,
        Leviathan::PhysicsBody& first,
        Leviathan::PhysicsBody& second)
{
    // This will call a script that pulls cells in towards engulfers
    GameWorld* gameWorld = physicalWorld.GetGameWorld();

    ScriptRunningSetup setup("cellOnCellActualContact");

    auto returned =
        ThriveGame::Get()->getMicrobeScripts()->ExecuteOnModule<void>(setup,
            false, gameWorld, first.GetOwningEntity(),
            second.GetOwningEntity());

    if(returned.Result != SCRIPT_RUN_RESULT::Success) {
        LOG_ERROR("Failed to run script side beingEngulfed");
    }
}

std::unique_ptr<Leviathan::PhysicsMaterialManager>
    ThriveGame::createPhysicsMaterials() const
{
    // Setup materials
    auto cellMaterial =
        std::make_unique<Leviathan::PhysicalMaterial>("cell", 1);
    auto floatingOrganelleMaterial =
        std::make_unique<Leviathan::PhysicalMaterial>("floatingOrganelle", 2);
    auto agentMaterial =
        std::make_unique<Leviathan::PhysicalMaterial>("agentCollision", 3);

    // Set callbacks //

    // Floating organelles
    cellMaterial->FormPairWith(*floatingOrganelleMaterial)
        .SetCallbacks(nullptr, cellHitFloatingOrganelle);
    // Agents
    cellMaterial->FormPairWith(*agentMaterial)
        .SetCallbacks(agentCallback, nullptr);
    // Engulfing
    cellMaterial->FormPairWith(*cellMaterial)
        .SetCallbacks(cellOnCellAABBHitCallback, cellOnCellActualContact);

    auto manager = std::make_unique<Leviathan::PhysicsMaterialManager>();

    manager->LoadedMaterialAdd(std::move(cellMaterial));
    manager->LoadedMaterialAdd(std::move(floatingOrganelleMaterial));
    manager->LoadedMaterialAdd(std::move(agentMaterial));

    return manager;
}

void
    ThriveGame::EnginePreShutdown()
{
    // Shutdown scripting first to allow it to still do anything it wants //
    if(m_impl->m_microbeScripts) {
        m_impl->m_microbeScripts->ReleaseScript();
        m_impl->m_microbeScripts.reset();
    }

    // All resources that need Ogre or the engine to be available when
    // they are destroyed need to be released here

    m_impl->releaseOgreResources();

    if(m_impl->m_cellStage != nullptr)
        m_impl->m_cellStage->Release();

    // And garbage collect //
    // This is needed here as otherwise script destructors might use the deleted
    // world
    Leviathan::ScriptExecutor::Get()->CollectGarbage();

    m_impl->m_cellStage.reset();

    m_impl.reset();

    LOG_INFO("Thrive EnginePreShutdown ran");
}
// ------------------------------------ //
void
    ThriveGame::CheckGameConfigurationVariables(Lock& guard,
        GameConfiguration* configobj)
{}

void
    ThriveGame::CheckGameKeyConfigVariables(Lock& guard,
        KeyConfiguration* keyconfigobj)
{
    keyconfigobj->AddKeyIfMissing(guard, "MoveForward", {"W"});
    keyconfigobj->AddKeyIfMissing(guard, "MoveBackwards", {"S"});
    keyconfigobj->AddKeyIfMissing(guard, "MoveLeft", {"A"});
    keyconfigobj->AddKeyIfMissing(guard, "MoveRight", {"D"});
    keyconfigobj->AddKeyIfMissing(guard, "ReproduceCheat", {"P"});
    keyconfigobj->AddKeyIfMissing(guard, "SpawnGlucoseCheat", {"O"});
    keyconfigobj->AddKeyIfMissing(guard, "EngulfMode", {"G"});
    keyconfigobj->AddKeyIfMissing(guard, "ShootToxin", {"E"});
    keyconfigobj->AddKeyIfMissing(guard, "Screenshot", {"PrintScreen"});
    keyconfigobj->AddKeyIfMissing(guard, "ZoomIn", {"+", "Keypad +"});
    keyconfigobj->AddKeyIfMissing(guard, "ZoomOut", {"-", "Keypad -"});
}
// ------------------------------------ //
bool
    ThriveGame::InitLoadCustomScriptTypes(asIScriptEngine* engine)
{
    return registerThriveScriptTypes(engine);
}
