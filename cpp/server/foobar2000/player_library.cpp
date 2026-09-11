#include "player.hpp"
#include "file_system.hpp"

#include <algorithm>
#include <map>

namespace msrv {
namespace player_foobar2000 {

namespace {

// Media library paths are identifiers used within the API only,
// they are kept platform independent and are never passed to the file system
constexpr char PATH_SEPARATOR = '/';

#ifdef MSRV_OS_WINDOWS
constexpr char NATIVE_PATH_SEPARATOR = '\\';
#else
constexpr char NATIVE_PATH_SEPARATOR = '/';
#endif

bool isSeparator(char ch)
{
    return ch == PATH_SEPARATOR;
}

size_t findSeparator(const std::string& path, size_t start)
{
    for (size_t i = start; i < path.length(); i++)
    {
        if (isSeparator(path[i]))
            return i;
    }

    return std::string::npos;
}

std::string normalizeNodePath(const std::string& path)
{
    size_t start = 0;
    size_t end = path.length();

    while (end > start && isSeparator(path[end - 1]))
        end--;

    while (start < end && isSeparator(path[start]))
        start++;

    return path.substr(start, end - start);
}

std::string joinNodePath(const std::string& prefix, const std::string& name)
{
    return prefix.empty() ? name : prefix + PATH_SEPARATOR + name;
}

using NodeItem = std::pair<std::string, metadb_handle_ptr>;

// Single file may hold several tracks (cue sheets), keep such tracks in subsong order
void sortItems(std::vector<NodeItem>* items)
{
    std::sort(items->begin(), items->end(), [](const NodeItem& left, const NodeItem& right) {
        if (left.first != right.first)
            return left.first < right.first;

        return left.second->get_location().get_subsong() < right.second->get_location().get_subsong();
    });
}

// Folder is expected to be normalized, empty folder is the top level and contains everything
bool isSubpath(const std::string& path, const std::string& folder)
{
    if (folder.empty())
        return true;

    return path.length() > folder.length()
        && isSeparator(path[folder.length()])
        && path.compare(0, folder.length(), folder) == 0;
}

// Item reference matches a single track when it has a subsong, all tracks of a file
// when it does not, everything below it when it points to a folder,
// and the whole library when its path is empty
bool matchesRef(const LibraryItemRef& ref, const std::string& itemPath, const metadb_handle_ptr& item)
{
    auto path = normalizeNodePath(ref.path);

    if (!path.empty() && itemPath == path)
    {
        return ref.subsong < 0
            || static_cast<t_uint32>(ref.subsong) == item->get_location().get_subsong();
    }

    return isSubpath(itemPath, path);
}

// Item locations are prefixed with a scheme, plain file system path is what artwork lookup needs
std::string getAbsolutePath(const metadb_handle_ptr& item)
{
    constexpr char fileScheme[] = "file://";
    constexpr size_t fileSchemeLength = sizeof(fileScheme) - 1;

    auto path = item->get_path();

    if (::strncmp(path, fileScheme, fileSchemeLength) == 0)
        path += fileSchemeLength;

    return std::string(path);
}

// Folders may hold an image file that represents the folder itself.
// This is not how the player resolves artwork (it uses configurable per track patterns),
// so it is only used when explicitly requested
std::string findFolderArtwork(const std::string& folderPath)
{
    static const char* const names[] = {"folder", "cover", "front", "album", "artwork"};
    static const char* const extensions[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"};

    auto folder = pathFromUtf8(folderPath);

    for (auto name : names)
    {
        for (auto extension : extensions)
        {
            auto file = folder / pathFromUtf8(std::string(name) + extension);
            auto info = file_io::tryQueryInfo(file);

            if (info && info->type == FileType::REGULAR)
                return pathToUtf8(file);
        }
    }

    return std::string();
}

// Media library items are addressed by path relative to the media library folder they belong to,
// so that browsing starts at library folders instead of file system roots
std::string getNodePath(
    const library_manager::ptr& libraryManager,
    const metadb_handle_ptr& item,
    pfc::string8* buffer)
{
    std::string path;

    if (libraryManager->get_relative_path(item, *buffer))
    {
        path.assign(buffer->get_ptr(), buffer->get_length());
    }
    else
    {
        // Should not happen for items coming from the media library,
        // fall back to full path so that an item is never silently dropped
        path = getAbsolutePath(item);
    }

    std::replace(path.begin(), path.end(), NATIVE_PATH_SEPARATOR, PATH_SEPARATOR);

    return path;
}

// Absolute paths keep the native separator, node paths do not
bool endsWithNodePath(const std::string& absolutePath, const std::string& nodePath)
{
    if (absolutePath.length() < nodePath.length())
        return false;

    auto offset = absolutePath.length() - nodePath.length();

    for (size_t i = 0; i < nodePath.length(); i++)
    {
        auto left = absolutePath[offset + i];
        auto right = nodePath[i];

        if (left == right)
            continue;

        if (left == NATIVE_PATH_SEPARATOR && right == PATH_SEPARATOR)
            continue;

        return false;
    }

    return true;
}

// Absolute path of the folder an item reference points to, derived from a track below it.
// Empty when the reference points to a file or to the whole library
std::string getFolderPath(const LibraryItemRef& ref, const metadb_handle_ptr& firstItem)
{
    auto path = normalizeNodePath(ref.path);
    if (path.empty())
        return std::string();

    pfc::string8 buffer;
    auto nodePath = getNodePath(library_manager::get(), firstItem, &buffer);

    if (nodePath.length() <= path.length())
        return std::string();

    auto absolutePath = getAbsolutePath(firstItem);
    auto suffixLength = nodePath.length() - path.length();

    if (absolutePath.length() <= suffixLength || !endsWithNodePath(absolutePath, nodePath))
        return std::string();

    return absolutePath.substr(0, absolutePath.length() - suffixLength);
}

class ItemCounter : public library_manager::enum_callback
{
public:
    bool on_item(const metadb_handle_ptr& item) override
    {
        (void) item;
        count_++;
        return true;
    }

    t_size count() const
    {
        return count_;
    }

private:
    t_size count_ = 0;
};

void filterItems(metadb_handle_list* items, const std::string& expression)
{
    auto count = items->get_count();
    if (count == 0)
        return;

    search_filter_v2::ptr filter;

    try
    {
        filter = search_filter_manager_v2::get()->create_ex(
            expression.c_str(),
            completion_notify::ptr(),
            search_filter_manager_v2::KFlagSuppressNotify);
    }
    catch (std::exception& ex)
    {
        throw InvalidRequestException("invalid search query: " + std::string(ex.what()));
    }

    if (filter.is_empty())
        throw InvalidRequestException("invalid search query: " + expression);

    pfc::array_t<bool> matches;
    matches.set_size(count);

    filter->test_multi(*items, matches.get_ptr());

    metadb_handle_list result;
    result.prealloc(count);

    for (t_size i = 0; i < count; i++)
    {
        if (matches[i])
            result.add_item((*items)[i]);
    }

    *items = result;
}

}

bool PlayerImpl::supportsLibrary()
{
    return true;
}

LibraryInfo PlayerImpl::getLibraryInfo()
{
    auto libraryManager = library_manager::get();

    ItemCounter counter;
    libraryManager->enum_items(counter);

    LibraryInfo info;
    info.supported = true;
    info.enabled = libraryManager->is_library_enabled();
    info.itemCount = static_cast<int32_t>(counter.count());
    return info;
}

LibraryItemsResult PlayerImpl::getLibraryItems(
    const LibraryQuery& query, const Range& range, ColumnsQuery* columns)
{
    auto queryImpl = dynamic_cast<ColumnsQueryImpl*>(columns);
    if (!queryImpl)
        throw std::logic_error("ColumnsQueryImpl is required");

    metadb_handle_list items;
    library_manager::get()->get_all_items(items);

    if (!query.search.empty())
        filterItems(&items, query.search);

    if (query.sortBy.empty())
    {
        // Media library content has no inherent order, sort by path to make paging stable
        metadb_handle_list_helper::sort_by_path(items);
    }
    else
    {
        titleformat_object::ptr sortBy;

        if (!titleFormatCompiler_->compile(sortBy, query.sortBy.c_str()))
            throw InvalidRequestException("invalid format expression: " + query.sortBy);

        metadb_handle_list_helper::sort_by_format(
            items, sortBy, nullptr, query.sortDescending ? -1 : 1);
    }

    auto totalCount = items.get_count();
    auto offset = std::min(static_cast<t_size>(range.offset), totalCount);
    auto endOffset = std::min(static_cast<t_size>(range.endOffset()), totalCount);

    std::vector<LibraryItemInfo> result;

    if (offset < endOffset)
    {
        result.reserve(endOffset - offset);

        auto libraryManager = library_manager::get();
        pfc::string8 buffer;

        for (t_size i = offset; i < endOffset; i++)
        {
            const auto& item = items[i];

            LibraryItemInfo info;
            info.path = getNodePath(libraryManager, item, &buffer);
            info.subsong = static_cast<int32_t>(item->get_location().get_subsong());
            info.columns = evaluateItemColumns(item, queryImpl->columns, &buffer);
            result.emplace_back(std::move(info));
        }
    }

    return LibraryItemsResult(
        static_cast<int32_t>(offset),
        static_cast<int32_t>(totalCount),
        std::move(result));
}

void PlayerImpl::collectLibraryItems(const LibraryItemQuery& query, metadb_handle_list* outItems)
{
    auto libraryManager = library_manager::get();

    metadb_handle_list items;
    libraryManager->get_all_items(items);

    if (!query.search.empty())
        filterItems(&items, query.search);

    std::vector<NodeItem> matches;
    pfc::string8 buffer;

    for (t_size i = 0; i < items.get_count(); i++)
    {
        const auto& item = items[i];
        auto itemPath = getNodePath(libraryManager, item, &buffer);

        bool matched = query.items.empty();

        for (auto it = query.items.begin(); !matched && it != query.items.end(); ++it)
            matched = matchesRef(*it, itemPath, item);

        if (matched)
            matches.emplace_back(std::move(itemPath), item);
    }

    sortItems(&matches);

    outItems->remove_all();
    outItems->prealloc(matches.size());

    for (auto& match : matches)
        outItems->add_item(match.second);
}

void PlayerImpl::addLibraryItems(
    const PlaylistRef& plref,
    const LibraryItemQuery& query,
    int32_t targetIndex,
    AddItemsOptions options)
{
    metadb_handle_list items;
    collectLibraryItems(query, &items);

    auto playlist = playlists_->getIndex(plref);
    auto hasAddedItems = items.get_count() > 0;
    t_size itemIndex;

    if (hasFlags(options, AddItemsOptions::REPLACE))
    {
        playlistManager_->playlist_clear(playlist);
        itemIndex = 0;
    }
    else
    {
        auto itemCount = playlistManager_->playlist_get_item_count(playlist);

        itemIndex = targetIndex >= 0 && static_cast<t_size>(targetIndex) < itemCount
            ? static_cast<t_size>(targetIndex)
            : itemCount;
    }

    if (hasAddedItems)
        playlistManager_->playlist_insert_items(playlist, itemIndex, items, bit_array_false());

    if (!hasFlags(options, AddItemsOptions::PLAY))
        return;

    if (hasAddedItems)
    {
        playlistManager_->set_active_playlist(playlist);
        playlistManager_->playlist_execute_default_action(playlist, itemIndex);
    }
    else
    {
        playbackControl_->stop();
    }
}

boost::unique_future<ArtworkResult> PlayerImpl::fetchLibraryArtwork(
    const LibraryItemRef& item, bool preferFolderImage)
{
    LibraryItemQuery query;
    query.items.emplace_back(item);

    metadb_handle_list items;
    collectLibraryItems(query, &items);

    if (items.get_count() == 0)
        return boost::make_future(ArtworkResult());

    const auto& firstItem = items[0];

    if (preferFolderImage)
    {
        auto folderPath = getFolderPath(item, firstItem);

        if (!folderPath.empty())
        {
            auto artwork = findFolderArtwork(folderPath);

            if (!artwork.empty())
                return boost::make_future(ArtworkResult(std::move(artwork)));
        }
    }

    return fetchArtwork(firstItem);
}

LibraryNodesResult PlayerImpl::getLibraryNodes(
    const LibraryQuery& query, const Range& range, ColumnsQuery* columns)
{
    auto queryImpl = dynamic_cast<ColumnsQueryImpl*>(columns);
    if (!queryImpl)
        throw std::logic_error("ColumnsQueryImpl is required");

    auto libraryManager = library_manager::get();

    metadb_handle_list items;
    libraryManager->get_all_items(items);

    if (!query.search.empty())
        filterItems(&items, query.search);

    auto prefix = normalizeNodePath(query.path);
    auto childOffset = prefix.empty() ? 0 : prefix.length() + 1;

    std::map<std::string, int32_t> folders;
    std::vector<NodeItem> files;

    pfc::string8 buffer;

    for (t_size i = 0; i < items.get_count(); i++)
    {
        const auto& item = items[i];
        auto path = getNodePath(libraryManager, item, &buffer);

        if (!isSubpath(path, prefix))
            continue;

        auto separator = findSeparator(path, childOffset);

        if (separator == std::string::npos)
            files.emplace_back(path.substr(childOffset), item);
        else
            folders[path.substr(childOffset, separator - childOffset)]++;
    }

    sortItems(&files);

    std::vector<LibraryNodeInfo> nodes;
    std::vector<metadb_handle_ptr> handles;

    nodes.reserve(folders.size() + files.size());
    handles.reserve(folders.size() + files.size());

    for (auto& folder : folders)
    {
        LibraryNodeInfo node;
        node.isFolder = true;
        node.name = folder.first;
        node.path = joinNodePath(prefix, folder.first);
        node.itemCount = folder.second;
        nodes.emplace_back(std::move(node));
        handles.emplace_back();
    }

    for (auto& file : files)
    {
        LibraryNodeInfo node;
        node.name = file.first;
        node.path = joinNodePath(prefix, file.first);
        node.subsong = static_cast<int32_t>(file.second->get_location().get_subsong());
        nodes.emplace_back(std::move(node));
        handles.emplace_back(file.second);
    }

    auto totalCount = nodes.size();
    auto offset = std::min(static_cast<size_t>(range.offset), totalCount);
    auto endOffset = std::min(static_cast<size_t>(range.endOffset()), totalCount);

    std::vector<LibraryNodeInfo> result;

    if (offset < endOffset)
    {
        result.reserve(endOffset - offset);

        for (size_t i = offset; i < endOffset; i++)
        {
            if (handles[i].is_valid())
                nodes[i].columns = evaluateItemColumns(handles[i], queryImpl->columns, &buffer);

            result.emplace_back(std::move(nodes[i]));
        }
    }

    LibraryNodesResult nodesResult(
        static_cast<int32_t>(offset),
        static_cast<int32_t>(totalCount),
        std::move(result));

    nodesResult.path = prefix;

    if (!prefix.empty())
    {
        auto separator = prefix.find_last_of("\\/");

        nodesResult.hasParent = true;
        nodesResult.parentPath = separator == std::string::npos
            ? std::string()
            : prefix.substr(0, separator);
    }

    return nodesResult;
}

}
}
