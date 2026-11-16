/* Modtopia configuration: global defaults, sparse account overrides, and settings access. MIT. */
#include "mt_loader.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>

int mt_loader_set_config_account(MtLoader *L, const char *key) {
    if (!L) return MT_ERR;
    if (L->nmods || L->started) return MT_ERR_STATE;
    if (!key || !*key) { L->config_account[0] = 0; return MT_OK; }
    if (strlen(key) != 32 || strspn(key, "0123456789abcdefABCDEF") != 32 ||
        !strcmp(key, "00000000000000000000000000000000")) return MT_ERR;
    for (int i = 0; i < 32; ++i) L->config_account[i] = (char)tolower((unsigned char)key[i]);
    L->config_account[32] = 0;
    return MT_OK;
}

static char *config_dir(MtLoader *L, int profile) {
    return profile ? mt_fmt("%s/accounts/%s", L->config_dir, L->config_account) : mt_strdup(L->config_dir);
}
static char *config_path(MtLoader *L, const char *id, int profile) {
    char *dir = config_dir(L, profile), *file = mt_fmt("%s/%s.toml", dir, id);
    free(dir);
    return file;
}
static MtToml *read_config(const char *path, int *bad) {
    errno = 0;
    char *text = mt_read_file(path, NULL);
    if (!text) {
        *bad = (errno && errno != ENOENT) || mt_is_file(path) || mt_is_dir(path);
        return NULL;
    }
    MtToml *t = mt_toml_parse(text, NULL, 0);
    free(text);
    *bad = !t;
    return t;
}

void mt_profile_load(MtLoader *L) {
    mt_strs_free(&L->profile_on), mt_strs_free(&L->profile_off);
    L->profile_error = 0;
    if (!L->config_account[0]) return;
    char *p = config_path(L, ".enabled", 1);
    MtToml *t = read_config(p, &L->profile_error), *enabled = mt_toml_get(t, "enabled");
    if (enabled && enabled->type != MT_T_TABLE) L->profile_error = 1;
    for (int i = 0; enabled && enabled->type == MT_T_TABLE && i < enabled->n; ++i) {
        if (enabled->items[i]->type != MT_T_BOOL || !enabled->keys[i][0]) { L->profile_error = 1; break; }
        mt_strs_add(enabled->items[i]->b ? &L->profile_on : &L->profile_off, enabled->keys[i]);
    }
    if (L->profile_error) mt_log(L, MT_LOG_ERROR, "cannot read profile mod overrides: %s", p);
    mt_toml_free(t); free(p);
}
int mt_loader_global_enabled(MtLoader *L, const char *id) { return mt_strs_find(&L->disabled, id) < 0; }
int mt_loader_enabled_override(MtLoader *L, const char *id) {
    if (!L->config_account[0]) return -1;
    if (mt_strs_find(&L->profile_on, id) >= 0) return 1;
    return mt_strs_find(&L->profile_off, id) >= 0 ? 0 : -1;
}
static void enabled_value(MtLoader *L, const char *id, int value) {
    int i = mt_strs_find(&L->profile_on, id); if (i >= 0) mt_strs_remove(&L->profile_on, i);
    i = mt_strs_find(&L->profile_off, id); if (i >= 0) mt_strs_remove(&L->profile_off, i);
    if (value >= 0) mt_strs_add(value ? &L->profile_on : &L->profile_off, id);
}
int mt_profile_set_enabled(MtLoader *L, const char *id, int value) {
    if (!L->config_account[0] || L->profile_error) return MT_ERR_STATE;
    int index = mt_loader_find(L, id);
    if (index < 0) return MT_ERR_UNKNOWN;
    int old = mt_loader_enabled_override(L, id);
    if (value == old) return MT_OK;
    enabled_value(L, id, value);
    MtBuf b = {0};
    mt_buf_str(&b, "# Profile overrides; absent mods inherit global enabled states.\n[enabled]\n");
    for (int on = 0; on <= 1; ++on) {
        MtStrs *ids = on ? &L->profile_on : &L->profile_off;
        for (int i = 0; i < ids->n; ++i) { mt_toml_quote(&b, ids->v[i]); mt_buf_str(&b, on ? " = true\n" : " = false\n"); }
    }
    char *dir = config_dir(L, 1), *path = config_path(L, ".enabled", 1);
    mt_mkdirs(dir);
    int rc = mt_write_file(path, b.p, b.n);
    free(dir), free(path), free(b.p);
    if (rc) enabled_value(L, id, old);
    else mt_mod_at(L, index)->info.enabled = value < 0 ? mt_loader_global_enabled(L, id) : value;
    return rc;
}
int mt_loader_clear_enabled_override(MtLoader *L, const char *id) { return mt_profile_set_enabled(L, id, -1); }

