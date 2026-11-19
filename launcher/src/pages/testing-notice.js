// The question asked when someone switches the launcher to the Experimental channel: what it means, what is expected of a tester, and
// how to report. Resolves true for "I understand, show the builds"; Cancel, Esc or a click outside give false and the channel stays as
// it was (pages/version-picker.js asks it before the list of Experimental builds).
import { confirm } from '../ui.js';

export const EXPERIMENTAL_NOTICE_HTML = `
  <p>Experimental builds test new features before they reach a stable release. Each one tests its own features, so you pick the one you want to try. They are less tested than the stable releases and can have bugs, crashes or things that stop working.</p>
  <p><strong>By joining you are helping us test, so please tell us what you find.</strong> Use <em>Settings &gt; About &gt; Report a problem</em>, say which Experimental build you have, and attach <code>pkp.log</code> from the game folder (the <em>Show pkp.log</em> button opens it), plus <code>setup.log</code> if Set up ran. Quick questions and reports are also welcome on the Discord (<em>Settings &gt; About &gt; Community</em>).</p>
  <p>Each Experimental build lists what to test and its known problems in its notes and in <em>What's New</em>. Nothing updates by itself on Experimental: you pick every build yourself. Your saves, profiles and settings are kept; if you want to be safe, copy your save folder first.</p>
  <p>To leave, switch back to Stable and pick <em>Latest</em> or a version next to Play.</p>`;

export const askExperimental = () => confirm({ title: 'Try Experimental builds?', html: EXPERIMENTAL_NOTICE_HTML, ok: 'I understand, show the builds', cancel: 'Cancel' });
