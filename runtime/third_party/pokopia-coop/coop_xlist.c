// pokopia-recomp extension (not in pokopia-native): the room list. MIT, as the module it extends.
// Internet play through a relay with no room code (cfg.x_list): the host lists its room at the relay with this session's new code, so anyone can
// find it with the game's own search (Visit a Friend); the game's own Link Code keeps a room private (the game checks it). A Scan asks the relay
// for its list, and a Connect to a listed room makes a peer for it with the room id and keys its listed code gives (cxl_kauth, cxl_room): the
// link then proves that code as any internet link does. The wire format is in coop_int.h and launcher/relay/src/lib.rs "Room list". A relay
// without the list (pokopia-native's, or an older one) says nothing in its OK and refuses a LIST hello: then nothing is listed and nothing breaks.
#include "coop_int.h"

#include <stdio.h>
#include <string.h>

#define XL_LIST_MS 3000    // a Scan asks the relay at most this often (it answers 120 lists a minute per address)
#define XL_WAIT_MS 2500    // a Scan waits this long for the list; the LIST link is given up after it
#define XL_KEEP_MS 15000   // after a failed list, the last good one is shown for this long
#define XL_GAP_MS 500      // a host lists a changed NetworkInfo at most this often (the relay takes one every 250 ms)
#define XL_REC_MAX (XL_REC_HDR + LDN_NETINFO)
#define XL_RX_MAX (RLY_MSG_LEN + XL_MAX * (RLY_MSG_LEN + XL_REC_MAX))

// ---------- host: the room's listing on the control link ----------
void cxl_host_ok(coop_node *n, uint32_t arg) {
    n->xl_cap = n->cfg.x_list && (arg & RLY_X_CAP_LIST);
    n->xl_gen = ~0ull; n->xl_next = 0;
    if (n->cfg.x_list && !n->xl_cap && n->xl_said != 1) {
        n->xl_said = 1;
        coop_logf(n, "mp: this relay does not list rooms (an older relay): visitors cannot find this room; give every player one room code instead");
    }
}
void cxl_host_reset(coop_node *n) { n->xl_cap = 0; }
void cxl_host_tick(coop_node *n, int64_t now) {
    if (!n->xl_cap || n->ls != COOP_LS_AP_CREATED || !n->have_info || n->xl_gen == n->info_gen || now < n->xl_next) return;
    size_t il = LDN_NETINFO;
    while (il > XL_INFO_MIN && !n->info[il - 1]) il--;   // the zeros at the end are not sent
    uint8_t m[RLY_MSG_LEN + XL_REC_MAX] = {0};             // the I/O thread's own stack
    put32(m, RLY_MAGIC); m[4] = RLY_X_LISTING; put32(m + 8, (uint32_t)(XL_REC_HDR + il));
    uint8_t *r = m + RLY_MSG_LEN;
    r[0] = 1; put16(r + 2, (uint32_t)il);
    memcpy(r + 4, n->cfg.room, COOP_ROOM_LEN);
    memcpy(r + 20, n->build, strlen(n->build));
    put64(r + 36, n->mods);
    memcpy(r + XL_REC_HDR, n->info, il);
    cmp_conn_raw(&n->rc, m, RLY_MSG_LEN + XL_REC_HDR + il);
    if (n->xl_said != 2) {
        n->xl_said = 2;
        coop_logf(n, "mp: the relay lists this room: anyone can find it with the game's search (the game's Link Code keeps a room private)");
    }
    n->xl_gen = n->info_gen; n->xl_next = now + XL_GAP_MS;
}

