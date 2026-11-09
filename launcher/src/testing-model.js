// The Testing tab's pure parts (no DOM, no Tauri; `node --test` runs them): the report link, file sizes and the viewer's filter.
import { REPO_URL } from './settings-schema.js';

/** The bug report form. The launcher opens only links without `&`, so nothing is prefilled: the tester drags the zip into the form. */
export const REPORT_URL = `${REPO_URL}/issues/new?template=bug_report.yml`;

export function sizeText(bytes) {
  const n = Number(bytes) || 0;
  return n >= 1 << 20 ? `${(n / (1 << 20)).toFixed(1)} MB` : n >= 1 << 10 ? `${Math.round(n / (1 << 10))} KB` : `${n} B`;
}

/** The lines of a log that contain every word of the query (any case); no query keeps the text as it is. */
export function filterLines(text, query) {
  const words = String(query ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return String(text ?? '');
  return String(text ?? '').split('\n').filter((l) => { const low = l.toLowerCase(); return words.every((w) => low.includes(w)); }).join('\n');
}

/** One entry of the viewer's file list: "pkp.log  (1.2 MB)". */
export const fileLabel = (f) => `${f.name}  (${sizeText(f.bytes)})`;

/** The file name of an exported zip, from its full path (either slash). */
export const zipName = (path) => String(path ?? '').split(/[\\/]/).pop();
