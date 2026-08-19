#include "library_controller.hpp"
#include "router.hpp"
#include "core_types_parsers.hpp"
#include "core_types_json.hpp"
#include "player_api.hpp"
#include "player_api_json.hpp"
#include "player_api_parsers.hpp"

#include <limits>

namespace msrv {

LibraryController::LibraryController(Request* request, Player* player)
    : ControllerBase(request), player_(player)
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

Range LibraryController::readRange()
{
    // Media library results are produced by applying search criteria, not naturally ordered,
    // so paging is optional and everything is returned by default
    return optionalParam<Range>("range", Range(0, std::numeric_limits<int32_t>::max()));
}

ResponsePtr LibraryController::getItems()
{
    if (!player_->supportsLibrary())
        return notSupportedResponse();

    auto columnsQuery = player_->createColumnsQuery(param<std::vector<std::string>>("columns"));

    LibraryQuery query;
    query.search = optionalParam<std::string>("query", std::string());
    query.sortBy = optionalParam<std::string>("sort", std::string());
    query.sortDescending = optionalParam<bool>("desc", false);

    return Response::json({{"libraryItems", player_->getLibraryItems(query, readRange(), columnsQuery.get())}});
}

ResponsePtr LibraryController::getItemsByPath()
{
    if (!player_->supportsLibrary())
        return notSupportedResponse();

    auto columnsQuery = player_->createColumnsQuery(param<std::vector<std::string>>("columns"));

    LibraryQuery query;
    query.path = optionalParam<std::string>("path", std::string());
    query.search = optionalParam<std::string>("query", std::string());

    return Response::json({{"libraryNodes", player_->getLibraryNodes(query, readRange(), columnsQuery.get())}});
}

void LibraryController::defineRoutes(Router* router, WorkQueue* workQueue, Player* player)
{
    auto routes = router->defineRoutes<LibraryController>();

    routes.createWith([=](Request* request) { return new LibraryController(request, player); });
    routes.useWorkQueue(workQueue);
    routes.setPrefix("api/library");

    routes.get("info", &LibraryController::getInfo);
    routes.get("items", &LibraryController::getItems);
    routes.get("items/by-path", &LibraryController::getItemsByPath);
}

}
