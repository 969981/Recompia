// From pokopia-native (MIT) windows/src/coop/coop_disc.c @035bd9c, modified (marked "pokopia-recomp extension: LAN sweep" / "mDNS responder")
// coop disc (MIT, our own code): DNS-SD on the LAN (research/06 §5.5, docs/coop-step1c.md). A hosting node registers "Pokopia-<8 hex>" as
// _pokopia-ldn._udp with its control port and a TXT record: p = the coop protocol, b = main's build ID (16 hex digits, "-" unknown), m = the mods
// fingerprint, a = the host's best LAN IPv4, x = its other LAN IPv4s, comma-separated (a PC on two networks, such as Wi-Fi and a phone's USB link).
// Nothing else is published: no player, town or computer names. A scanning node browses for the type and turns each instance into an address and
// port: of the responder's A record and the TXT's addresses, the first on one of its own subnets (else the first), the others tried in turn.
// Backends: Apple dns_sd (macOS, iOS: one shared connection to mDNSResponder that the I/O thread polls; unicast and Bonjour need no multicast
// entitlement), Windows 10 1809+ DnsServiceRegister / DnsServiceBrowse (asynchronous, callbacks on system threads, loaded from dnsapi.dll at run
// time; its browse delivers PTR, SRV and TXT records but resolves no A record for a registration on the same machine, hence a=), nothing elsewhere.
// Everything from the network is untrusted: names, TXT values, ports and addresses are checked before an entry is kept, an entry only names a host
// to try, and the control link checks its WELCOME and every NetworkInfo again (coop_mpnet.c, coop_ldn.c).
// pokopia-recomp extension: LAN sweep / mDNS responder: on Windows without DnsService* (Wine, Proton) a LAN sweep and a small mDNS responder stand in
// for the Windows backend (x_*, after w_reg_cb); with DnsService* nothing changes.
#include "coop_int.h"
#include <stdio.h>

#define DISC_RETRY_MS 5000        // after a failed start
#define RESOLVE_MS 5000           // Apple: a resolve or address lookup that has not answered is dropped
#define GAI_GRACE_MS 1000         // Apple: the responder's A record gets this long before the TXT's a= is used
#define WBROWSE_REFRESH_MS 15000  // Windows: the browse is restarted (fresh answers from every live host) ...
#define WENTRY_STALE_MS 35000     // ... and an entry not reported again for this long is dropped

static int64_t now_ms(void) { return cplat_now_ms(); }

// ---------- TXT records and names (shared, tested directly) ----------
static int hexval(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
static int hex16(const uint8_t *s, size_t n, uint64_t *v, char *text) {   // exactly 16 hex digits
    if (n != 16) return 0;
    uint64_t x = 0;
    for (size_t i = 0; i < 16; i++) { int h = hexval(s[i]); if (h < 0) return 0; x = x << 4 | (uint64_t)h; if (text) text[i] = "0123456789abcdef"[h]; }
    if (text) text[16] = 0;
    if (v) *v = x;
    return 1;
}
static int dotted4(const uint8_t *s, size_t n, uint32_t *ip) {
    uint32_t v = 0; size_t i = 0;
    for (int part = 0; part < 4; part++) {
        unsigned b = 0, digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 3) { b = b * 10 + (unsigned)(s[i++] - '0'); digits++; }
        if (!digits || b > 255) return 0;
        v = v << 8 | b;
        if (part < 3) { if (i >= n || s[i] != '.') return 0; i++; }
    }
    if (i != n) return 0;
    *ip = v; return 1;
}
static int dotted_list(const uint8_t *s, size_t n, uint32_t *ip, int max) {   // "a.b.c.d,e.f.g.h": the count, -1 when malformed or too long
    int k = 0;
    for (size_t i = 0; i <= n;) {
        size_t j = i; while (j < n && s[j] != ',') j++;
        if (k == max || !dotted4(s + i, j - i, &ip[k])) return -1;
        k++; i = j + 1;
    }
    return k;
}
int cdisc_txt_parse(const uint8_t *t, size_t len, cdisc_entry *e) {
    int have = 0;
    e->proto = 0; e->build[0] = 0; e->mods = 0; e->txt_ip = 0; e->txt_nx = 0; e->has_txt = 0;
    if (!t || len > 1300) return 0;   // one mDNS packet's worth
    for (size_t i = 0; i < len;) {
        size_t l = t[i++];
        if (l > len - i) return 0;    // a string runs past the record
        const uint8_t *s = t + i; i += l;
        if (l < 2 || s[1] != '=') continue;
        const uint8_t *v = s + 2; size_t vl = l - 2;
        switch (s[0]) {
        case 'p': { unsigned p = 0; if (!vl || vl > 5) return 0; for (size_t k = 0; k < vl; k++) { if (v[k] < '0' || v[k] > '9') return 0; p = p * 10 + (unsigned)(v[k] - '0'); }
                    if (!p || p > 65535) return 0; e->proto = (int)p; have |= 1; break; }
        case 'b': if (vl == 1 && v[0] == '-') e->build[0] = 0; else if (!hex16(v, vl, NULL, e->build)) return 0; have |= 2; break;
        case 'm': if (!hex16(v, vl, &e->mods, NULL)) return 0; have |= 4; break;
        case 'a': if (!dotted4(v, vl, &e->txt_ip)) return 0; break;
        case 'x': if ((e->txt_nx = dotted_list(v, vl, e->txt_x, DISC_ADDRS - 1)) < 0) return 0; break;
        default: break;   // keys of later protocols
        }
    }
    e->has_txt = have == 7;
    return e->has_txt;
}
static size_t txt_put(uint8_t *out, size_t cap, size_t at, const char *kv) {
    size_t l = strlen(kv);
    if (l > 255 || at + 1 + l > cap) return at;
    out[at] = (uint8_t)l; memcpy(out + at + 1, kv, l);
    return at + 1 + l;
}
static int addr_list(char *o, size_t cap, const uint32_t *ip, int n) {   // "a.b.c.d,e.f.g.h"; the length
    size_t at = 0;
    for (int i = 0; i < n && at < cap; i++)
        at += (size_t)snprintf(o + at, cap - at, "%s%u.%u.%u.%u", i ? "," : "", ip[i] >> 24, ip[i] >> 16 & 0xFF, ip[i] >> 8 & 0xFF, ip[i] & 0xFF);
    return (int)(at < cap ? at : cap - 1);
}
size_t cdisc_txt_make(uint8_t *out, size_t cap, int proto, const char *build, uint64_t mods, const uint32_t *ip, int nip) {
    char kv[80]; size_t at = 0;
    snprintf(kv, sizeof kv, "p=%d", proto); at = txt_put(out, cap, at, kv);
    snprintf(kv, sizeof kv, "b=%s", build && build[0] ? build : "-"); at = txt_put(out, cap, at, kv);
    snprintf(kv, sizeof kv, "m=%016llx", (unsigned long long)mods); at = txt_put(out, cap, at, kv);
    if (nip > DISC_ADDRS) nip = DISC_ADDRS;
    if (nip > 0) { memcpy(kv, "a=", 2); addr_list(kv + 2, sizeof kv - 2, ip, 1); at = txt_put(out, cap, at, kv); }
    if (nip > 1) { memcpy(kv, "x=", 2); addr_list(kv + 2, sizeof kv - 2, ip + 1, nip - 1); at = txt_put(out, cap, at, kv); }
    return at;
}
int cdisc_host_addrs(const caddr *mine, int n, uint32_t route, uint32_t *out, int max) {
    int rank[64], idx[64], k = 0;
    for (int i = 0; i < n && k < 64; i++) {
        int cls = coop_addr4_class(mine[i].ip), dup = 0;
        if (cls != COOP_ADDR_PRIVATE && cls != COOP_ADDR_LINKLOCAL) continue;
        for (int j = 0; j < k; j++) dup |= mine[idx[j]].ip == mine[i].ip;
        if (dup) continue;
        rank[k] = (mine[i].is_public ? 4 : 0) + (cls == COOP_ADDR_LINKLOCAL ? 2 : 0) + (mine[i].ip != route);
        idx[k++] = i;
    }
    for (int i = 1; i < k; i++)   // stable: equals keep the system's order
        for (int j = i; j > 0 && rank[j] < rank[j - 1]; j--) { int t = rank[j]; rank[j] = rank[j - 1]; rank[j - 1] = t; t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    int m = k < max ? k : max;
    for (int i = 0; i < m; i++) out[i] = mine[idx[i]].ip;
    return m;
}
int cdisc_pick(const uint32_t *cand, int n, const caddr *mine, int nm) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nm; j++)
            if (mine[j].mask && mine[j].ip != cand[i] && (mine[j].ip & mine[j].mask) == (cand[i] & mine[j].mask)) return i;
    return 0;
}
int cdisc_name_ok(const char *s, size_t max) {
    size_t k = 0;
    for (; k < max && s[k]; k++) if ((unsigned char)s[k] < 0x20 || s[k] == 0x7F) return 0;
    return k > 0 && k < max;
}

