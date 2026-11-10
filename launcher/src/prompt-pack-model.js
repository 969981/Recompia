// The button icon pack (runtime/assets/prompts/prompt_icons.pak; format in runtime/scripts/build_prompt_pack.py), read for the Settings
// page's Button icons preview. Pure: no window needed.
//   header (24 bytes, little endian): "MPIP", u32 version 1, u32 count, u32 width, u32 height, u32 reserved
//   count entries of 48 bytes: char name[40] (ASCII, NUL padded), u32 offset, u32 size (= width * height * 4)
//   then the RGBA8 pixels (straight alpha, row 0 = top)

const HEADER = 24;
const ENTRY = 48;
const NAME = 40;

/** { width, height, pixels(name) } of a pack (an ArrayBuffer); pixels gives a Uint8ClampedArray, or null for a name it lacks. A key
 *  without an icon of its own gets the plain key (kbm/any), as in the game. Throws on a malformed pack. */
export function parsePack(buffer) {
  const dv = new DataView(buffer);
  const magic = String.fromCharCode(...new Uint8Array(buffer, 0, 4));
  if (buffer.byteLength < HEADER || magic !== 'MPIP' || dv.getUint32(4, true) !== 1) throw new Error('not an MPIP v1 icon pack');
  const count = dv.getUint32(8, true), width = dv.getUint32(12, true), height = dv.getUint32(16, true);
  const size = width * height * 4;
  if (HEADER + count * ENTRY > buffer.byteLength) throw new Error('icon pack directory is cut off');
  const offsets = new Map();
  for (let i = 0; i < count; i++) {
    const at = HEADER + i * ENTRY;
    const raw = new Uint8Array(buffer, at, NAME);
    const end = raw.indexOf(0);
    const name = String.fromCharCode(...raw.subarray(0, end < 0 ? NAME : end));
    const off = dv.getUint32(at + NAME, true);
    if (dv.getUint32(at + NAME + 4, true) !== size || off + size > buffer.byteLength) throw new Error(`icon pack entry ${name} is out of range`);
    offsets.set(name, off);
  }
  const pixels = (name) => {
    const off = offsets.get(name) ?? (name.startsWith('kbm/') ? offsets.get('kbm/any') : undefined);
    return off === undefined ? null : new Uint8ClampedArray(buffer, off, size);
  };
  return { width, height, pixels };
}

/** "A=xbox/b" (the schema's choice_details of [prompts] style) as { button, icon }; icon '' = the game's own. */
export const previewItem = (s) => {
  const i = s.indexOf('=');
  return { button: s.slice(0, i), icon: s.slice(i + 1) };
};
