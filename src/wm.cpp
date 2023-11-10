#include "wm.h"
#include "app.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <glm/glm.hpp>
#include <iostream>

#include <X11/extensions/shape.h>
#include <memory>
#include <spdlog/common.h>
#include <sstream>
#include <thread>

int APP_WIDTH = 1920 * .85;
int APP_HEIGHT = 1920 * .85 * .54;
void WM::forkOrFindApp(string cmd, string pidOf, string className, X11App *&app, char **envp) {
  char *line;
  std::size_t len = 0;
  FILE *pidPipe = popen(string("pidof -x " + pidOf).c_str(), "r");
  if (getline(&line, &len, pidPipe) == -1) {
    int pid = fork();
    if (pid == 0) {
      setsid();
      execle(cmd.c_str(), cmd.c_str(), NULL, envp);
      exit(0);
    }
    if (className == "obs") {
      sleep(30);
    } else {
      sleep(4);
    }
  }
  app = X11App::byClass(className, display, screen, APP_WIDTH, APP_HEIGHT);
  dynamicApps[app->getWindow()] = app;
  logger->info("created " + className + " app");
}

void WM::createAndRegisterApps(char **envp) {
  logger->info("enter createAndRegisterApps()");

  forkOrFindApp("/usr/bin/emacs", "emacs", "Emacs", emacs, envp);
  forkOrFindApp("/usr/bin/microsoft-edge", "msedge", "Microsoft-edge",
                microsoftEdge, envp);
  forkOrFindApp("/usr/bin/terminator", "terminator", "Terminator", terminator,
                envp);
  forkOrFindApp("/usr/bin/obs", "obs", "obs", obs, envp);

  logger->info("exit createAndRegisterApps()");
}


void WM::allow_input_passthrough(Window window) {
  XserverRegion region = XFixesCreateRegion(display, NULL, 0);

  XFixesSetWindowShapeRegion(display, window, ShapeBounding, 0, 0, 0);
  XFixesSetWindowShapeRegion(display, window, ShapeInput, 0, 0, region);

  XFixesDestroyRegion(display, region);
}

void WM::addAppsToWorld() {
  world->addApp(terminator);
  world->addApp(emacs);
  world->addApp(microsoftEdge);
  world->addApp(obs);
}

void WM::createApp(Window window) {
    X11App *app =
        X11App::byWindow(window, display, screen, APP_WIDTH, APP_HEIGHT);
    renderLoopMutex.lock();
    appsToAdd.push_back(app);
    renderLoopMutex.unlock();
    dynamicApps[window] = app;
}

void WM::onMapRequest(XMapRequestEvent event) {
  char *name;
  XFetchName(display, event.window, &name);
  string sName(name);
  bool alreadyRegistered = dynamicApps.count(event.window);
  if (!alreadyRegistered && !sName.ends_with("one")) {
    createApp(event.window);
  }
  stringstream ss;
  ss << "window created: " << event.window << " "<< name;
  logger->info(ss.str());
  logger->flush();
}

void WM::onDestroyNotify(XDestroyWindowEvent event) {
  if(dynamicApps.contains(event.window)) {
    X11App *app = dynamicApps.at(event.window);
    dynamicApps.erase(event.window);
    renderLoopMutex.lock();
    appsToRemove.push_back(app);
    renderLoopMutex.unlock();
  }
}

void WM::onHotkeyPress(XKeyEvent event) {
  KeyCode eKeyCode = XKeysymToKeycode(display, XK_e);
  KeyCode oneKeyCode = XKeysymToKeycode(display, XK_1);
  vector<X11App*> appsWithHotKeys = {emacs, microsoftEdge, terminator, obs};
  if (event.keycode == eKeyCode && event.state & Mod4Mask) {
    // Windows Key (Super_L) + Ctrl + E is pressed
    unfocusApp();
  }
  for (int i = 0; i < appsWithHotKeys.size(); i++) {
    KeyCode code = XKeysymToKeycode(display, XK_1 + i);
    if (event.keycode == code && event.state & Mod4Mask) {
      unfocusApp();
      controls->goToApp(appsWithHotKeys[i]);
    }
  }
  KeyCode code = XKeysymToKeycode(display, XK_0);
  if (event.keycode == code && event.state & Mod4Mask) {
    unfocusApp();
    controls->moveTo(glm::vec3(3.0, 5.0, 16), 4);
  }
}

