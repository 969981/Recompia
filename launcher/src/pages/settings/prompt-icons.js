// The icons of the Button icons preview: the game folder's icon pack, read once per launcher run (api.promptIconPack), parsed by
// prompt-pack-model.js and turned into image URLs on first use. A missing or broken pack gives null: the row says so.
import { api } from '../../api.js';
import { parsePack } from '../../prompt-pack-model.js';

let loading = null;

function urlsOf(pack) {
  const urls = new Map();
  const canvas = document.createElement('canvas');
  canvas.width = pack.width;
  canvas.height = pack.height;
  const g = canvas.getContext('2d');
  return (name) => {
    if (!urls.has(name)) {
      const px = pack.pixels(name);
      let url = '';
      if (px) {
        g.putImageData(new ImageData(new Uint8ClampedArray(px), pack.width, pack.height), 0, 0);
        url = canvas.toDataURL('image/png');
      }
      urls.set(name, url);
    }
    return urls.get(name);
  };
}

/** A function from a pack entry ("xbox/a") to an image URL ('' when the pack lacks it), or null without a usable pack. */
export function loadPromptIcons() {
  loading ??= api.promptIconPack().then((buf) => urlsOf(parsePack(buf))).catch(() => {
    loading = null;   // ask again next time (the game folder may change)
    return null;
  });
  return loading;
}
