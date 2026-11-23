// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Recompia: Co-op on the phone, the PC launcher's contract (launcher/src-tauri/src/coop.rs, coop_save.rs, coop_launch.rs) for joining:
 * a co-op save is a folder `<dataRoot>/coop-saves/<group id>/` (beside the singleplayer `save/`, coop.rs root_beside) with coop.marker
 * and coop.json; the world in `account/` arrives from the group (the game side's copy channel: incoming/, stamp.json, peers.json).
 * The invite is the launcher's long text `PKCOOP1.<base64url(JSON)>` (coop.rs invite), or its short code `XXX-XXX` kept on the public
 * relay for 6 hours (coop_code.rs, launcher/relay invite_client.rs: lookup and store). A phone can also make a co-op save and add
 * players (coop.rs create / add_member), as the creator's launcher does.
 * A start sets the launcher's environment (coop_launch.rs build / member_env) in GameActivity before SDL loads the runtime.
 */
object Coop {
    const val EXTRA_GROUP = "coop_group"
    const val EXTRA_START_AS = "coop_start_as"   // auto | host | visit (coop_save.rs StartAs: per start, never stored)

    private const val INVITE_PREFIX = "PKCOOP1."
    private const val MARKER = "coop.marker"
    private const val FILE = "coop.json"
    private const val KIND = "pokopia-recomp-coop-save"
    private const val MAX_MEMBERS = 4
    private const val MAX_NICK_BYTES = 32
    private const val ROOM_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

    data class Member(val id: String, val name: String)
    data class Invite(val id: String, val name: String, val accountUid: String, val roomCode: String, val relay: String, val members: List<Member>)
    data class Profile(val invite: Invite, val me: String) {
        val id get() = invite.id
        val name get() = invite.name
        fun member(id: String) = invite.members.firstOrNull { it.id == id }
    }

    val root: File get() = File(Paths.dataRoot, "coop-saves")
    fun folder(id: String) = File(root, id)

    private fun hex(s: String, n: Int) = s.length == n && s.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
    private fun roomOk(c: String) = c.split('-').let { p -> p.size == 4 && p.all { it.length == 4 && it.all { ch -> ch in ROOM_ALPHABET } } }
    private fun nameOk(s: String) = s.trim().length in 1..24
    /** coop.rs relay_ok: IPv4[:port], unicast, not 0.x. */
    fun relayOk(s: String): Boolean {
        val t = s.trim(); val ip = t.substringBefore(':')
        val port = if (':' in t) t.substringAfter(':').toIntOrNull() else 1
        val o = ip.split('.').map { it.toIntOrNull() }
        return o.size == 4 && o.all { it != null && it in 0..255 } && o[0] != 0 && o[0]!! < 224 && port != null && port in 1..65535
    }

    /** coop.rs parse_invite; null: not an invite. */
    fun parseInvite(text: String): Invite? = runCatching {
        val body = text.trim().removePrefix(INVITE_PREFIX).takeIf { text.trim().startsWith(INVITE_PREFIX) } ?: return null
        val j = JSONObject(String(Base64.decode(body, Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP), Charsets.UTF_8))
        val ms = j.getJSONArray("members").let { a -> (0 until a.length()).map { a.getJSONObject(it).let { m -> Member(m.getString("id"), m.getString("name")) } } }
        Invite(j.getString("id"), j.getString("name"), j.getString("account_uid"), j.getString("room_code"), j.getString("relay"), ms)
            .takeIf { i -> hex(i.id, 16) && hex(i.accountUid, 32) && roomOk(i.roomCode) && relayOk(i.relay) && i.members.size in 1..MAX_MEMBERS &&
                i.members.all { hex(it.id, 32) && nameOk(it.name) } }
    }.getOrNull()

    private fun markerGroup(dir: File): String? = runCatching {
        JSONObject(File(dir, MARKER).readText()).takeIf { it.getString("kind") == KIND && it.getInt("version") == 1 }?.getString("group")
    }.getOrNull()

    private fun read(dir: File): Profile? = runCatching {
        val j = JSONObject(File(dir, FILE).readText())
        if (j.optBoolean("split")) return null   // split screen is a PC thing
        val ms = j.getJSONArray("members").let { a -> (0 until a.length()).map { a.getJSONObject(it).let { m -> Member(m.getString("id"), m.getString("name")) } } }
        val p = Profile(Invite(j.getString("id"), j.getString("name"), j.getString("account_uid"), j.getString("room_code"), j.getString("relay"), ms), j.getString("me"))
        p.takeIf { markerGroup(dir) == p.id && p.member(p.me) != null }
    }.getOrNull()

    private fun write(p: Profile) {
        val j = JSONObject().put("version", 1).put("id", p.id).put("name", p.name).put("account_uid", p.invite.accountUid)
            .put("room_code", p.invite.roomCode).put("relay", p.invite.relay)
            .put("members", JSONArray(p.invite.members.map { JSONObject().put("id", it.id).put("name", it.name) })).put("me", p.me).put("split", false)
        val f = File(folder(p.id), FILE); val tmp = File(f.parentFile, "$FILE.tmp")
        tmp.writeText(j.toString(2)); if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }

    fun list(): List<Profile> = (root.listFiles() ?: emptyArray()).mapNotNull { read(it) }.sortedBy { it.name.lowercase() }
    fun get(id: String): Profile? = if (id.isNotEmpty() && id.all { it.isLetterOrDigit() }) read(folder(id)) else null

    /**
     * coop.rs accept: joins as `member` (never the creator, the first member) in a new, empty folder; when the save is here already, the
     * invite only refreshes its name, relay and players. Returns an error text, or null.
     */
    fun accept(text: String, member: String): String? {
        val inv = parseInvite(text) ?: return "This is not a co-op invite."
        read(folder(inv.id))?.let { have ->
            if (have.invite.accountUid != inv.accountUid || have.invite.roomCode != inv.roomCode) return "This invite does not match the co-op save you have."
            if (have.invite.members.firstOrNull()?.id == have.me) return "You made this co-op save: send the invite to the other players."
            if (inv.members.none { it.id == have.me }) return "You are no longer a player in this co-op save."
            write(Profile(inv, have.me)); return null
        }
        if (inv.members.firstOrNull()?.id == member || inv.members.none { it.id == member }) return "That player is not in the invite."
        val dir = folder(inv.id)
        if (dir.exists() && !dir.list().isNullOrEmpty()) return "${dir.path} already has files in it. A co-op save starts in a new, empty folder."
        if (!dir.mkdirs() && !dir.isDirectory) return "Cannot make ${dir.path}."
        File(dir, MARKER).writeText(JSONObject().put("kind", KIND).put("version", 1).put("group", inv.id).toString(2))
        write(Profile(inv, member))
        return null
    }

    // ---- making (coop.rs create / add_member / invite) ----------------------------------------------------------------------------------

    /** The launcher's public relay (launcher/src/linkplay-model.js PUBLIC_RELAY): where a new save plays and short codes live. */
    const val PUBLIC_RELAY = "204.216.111.91:47610"
    private const val BUILT_IN_UID = "000000000000000141434e4852656370"   // profiles.rs: the default user, never a new id
    private val rng = java.security.SecureRandom()

    /** profiles.rs new_uid: 32 hex digits, neither half zero, not the built-in user, not taken. */
    private fun newUid(taken: Collection<String>): String {
        while (true) {
            val u = (0 until 16).joinToString("") { "%02x".format(rng.nextInt(256)) }
            if (u.take(16) != "0".repeat(16) && u.drop(16) != "0".repeat(16) && u != BUILT_IN_UID && taken.none { it.equals(u, true) }) return u
        }
    }
    private fun newRoomCode() = (0 until 16).map { ROOM_ALPHABET[rng.nextInt(32)] }.chunked(4).joinToString("-") { it.joinToString("") }
    private fun cleanName(raw: String) = raw.trim().takeIf { it.isNotEmpty() && it.codePointCount(0, it.length) <= 24 && it.none { c -> c.isISOControl() } }

    /** coop.rs create: a new co-op save with this phone's player as its creator, in a new empty folder. Returns an error text, or null. */
    fun create(name: String, myName: String, relay: String = PUBLIC_RELAY): String? {
        val n = cleanName(name) ?: return "Type a name for the co-op save (1 to 24 characters)."
        val me = cleanName(myName) ?: return "Type a name for the player (1 to 24 characters)."
        if (!relayOk(relay)) return "The relay needs an address like 192.0.2.10 or 192.0.2.10:47610."
        if (list().any { it.name.equals(n, true) }) return "There is already a co-op save called $n."
        val m = Member(newUid(emptyList()), me)
        val p = Profile(Invite(newUid(listOf(m.id)).take(16), n, newUid(listOf(m.id)), newRoomCode(), relay.trim(), listOf(m)), m.id)
        val dir = folder(p.id)
        if (dir.exists() && !dir.list().isNullOrEmpty()) return "${dir.path} already has files in it. A co-op save starts in a new, empty folder."
        if (!dir.mkdirs() && !dir.isDirectory) return "Cannot make ${dir.path}."
        File(dir, MARKER).writeText(JSONObject().put("kind", KIND).put("version", 1).put("group", p.id).toString(2))
        write(p)
        return null
    }

    /**
     * coop.rs delete: removes co-op save `id` from this phone, its folder with this phone's copy of the world (coop.json, the marker,
     * the saves). Only a folder this module made (this group's marker) is removed; the relay and the other players are not touched.
     * Returns an error text, or null.
     */
    fun delete(id: String): String? {
        val p = get(id) ?: return "That co-op save does not exist."
        val dir = folder(p.id)
        if (markerGroup(dir) != p.id) return "That co-op save does not exist."
        return if (dir.deleteRecursively()) null else "Cannot delete ${dir.path}."
    }

    fun isCreator(p: Profile) = p.invite.members.firstOrNull()?.id == p.me

    /** coop.rs add_member: a player the invite names before they join; creator only, at most 4. Returns an error text, or null. */
    fun addMember(id: String, name: String): String? {
        val p = get(id) ?: return "That co-op save does not exist."
        if (!isCreator(p)) return "Only the player who made this co-op save can change its players."
        val n = cleanName(name) ?: return "Type a name for the player (1 to 24 characters)."
        if (p.invite.members.size >= MAX_MEMBERS) return "A co-op save has at most $MAX_MEMBERS players."
        if (p.invite.members.any { it.name.equals(n, true) }) return "There is already a player called $n."
        write(p.copy(invite = p.invite.copy(members = p.invite.members + Member(newUid(p.invite.members.map { it.id }), n))))
        return null
    }

    /** coop.rs invite: the text the creator hands to the others (it carries the room code: members only). */
    fun invite(p: Profile): String {
        val j = JSONObject().put("id", p.id).put("name", p.name).put("account_uid", p.invite.accountUid).put("room_code", p.invite.roomCode)
            .put("relay", p.invite.relay).put("members", JSONArray(p.invite.members.map { JSONObject().put("id", it.id).put("name", it.name) }))
        return INVITE_PREFIX + Base64.encodeToString(j.toString().toByteArray(), Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
    }

    // ---- short codes (coop_code.rs; launcher/relay/src/invite_client.rs, engine.rs "PKR1" 0xC9 put / 0xCA get) ------------------------

    private const val CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ"   // relay invites.rs ALPHABET
    private const val RELAY_MAGIC = 0x31524B50                          // "PKR1", little endian on the wire
    private const val X_INVITE_PUT = 0xC9; private const val X_INVITE_GET = 0xCA; private const val T_ERR = 4
    private const val E_NOROOM = 1; private const val E_LIMIT = 4; private const val BLOB_MAX = 2048; private const val TIMEOUT_MS = 4000

    /** invites.rs normalize: 6 symbols of the code alphabet, dashes, spaces and case ignored; null: not a short code. */
    fun shortCode(text: String): String? = text.filter { it != '-' && !it.isWhitespace() }.uppercase().takeIf { it.length == 6 && it.all { c -> c in CODE_ALPHABET } }

    private fun relayAddr(s: String): java.net.InetSocketAddress? {
        if (!relayOk(s)) return null
        val t = s.trim()
        return java.net.InetSocketAddress(t.substringBefore(':'), if (':' in t) t.substringAfter(':').toInt() else 47610)
    }

    /** One request on its own short link (invite_client.rs send / answer): the 32-byte hello and its body, then the 16-byte answer. */
    private fun ask(relay: java.net.InetSocketAddress, kind: Int, key: ByteArray, body: ByteArray): Pair<ByteArray, java.net.Socket> {
        val h = java.nio.ByteBuffer.allocate(32).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        h.putInt(RELAY_MAGIC).put(kind.toByte()).put(1); h.position(8); h.put(key); h.position(24); h.putInt(body.size)
        val sock = java.net.Socket()
        sock.connect(relay, TIMEOUT_MS); sock.soTimeout = TIMEOUT_MS
        sock.getOutputStream().apply { write(h.array()); write(body); flush() }
        val m = ByteArray(16); java.io.DataInputStream(sock.getInputStream()).readFully(m)
        return m to sock
    }
    private fun u32(b: ByteArray, at: Int) = java.nio.ByteBuffer.wrap(b, at, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int

    /**
     * coop_code.rs resolve: a short code is looked up on the public relay and its long invite comes back; any other text comes back as it
     * is. Blocking (network): never on the main thread. Failure: the launcher's error text.
     */
    fun resolve(text: String): Result<String> {
        val code = shortCode(text) ?: return Result.success(text.trim())
        val unreachable = "The relay could not look up this code. Ask your friend for the long code instead."
        return runCatching {
            val (m, sock) = ask(relayAddr(PUBLIC_RELAY)!!, X_INVITE_GET, code.toByteArray(), ByteArray(0))
            sock.use {
                if (u32(m, 0) != RELAY_MAGIC) throw IllegalStateException(unreachable)
                when {
                    (m[4].toInt() and 0xff) == X_INVITE_GET -> {
                        val n = u32(m, 8)
                        if (n !in 1..BLOB_MAX) throw IllegalStateException(unreachable)
                        ByteArray(n).also { java.io.DataInputStream(sock.getInputStream()).readFully(it) }.toString(Charsets.UTF_8)
                    }
                    (m[4].toInt() and 0xff) == T_ERR && (m[5].toInt() and 0xff) == E_NOROOM -> throw IllegalStateException("No invite has this code. Codes last 6 hours: ask for a new one, or for the long code.")
                    (m[4].toInt() and 0xff) == T_ERR && (m[5].toInt() and 0xff) == E_LIMIT -> throw IllegalStateException("Too many codes were tried from this phone. Wait a minute, then try again.")
                    else -> throw IllegalStateException(unreachable)
                }
            }
        }.recoverCatching { throw IllegalStateException(if (it is IllegalStateException) it.message else unreachable) }
    }

    /**
     * coop_code.rs shorten: keeps save `p`'s invite on the public relay for 6 hours under a short code (`XXX-XXX`), only when the save plays
     * on that relay (the invite holds the room's code; it never goes to another relay). The token names the save (sha256 of the group id and
     * room code, first 16 bytes), so a newer invite keeps the code. Blocking (network); null when there is none for any reason.
     */
    fun shorten(p: Profile): String? = runCatching {
        val at = relayAddr(PUBLIC_RELAY)!!
        if (relayAddr(p.invite.relay) != at) return null
        val md = java.security.MessageDigest.getInstance("SHA-256")
        md.update("recompia coop invite token\u0000".toByteArray()); md.update(p.id.toByteArray()); md.update(0.toByte()); md.update(p.invite.roomCode.toByteArray())
        val inv = invite(p).toByteArray()
        if (inv.size > BLOB_MAX) return null
        val (m, sock) = ask(at, X_INVITE_PUT, md.digest().copyOf(16), inv)
        sock.use {
            if (u32(m, 0) != RELAY_MAGIC || (m[4].toInt() and 0xff) != X_INVITE_PUT) return null
            shortCode(String(m, 8, 6, Charsets.US_ASCII))?.let { it.take(3) + "-" + it.drop(3) }
        }
    }.getOrNull()

    // coop_save.rs stamps: (seq, saved_at), stamp.json else the newest file time under account/ with seq 0
    private data class Stamp(val seq: Long, val at: Long) : Comparable<Stamp> {
        override fun compareTo(other: Stamp) = compareValuesBy(this, other, { it.seq }, { it.at })
    }
    private fun newest(f: File): Long = if (f.isDirectory) (f.listFiles() ?: emptyArray()).maxOfOrNull { newest(it) } ?: 0 else f.lastModified() / 1000
    private fun ownStamp(dir: File): Stamp = runCatching { JSONObject(File(dir, "stamp.json").readText()).let { Stamp(it.getLong("seq"), it.getLong("saved_at")) } }
        .getOrElse { Stamp(0, newest(File(dir, "account"))) }
    private fun peerStamps(dir: File): Map<String, Stamp> = runCatching {
        val p = JSONObject(File(dir, "peers.json").readText()).getJSONObject("peers")
        p.keys().asSequence().associateWith { k -> p.getJSONObject(k).let { Stamp(it.getLong("seq"), it.getLong("saved_at")) } }
    }.getOrDefault(emptyMap())

    /** coop_save.rs choose_host: the newest copy hosts when every other member's stamp is known, else the creator; true: this phone hosts. */
    fun hostsByDefault(p: Profile): Boolean {
        val dir = folder(p.id); val own = ownStamp(dir); val peers = peerStamps(dir); val ids = p.invite.members.map { it.id }
        val host = if (ids.all { it == p.me || it in peers }) ids.reversed().maxByOrNull { if (it == p.me) own else peers[it] ?: Stamp(0, 0) }
                   else ids.firstOrNull()
        return (host ?: p.me) == p.me
    }

    /** coop_launch.rs build: the environment of one co-op start (startAs: auto | host | visit). */
    fun env(p: Profile, startAs: String): Map<String, String> {
        val me = p.member(p.me)!!
        val host = when (startAs) { "host" -> true; "visit" -> false; else -> hostsByDefault(p) }
        val nick = buildString { for (c in me.name) { if (toString().toByteArray().size + c.toString().toByteArray().size > MAX_NICK_BYTES) break; append(c) } }
        return linkedMapOf(
            "NXRT_PKP_COOP" to "1", "NXRT_PKP_COOP_ROLE" to if (host) "host" else "visit", "NXRT_PKP_COOP_GROUP" to p.id,
            "NXRT_PKP_COOP_MEMBER" to me.id, "NXRT_PKP_COOP_MEMBERS" to p.invite.members.size.toString(),
            "NXRT_FS_SAVE_DIR" to folder(p.id).absolutePath, "NXRT_ACCOUNT_UID" to p.invite.accountUid, "NXRT_ACCOUNT_NICKNAME" to nick,
            "NXRT_NET_PROTOCOL" to "native", "NXRT_NET_UID" to me.id, "NXRT_PKP_LINK_INTERACT_EVERYWHERE" to "0",
            "NXRT_NET_MODE" to "auto", "NXRT_NET_INTERNET" to "true", "NXRT_NET_RELAY" to p.invite.relay, "NXRT_NET_PRIVATE_ROOM" to p.invite.roomCode,
        )
    }
}
