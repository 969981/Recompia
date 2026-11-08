#!/usr/bin/env node
// Generate Shields endpoint files from the GitHub releases API response.
// Both release-page downloads and launcher updates increment the same asset download_count values.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export const PLATFORMS = Object.freeze({
  win64: '-win64.zip',
  'linux-x64': '-linux-x64.tar.gz',
});
const PACKAGE_PREFIX = 'PokopiaRecomp-';

export function downloadTotals(releases) {
  if (!Array.isArray(releases)) throw new TypeError('releases must be an array');
  const totals = Object.fromEntries(Object.keys(PLATFORMS).map((platform) => [platform, 0]));
  for (const [releaseIndex, release] of releases.entries()) {
    if (!release || !Array.isArray(release.assets)) throw new TypeError(`release ${releaseIndex} has no assets array`);
    for (const asset of release.assets) {
      if (!asset || typeof asset.name !== 'string') throw new TypeError(`release ${releaseIndex} has an asset without a name`);
      const platform = Object.entries(PLATFORMS).find(([, suffix]) => asset.name.startsWith(PACKAGE_PREFIX) && asset.name.endsWith(suffix))?.[0];
      if (!platform) continue;
      if (!Number.isSafeInteger(asset.download_count) || asset.download_count < 0) {
        throw new TypeError(`${asset.name} has an invalid download_count`);
      }
      const next = totals[platform] + asset.download_count;
      if (!Number.isSafeInteger(next)) throw new RangeError(`${platform} download total exceeds the safe integer range`);
      totals[platform] = next;
    }
  }
  return totals;
}

export function formatDownloads(total) {
  if (!Number.isSafeInteger(total) || total < 0) throw new TypeError('download total must be a non-negative safe integer');
  if (total >= 1_000_000) return `${(total / 1_000_000).toFixed(1)}M`;
  if (total >= 1_000) return `${(total / 1_000).toFixed(1)}k`;
  return String(total);
}

export function writeBadges(releases, outputDirectory) {
  const totals = downloadTotals(releases);
  mkdirSync(outputDirectory, { recursive: true });
  for (const [platform, total] of Object.entries(totals)) {
    const badge = { schemaVersion: 1, label: 'downloads', message: formatDownloads(total), color: '3fb950' };
    writeFileSync(join(outputDirectory, `${platform}.json`), `${JSON.stringify(badge)}\n`);
  }
  return totals;
}

function main() {
  const [input, output] = process.argv.slice(2);
  if (!input || !output || process.argv.length !== 4) throw new Error('usage: node download-badges.mjs releases.json output-directory');
  let releases;
  try {
    releases = JSON.parse(readFileSync(input, 'utf8'));
  } catch (error) {
    throw new Error(`could not read releases JSON: ${error.message}`);
  }
  const totals = writeBadges(releases, output);
  for (const [platform, total] of Object.entries(totals)) console.log(`${platform}: ${total} (${formatDownloads(total)})`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    main();
  } catch (error) {
    console.error(`download-badges: ${error.message}`);
    process.exitCode = 1;
  }
}
