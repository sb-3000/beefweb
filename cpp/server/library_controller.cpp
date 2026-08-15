#include "library_controller.hpp"
#include "router.hpp"
#include "core_types_parsers.hpp"
#include "core_types_json.hpp"
#include "player_api.hpp"
#include "player_api_json.hpp"
#include "player_api_parsers.hpp"

namespace msrv {

namespace {

constexpr char VIEW_FLAT[] = "flat";
constexpr char VIEW_FOLDERS[] = "folders";

}

LibraryController::LibraryController(Request* request, Player* player, SettingsDataPtr settings)
    : ControllerBase(request), player_(player), settings_(std::move(settings))
{
}

LibraryController::~LibraryController() = default;

ResponsePtr LibraryController::getInfo()
{
    return Response::json({{"library", player_->getLibraryInfo()}});
}

ResponsePtr LibraryController::notSupportedResponse()
{
    return Response::error(
        HttpStatus::S_501_NOT_IMPLEMENTED, "media library is not supported by this player");
}

ResponsePtr LibraryController::getItems()
{
    if (!player_->getLibraryInfo().supported)
        return notSupportedResponse();

    auto range = param<Range>("range");
    auto columns = param<std::vector<std::string>>("columns");
    auto view = optionalParam<std::string>("view", VIEW_FLAT);

    LibraryQuery query;
    query.search = optionalParam<std::string>("query", std::string());
    query.sortBy = optionalParam<std::string>("sort", std::string());
    query.sortDescending = optionalParam<bool>("desc", false);

    auto columnsQuery = player_->createColumnsQuery(columns);

    if (view == VIEW_FOLDERS)
    {
        query.path = optionalParam<std::string>("path", std::string());
        return Response::json({{"libraryItems", player_->getLibraryNodes(query, range, columnsQuery.get())}});
    }

    if (view != VIEW_FLAT)
        throw InvalidRequestException("invalid view: " + view);

    return Response::json({{"libraryItems", player_->getLibraryItems(query, range, columnsQuery.get())}});
}

ResponsePtr LibraryController::addItems()
{
    settings_->ensurePermissions(ApiPermissions::CHANGE_PLAYLISTS);

    if (!player_->getLibraryInfo().supported)
        return notSupportedResponse();

    LibraryItemQuery query;
    query.path = optionalParam<std::string>("path", std::string());
    query.subsong = optionalParam<int32_t>("subsong", -1);
    query.search = optionalParam<std::string>("query", std::string());

    auto options = AddItemsOptions::NONE;

    if (optionalParam("replace", false))
        options |= AddItemsOptions::REPLACE;

    if (optionalParam("play", false))
        options |= AddItemsOptions::PLAY;

    player_->addLibraryItems(
        param<PlaylistRef>("plref"),
        query,
        optionalParam<int32_t>("index", -1),
        options);

    return Response::ok();
}

void LibraryController::defineRoutes(
    Router* router, WorkQueue* workQueue, Player* player, SettingsDataPtr settings)
{
    auto routes = router->defineRoutes<LibraryController>();

    routes.createWith([=](Request* request) { return new LibraryController(request, player, settings); });
    routes.useWorkQueue(workQueue);
    routes.setPrefix("api/library");

    routes.get("info", &LibraryController::getInfo);
    routes.get("items/:range", &LibraryController::getItems);
    routes.post("items/add", ControllerAction<LibraryController>(&LibraryController::addItems));
}

}