// ---------- the entry table (dlk) ----------
static void ipstr(char *o, uint32_t ip) { snprintf(o, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF); }
// A resolved instance. txt has the TXT's fields; ip is the responder's A record (0: none). The addresses to try: the A record, a= and x=, LAN ones
// only, the first on one of this machine's subnets moved to the front. Returns 1 when kept
static int found(coop_node *n, const char *name, uint32_t ifi, uint32_t ip, uint16_t port, const cdisc_entry *txt, int timed) {
    cdisc *d = &n->disc;
    char a[16];
    uint32_t cand[DISC_ADDRS] = {0}, all[2 + DISC_ADDRS - 1] = {ip, txt->txt_ip}; int nc = 0;
    memcpy(all + 2, txt->txt_x, sizeof txt->txt_x);
    for (int i = 0; i < 2 + txt->txt_nx && nc < DISC_ADDRS; i++) {
        int dup = 0;
        for (int j = 0; j < nc; j++) dup |= cand[j] == all[i];
        if (!dup && all[i] && coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(all[i]))) cand[nc++] = all[i];
    }
    if (nc > 1) {
        caddr mine[16]; int nm = cplat_local_addrs(mine, 16, 0), k = cdisc_pick(cand, nc, mine, nm > 0 ? nm : 0);
        uint32_t t = cand[k]; memmove(cand + 1, cand, (size_t)k * sizeof cand[0]); cand[0] = t;
    }
    ip = nc ? cand[0] : ip ? ip : txt->txt_ip;
    if (!cdisc_name_ok(name, sizeof d->e[0].name) || !port || !txt->has_txt || !nc) {
        cplat_lock(&n->dlk); d->bad++; uint64_t k = d->bad; cplat_unlock(&n->dlk);
        ipstr(a, ip);
        if (k <= 4) coop_logf(n, "disc: an instance ignored (%s, %s:%u): not a LAN address, no port or no valid TXT record", cdisc_name_ok(name, 64) ? name : "a bad name", a, port);
        return 0;
    }
    cplat_lock(&n->dlk);
    cdisc_entry *e = NULL, *fr = NULL;
    for (int i = 0; i < DISC_MAX; i++) {
        if (!d->e[i].used) { if (!fr) fr = &d->e[i]; continue; }
        if (!strcmp(d->e[i].name, name) && (d->e[i].ifindex == ifi || !ifi || !d->e[i].ifindex)) { e = &d->e[i]; break; }
    }
    int fresh = !e || e->ip != ip || e->port != port;
    if (!e) e = fr;
    if (e) {
        cdisc_entry t = *txt;
        snprintf(t.name, sizeof t.name, "%s", name);
        t.used = 1; t.ifindex = ifi; t.ip = ip; t.port = port; t.seen = timed ? now_ms() : 0;
        memcpy(t.addrs, cand, sizeof cand); t.naddrs = nc;
        *e = t;
    }
    cplat_unlock(&n->dlk);
    ipstr(a, ip);
    if (!e) coop_logf(n, "disc: %s at %s:%u not kept: %d hosts known already", name, a, port, DISC_MAX);
    else if (fresh) coop_logf(n, "disc: found %s at %s:%u%s (coop protocol %d, build %s, mods %016llx)", name, a, port, nc > 1 ? " (and other addresses)" : "", txt->proto,
                              txt->build[0] ? txt->build : "unknown", (unsigned long long)txt->mods);
    return e != NULL;
}
static void lost(coop_node *n, const char *name, uint32_t ifi) {
    int k = 0;
    cplat_lock(&n->dlk);
    for (int i = 0; i < DISC_MAX; i++) {
        cdisc_entry *e = &n->disc.e[i];
        if (e->used && !strcmp(e->name, name) && (!ifi || !e->ifindex || e->ifindex == ifi)) { memset(e, 0, sizeof *e); k++; }
    }
    cplat_unlock(&n->dlk);
    if (k) coop_logf(n, "disc: %s is gone", name);
}
static void clear_entries(coop_node *n) { cplat_lock(&n->dlk); memset(n->disc.e, 0, sizeof n->disc.e); cplat_unlock(&n->dlk); }
#if defined(__APPLE__) || defined(_WIN32)
static int host_addrs(uint32_t *out, char *shown, size_t cap) {   // this host's LAN addresses for its record, best first, and as text for the log
    caddr mine[16]; uint32_t route = 0; int k = 0;
    if (cplat_route_ip(&route)) route = 0;
    int nm = cplat_local_addrs(mine, 16, 1);
    if (nm > 0) k = cdisc_host_addrs(mine, nm, route, out, DISC_ADDRS);
    else if (route && coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(route)) && coop_addr4_class(route) != COOP_ADDR_LOOPBACK) out[k++] = route;
    addr_list(shown, cap, out, k);
    return k;
}
#endif
static void make_instance(char *out, size_t n) { uint32_t r; cplat_random(&r, sizeof r); snprintf(out, n, "Pokopia-%08x", r); }

