// The launcher's logo and icon pictures. src-tauri/src/branding.rs puts the icon in the exes, this window and the Steam art.
import logoUrl from './art/logo.png';
import markUrl from './art/mark.png';
import markTestingUrl from './art/mark-testing.png';

const ART = { logo: logoUrl, mark: markUrl, 'mark-testing': markTestingUrl };

/** An <img> of `kind` (logo, mark, mark-testing). `attrs`: the rest of the tag (class, alt). */
export const brandImg = (kind, attrs = '') => `<img src="${ART[kind]}" ${attrs} />`;
