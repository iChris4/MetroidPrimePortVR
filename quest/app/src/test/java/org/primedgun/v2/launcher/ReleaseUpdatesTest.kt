package org.primedgun.v2.launcher
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
class ReleaseUpdatesTest {
    private fun release(v: String, suffix: String="Quest.apk", preview: Boolean=true) = JSONObject()
        .put("tag_name","v$v").put("prerelease",preview).put("assets",JSONArray().put(JSONObject()
            .put("name","PrimedGun-v$v-$suffix").put("state","uploaded").put("size",1234)
            .put("digest","sha256:"+"a".repeat(64))
            .put("browser_download_url","https://github.com/iChris4/MetroidPrimePortVR/releases/download/v$v/PrimedGun-v$v-$suffix")))
    @Test fun versionOrdering() {
        for ((a,b) in listOf("2.0.0-beta.10" to "2.0.0-beta.9", "2.0.0" to "2.0.0-rc.99",
                "2.0.0-rc.1" to "2.0.0-beta.99", "2.0.0-beta.1" to "2.0.0-alpha.99",
                "2.1.0-alpha.1" to "2.0.9")) assertTrue(ReleaseUpdates.compare(a,b)>0)
        assertEquals(0,ReleaseUpdates.compare("v2.0.0-beta.6","2.0.0-beta.6"))
    }
    @Test fun selectCorrectPlatformAndHighestReadyVersion() {
        val all=JSONArray().put(release("2.0.0-beta.7")).put(release("2.0.0-beta.10"))
            .put(release("2.0.0-beta.8")).put(release("2.0.0-beta.11","Windows-x64.zip"))
        assertEquals("2.0.0-beta.10",ReleaseUpdates.select(all,"2.0.0-beta.6",true)?.version)
        assertNull(ReleaseUpdates.select(all,"2.0.0-beta.10",true))
        assertNull(ReleaseUpdates.select(all,"2.0.0-beta.6",false))
        all.put(release("2.0.0",preview=false))
        assertEquals("2.0.0",ReleaseUpdates.select(all,"2.0.0-beta.6",false)?.version)
    }
    @Test fun rejectDraftInvalidAndForeignDownloads() {
        assertNull(ReleaseUpdates.select(JSONArray().put(release("3.0.0").put("draft",true)),"2.0.0",true))
        assertNull(ReleaseUpdates.select(JSONArray().put(release("2.0.0-beta.06")),"2.0.0-beta.5",true))
        val r=release("3.0.0")
        r.getJSONArray("assets").getJSONObject(0).put("browser_download_url","https://example.com/app.apk")
        assertNull(ReleaseUpdates.select(JSONArray().put(r),"2.0.0",true))
    }
    @Test fun exactChecksumAndAmbiguity() {
        val hash="a".repeat(64)
        assertEquals(hash,ReleaseUpdates.checksum("$hash  app.apk\r\n","app.apk"))
        assertEquals(hash,ReleaseUpdates.checksum("$hash *app.apk\n","app.apk"))
        assertEquals("",ReleaseUpdates.checksum("$hash  other.apk\n","app.apk"))
        assertEquals("",ReleaseUpdates.checksum("$hash  app.apk\n$hash  app.apk","app.apk"))
    }
}