#if defined(__APPLE__)
// ---------- Apple: dns_sd ----------
#include <arpa/inet.h>
#include <dns_sd.h>
typedef struct aslot { coop_node *n; int used; DNSServiceRef res, gai; char name[64], type[64], dom[64]; uint32_t ifi; uint16_t port; cdisc_entry txt; int64_t t0, txt_at; } aslot;
typedef struct aimpl { DNSServiceRef conn, reg, browse; uint16_t reg_port; char inst[32], addrs[80]; int64_t retry_at; aslot s[DISC_MAX]; } aimpl;
static aimpl *AI(coop_node *n) { return n->disc.impl; }
static const char *policy_hint(DNSServiceErrorType e) {
    return e == kDNSServiceErr_PolicyDenied ? ": local network access is off for this app (Settings, Privacy & Security, Local Network)" : "";
}
static void slot_free(aslot *s) {
    if (s->res) DNSServiceRefDeallocate(s->res);
    if (s->gai) DNSServiceRefDeallocate(s->gai);
    coop_node *n = s->n; memset(s, 0, sizeof *s); s->n = n;
}
static void a_reset(coop_node *n) {
    aimpl *a = AI(n);
    for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used) slot_free(&a->s[i]);
    if (a->reg) DNSServiceRefDeallocate(a->reg);
    if (a->browse) DNSServiceRefDeallocate(a->browse);
    if (a->conn) DNSServiceRefDeallocate(a->conn);
    a->conn = a->reg = a->browse = NULL;
    n->disc.adv_on = n->disc.browse_on = 0;
}
static void DNSSD_API reg_cb(DNSServiceRef r, DNSServiceFlags fl, DNSServiceErrorType err, const char *name, const char *type, const char *dom, void *ctx) {
    coop_node *n = ctx; (void)r; (void)fl; (void)type; (void)dom;
    if (err) { coop_logf(n, "disc: advertising failed (dns_sd %d)%s", (int)err, policy_hint(err)); n->disc.adv_on = 0; return; }
    n->disc.adv_on = 1;
    coop_logf(n, "disc: advertising \"%s\" (%s) on port %u, addresses %s", name ? name : "?", COOP_SERVICE, AI(n)->reg_port, AI(n)->addrs[0] ? AI(n)->addrs : "none");
}
static void DNSSD_API gai_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *host, const struct sockaddr *addr, uint32_t ttl, void *ctx) {
    aslot *s = ctx; (void)r; (void)host; (void)ttl;
    if (err || !(fl & kDNSServiceFlagsAdd) || !addr || addr->sa_family != AF_INET) return;   // more answers may come; the slot's time limit ends it
    uint32_t ip = ntohl(((const struct sockaddr_in *)addr)->sin_addr.s_addr);
    if (!coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(ip))) return;   // a public or other address on some interface: wait for a LAN one
    found(s->n, s->name, ifi, ip, s->port, &s->txt, 0);
    slot_free(s);
}
static void DNSSD_API resolve_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *full, const char *host, uint16_t port_be,
                                 uint16_t txtlen, const unsigned char *txt, void *ctx) {
    aslot *s = ctx; coop_node *n = s->n; (void)r; (void)fl; (void)full;
    DNSServiceRefDeallocate(s->res); s->res = NULL;
    if (err || !host || strnlen(host, 256) >= 256) { slot_free(s); return; }
    s->port = ntohs(port_be);
    if (!cdisc_txt_parse(txt, txtlen, &s->txt)) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); coop_logf(n, "disc: %s has no valid TXT record: ignored", s->name); slot_free(s); return; }
    s->txt_at = now_ms();
    s->gai = AI(n)->conn;
    if (DNSServiceGetAddrInfo(&s->gai, kDNSServiceFlagsShareConnection, ifi, kDNSServiceProtocol_IPv4, host, gai_cb, s) != kDNSServiceErr_NoError) s->gai = NULL;
}
static void DNSSD_API browse_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *name, const char *type, const char *dom, void *ctx) {
    coop_node *n = ctx; aimpl *a = AI(n); (void)r;
    if (err) { coop_logf(n, "disc: browsing failed (dns_sd %d)%s", (int)err, policy_hint(err)); return; }
    if (!name || !cdisc_name_ok(name, 64) || !type || strnlen(type, 64) >= 64 || !dom || strnlen(dom, 64) >= 64) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); return; }
    if (!(fl & kDNSServiceFlagsAdd)) {
        for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used && !strcmp(a->s[i].name, name) && a->s[i].ifi == ifi) slot_free(&a->s[i]);
        lost(n, name, ifi); return;
    }
    if (!strcmp(name, a->inst)) return;   // this node's own service
    aslot *s = NULL;
    for (int i = 0; i < DISC_MAX; i++) { if (a->s[i].used && !strcmp(a->s[i].name, name) && a->s[i].ifi == ifi) return; if (!a->s[i].used && !s) s = &a->s[i]; }
    if (!s) return;
    memset(s, 0, sizeof *s); s->n = n; s->used = 1; s->ifi = ifi; s->t0 = now_ms();
    snprintf(s->name, sizeof s->name, "%s", name); snprintf(s->type, sizeof s->type, "%s", type); snprintf(s->dom, sizeof s->dom, "%s", dom);
    s->res = a->conn;
    if (DNSServiceResolve(&s->res, kDNSServiceFlagsShareConnection, ifi, s->name, s->type, s->dom, resolve_cb, s) != kDNSServiceErr_NoError) { s->res = NULL; slot_free(s); }
}
void cdisc_init(coop_node *n) { n->disc.impl = calloc(1, sizeof(aimpl)); }
void cdisc_fini(coop_node *n) { if (n->disc.impl) { a_reset(n); free(n->disc.impl); n->disc.impl = NULL; } }
csock cdisc_fd(coop_node *n) { aimpl *a = AI(n); return a && a->conn ? DNSServiceRefSockFD(a->conn) : CSOCK_BAD; }
void cdisc_process(coop_node *n) {
    aimpl *a = AI(n);
    if (!a || !a->conn) return;
    DNSServiceErrorType e = DNSServiceProcessResult(a->conn);
    if (e != kDNSServiceErr_NoError) { coop_logf(n, "disc: the mDNSResponder connection failed (dns_sd %d); retrying", (int)e); a_reset(n); a->retry_at = now_ms() + DISC_RETRY_MS; }
}
void cdisc_tick(coop_node *n) {
    aimpl *a = AI(n); cdisc *d = &n->disc; int64_t now = now_ms();
    if (!a) return;
    if (a->reg && (!d->adv_want || a->reg_port != d->adv_port)) { DNSServiceRefDeallocate(a->reg); a->reg = NULL; d->adv_on = 0; coop_logf(n, "disc: stopped advertising"); }
    if (a->browse && !d->browse_want) {
        DNSServiceRefDeallocate(a->browse); a->browse = NULL; d->browse_on = 0;
        for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used) slot_free(&a->s[i]);
        clear_entries(n);
    }
    if (!a->reg && !a->browse && a->conn) { DNSServiceRefDeallocate(a->conn); a->conn = NULL; }
    if (!(d->adv_want && !a->reg) && !(d->browse_want && !a->browse)) goto slots;
    if (now < a->retry_at) goto slots;
    if (!a->conn && DNSServiceCreateConnection(&a->conn) != kDNSServiceErr_NoError) {
        a->conn = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: mDNSResponder is not reachable; retrying in 5 s"); goto slots;
    }
    if (d->adv_want && !a->reg) {
        uint8_t txt[200]; uint32_t ips[DISC_ADDRS];   // mDNSResponder publishes an A record per interface itself; the TXT lists them all
        int k = host_addrs(ips, a->addrs, sizeof a->addrs);
        size_t tl = cdisc_txt_make(txt, sizeof txt, n->proto, n->build, n->mods, ips, k);
        make_instance(a->inst, sizeof a->inst);
        a->reg = a->conn; a->reg_port = d->adv_port;
        DNSServiceErrorType e = DNSServiceRegister(&a->reg, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny, a->inst, COOP_SERVICE, NULL, NULL,
                                                   htons(d->adv_port), (uint16_t)tl, txt, reg_cb, n);
        if (e != kDNSServiceErr_NoError) { a->reg = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: advertising failed to start (dns_sd %d)", (int)e); }
    }
    if (d->browse_want && !a->browse) {
        a->browse = a->conn;
        DNSServiceErrorType e = DNSServiceBrowse(&a->browse, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny, COOP_SERVICE, NULL, browse_cb, n);
        if (e != kDNSServiceErr_NoError) { a->browse = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: browsing failed to start (dns_sd %d)", (int)e); }
        else { d->browse_on = 1; coop_logf(n, "disc: browsing for %s", COOP_SERVICE); }
    }
slots:
    for (int i = 0; i < DISC_MAX; i++) {   // the responder's A record had its chance: the TXT's a= then; a slot that never answers is dropped
        aslot *s = &a->s[i];
        if (!s->used) continue;
        if (s->txt_at && s->txt.txt_ip && now - s->txt_at >= GAI_GRACE_MS) { found(n, s->name, s->ifi, 0, s->port, &s->txt, 0); slot_free(s); continue; }
        if (now - s->t0 >= RESOLVE_MS) slot_free(s);
    }
}
void cdisc_stop(coop_node *n) { if (n->disc.impl) a_reset(n); clear_entries(n); }

#elif defined(_WIN32)
// ---------- Windows: DnsServiceRegister / DnsServiceBrowse (Windows 10 1809+), loaded at run time ----------
#include <windns.h>
#include <iphlpapi.h>   // pokopia-recomp extension: LAN sweep (adapters and their subnets)
// pokopia-recomp extension: LAN sweep / mDNS responder (not in pokopia-native; the functions follow w_reg_cb). Their state, in wimpl
#define XSW_WAIT_MS 300           // a sweep's connects get this long, all at once
#define XSW_GAP_MS 4000           // a Scan starts a new sweep at most this often (the game scans about once a second) ...
#define XSW_SCAN_RECENT_MS 3000   // ... and only while it scans: a joined visitor's game stops scanning, its sweeps stop
#define XSW_IF_MAX 254            // addresses per interface (a /24; a larger subnet: the /24 around this machine's address)
#define XSW_MAX 512               // addresses per sweep
#define XSW_BATCH 256             // connects in flight
#define XSW_LOG_MS 30000          // an unchanged result is logged again after this
#define XMD_GROUP 0xE00000FBu     // 224.0.0.251
#define XMD_PORT 5353
typedef struct xsweep {
    cthread t; int on; volatile LONG done, stop;
    uint16_t port; int64_t t0;
    int naddr, nhit, nalive;
    uint32_t addr[XSW_MAX + 1], hit[XSW_MAX + 1], alive[PEERS];
    char what[160];               // the subnets swept, for the log
} xsweep;
typedef struct xmdns {
    csock s; int on, dead, said;  // dead: port 5353 could not be had (until hosting stops)
    uint16_t port; char inst[32], shown[80];
    uint32_t ips[DISC_ADDRS]; int nip;
    uint32_t ifs[8]; int nif;     // interfaces the group was joined on: multicast answers go out on each
    int64_t ann_at, retry_at; int ann_left;
    bucket mrate, urate;
} xmdns;
typedef DWORD (WINAPI *f_reg)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef DNS_STATUS (WINAPI *f_browse)(PDNS_SERVICE_BROWSE_REQUEST, PDNS_SERVICE_CANCEL);
typedef DNS_STATUS (WINAPI *f_bcancel)(PDNS_SERVICE_CANCEL);
typedef PDNS_SERVICE_INSTANCE (WINAPI *f_construct)(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD, PCWSTR *, PCWSTR *);
typedef VOID (WINAPI *f_free)(PDNS_SERVICE_INSTANCE);
typedef VOID (WINAPI *f_rlfree)(PDNS_RECORD, DNS_FREE_TYPE);
// One context per asynchronous operation: it outlives the node's interest in it (a browse's last callback is ERROR_CANCELLED, a registration's
// last is the deregistration's) and is freed by that last callback. g_wpending counts them, so coop_destroy can wait for the stragglers
typedef struct wctx { coop_node *n; volatile LONG dead; int kind; } wctx;
static volatile LONG g_wpending;
typedef struct wimpl {
    HMODULE dll; int loaded;
    f_reg reg, dereg; f_browse browse; f_bcancel bcancel; f_construct construct; f_free ifree; f_rlfree rlfree;
    // dlk: shared with the callbacks
    wctx *bctx; DNS_SERVICE_CANCEL bcancel_h; int64_t browse_at;
    wctx *rctx; DNS_SERVICE_REGISTER_REQUEST rreq; PDNS_SERVICE_INSTANCE inst; int reg_state;   // 0 none, 1 registering, 2 registered, 3 deregistering
    uint16_t reg_port; char inst_name[32], addrs[80]; int64_t retry_at; int warned, noaddr_said;
    // pokopia-recomp extension: LAN sweep / mDNS responder (without DnsService*; I/O thread): x_diag NXRT_COOP_NO_DNSSD (2: loopback), the sweep and when the
    // last one started, the last result logged, the responder
    int x_diag; xsweep xs; int64_t xs_at, xs_logat; uint64_t xs_logsum; xmdns xm;
} wimpl;
static wimpl *WI(coop_node *n) { return n->disc.impl; }
static wctx *wctx_new(coop_node *n, int kind) { wctx *c = calloc(1, sizeof *c); if (c) { c->n = n; c->kind = kind; InterlockedIncrement(&g_wpending); } return c; }
static void wctx_done(wctx *c) { free(c); InterlockedDecrement(&g_wpending); }
static void w8(char *out, size_t n, const WCHAR *w) {   // a wide name from the network as UTF-8, or "" when it does not fit
    out[0] = 0;
    if (!w) return;
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    if (k <= 0) out[0] = 0;
}
static int wsuffix(const WCHAR *s, const WCHAR *suf) {   // s ends with suf, ignoring case
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && !_wcsicmp(s + a - b, suf);
}
static const WCHAR k_wtype[] = L"._pokopia-ldn._udp.local";
static VOID WINAPI w_browse_cb(DWORD st, PVOID ctx, PDNS_RECORD rec) {
    wctx *c = ctx; coop_node *n = c->n;
    if (st == ERROR_CANCELLED || !rec) { if (rec) WI(n)->rlfree(rec, DnsFreeRecordList); if (st == ERROR_CANCELLED) wctx_done(c); return; }
    if (c->dead) { WI(n)->rlfree(rec, DnsFreeRecordList); return; }
    for (DNS_RECORDW *p = (DNS_RECORDW *)rec; p; p = p->pNext) {
        if (p->wType != DNS_TYPE_PTR || !p->Data.PTR.pNameHost || !wsuffix(p->Data.PTR.pNameHost, k_wtype)) continue;
        const WCHAR *inst = p->Data.PTR.pNameHost;
        char name[64]; WCHAR wn[64]; size_t il = wcslen(inst) - wcslen(k_wtype);
        if (il == 0 || il >= 64) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); continue; }
        memcpy(wn, inst, il * sizeof(WCHAR)); wn[il] = 0; w8(name, sizeof name, wn);
        if (p->dwTtl == 0) { if (name[0]) lost(n, name, 0); continue; }   // a goodbye
        if (!name[0] || !strcmp(name, WI(n)->inst_name)) continue;
        const WCHAR *target = NULL; WORD port = 0; cdisc_entry txt; memset(&txt, 0, sizeof txt);
        int have_txt = 0;
        for (DNS_RECORDW *q = (DNS_RECORDW *)rec; q; q = q->pNext) {
            if (!q->pName || _wcsicmp(q->pName, inst)) continue;
            if (q->wType == DNS_TYPE_SRV && q->Data.SRV.pNameTarget) { target = q->Data.SRV.pNameTarget; port = q->Data.SRV.wPort; }
            if (q->wType == DNS_TYPE_TEXT && !have_txt) {   // the strings as a TXT record's wire form, for the shared parser
                uint8_t w[512]; size_t at = 0;
                for (DWORD i = 0; i < q->Data.TXT.dwStringCount && i < 16; i++) {
                    char kv[256]; w8(kv, sizeof kv, q->Data.TXT.pStringArray[i]);
                    at = txt_put(w, sizeof w, at, kv);
                }
                have_txt = cdisc_txt_parse(w, at, &txt);
            }
        }
        uint32_t ip = 0;
        for (DNS_RECORDW *q = (DNS_RECORDW *)rec; q && target; q = q->pNext)   // a responder that sends its A record along (Apple's does)
            if (q->wType == DNS_TYPE_A && q->pName && !_wcsicmp(q->pName, target)) {
                uint32_t a = ntohl(q->Data.A.IpAddress);
                if (coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(a))) { ip = a; break; }
            }
        if (!target || !port || !have_txt) continue;   // a later answer carries the rest
        found(n, name, 0, ip, port, &txt, 1);
    }
    WI(n)->rlfree(rec, DnsFreeRecordList);
}
static VOID WINAPI w_reg_cb(DWORD st, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    wctx *c = ctx; coop_node *n = c->n; wimpl *w = WI(n);
    if (inst) w->ifree(inst);
    cplat_lock(&n->dlk);
    int state = w->reg_state, done = 0;
    if (state == 3) { w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } w->rctx = NULL; done = 1; }
    else if (st == ERROR_SUCCESS) w->reg_state = 2;
    else { w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } w->rctx = NULL; done = 1; }
    n->disc.adv_on = w->reg_state == 2;
    cplat_unlock(&n->dlk);
    if (state == 3) coop_logf(n, "disc: stopped advertising");
    else if (st == ERROR_SUCCESS) coop_logf(n, "disc: advertising \"%s\" (%s) on port %u, addresses %s", w->inst_name, COOP_SERVICE, w->reg_port, w->addrs);
    else coop_logf(n, "disc: advertising failed (status %lu)", (unsigned long)st);
    if (done) wctx_done(c);
}

