import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { downloadTotals, formatDownloads, writeBadges } from './download-badges.mjs';

const asset = (name, download_count) => ({ name, download_count });
const release = (...assets) => ({ assets });
const temp = (t) => {
  const directory = mkdtempSync(join(tmpdir(), 'pokopia-download-badges-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
};

test('sums every matching package across all releases', () => {
  const releases = [
    release(asset('PokopiaRecomp-0.7.9-win64.zip', 660), asset('PokopiaRecomp-0.7.9-linux-x64.tar.gz', 94), asset('SHA256SUMS.txt', 221)),
    release(asset('PokopiaRecomp-0.7.8-win64.zip', 1004), asset('PokopiaRecomp-0.7.8-linux-x64.tar.gz', 146), asset('PokopiaRecomp-0.7.8-win64.zip.sig', 900), asset('Other-win64.zip', 800)),
  ];
  assert.deepEqual(downloadTotals(releases), { win64: 1664, 'linux-x64': 240 });
});

test('counts asset downloads regardless of release-page or updater source', () => {
  const releases = [release(asset('PokopiaRecomp-0.8.0-win64.zip', 42), asset('PokopiaRecomp-0.8.0-linux-x64.tar.gz', 17))];
  assert.deepEqual(downloadTotals(releases), { win64: 42, 'linux-x64': 17 });
});

test('empty releases and releases without platform packages produce zero totals', () => {
  assert.deepEqual(downloadTotals([]), { win64: 0, 'linux-x64': 0 });
  assert.deepEqual(downloadTotals([release(asset('SHA256SUMS.txt', 50))]), { win64: 0, 'linux-x64': 0 });
});

test('formats small, thousand and million totals without inventing precision', () => {
  assert.equal(formatDownloads(0), '0');
  assert.equal(formatDownloads(999), '999');
  assert.equal(formatDownloads(1000), '1.0k');
  assert.equal(formatDownloads(1664), '1.7k');
  assert.equal(formatDownloads(1_000_000), '1.0M');
  assert.throws(() => formatDownloads(-1), /non-negative/);
});

test('writes both Shields endpoint files with exact totals', (t) => {
  const output = join(temp(t), 'nested');
  const totals = writeBadges([release(asset('PokopiaRecomp-0.8.0-win64.zip', 1200), asset('PokopiaRecomp-0.8.0-linux-x64.tar.gz', 75))], output);
  assert.deepEqual(totals, { win64: 1200, 'linux-x64': 75 });
  assert.deepEqual(JSON.parse(readFileSync(join(output, 'win64.json'), 'utf8')), { schemaVersion: 1, label: 'downloads', message: '1.2k', color: '3fb950' });
  assert.deepEqual(JSON.parse(readFileSync(join(output, 'linux-x64.json'), 'utf8')), { schemaVersion: 1, label: 'downloads', message: '75', color: '3fb950' });
});

test('CLI reads the paginated API response and writes both badges', (t) => {
  const directory = temp(t);
  const input = join(directory, 'releases.json');
  const output = join(directory, 'out');
  writeFileSync(input, JSON.stringify([release(asset('PokopiaRecomp-0.8.0-win64.zip', 20)), release(asset('PokopiaRecomp-0.7.9-win64.zip', 30))]));
  const result = spawnSync(process.execPath, ['.github/scripts/download-badges.mjs', input, output], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert(result.stdout.includes('win64: 50 (50)'));
  assert.equal(JSON.parse(readFileSync(join(output, 'linux-x64.json'), 'utf8')).message, '0');
});

test('rejects malformed JSON, invalid shapes and negative or unsafe counts', (t) => {
  const directory = temp(t);
  const input = join(directory, 'bad.json');
  writeFileSync(input, '{not json');
  const result = spawnSync(process.execPath, ['.github/scripts/download-badges.mjs', input, join(directory, 'out')], { encoding: 'utf8' });
  assert.notEqual(result.status, 0, 'malformed JSON was accepted');
  assert(result.stderr.includes('could not read releases JSON'));
  assert.throws(() => downloadTotals({}), /releases must be an array/);
  assert.throws(() => downloadTotals([{}]), /assets array/);
  assert.throws(() => downloadTotals([release(asset('PokopiaRecomp-0.8.0-win64.zip', -1))]), /invalid download_count/);
  assert.throws(() => downloadTotals([release(asset('PokopiaRecomp-0.8.0-win64.zip', 1.5))]), /invalid download_count/);
  assert.throws(() => downloadTotals([release(asset('PokopiaRecomp-0.8.0-win64.zip', Number.MAX_SAFE_INTEGER)), release(asset('PokopiaRecomp-0.7.9-win64.zip', 1))]), /safe integer range/);
});
