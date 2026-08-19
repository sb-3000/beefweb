import { describe, test, assert } from 'vitest';
import { client, config, setupPlayer } from './test_env.js';
import { PlayerId } from './test_context.js';

const isSupported = config.playerId === PlayerId.foobar2000;

describe('library api', () => {
    setupPlayer();

    test('get library info', async () => {
        const info = await client.getLibraryInfo();

        assert.equal(info.supported, isSupported);
        assert.equal(typeof info.enabled, 'boolean');
        assert.equal(typeof info.itemCount, 'number');
    });

    test('get library items', async () => {
        if (!isSupported)
        {
            const response = await client.handler.axios.get(
                '/api/library/items/0:100',
                { params: { columns: ['%path%'] }, validateStatus: () => true });

            assert.equal(response.status, 501);
            return;
        }

        const result = await client.getLibraryItems(['%path%', '%title%'], { offset: 0, count: 100 });

        assert.equal(result.offset, 0);
        assert.equal(typeof result.totalCount, 'number');
        assert.ok(Array.isArray(result.items));

        for (const item of result.items)
            assert.equal(item.columns.length, 2);
    });

    test('browse library folders', async () => {
        if (!isSupported)
            return;

        const result = await client.browseLibrary('', ['%title%'], { offset: 0, count: 100 });

        assert.equal(result.offset, 0);
        assert.equal(typeof result.totalCount, 'number');
        assert.ok(Array.isArray(result.items));

        // Top level has nowhere to navigate up to
        assert.equal(result.path, '');
        assert.equal(result.parentPath, undefined);

        for (const item of result.items)
        {
            assert.ok(item.type === 'D' || item.type === 'F');
            assert.ok(typeof item.name === 'string');
            assert.ok(typeof item.path === 'string');
        }
    });

    test('library artwork for missing item', async () => {
        const response = await client.handler.axios.get(
            '/api/artwork/library',
            { params: { path: 'no\\such\\track.flac' }, validateStatus: () => true });

        assert.equal(response.status, isSupported ? 404 : 501);
    });

    test('add library items to playlist', async () => {
        if (!isSupported)
            return;

        const playlist = await client.addPlaylist({ title: 'library add test' });

        // Library is empty in tests, adding everything must still succeed and change nothing
        await client.addLibraryItems(playlist.id, { path: '' });

        const items = await client.getPlaylistItems(playlist.id, ['%path%'], { offset: 0, count: 100 });
        assert.equal(items.totalCount, 0);
    });

    test('browse requires supported player', async () => {
        const response = await client.handler.axios.get(
            '/api/library/browse/0:10',
            { params: { columns: ['%title%'] }, validateStatus: () => true });

        assert.equal(response.status, isSupported ? 200 : 501);
    });

    test('get library items with query', async () => {
        if (!isSupported)
            return;

        const result = await client.getLibraryItems(
            ['%path%'],
            { offset: 0, count: 100 },
            { query: 'artist HAS beefweb_no_such_artist' });

        assert.equal(result.totalCount, 0);
        assert.deepEqual(result.items, []);
    });
});
