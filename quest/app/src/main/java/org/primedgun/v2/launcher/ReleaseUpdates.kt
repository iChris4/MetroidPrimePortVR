// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

internal object ReleaseUpdates {
    const val PAGE = "https://github.com/iChris4/MetroidPrimePortVR/releases"
    private const val ASSETS = "$PAGE/download/"
    private val version = Regex("""^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(alpha|beta|rc)\.(0|[1-9][0-9]*))?$""")
    private val sha = Regex("[0-9a-fA-F]{64}")
    data class Release(val version: String, val notes: String, val name: String, val url: String,
                       val size: Long, val digest: String, val checksumUrl: String, val page: String)
    fun compare(a: String, b: String): Int {
        val x = version.matchEntire(a)?.groupValues ?: return 0
        val y = version.matchEntire(b)?.groupValues ?: return 0
        for (i in 1..5) {
            val c = if (i == 4) {
                val stages = listOf("alpha", "beta", "rc", "")
                stages.indexOf(x[i]).compareTo(stages.indexOf(y[i]))
            } else x[i].length.compareTo(y[i].length).takeIf { it != 0 } ?: x[i].compareTo(y[i])
            if (c != 0) return c
        }
        return 0
    }
    fun select(releases: JSONArray, current: String, previews: Boolean): Release? {
        var best: Release? = null
        for (i in 0 until releases.length()) {
            val r = releases.getJSONObject(i)
            val tag = r.optString("tag_name")
            if (r.optBoolean("draft") || !version.matches(tag) || compare(tag, current) <= 0 ||
                (!previews && (r.optBoolean("prerelease") || '-' in tag)) ||
                (best != null && compare(tag, best.version) <= 0)) continue
            val v = tag.removePrefix("v")
            val expected = "PrimedGun-v$v-Quest.apk"
            var assetUrl = ""; var size = 0L; var digest = ""; var sums = ""
            val assets = r.optJSONArray("assets") ?: continue
            for (j in 0 until assets.length()) {
                val a = assets.getJSONObject(j)
                val url = a.optString("browser_download_url")
                if (!url.startsWith(ASSETS) || a.optString("state") != "uploaded") continue
                if (a.optString("name") == "SHA256SUMS.txt") sums = url
                if (a.optString("name") == expected) {
                    assetUrl = url; size = a.optLong("size")
                    val raw = a.optString("digest")
                    digest = if (raw.startsWith("sha256:") && sha.matches(raw.removePrefix("sha256:"))) raw.removePrefix("sha256:").lowercase() else ""
                }
            }
            if (assetUrl.isNotEmpty() && size in 1..2147483648L) {
                best = Release(v, r.optString("body"), expected, assetUrl, size, digest, sums, "$PAGE/tag/$tag")
            }
        }
        return best
    }
    fun checksum(text: String, name: String): String {
        val pattern = Regex("""^([0-9a-fA-F]{64})[ \t]+\*?(.+)$""")
        val matches = text.lineSequence().mapNotNull { pattern.matchEntire(it.trim()) }
            .filter { it.groupValues[2] == name }.toList()
        return if (matches.size == 1) matches[0].groupValues[1].lowercase() else ""
    }
    private fun connection(address: String): HttpURLConnection {
        var url = URL(address)
        repeat(8) {
            require(url.protocol == "https") { "Update downloads require HTTPS." }
            val c = url.openConnection() as HttpURLConnection
            c.instanceFollowRedirects = false; c.connectTimeout = 15000; c.readTimeout = 30000
            c.setRequestProperty("User-Agent", "PrimedGun-Quest-Updater")
            c.setRequestProperty("Accept", "application/vnd.github+json")
            val status = c.responseCode
            if (status in listOf(301,302,303,307,308)) {
                val location = c.getHeaderField("Location")
                c.disconnect()
                require(!location.isNullOrEmpty()) { "Invalid update redirect." }
                url = URL(url, location)
            } else {
                if (status != 200) { c.disconnect(); error("GitHub returned HTTP $status. Please try again later.") }
                return c
            }
        }
        error("Too many update redirects.")
    }
    suspend fun text(url: String, limit: Long = 8L*1024*1024): String {
        val c = connection(url)
        try {
            c.inputStream.use { input ->
                val out = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(16384)
                while (true) {
                    currentCoroutineContext().ensureActive()
                    val n = input.read(buffer); if (n < 0) break
                    require(out.size().toLong() + n <= limit) { "Release information is too large." }
                    out.write(buffer, 0, n)
                }
                return out.toString("UTF-8")
            }
        } finally { c.disconnect() }
    }
    suspend fun latest(current: String, previews: Boolean) = select(
        JSONArray(text("https://api.github.com/repos/iChris4/MetroidPrimePortVR/releases?per_page=100")), current, previews)
    suspend fun download(release: Release, file: File, progress: (Int) -> Unit) {
        val expected = release.digest.ifEmpty {
            require(release.checksumUrl.isNotEmpty()) { "This release has no SHA-256 checksum. Please update manually from GitHub." }
            checksum(text(release.checksumUrl, 1048576), release.name)
        }
        require(sha.matches(expected)) { "No unambiguous checksum was found for this APK." }
        val c = connection(release.url)
        var complete = false
        try {
            val hash = MessageDigest.getInstance("SHA-256")
            var total = 0L; var last = -1
            c.inputStream.use { input -> file.outputStream().use { output ->
                val buffer = ByteArray(65536)
                while (true) {
                    currentCoroutineContext().ensureActive()
                    val n = input.read(buffer); if (n < 0) break
                    total += n
                    require(total <= release.size) { "APK exceeds the published download size." }
                    hash.update(buffer,0,n); output.write(buffer,0,n)
                    val percent = (total * 100 / release.size).toInt()
                    if (percent != last) { last=percent; progress(percent) }
                }
            } }
            require(total == release.size && hash.digest().joinToString("") { "%02x".format(it) } == expected.lowercase()) {
                "Update verification failed. Nothing has been installed."
            }
            complete = true
        } finally { c.disconnect(); if (!complete) file.delete() }
    }
}
