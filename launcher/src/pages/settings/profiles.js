// Save profiles: add, switch, rename, remove. Each goes through changeProfiles: flush pending edits, run the command, reload the
// settings (a profile has its own save folder), say what happened.
import { api } from '../../api.js';
import { num, t } from '../../i18n.js';
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
        const name = await askText({ title: t('New profile'), body: t('It starts with an empty save. Your other saves are not touched.'), label: t('Profile name'), ok: t('Add') });
        if (name === '') toast(t('Type a name for the profile'), 'error');
        else if (name) await changeProfiles(api.profileAdd, { name }, t('Profile {name} added', { name }));
      },
      'profile-use': (arg, name) => changeProfiles(api.profileSelect, { id: arg }, t('Playing as {name}', { name })),
      async 'profile-rename'(arg, name) {
        const next = await askText({ title: t('Rename profile'), label: t('Profile name'), value: name, ok: t('Rename') });
        if (next === '') toast(t('Type a name for the profile'), 'error');
        else if (next && next !== name) await changeProfiles(api.profileRename, { id: arg, name: next }, t('Profile renamed'));
      },
      async 'profile-remove'(arg, name) {
        const yes = await confirm({
          title: t('Remove {name}?', { name }),
          body: t('This only removes the profile from the list. Its save stays in the save folder, so nothing is lost.'),
          ok: t('Remove'), danger: true,
        });
        if (yes) await changeProfiles(api.profileRemove, { id: arg }, t('Profile removed'));
      },
    },
  };
}