// ---------- pokopia-recomp extension: LAN sweep / mDNS responder (not in pokopia-native) ----------
// Where dnsapi.dll has no DnsService* (Wine and Proton, Windows before 10 1809; NXRT_COOP_NO_DNSSD=1 acts as if, a diagnostic that also sweeps
// 127.0.0.1; NXRT_COOP_NO_DNSSD=loopback sweeps 127.0.0.1 only) and
// cfg.x_lan_sweep_off is 0 ([net] lan_sweep = auto), two stand-ins for the backend above. Windows with DnsService* never runs them.
// LAN sweep (scanning): the first Scan, then a Scan at most every XSW_GAP_MS, starts a sweep on a thread of its own: each address of this machine's
// private (RFC 1918) IPv4 subnets (the subnet, or the /24 around this machine's address when larger: at most 254 per interface and 512 in all; its
// own addresses, loopback, link-local, public, point-to-point and virtual or VPN adapters left out) gets a non-blocking TCP connect to cfg.port, all
// at once, XSW_WAIT_MS for the lot. Each address that accepts becomes an entry ("sweep-<IPv4>", its TXT taken as this node's coop protocol), so the
// station's peers dial it, check its WELCOME and ask SCAN_REQ exactly as for a host DNS-SD found; the first Scan waits BROWSE_GRACE_MS for the
// first sweep (coop_ldn.c). Hosts already linked are not dialled again; an entry the next sweep finds neither linked nor answering is dropped.
// Nothing outside the private ranges is dialled; a host whose port was taken (it listens on one the system picked) is not found.
// mDNS responder (hosting): UDP 5353 (SO_REUSEADDR; the group 224.0.0.251 joined on each LAN interface) answers queries for _pokopia-ldn._udp.local
// PTR (and its instance's SRV / TXT, its host's A) with PTR + SRV + TXT + A, the records DnsServiceRegister publishes above: instance
// "Pokopia-<8 hex>", host pokopia-<8 hex>.local, the control port, TXT p, b, m, a, x (cdisc_txt_make), an A record per LAN address. It announces
// twice when hosting starts and says goodbye (TTL 0) when it stops, so visitors with DNS-SD (pokopia-native and this build on Windows) find a host
// under Wine. Queries from port 5353 are answered on the group (to the asker when it asks for a unicast answer), others as legacy unicast queries
// (ID and questions echoed, TTL 10). Responses and malformed packets are ignored, answers rate-limited; a port 5353 that cannot be had is logged once.
static const char k_xsvc[] = "_pokopia-ldn._udp.local";
static int x_virtual_name(const char *s) {   // lower case: a virtual, VPN or container adapter by its name or description
    static const char *has[] = {"virtual", "vpn", "vethernet", "hyper-v", "vmware", "virtualbox", "wireguard", "tailscale", "zerotier", "hamachi", "docker", "virbr", "veth"};
    static const char *starts[] = {"tun", "tap", "wg", "zt", "br-"};
    for (size_t i = 0; i < sizeof has / sizeof has[0]; i++) if (strstr(s, has[i])) return 1;
    for (size_t i = 0; i < sizeof starts / sizeof starts[0]; i++) if (!strncmp(s, starts[i], strlen(starts[i]))) return 1;
    return 0;
}
static void x_lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a'); }
static int x_virtual(const IP_ADAPTER_ADDRESSES *a) {
    if (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) return 1;
    char s[128];
    snprintf(s, sizeof s, "%s", a->AdapterName ? a->AdapterName : ""); x_lower(s); if (x_virtual_name(s)) return 1;
    w8(s, sizeof s, a->FriendlyName); x_lower(s); if (x_virtual_name(s)) return 1;
    w8(s, sizeof s, a->Description); x_lower(s); return x_virtual_name(s);
}
// The addresses to sweep (out has room for max + 1), this machine's own and the skipped ones left out; what: the subnets as text
static int x_sweep_list(uint32_t *out, int max, const uint32_t *skip, int nskip, char *what, size_t wn, int diag) {
    ULONG len = 16384, r = ERROR_BUFFER_OVERFLOW; IP_ADAPTER_ADDRESSES *aa = NULL;
    int k = 0; size_t wl = 0; what[0] = 0;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa);
        if (!(aa = malloc(len))) return 0;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &len);
    }
    uint32_t own[32], nets[16], masks[16]; int nown = 0, nnet = 0;
    for (IP_ADAPTER_ADDRESSES *a = r == NO_ERROR ? aa : NULL; a; a = a->Next)
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && nown < 32; u = u->Next)
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) own[nown++] = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
    for (IP_ADAPTER_ADDRESSES *a = r == NO_ERROR && diag != 2 ? aa : NULL; a && k < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || x_virtual(a)) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && k < max; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr); unsigned pl = u->OnLinkPrefixLength;
            if (!pl) pl = 24;   // no prefix reported: the /24 around it
            if (coop_addr4_class(ip) != COOP_ADDR_PRIVATE || pl > 30) continue;   // RFC 1918 only; /31 and /32 are point-to-point links
            uint32_t mask = pl < 24 ? 0xFFFFFF00u : 0xFFFFFFFFu << (32 - pl), net = ip & mask, bc = net | ~mask;
            int dup = 0;
            for (int i = 0; i < nnet; i++) dup |= nets[i] == net && masks[i] == mask;
            if (dup || nnet == 16) continue;
            nets[nnet] = net; masks[nnet++] = mask;
            int added = 0;
            for (uint32_t h = net + 1; h < bc && added < XSW_IF_MAX && k < max; h++, added++) {
                int no = 0;
                for (int i = 0; i < nown; i++) no |= own[i] == h;
                for (int i = 0; i < nskip; i++) no |= skip[i] == h;
                if (!no) out[k++] = h;
            }
            if (wl + 24 < wn) wl += (size_t)snprintf(what + wl, wn - wl, "%s%u.%u.%u.%u/%u", wl ? ", " : "", net >> 24, net >> 16 & 0xFF, net >> 8 & 0xFF, net & 0xFF, pl < 24 ? 24 : pl);
        }
    }
    free(aa);
    int lo_linked = 0;
    for (int i = 0; i < nskip; i++) lo_linked |= skip[i] == 0x7F000001u;
    if (diag && !lo_linked) {   // the diagnostic: a host on this machine (out has one spare)
        out[k++] = 0x7F000001u;
        if (wl + 40 < wn) snprintf(what + wl, wn - wl, "%s127.0.0.1 (NXRT_COOP_NO_DNSSD)", wl ? ", " : "");
    }
    return k;
}
static void xsw_main(void *arg) {   // the sweep thread: connects only; the I/O thread reads the result once done is set
    xsweep *s = arg;
    for (int b = 0; b < s->naddr && !s->stop; b += XSW_BATCH) {
        WSAPOLLFD pf[XSW_BATCH]; uint32_t ip[XSW_BATCH]; int k = 0;
        for (int i = b; i < s->naddr && i < b + XSW_BATCH; i++) {
            csock c = cplat_tcp_connect(s->addr[i], s->port);
            if (c == CSOCK_BAD) continue;
            pf[k].fd = c; pf[k].events = POLLWRNORM; pf[k].revents = 0; ip[k++] = s->addr[i];
        }
        int64_t until = cplat_now_ms() + XSW_WAIT_MS;
        while (k > 0 && !s->stop) {
            int64_t ms = until - cplat_now_ms();
            if (ms <= 0) break;
            int r = WSAPoll(pf, (ULONG)k, (INT)ms);
            if (r < 0) break;
            for (int i = 0; i < k; i++) {
                if (!pf[i].revents) continue;
                if ((pf[i].revents & POLLWRNORM) && cplat_tcp_connected(pf[i].fd) == 1 && s->nhit < XSW_MAX) s->hit[s->nhit++] = ip[i];   // accepted
                cplat_close(pf[i].fd);   // a host sees a link closed before its HELLO: dropped without a line
                k--; pf[i] = pf[k]; ip[i] = ip[k]; i--;
            }
        }
        for (int i = 0; i < k; i++) cplat_close(pf[i].fd);   // no answer in time
    }
    InterlockedExchange(&s->done, 1);
}
static int x_is_sweep(const cdisc_entry *e) { return e->used && !strncmp(e->name, "sweep-", 6); }
static void xsw_start(coop_node *n, wimpl *w, int64_t now) {
    xsweep *s = &w->xs;
    s->t = NULL; s->done = 0; s->stop = 0; s->nhit = 0; s->nalive = 0; s->t0 = now;
    s->port = n->cfg.port ? n->cfg.port : COOP_PORT_DEFAULT;
    for (int j = 0; j < PEERS; j++) {   // linked already (or dialling): not dialled again, kept
        peer *p = &n->pr[j];
        if (p->used && p->src == PEER_DISC && p->c.state != CONN_FREE && s->nalive < PEERS) s->alive[s->nalive++] = p->ip;
    }
    s->naddr = x_sweep_list(s->addr, XSW_MAX, s->alive, s->nalive, s->what, sizeof s->what, w->x_diag);
    s->on = 1; w->xs_at = now;
    if (!s->naddr || cplat_thread_start(&s->t, xsw_main, s)) { s->t = NULL; s->done = 1; }
}
static void xsw_join(wimpl *w) {
    xsweep *s = &w->xs;
    if (!s->on) return;
    InterlockedExchange(&s->stop, 1);
    if (s->t) cplat_thread_join(s->t);
    s->t = NULL; s->on = 0;
}
static void xsw_finish(coop_node *n, wimpl *w, int64_t now) {
    xsweep *s = &w->xs;
    if (s->t) cplat_thread_join(s->t);   // done: it has returned or is returning
    s->t = NULL; s->on = 0;
    cdisc_entry t; memset(&t, 0, sizeof t);
    t.proto = n->proto; t.has_txt = 1;   // no TXT: the link's WELCOME and SCAN_RESP tell the protocol, build and mods
    char name[64], a[16], hits[96] = ""; size_t hl = 0;
    for (int i = 0; i < s->nhit; i++) {
        ipstr(a, s->hit[i]); snprintf(name, sizeof name, "sweep-%s", a);
        found(n, name, 0, s->hit[i], s->port, &t, 0);
        if (hl + 18 < sizeof hits) hl += (size_t)snprintf(hits + hl, sizeof hits - hl, "%s%s", hl ? ", " : "", a);
    }
    cplat_lock(&n->dlk);   // an entry neither linked nor answering is gone
    for (int i = 0; i < DISC_MAX; i++) {
        cdisc_entry *e = &n->disc.e[i]; int keep = 0;
        if (!x_is_sweep(e)) continue;
        for (int j = 0; j < s->nhit; j++) keep |= e->ip == s->hit[j];
        for (int j = 0; j < s->nalive; j++) keep |= e->ip == s->alive[j];
        if (!keep) memset(e, 0, sizeof *e);
    }
    cplat_unlock(&n->dlk);
    uint64_t sum = 1469598103934665603ull;   // the result: an unchanged one is not logged every few seconds
    for (const char *p = s->what; *p; p++) sum = (sum ^ (uint8_t)*p) * 1099511628211ull;
    for (int i = 0; i < s->nhit; i++) sum = (sum ^ s->hit[i]) * 1099511628211ull;
    sum ^= (uint64_t)s->nalive << 56;
    if (sum != w->xs_logsum || now - w->xs_logat >= XSW_LOG_MS) {
        w->xs_logsum = sum; w->xs_logat = now;
        coop_logf(n, "disc: LAN sweep of %s (%d addresses, port %u, %lld ms): %d host(s) answered%s%s%s, %d linked already", s->what[0] ? s->what : "no private subnet",
                  s->naddr, s->port, (long long)(now - s->t0), s->nhit, s->nhit ? " (" : "", hits, s->nhit ? ")" : "", s->nalive);
    }
}
// mDNS packets
typedef struct xpk { uint8_t b[1200]; size_t n; int over; } xpk;
static void xp_put(xpk *p, const void *d, size_t k) { if (p->over || p->n + k > sizeof p->b) { p->over = 1; return; } memcpy(p->b + p->n, d, k); p->n += k; }
static void xp_16(xpk *p, unsigned v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; xp_put(p, b, 2); }
static void xp_32(xpk *p, uint32_t v) { xp_16(p, v >> 16); xp_16(p, v & 0xFFFF); }
static void xp_name(xpk *p, const char *s) {   // dotted, labels of 1-63 bytes
    while (*s) {
        const char *e = strchr(s, '.'); size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!l || l > 63) { p->over = 1; return; }
        uint8_t lb = (uint8_t)l; xp_put(p, &lb, 1); xp_put(p, s, l);
        s += l + (e ? 1 : 0);
    }
    uint8_t z = 0; xp_put(p, &z, 1);
}
static void xp_rdlen(xpk *p, size_t at) { if (!p->over) { size_t l = p->n - at - 2; p->b[at] = (uint8_t)(l >> 8); p->b[at + 1] = (uint8_t)l; } }
enum { XR_PTR = 1, XR_SRV = 2, XR_TXT = 4, XR_A = 8 };
static void x_names(const xmdns *m, char *inst, size_t in, char *host, size_t hn) {
    snprintf(inst, in, "%s.%s", m->inst, k_xsvc);
    snprintf(host, hn, "pokopia-%s.local", m->inst + 8);   // a name of its own, as above: the computer's name is not published
}
static int xp_records(xpk *p, coop_node *n, const xmdns *m, int which, uint32_t ttl_long, uint32_t ttl_short, int flush) {
    char inst[96], host[48]; size_t at; int c = 0; unsigned cls = flush ? 0x8001 : 1;   // the cache-flush bit on the unique records
    x_names(m, inst, sizeof inst, host, sizeof host);
    if (which & XR_PTR) { xp_name(p, k_xsvc); xp_16(p, 12); xp_16(p, 1); xp_32(p, ttl_long); at = p->n; xp_16(p, 0); xp_name(p, inst); xp_rdlen(p, at); c++; }
    if (which & XR_SRV) {
        xp_name(p, inst); xp_16(p, 33); xp_16(p, cls); xp_32(p, ttl_short); at = p->n; xp_16(p, 0);
        xp_16(p, 0); xp_16(p, 0); xp_16(p, m->port); xp_name(p, host); xp_rdlen(p, at); c++;
    }
    if (which & XR_TXT) {
        uint8_t t[200]; size_t tl = cdisc_txt_make(t, sizeof t, n->proto, n->build, n->mods, m->ips, m->nip);
        xp_name(p, inst); xp_16(p, 16); xp_16(p, cls); xp_32(p, ttl_long); xp_16(p, (unsigned)tl); xp_put(p, t, tl); c++;
    }
    if (which & XR_A)
        for (int i = 0; i < m->nip; i++) { xp_name(p, host); xp_16(p, 1); xp_16(p, cls); xp_32(p, ttl_short); xp_16(p, 4); xp_32(p, m->ips[i]); c++; }
    return c;
}
// A response: mode 0 multicast, 1 goodbye (TTL 0), 2 legacy unicast (ID and questions echoed, TTL 10, no cache-flush bit). 0 when it does not fit
static size_t xp_response(xpk *p, coop_node *n, const xmdns *m, uint16_t id, const uint8_t *qs, size_t qn, int nq, int ans, int add, int mode) {
    p->n = 0; p->over = 0;
    xp_16(p, id); xp_16(p, 0x8400); xp_16(p, (unsigned)nq); xp_16(p, 0); xp_16(p, 0); xp_16(p, 0);
    if (qn) xp_put(p, qs, qn);
    uint32_t lt = mode == 1 ? 0 : mode == 2 ? 10 : 4500, st = mode == 1 ? 0 : mode == 2 ? 10 : 120;
    int an = xp_records(p, n, m, ans, lt, st, mode != 2), ar = xp_records(p, n, m, add & ~ans, lt, st, mode != 2);
    if (p->over) return 0;
    p->b[6] = (uint8_t)(an >> 8); p->b[7] = (uint8_t)an; p->b[10] = (uint8_t)(ar >> 8); p->b[11] = (uint8_t)ar;
    return p->n;
}
static void xmd_multicast(xmdns *m, const xpk *p) {
    for (int i = 0; i < m->nif; i++) {
        struct in_addr ia; ia.s_addr = htonl(m->ifs[i]);
        setsockopt(m->s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&ia, sizeof ia);
        cplat_sendto(m->s, p->b, p->n, XMD_GROUP, XMD_PORT);
    }
    if (!m->nif) cplat_sendto(m->s, p->b, p->n, XMD_GROUP, XMD_PORT);
}
static int x_qname(const uint8_t *b, size_t n, size_t at, char *out, size_t cap, size_t *next) {   // a name, lower case and dotted; pointers followed
    size_t o = 0; int jumps = 0, jumped = 0;
    for (;;) {
        if (at >= n) return -1;
        uint8_t l = b[at];
        if (!l) { if (!jumped) *next = at + 1; break; }
        if ((l & 0xC0) == 0xC0) {
            if (at + 1 >= n || ++jumps > 16) return -1;
            if (!jumped) *next = at + 2;
            jumped = 1; at = (size_t)(l & 0x3F) << 8 | b[at + 1]; continue;
        }
        if ((l & 0xC0) || at + 1 + l > n || o + l + 2 > cap) return -1;
        if (o) out[o++] = '.';
        for (size_t i = 0; i < l; i++) { char c = (char)b[at + 1 + i]; out[o++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
        at += 1 + l;
    }
    out[o] = 0;
    return 0;
}
static void xmd_query(coop_node *n, xmdns *m, const uint8_t *b, size_t len, uint32_t ip, uint16_t port, int64_t now) {
    if (len < 12) return;
    unsigned id = (unsigned)b[0] << 8 | b[1], fl = (unsigned)b[2] << 8 | b[3], qd = (unsigned)b[4] << 8 | b[5];
    if ((fl & 0x8000) || (fl >> 11 & 0xF) || !qd || qd > 16) return;   // a response, another opcode, no or too many questions
    char inst[96], host[48], nm[256]; size_t at = 12, next = 0; int ans = 0, qu = 0;
    x_names(m, inst, sizeof inst, host, sizeof host); x_lower(inst); x_lower(host);
    for (unsigned q = 0; q < qd; q++) {
        if (x_qname(b, len, at, nm, sizeof nm, &next) || next + 4 > len) return;
        at = next;
        unsigned qt = (unsigned)b[at] << 8 | b[at + 1], qc = (unsigned)b[at + 2] << 8 | b[at + 3]; at += 4;
        if ((qc & 0x7FFF) != 1 && (qc & 0x7FFF) != 255) continue;
        qu |= (qc & 0x8000) != 0;
        if (!strcmp(nm, k_xsvc) && (qt == 12 || qt == 255)) ans |= XR_PTR;
        else if (!strcmp(nm, inst)) ans |= (qt == 33 || qt == 255 ? XR_SRV : 0) | (qt == 16 || qt == 255 ? XR_TXT : 0);
        else if (!strcmp(nm, host) && (qt == 1 || qt == 255)) ans |= XR_A;
    }
    if (!ans) return;
    int legacy = port != XMD_PORT, uni = legacy || qu;
    if (!bucket_take(uni ? &m->urate : &m->mrate, now, uni ? 10 : 2, uni ? 10 : 4, 1)) return;
    int add = ans & XR_PTR ? XR_SRV | XR_TXT | XR_A : ans & XR_SRV ? XR_TXT | XR_A : 0;
    xpk p;
    if (!xp_response(&p, n, m, legacy ? (uint16_t)id : 0, legacy ? b + 12 : NULL, legacy ? at - 12 : 0, legacy ? (int)qd : 0, ans, add, legacy ? 2 : 0)) return;
    if (uni) cplat_sendto(m->s, p.b, p.n, ip, port); else xmd_multicast(m, &p);
}
static void xmd_read(coop_node *n, wimpl *w) {
    xmdns *m = &w->xm; int64_t now = now_ms();
    for (int i = 0; i < 64 && m->on; i++) {
        uint8_t b[1500]; uint32_t ip; uint16_t port;
        int64_t k = cplat_recvfrom(m->s, b, sizeof b, &ip, &port);
        if (k == -3) continue;   // too big: dropped
        if (k < 0) break;
        xmd_query(n, m, b, (size_t)k, ip, port, now);
    }
}
static void xmd_stop(coop_node *n, wimpl *w) {
    xmdns *m = &w->xm;
    if (!m->on) return;
    xpk p;
    if (xp_response(&p, n, m, 0, NULL, 0, 0, XR_PTR | XR_SRV | XR_TXT | XR_A, 0, 1)) xmd_multicast(m, &p);   // goodbye
    cplat_close(m->s); m->s = CSOCK_BAD; m->on = 0; n->disc.adv_on = 0;
    coop_logf(n, "disc: stopped answering mDNS for \"%s\"", m->inst);
}
static void xmd_start(coop_node *n, wimpl *w, int64_t now) {
    xmdns *m = &w->xm;
    m->retry_at = now + DISC_RETRY_MS;
    m->nip = host_addrs(m->ips, m->shown, sizeof m->shown);
    if (!m->nip) { if (!w->noaddr_said++) coop_logf(n, "disc: no LAN address to answer mDNS with (this PC has no private or link-local address): visitors join by address"); return; }
    csock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == CSOCK_BAD) return;
    BOOL on = TRUE; DWORD ttl = 255, loop = 1; struct sockaddr_in a; memset(&a, 0, sizeof a);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);   // shared with any other responder on this machine
    a.sin_family = AF_INET; a.sin_port = htons(XMD_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) {
        int e = WSAGetLastError(); cplat_close(s); m->dead = 1;
        if (!m->said++) coop_logf(n, "disc: mDNS port 5353 cannot be had (error %d): visitors with DNS-SD do not see this host (they join by address); "
                                     "visitors of this build still find it with their LAN sweep", e);
        return;
    }
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof loop);
    caddr mine[16]; int nm = cplat_local_addrs(mine, 16, 0);
    m->nif = 0;
    for (int i = 0; i < nm && m->nif < 8; i++) {
        int cls = coop_addr4_class(mine[i].ip);
        if (cls != COOP_ADDR_PRIVATE && cls != COOP_ADDR_LINKLOCAL) continue;
        struct ip_mreq mr; mr.imr_multiaddr.s_addr = htonl(XMD_GROUP); mr.imr_interface.s_addr = htonl(mine[i].ip);
        if (!setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mr, sizeof mr)) m->ifs[m->nif++] = mine[i].ip;
    }
    if (!m->nif) {
        struct ip_mreq mr; mr.imr_multiaddr.s_addr = htonl(XMD_GROUP); mr.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mr, sizeof mr);
    }
    make_instance(m->inst, sizeof m->inst);
    m->s = s; m->on = 1; m->port = n->disc.adv_port; m->ann_left = 2; m->ann_at = now;
    memset(&m->mrate, 0, sizeof m->mrate); memset(&m->urate, 0, sizeof m->urate);
    n->disc.adv_on = 1;
    coop_logf(n, "disc: answering mDNS for \"%s\" (%s) on port %u, addresses %s (no DNS-SD here)", m->inst, COOP_SERVICE, m->port, m->shown);
}
static void x_tick(coop_node *n, wimpl *w, int64_t now) {
    cdisc *d = &n->disc; xmdns *m = &w->xm; xsweep *s = &w->xs;
    if ((d->adv_want || d->browse_want) && !w->warned) {
        w->warned = 1;
        coop_logf(n, "disc: no DNS-SD here (%s): a LAN sweep finds hosts while the game scans, an mDNS responder answers for this host while it hosts "
                     "([net] lan_sweep = off: join by address)", w->x_diag ? "NXRT_COOP_NO_DNSSD" : "dnsapi.dll has no DnsService*: Wine / Proton, or Windows before 10 1809");
    }
    if (m->on && (!d->adv_want || m->port != d->adv_port)) xmd_stop(n, w);
    if (!d->adv_want) m->dead = 0;
    else if (!m->on && !m->dead && now >= m->retry_at) xmd_start(n, w, now);
    if (m->on && m->ann_left && now >= m->ann_at) {   // unsolicited announcements
        xpk p;
        if (xp_response(&p, n, m, 0, NULL, 0, 0, XR_PTR | XR_SRV | XR_TXT | XR_A, 0, 0)) xmd_multicast(m, &p);
        m->ann_left--; m->ann_at = now + 1000;
    }
    if (s->on && s->done) xsw_finish(n, w, now);
    if (!d->browse_want) {
        if (s->on) xsw_join(w);
        w->xs_at = 0; d->browse_on = 0;
        int any = 0; cplat_lock(&n->dlk); for (int i = 0; i < DISC_MAX; i++) any |= d->e[i].used; cplat_unlock(&n->dlk);
        if (any) clear_entries(n);
    } else {
        if (!d->browse_on) coop_logf(n, "disc: sweeping the LAN for hosts on port %u while the game scans", n->cfg.port ? n->cfg.port : COOP_PORT_DEFAULT);
        d->browse_on = 1;
        if (!s->on && (!w->xs_at || (now - d->x_scan_at < XSW_SCAN_RECENT_MS && now - w->xs_at >= XSW_GAP_MS))) xsw_start(n, w, now);
    }
}
static void x_stop(coop_node *n, wimpl *w) { xsw_join(w); xmd_stop(n, w); w->xs_at = 0; n->disc.browse_on = 0; }