// ---------- station: the LIST link ----------
static void xl_end(coop_node *n, int state, const char *why) {
    conn *c = &n->xl_c;
    if (c->state != CONN_FREE) cplat_close(c->s);
    cmp_conn_init(c, CSOCK_BAD, CONN_FREE);
    n->xl_state = state;
    if (state == XL_DONE) n->xl_ok_at = cplat_now_ms();
    else if (cplat_now_ms() - n->xl_ok_at >= XL_KEEP_MS) n->nxl = 0;
    if (why && n->xl_slog != -1) { coop_logf(n, "mp: no room list from the relay: %s", why); n->xl_slog = -1; }
}
// One record from the relay: 1 and x filled when it is a room this build may join (its NetworkInfo checked as a host's SCAN_RESP is)
static int xl_take(coop_node *n, const uint8_t *r, size_t rl, xl_room *x) {
    size_t il = get16(r + 2);
    if (r[0] != 1 || il < XL_INFO_MIN || il > LDN_NETINFO || rl != XL_REC_HDR + il) return 0;
    char code[COOP_ROOM_LEN + 1];
    memcpy(code, r + 4, COOP_ROOM_LEN); code[COOP_ROOM_LEN] = 0;
    if (!coop_room_normalize(code, x->code) || memcmp(code, x->code, COOP_ROOM_LEN)) return 0;   // normalized, as hosts send it
    for (int i = 0; i < 16; i++) {
        uint8_t b = r[20 + i];
        if (b && !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F'))) return 0;
        x->build[i] = (char)b;
    }
    x->build[16] = 0;
    x->mods = get64(r + 36);
    if ((n->build[0] && x->build[0] && strncmp(n->build, x->build, 16)) || x->mods != n->mods) return 0;   // the relay filtered; checked again
    memset(x->info, 0, LDN_NETINFO);
    memcpy(x->info, r + XL_REC_HDR, il);
    char why[96];
    if (!cldn_info_sane(x->info, why, sizeof why)) { n->bad_input++; return 0; }
    croom_keys(x->code, x->k_auth, x->room_id);
    return 1;
}
// The answer so far: -1 not all of it yet, 0 bad (or the relay refused), 1 taken into n->xl
static int xl_parse(coop_node *n) {
    const uint8_t *b = n->xl_c.rx; size_t len = n->xl_c.rxn;
    if (len < RLY_MSG_LEN) return -1;
    if (get32(b) != RLY_MAGIC) return 0;
    if (b[4] == RLY_ERR) return 0;
    if (b[4] != RLY_X_LISTHDR) return 0;
    uint32_t a = get32(b + 8); int cnt = (int)(a & 0xFFFF);
    if (cnt > XL_MAX) return 0;
    size_t off = RLY_MSG_LEN;
    for (int i = 0; i < cnt; i++) {
        if (len - off < RLY_MSG_LEN) return -1;
        const uint8_t *e = b + off; size_t rl = get32(e + 8);
        if (get32(e) != RLY_MAGIC || e[4] != RLY_X_ENTRY || rl < XL_REC_HDR + XL_INFO_MIN || rl > XL_REC_MAX) return 0;
        if (len - off - RLY_MSG_LEN < rl) return -1;
        off += RLY_MSG_LEN + rl;
    }
    n->nxl = 0; off = RLY_MSG_LEN;
    for (int i = 0; i < cnt; i++) {
        size_t rl = get32(b + off + 8);
        if (xl_take(n, b + off + RLY_MSG_LEN, rl, &n->xl[n->nxl])) n->nxl++;
        off += RLY_MSG_LEN + rl;
    }
    n->xl_rooms = (int)(a >> 16); n->xl_players = (int)get32(b + 12);
    int said = n->nxl | (n->xl_rooms & 0xFFFF) << 8;
    if (said != n->xl_slog) {
        coop_logf(n, "mp: the relay lists %d room(s) this build can visit (%d room(s) open there, %d player(s) in the listed ones)", n->nxl, n->xl_rooms,
                  n->xl_players);
        n->xl_slog = said;
    }
    return 1;
}
void cxl_io(coop_node *n, csock s, int got) {
    conn *c = &n->xl_c;
    if (c->state == CONN_FREE || c->s != s) return;
    if (c->state == CONN_CONNECTING) {
        if (cplat_tcp_connected(c->s) != 1) { xl_end(n, XL_FAILED, "the relay cannot be reached"); return; }
        c->state = CONN_READY;
        uint8_t h[RLY_HELLO_LEN] = {0};
        put32(h, RLY_MAGIC); h[4] = RLY_X_LIST; h[5] = 1;
        h[6] = (uint8_t)(2 | (n->build[0] ? 1 : 0)); h[7] = XL_MAX;   // filter: these mods, and this build when it is known
        memcpy(h + 8, n->build, strlen(n->build)); put64(h + 24, n->mods);
        cmp_conn_raw(c, h, sizeof h);
        return;
    }
    for (int round = 0; round < 16 && (got & (CP_IN | CP_ERR)); round++) {
        if (c->rxn >= XL_RX_MAX || cmp_grow(&c->rx, &c->rxcap, c->rxn + 4096)) { xl_end(n, XL_FAILED, "an answer too long"); return; }
        size_t room = XL_RX_MAX - c->rxn; if (room > 4096) room = 4096;
        int64_t k = cplat_recv(c->s, c->rx + c->rxn, room);
        if (k < -1) { xl_end(n, XL_FAILED, "the link failed"); return; }
        if (k == -1) break;
        if (k > 0) c->rxn += (size_t)k;
        int r = xl_parse(n);
        if (r == 1) { xl_end(n, XL_DONE, NULL); return; }
        if (r == 0) {
            int refused = c->rxn >= RLY_MSG_LEN && get32(c->rx) == RLY_MAGIC && c->rx[4] == RLY_ERR;
            xl_end(n, XL_FAILED, refused && c->rx[5] == RLY_E_BAD ? "this relay does not list rooms (an older relay): every player needs one room code"
                                 : refused ? cmp_relay_err(c->rx[5]) : "a bad answer");
            return;
        }
        if (k == 0) { xl_end(n, XL_FAILED, "the relay closed the link early"); return; }
    }
    if (c->state != CONN_FREE && (got & CP_OUT)) cmp_conn_flush(c);
}
void cxl_timers(coop_node *n, int64_t now) {
    if (n->xl_c.state != CONN_FREE && now - n->xl_c.opened >= XL_WAIT_MS) xl_end(n, XL_FAILED, "the relay did not answer in time");
}
void cxl_stop(coop_node *n) {
    if (n->xl_c.state != CONN_FREE) cplat_close(n->xl_c.s);
    cmp_conn_init(&n->xl_c, CSOCK_BAD, CONN_FREE);
    n->xl_state = XL_IDLE; n->nxl = 0; n->xl_at = n->xl_ok_at = 0; n->xl_slog = -2;
}
void cxl_scan(coop_node *n) {
    if (!n->cfg.x_list) return;
    for (int j = 0; j < PEERS; j++) {   // listed rooms dialled for a Connect that is over (not joined): their links close
        peer *p = &n->pr[j];
        if (p->used && p->src == PEER_X_LIST && j != n->upi && !(n->join_wait && n->join_peer == j)) cmp_peer_free(n, p);
    }
    int64_t now = cplat_now_ms();
    if (n->xl_state != XL_BUSY && (n->xl_state != XL_DONE || now - n->xl_at >= XL_LIST_MS)) {
        n->xl_at = now;
        csock s = cplat_tcp_connect(n->cfg.relay_ip, n->cfg.relay_port);
        if (s == CSOCK_BAD) { xl_end(n, XL_FAILED, "no socket"); return; }
        cmp_conn_init(&n->xl_c, s, CONN_CONNECTING);
        n->xl_c.peer_ip = n->cfg.relay_ip; n->xl_c.peer_port = n->cfg.relay_port;
        n->xl_state = XL_BUSY;
    }
    while (n->xl_state == XL_BUSY && n->io_on) cldn_wait(n, 2000000);   // the I/O thread ends it (cxl_timers: XL_WAIT_MS at most)
}
peer *cxl_peer(coop_node *n, const uint8_t *netid) {
    if (!n->cfg.x_list) return NULL;
    xl_room *x = NULL;
    for (int i = 0; i < n->nxl && !x; i++) if (!memcmp(n->xl[i].info, netid, 0x20)) x = &n->xl[i];   // NetworkId
    if (!x) return NULL;
    peer *p = NULL;
    for (int j = 0; j < PEERS && !p; j++) if (!n->pr[j].used) p = &n->pr[j];
    for (int j = 0; j < PEERS && !p; j++) if (n->pr[j].src == PEER_X_LIST && j != n->upi) p = &n->pr[j];
    if (!p) return NULL;
    cmp_peer_free(n, p);
    p->used = 1; p->src = PEER_X_LIST; p->ip = n->cfg.relay_ip; p->port = n->cfg.relay_port; p->relay = 1; p->compat = 1;
    memcpy(p->build, x->build, sizeof p->build); p->mods = x->mods;
    memcpy(p->scan_info, x->info, LDN_NETINFO); p->scan_has = 1;
    memcpy(p->x_kauth, x->k_auth, 32); memcpy(p->x_room, x->room_id, 16);
    snprintf(p->name, sizeof p->name, "listed room %.4s", x->code);
    return p;
}
