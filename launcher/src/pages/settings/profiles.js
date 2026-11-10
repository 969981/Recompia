// Save profiles: add, switch, rename, remove. Each goes through changeProfiles: flush pending edits, run the command, reload the
// settings (a profile has its own save folder), say what happened.
import { api } from '../../api.js';
import { askText, confirm, errText, toast } from '../../ui.js';

export function createProfileActions(h) {
  const { st } = h;

  async function changeProfiles(command, args, done) {
    try {
      await h.flush();
      st.profiles = await command(args);
      await h.reload();
      toast(done, 'ok');
    } catch (e) {
      toast(errText(e), 'error');
    }
    h.draw();
  }

  return {
    load() {
      api.profilesList().then((list) => {
        st.profiles = list;
        h.draw();
      }).catch((e) => console.warn('profiles_list', e));
    },
    actions: {
      async 'profile-new'() {
        const name = await askText({ title: 'New profile', body: 'It starts with an empty save. Your other saves are not touched.', label: 'Profile name', ok: 'Add' });
        if (name === '') toast('Type a name for the profile', 'error');
        else if (name) await changeProfiles(api.profileAdd, { name }, `Profile ${name} added`);
      },
      'profile-use': (arg, name) => changeProfiles(api.profileSelect, { id: arg }, `Playing as ${name}`),
      async 'profile-rename'(arg, name) {
        const next = await askText({ title: 'Rename profile', label: 'Profile name', value: name, ok: 'Rename' });
        if (next === '') toast('Type a name for the profile', 'error');
        else if (next && next !== name) await changeProfiles(api.profileRename, { id: arg, name: next }, 'Profile renamed');
      },
      async 'profile-remove'(arg, name) {
        const yes = await confirm({
          title: `Remove ${name}?`,
          body: 'This only removes the profile from the list. Its save stays in the save folder, so nothing is lost.',
          ok: 'Remove', danger: true,
        });
        if (yes) await changeProfiles(api.profileRemove, { id: arg }, 'Profile removed');
      },
    },
  };
}