void WM::handleSubstructure() {
  for (;;) {
    XEvent e;
    XNextEvent(display, &e);
    stringstream eventInfo;
    Window root = XRootWindow(display, 0);

    XWindowAttributes attrs;

    switch (e.type) {
    case CreateNotify:
      logger->info("CreateNotify event");
      XGetWindowAttributes(display, e.xcreatewindow.window, &attrs);
      if(e.xcreatewindow.override_redirect == True) {
        createApp(e.xcreatewindow.window);
      }
      logger->flush();
      break;
    case DestroyNotify:
      logger->info("DestroyNotify event");
      logger->flush();
      onDestroyNotify(e.xdestroywindow);
      break;
    case ReparentNotify:
      logger->info("ReparentNotify event");
      logger->flush();
      break;
    case ConfigureNotify:
      logger->info("ConfigureNotify event");
      eventInfo <<
        "width: " <<
        e.xconfigure.width <<
        ", height: " <<
        e.xconfigure.height;

      logger->info(eventInfo.str());
      logger->flush();
      break;
    case ConfigureRequest: {
      logger->info("ConfigureRequest event");
      break;
    }
    case MapRequest:
      logger->info("MapRequest event");
      onMapRequest(e.xmaprequest);
      break;
    case KeyPress:
      onHotkeyPress(e.xkey);
      break;
    }
  }
}

void WM::mutateWorld() {
  renderLoopMutex.lock();
  for(auto it = appsToAdd.begin(); it != appsToAdd.end(); it++) {
    try {
      world->addApp(*it);
    } catch(exception &e) {
      logger->error(e.what());
      logger->flush();
    }
  }
  appsToAdd.clear();

  for (auto it = appsToRemove.begin(); it != appsToRemove.end(); it++) {
    try {
      if (currentlyFocusedApp == *it) {
        unfocusApp();
      }
      world->removeApp(*it);
      delete *it;
    } catch (exception &e) {
      logger->error(e.what());
      logger->flush();
    }
  }
  appsToRemove.clear();

  renderLoopMutex.unlock();
}

void WM::focusApp(X11App* app) {
  currentlyFocusedApp = app;
  app->focus(matrix);
}

void WM::unfocusApp() {
  if(currentlyFocusedApp != NULL) {
    currentlyFocusedApp->unfocus(matrix);
    currentlyFocusedApp = NULL;
  }
}

void WM::registerControls(Controls *controls) {
  this->controls = controls;
}

WM::WM(Window matrix): matrix(matrix) {
  logger = make_shared<spdlog::logger>("wm", fileSink);
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  logger->info("WM()");
  display = XOpenDisplay(NULL);
  screen = XDefaultScreen(display);
  X11App::initAppClass(display, screen);
  Window root = RootWindow(display, screen);
  XCompositeRedirectSubwindows(display, RootWindow(display, screen),
                               CompositeRedirectAutomatic);

  Window overlay = XCompositeGetOverlayWindow(display, root);
  XReparentWindow(display, matrix, overlay, 0, 0);

  XFixesSelectCursorInput(display, overlay, XFixesDisplayCursorNotifyMask);

  XSelectInput(display, root,
               SubstructureRedirectMask | SubstructureNotifyMask);


  for(int i = 0; i<10; i++) {
    KeyCode code = XKeysymToKeycode(display, XK_0+i);
    XGrabKey(display, code, Mod4Mask, root, true, GrabModeAsync, GrabModeAsync);
  }
  XSync(display, false);
  XFlush(display);

  allow_input_passthrough(overlay);
  allow_input_passthrough(matrix);
  substructureThread = thread(&WM::handleSubstructure, this);
  substructureThread.detach();
}

WM::~WM() {
  XCompositeReleaseOverlayWindow(display, RootWindow(display, screen));
}
