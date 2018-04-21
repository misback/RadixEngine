#include <radix/BaseGame.hpp>

#include <SDL2/SDL_timer.h>

#include <radix/data/map/XmlMapLoader.hpp>
#include <radix/env/Environment.hpp>
#include <radix/SoundManager.hpp>
#include <radix/renderer/Renderer.hpp>
#include <radix/renderer/ScreenRenderer.hpp>
#include <radix/simulation/Player.hpp>
#include <radix/simulation/Physics.hpp>
#include <radix/entities/Player.hpp>
#include <radix/env/ArgumentsParser.hpp>
#include <radix/env/GameConsole.hpp>
#include <radix/util/Profiling.hpp>
#include <radix/World.hpp>
#include <chaiscript/chaiscript.hpp>
#include <chaiscript/chaiscript_stdlib.hpp>

namespace radix {

Fps BaseGame::fps;

std::string helloWorld(const std::string &t_name){
  return "Hello " + t_name + "!";
}

BaseGame::BaseGame() :
    gameWorld(window),
    closed(false) {
  radix::Environment::init();
  config = Environment::getConfig();
  radix::ArgumentsParser::populateConfig(config);

  if (config.isProfilerEnabled()) {
    Util::Log(Info, "BaseGame") << "Enabling profiler";
    PROFILER_PROFILER_ENABLE;
    profiler::startListen();
  }

  window.setConfig(config);
}

BaseGame::~BaseGame() {
  if (config.isProfilerEnabled()) {
    profiler::stopListen();
    PROFILER_PROFILER_DISABLE;
  }
  screenshotCallbackHolder.removeThis();
  imguiRenderer->shutdown();
}

void BaseGame::setup() {
  radix::GameConsole console;
  if (config.isConsoleEnabled()) {
    console.run(*this);
  }
  SoundManager::init();
  createWindow();
  initHook();
  customTriggerHook();

  if (!inputManager) {
    inputManager = std::make_unique<InputManager>(this);
  }

  std::unique_ptr<World> newWorld = std::make_unique<World>(*this);
  createWorld(*newWorld);

  lastUpdate = 0;
  lastRender = 0;

  loadMap(*newWorld);
  setWorld(std::move(newWorld));
  // From this point on, this->world is the moved newWorld

  createScreenshotCallbackHolder();

  imguiRenderer = std::make_unique<ImguiRenderer>(*world, *renderer.get());

  window.registerImgui([this](SDL_Event* event) -> bool {
      return imguiRenderer->processEvent(event);
      },
      [this](){
      imguiRenderer->newFrame();
      });

  screenRenderer = std::make_unique<ScreenRenderer>(*world, *renderer.get(), gameWorld);
  renderer->addRenderer(*screenRenderer);

  postSetup();
}

bool BaseGame::isRunning() {
  return !closed;
}

World* BaseGame::getWorld() {
  return world.get();
}

Config& BaseGame::getConfig() {
  return config;
}

void BaseGame::switchToOtherWorld(const std::string &name) {
  auto it = otherWorlds.find(name);
  if (it == otherWorlds.end()) {
    throw std::out_of_range("No other world by this name");
  }
  setWorld(std::move(it->second));
  otherWorlds.erase(it);
}

void BaseGame::clearOtherWorldList() {
  otherWorlds.clear();
}

ScreenRenderer* BaseGame::getScreenRenderer() {
  return screenRenderer.get();
}

GameWorld* BaseGame::getGameWorld() {
  return &gameWorld;
}

chaiscript::ChaiScript& BaseGame::getScriptEngine() {
  return scriptEngine;
}

void BaseGame::preCycle() {
  PROFILER_NONSCOPED_BLOCK("Game cycle");
  window.processEvents();
}

void BaseGame::update() {
  currentTime = SDL_GetTicks();
  int elapsedTime = currentTime - lastUpdate;
  SoundManager::update(world->getPlayer());
  world->update(TimeDelta::msec(elapsedTime));
  lastUpdate = currentTime;
}

void BaseGame::postCycle() {
  if (postCycleDeferred.size() > 0) {
    PROFILER_BLOCK("Post-gamecycle deferred");
    for (auto deferred : postCycleDeferred) {
      deferred();
    }
    postCycleDeferred.clear();
  }
  PROFILER_END_BLOCK;
}

void BaseGame::deferPostCycle(const std::function<void()> &deferred) {
  postCycleDeferred.push_back(deferred);
}

void BaseGame::processInput() {
}
void BaseGame::initHook() { }
void BaseGame::removeHook() { }
void BaseGame::customTriggerHook() { }

void BaseGame::cleanUp() {
  removeHook();
  setWorld({});
  window.close();
  chaiscript::ChaiScript chai;
  chai.add(chaiscript::fun(&helloWorld), "helloWorld");

  chai.eval("puts(helloWorld(\"Bob\"));");
}

void BaseGame::render() {
  PROFILER_BLOCK("BaseGame::render");
  prepareCamera();
  renderer->render();
  imguiRenderer->render();
  gameWorld.getScreens()->clear();
  fps.countCycle();
  PROFILER_BLOCK("SwapBuffers", profiler::colors::White);
  window.swapBuffers();
  PROFILER_END_BLOCK;
  lastRender = currentTime;
}

void BaseGame::prepareCamera() {
  world->camera->setPerspective();
  int viewportWidth, viewportHeight;
  window.getSize(&viewportWidth, &viewportHeight);
  world->camera->setAspect((float)viewportWidth / viewportHeight);
  const entities::Player &player = world->getPlayer();
  Vector3f headOffset(0, player.getScale().y / 2, 0);
  world->camera->setPosition(player.getPosition() + headOffset);
  world->camera->setOrientation(player.getHeadOrientation());
}

void BaseGame::close() {
  closed = true;
}

void BaseGame::createWorld(World &world) {
  onPreCreateWorld(world);
  world.setConfig(config);
  world.onCreate();
  { SimulationManager::Transaction simTransact = world.simulations.transact();
    simTransact.addSimulation<simulation::Player>(this);
    simTransact.addSimulation<simulation::Physics>(this);
  }
  world.initPlayer();
  onPostCreateWorld(world);
}

void BaseGame::setWorld(std::unique_ptr<World> &&newWorld) {
  if (world) {
    onPreStopWorld();
    world->onStop();
    onPostStopWorld();
    onPreDestroyWorld(*world);
    world->onDestroy();
    onPostDestroyWorld(*world);
  }
  world = std::move(newWorld);
  if (world) {
    // TODO move
    renderer = std::make_unique<Renderer>(*world);
    renderer->setViewport(&window);
    renderer->init();
    inputManager->init();

    onPreStartWorld();
    world->onStart();
    onPostStartWorld();
  }
}

void BaseGame::loadMap(World &targetWorld) {
  XmlMapLoader mapLoader(targetWorld, customTriggers);
  std::string map = config.getMap(), mapPath = config.getMapPath();
  if (map.length() > 0) {
    mapLoader.load(Environment::getDataDir() + map);
  } else if (mapPath.length() > 0) {
    mapLoader.load(mapPath);
  } else {
    mapLoader.load(Environment::getDataDir() + defaultMap);
  }
}

void BaseGame::createWindow() {
  window.create(windowTitle.c_str());
  if(config.getCursorVisibility()) {
    window.unlockMouse();
  } else {
    window.lockMouse();
  }
}

void BaseGame::createScreenshotCallbackHolder() {
  world->event.addObserverRaw(InputSource::KeyReleasedEvent::Type, [this](const radix::Event &event) {
      const int key =  ((InputSource::KeyReleasedEvent &) event).key;
      if (key == SDL_SCANCODE_G) {
        this->window.printScreenToFile(radix::Environment::getDataDir() + "/screenshot.bmp");
      }
    });
}
} /* namespace radix */