static int load_value(MtSetting *s, MtCfgVal *dst, MtToml *v) {
    switch (s->info.type) {
    case MT_SET_BOOL: if (v->type != MT_T_BOOL) return 0; dst->num = v->b; break;
    case MT_SET_NUMBER: {
        if (v->type != MT_T_INT && v->type != MT_T_FLOAT) return 0;
        double d = v->type == MT_T_INT ? (double)v->i : v->f;
        if (!isfinite(d)) return 0;
        dst->num = d < s->info.min ? s->info.min : d > s->info.max ? s->info.max : d;
        break;
    }
    case MT_SET_CHOICE: {
        int k = v->type == MT_T_STRING ? mt_strs_find(&s->choices, v->s) : -1;
        if (k < 0) return 0;
        dst->num = k; break;
    }
    default:
        if (v->type != MT_T_STRING) return 0;
        free(dst->text); dst->text = mt_strdup(v->s);
    }
    return 1;
}
static void load_layer(MtLoader *L, Mod *m, int profile) {
    char *path = config_path(L, m->m.id, profile);
    int bad = 0;
    MtToml *t = read_config(path, &bad);
    for (int i = 0; t && i < m->m.nsettings; ++i) {
        MtToml *v = mt_toml_get(t, m->m.settings[i].info.key);
        if (!v) continue;
        if (!load_value(&m->m.settings[i], &m->cfg[i], v)) bad = 1;
        else if (profile) m->cfg_overrides[i] = 1;
    }
    if (bad) {
        m->cfg_error = 1;
        mt_mod_message(m, MT_STATE_ERROR, "cannot read %s settings; restore or repair %s", profile ? "profile" : "global", path);
    }
    mt_toml_free(t); free(path);
}
void mt_config_load(MtLoader *L, Mod *m) {
    if (!m->has_manifest || !m->m.nsettings) return;
    int n = m->m.nsettings;
    m->cfg = mt_alloc(sizeof *m->cfg * (size_t)n);
    m->global_cfg = mt_alloc(sizeof *m->global_cfg * (size_t)n);
    m->cfg_overrides = mt_alloc((size_t)n);
    for (int i = 0; i < n; ++i) {
        m->cfg[i].num = m->m.settings[i].def_num;
        m->cfg[i].text = mt_strdup(m->m.settings[i].def_text ? m->m.settings[i].def_text : "");
    }
    load_layer(L, m, 0);
    for (int i = 0; i < n; ++i) {
        m->global_cfg[i].num = m->cfg[i].num;
        m->global_cfg[i].text = mt_strdup(m->cfg[i].text);
    }
    if (L->config_account[0]) load_layer(L, m, 1);
}
static int config_save(MtLoader *L, Mod *m) {
    if (m->cfg_error) return MT_ERR_STATE;
    int profile = !!L->config_account[0];
    MtBuf b = {0};
    mt_buf_fmt(&b, "# %s %s: %s\n", m->m.name, m->m.version, profile ? "profile overrides; omitted keys inherit global values" : "global settings");
    for (int i = 0; i < m->m.nsettings; ++i) {
        if (profile && !m->cfg_overrides[i]) continue;
        MtSetting *s = &m->m.settings[i];
        mt_buf_fmt(&b, "\n# %s\n%s = ", s->info.label, s->info.key);
        switch (s->info.type) {
        case MT_SET_BOOL: mt_buf_str(&b, m->cfg[i].num ? "true" : "false"); break;
        case MT_SET_NUMBER: {
            double d = m->cfg[i].num;
            if (d == floor(d) && fabs(d) < 1e15) mt_buf_fmt(&b, "%.1f", d); else mt_buf_fmt(&b, "%.17g", d);
            break;
        }
        case MT_SET_CHOICE: mt_toml_quote(&b, s->choices.v[(int)m->cfg[i].num]); break;
        default: mt_toml_quote(&b, m->cfg[i].text);
        }
        mt_buf_str(&b, "\n");
    }
    char *dir = config_dir(L, profile), *path = config_path(L, m->m.id, profile);
    mt_mkdirs(dir);
    int rc = mt_write_file(path, b.p, b.n);
    if (rc) mt_log(L, MT_LOG_ERROR, "cannot write %s", path);
    free(dir), free(path), free(b.p);
    return rc;
}
static Mod *setting_mod(MtLoader *L, int mod, int i) {
    Mod *m = mt_mod_at(L, mod);
    return m && m->cfg && i >= 0 && i < m->m.nsettings ? m : NULL;
}
int mt_setting_count(MtLoader *L, int mod) { Mod *m = mt_mod_at(L, mod); return m && m->has_manifest ? m->m.nsettings : 0; }
const MtSettingInfo *mt_setting_info(MtLoader *L, int mod, int i) { Mod *m = setting_mod(L, mod, i); return m ? &m->m.settings[i].info : NULL; }
double mt_setting_get_number(MtLoader *L, int mod, int i) { Mod *m = setting_mod(L, mod, i); return m ? m->cfg[i].num : 0; }
int mt_setting_get_text(MtLoader *L, int mod, int i, char *out, size_t n) {
    Mod *m = setting_mod(L, mod, i);
    if (!m) return MT_ERR_UNKNOWN;
    switch (m->m.settings[i].info.type) {
    case MT_SET_BOOL: snprintf(out, n, "%s", m->cfg[i].num ? "On" : "Off"); break;
    case MT_SET_NUMBER: snprintf(out, n, "%g", m->cfg[i].num); break;
    case MT_SET_CHOICE: snprintf(out, n, "%s", m->m.settings[i].choices.v[(int)m->cfg[i].num]); break;
    default: snprintf(out, n, "%s", m->cfg[i].text);
    }
    return MT_OK;
}
static int commit_value(MtLoader *L, Mod *m, int i, MtCfgVal value, int override) {
    if (m->cfg_error) return MT_ERR_STATE;
    MtCfgVal old = m->cfg[i]; int was = m->cfg_overrides[i];
    mt_lock(&L->lock); m->cfg[i] = value; m->cfg_overrides[i] = (uint8_t)override; mt_unlock(&L->lock);
    int rc = config_save(L, m);
    if (rc) { mt_lock(&L->lock); m->cfg[i] = old; m->cfg_overrides[i] = (uint8_t)was; mt_unlock(&L->lock); }
    else mt_config_notify(m, m->m.settings[i].info.key);
    /* Replaced text is retained: a running mod may still hold the previous config_string pointer. */
    return rc;
}
int mt_setting_set_number(MtLoader *L, int mod, int i, double v) {
    Mod *m = setting_mod(L, mod, i);
    if (!m) return MT_ERR_UNKNOWN;
    if (!isfinite(v)) return MT_ERR;
    MtSetting *s = &m->m.settings[i];
    if (s->info.type == MT_SET_BOOL) v = v ? 1 : 0;
    else if (s->info.type == MT_SET_NUMBER) {
        if (s->info.step > 0) v = s->info.min + floor((v - s->info.min) / s->info.step + 0.5) * s->info.step;
        v = v < s->info.min ? s->info.min : v > s->info.max ? s->info.max : v;
    } else if (s->info.type == MT_SET_CHOICE) v = v < 0 ? 0 : v >= s->choices.n ? s->choices.n - 1 : (int)v;
    else return MT_ERR_STATE;
    MtCfgVal next = m->cfg[i]; next.num = v;
    return commit_value(L, m, i, next, !!L->config_account[0]);
}
int mt_setting_set_text(MtLoader *L, int mod, int i, const char *v) {
    Mod *m = setting_mod(L, mod, i);
    if (!m || !v) return MT_ERR_UNKNOWN;
    MtSetting *s = &m->m.settings[i];
    if (s->info.type == MT_SET_CHOICE) { int k = mt_strs_find(&s->choices, v); return k < 0 ? MT_ERR_UNKNOWN : mt_setting_set_number(L, mod, i, k); }
    if (s->info.type != MT_SET_TEXT && s->info.type != MT_SET_KEY) return MT_ERR_STATE;
    MtCfgVal next = m->cfg[i]; next.text = mt_strdup(v);
    return commit_value(L, m, i, next, !!L->config_account[0]);
}
int mt_setting_is_overridden(MtLoader *L, int mod, int i) {
    Mod *m = setting_mod(L, mod, i); return m && L->config_account[0] ? m->cfg_overrides[i] : 0;
}
int mt_setting_clear_override(MtLoader *L, int mod, int i) {
    Mod *m = setting_mod(L, mod, i);
    if (!m) return MT_ERR_UNKNOWN;
    if (!L->config_account[0] || m->cfg_error) return MT_ERR_STATE;
    if (!m->cfg_overrides[i]) return MT_OK;
    MtCfgVal next = m->global_cfg[i]; next.text = mt_strdup(next.text);
    return commit_value(L, m, i, next, 0);
}
