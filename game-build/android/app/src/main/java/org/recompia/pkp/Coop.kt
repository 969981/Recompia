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
 * The invite is the launcher's long text `PKCOOP1.<base64url(JSON)>` (coop.rs invite); the short 6-character codes live on the public
 * relay and are not resolved here. Making a co-op save and adding players stay on the PC (the creator's launcher).
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