void cdisc_init(coop_node *n) {
    wimpl *w = calloc(1, sizeof *w);
    n->disc.impl = w;
    if (!w) return;
    const char *nd = getenv("NXRT_COOP_NO_DNSSD");   // pokopia-recomp extension: LAN sweep: a diagnostic, Windows as if it had no DnsService* (Wine)
    if (nd && *nd && strcmp(nd, "0")) { w->x_diag = strcmp(nd, "loopback") ? 1 : 2; return; }   // loopback: the sweep dials 127.0.0.1 only
    w->dll = LoadLibraryW(L"dnsapi.dll");
    if (!w->dll) return;
#define LOADF(f, name) (*(FARPROC *)&w->f = GetProcAddress(w->dll, name))
    LOADF(reg, "DnsServiceRegister"); LOADF(dereg, "DnsServiceDeRegister"); LOADF(browse, "DnsServiceBrowse"); LOADF(bcancel, "DnsServiceBrowseCancel");
    LOADF(construct, "DnsServiceConstructInstance"); LOADF(ifree, "DnsServiceFreeInstance"); LOADF(rlfree, "DnsRecordListFree");
#undef LOADF
    w->loaded = w->reg && w->dereg && w->browse && w->bcancel && w->construct && w->ifree && w->rlfree;
}
static void w_stop_browse(coop_node *n, int refresh) {
    wimpl *w = WI(n);
    cplat_lock(&n->dlk); wctx *c = w->bctx; w->bctx = NULL; DNS_SERVICE_CANCEL h = w->bcancel_h; cplat_unlock(&n->dlk);
    if (!c) return;
    c->dead = 1;
    if (w->bcancel(&h) != ERROR_SUCCESS) {}   // its ERROR_CANCELLED callback frees the context
    if (!refresh) n->disc.browse_on = 0;
}
static void w_deregister(coop_node *n) {
    wimpl *w = WI(n);
    cplat_lock(&n->dlk);
    int go = w->reg_state == 2;
    if (go) w->reg_state = 3;
    cplat_unlock(&n->dlk);
    if (go && w->dereg(&w->rreq, NULL) != DNS_REQUEST_PENDING) {   // no callback will come
        cplat_lock(&n->dlk); wctx *c = w->rctx; w->rctx = NULL; w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } n->disc.adv_on = 0; cplat_unlock(&n->dlk);
        if (c) wctx_done(c);
    }
}
void cdisc_fini(coop_node *n) {
    wimpl *w = WI(n);
    if (!w) return;
    if (w->loaded) {
        w_stop_browse(n, 0); w_deregister(n);
        for (int i = 0; i < 200 && g_wpending; i++) Sleep(10);   // the last callbacks name this node
    }
    else x_stop(n, w);   // pokopia-recomp extension: LAN sweep (stopped already when the I/O thread ran)
    if (w->dll && !g_wpending) FreeLibrary(w->dll);
    free(w); n->disc.impl = NULL;
}
csock cdisc_fd(coop_node *n) { wimpl *w = WI(n); return w && !w->loaded && w->xm.on ? w->xm.s : CSOCK_BAD; }   // pokopia-recomp extension: mDNS responder
void cdisc_process(coop_node *n) { wimpl *w = WI(n); if (w && !w->loaded && w->xm.on) xmd_read(n, w); }        // pokopia-recomp extension: mDNS responder
void cdisc_tick(coop_node *n) {
    wimpl *w = WI(n); cdisc *d = &n->disc; int64_t now = now_ms();
    if (!w) return;
    if (!w->loaded) {
        if (!n->cfg.x_lan_sweep_off) { x_tick(n, w, now); return; }   // pokopia-recomp extension: LAN sweep / mDNS responder
        if ((d->adv_want || d->browse_want) && !w->warned) { w->warned = 1; coop_logf(n, "disc: DNS-SD needs Windows 10 1809 or later (dnsapi.dll has no DnsService*): join by address (DM_COOP=join:<IPv4>)"); }
        return;
    }
    cplat_lock(&n->dlk); int rs = w->reg_state; cplat_unlock(&n->dlk);
    if (rs == 2 && (!d->adv_want || w->reg_port != d->adv_port)) w_deregister(n);
    if (w->bctx && (!d->browse_want || now - w->browse_at >= WBROWSE_REFRESH_MS)) w_stop_browse(n, d->browse_want);
    if (!d->browse_want) {
        int any = 0; cplat_lock(&n->dlk); for (int i = 0; i < DISC_MAX; i++) any |= n->disc.e[i].used; cplat_unlock(&n->dlk);
        if (any) clear_entries(n);
    } else {
        cplat_lock(&n->dlk);
        for (int i = 0; i < DISC_MAX; i++) if (d->e[i].used && d->e[i].seen && now - d->e[i].seen > WENTRY_STALE_MS) memset(&d->e[i], 0, sizeof d->e[i]);
        cplat_unlock(&n->dlk);
    }
    if (now < w->retry_at) return;
    if (d->adv_want && rs == 0) {
        uint32_t ips[DISC_ADDRS]; int k = host_addrs(ips, w->addrs, sizeof w->addrs);
        if (!k) {
            w->retry_at = now + DISC_RETRY_MS;
            if (!w->noaddr_said++) coop_logf(n, "disc: no LAN address to advertise (this PC has no private or link-local address): visitors join by address");
        } else {
            WCHAR inst[96], host[48], wv[5][80]; char v[5][80];
            make_instance(w->inst_name, sizeof w->inst_name);
            swprintf(inst, 96, L"%hs%ls", w->inst_name, k_wtype);
            swprintf(host, 48, L"pokopia-%hs.local", w->inst_name + 8);   // a name of its own: the computer's name is not published
            snprintf(v[0], 80, "%d", n->proto); snprintf(v[1], 80, "%s", n->build[0] ? n->build : "-"); snprintf(v[2], 80, "%016llx", (unsigned long long)n->mods);
            addr_list(v[3], 80, ips, 1); addr_list(v[4], 80, ips + 1, k - 1);
            for (int i = 0; i < 5; i++) swprintf(wv[i], 80, L"%hs", v[i]);
            PCWSTR keys[5] = {L"p", L"b", L"m", L"a", L"x"}, vals[5] = {wv[0], wv[1], wv[2], wv[3], wv[4]};
            IP4_ADDRESS a4 = htonl(ips[0]);   // one A record: the best address; a visitor on another of this PC's networks finds its own in x=
            PDNS_SERVICE_INSTANCE si = w->construct(inst, host, &a4, NULL, d->adv_port, 0, 0, k > 1 ? 5 : 4, keys, vals);
            wctx *c = si ? wctx_new(n, 2) : NULL;
            if (!si || !c) { if (si) w->ifree(si); w->retry_at = now + DISC_RETRY_MS; }
            else {
                cplat_lock(&n->dlk);
                w->inst = si; w->rctx = c; w->reg_port = d->adv_port; w->reg_state = 1;
                memset(&w->rreq, 0, sizeof w->rreq);
                w->rreq.Version = DNS_QUERY_REQUEST_VERSION1; w->rreq.pServiceInstance = si; w->rreq.pRegisterCompletionCallback = w_reg_cb; w->rreq.pQueryContext = c;
                cplat_unlock(&n->dlk);
                DWORD st = w->reg(&w->rreq, NULL);
                if (st != DNS_REQUEST_PENDING) {
                    cplat_lock(&n->dlk); w->reg_state = 0; w->rctx = NULL; w->inst = NULL; cplat_unlock(&n->dlk);
                    w->ifree(si); wctx_done(c); w->retry_at = now + DISC_RETRY_MS;
                    coop_logf(n, "disc: advertising failed to start (status %lu)", (unsigned long)st);
                }
            }
        }
    }
    if (d->browse_want && !w->bctx) {
        wctx *c = wctx_new(n, 1);
        if (!c) return;
        DNS_SERVICE_BROWSE_REQUEST br; memset(&br, 0, sizeof br);
        br.Version = DNS_QUERY_REQUEST_VERSION1; br.QueryName = L"_pokopia-ldn._udp.local"; br.pBrowseCallback = w_browse_cb; br.pQueryContext = c;
        DNS_SERVICE_CANCEL h; memset(&h, 0, sizeof h);
        cplat_lock(&n->dlk); w->bctx = c; cplat_unlock(&n->dlk);
        DNS_STATUS st = w->browse(&br, &h);
        if (st != DNS_REQUEST_PENDING) {
            cplat_lock(&n->dlk); w->bctx = NULL; cplat_unlock(&n->dlk);
            wctx_done(c); w->retry_at = now + DISC_RETRY_MS;
            coop_logf(n, "disc: browsing failed to start (status %ld)", (long)st);
        } else {
            cplat_lock(&n->dlk); if (w->bctx == c) w->bcancel_h = h; cplat_unlock(&n->dlk);
            if (!d->browse_on) coop_logf(n, "disc: browsing for %s", COOP_SERVICE);
            d->browse_on = 1; w->browse_at = now;
        }
    }
}
void cdisc_stop(coop_node *n) {
    wimpl *w = WI(n);
    if (w && w->loaded) { w_stop_browse(n, 0); w_deregister(n); }
    else if (w) x_stop(n, w);   // pokopia-recomp extension: LAN sweep / mDNS responder
    clear_entries(n);
}

#else
// ---------- elsewhere (Linux, Android until its NsdManager glue): no DNS-SD; join by address ----------
void cdisc_init(coop_node *n) { n->disc.impl = NULL; }
void cdisc_fini(coop_node *n) { (void)n; }
csock cdisc_fd(coop_node *n) { (void)n; return CSOCK_BAD; }
void cdisc_process(coop_node *n) { (void)n; }
void cdisc_tick(coop_node *n) {
    static int warned;
    if ((n->disc.adv_want || n->disc.browse_want) && !warned) { warned = 1; coop_logf(n, "disc: no DNS-SD on this platform: visitors join by address (DM_COOP=join:<IPv4>)"); }
}
void cdisc_stop(coop_node *n) { clear_entries(n); }
#endif
