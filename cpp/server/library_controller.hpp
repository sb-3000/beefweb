#pragma once

#include "defines.hpp"
#include "controller.hpp"
#include "settings.hpp"

namespace msrv {

class Router;

class Player;

class WorkQueue;

class LibraryController : public ControllerBase
{
public:
    LibraryController(Request* request, Player* player, SettingsDataPtr settings);
    ~LibraryController();

    ResponsePtr getInfo();
    ResponsePtr getItems();
    ResponsePtr browse();
    ResponsePtr addItems();

    static void defineRoutes(Router* router, WorkQueue* workQueue, Player* player, SettingsDataPtr settings);

private:
    static ResponsePtr notSupportedResponse();

    Player* player_;
    SettingsDataPtr settings_;

    MSRV_NO_COPY_AND_ASSIGN(LibraryController);
};

}
